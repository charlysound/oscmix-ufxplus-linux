#include <string.h>
#include "transport.h"
#include "../intpack.h"
#include "../osc.h"

struct OscTransport {
	MixModel *model;
	GSocket *socket;
	GSocketAddress *send_address;
	GThread *thread;
	GMutex send_mutex;
	GMutex activity_mutex;
	gint64 last_receive_time;
	gint running;
	gint metering;
};

static GQuark
transport_error_quark(void)
{
	return g_quark_from_static_string("oscmix-gtk4-transport-error");
}

static gboolean process_message(OscTransport *self, const unsigned char *data,
	gsize size, GError **error);

static gboolean
process_bundle(OscTransport *self, struct oscmsg *message, GError **error)
{
	guint32 element_size;

	if (message->end - message->buf < 8) {
		g_set_error_literal(error, transport_error_quark(), 1,
		                    "OSC bundle without timetag");
		return FALSE;
	}
	message->buf += 8;
	while (message->buf < message->end) {
		if (message->end - message->buf < 4) {
			g_set_error_literal(error, transport_error_quark(), 1,
			                    "truncated OSC bundle element");
			return FALSE;
		}
		element_size = getbe32(message->buf);
		message->buf += 4;
		if (element_size == 0 || element_size > (guint32)(message->end - message->buf)) {
			g_set_error_literal(error, transport_error_quark(), 1,
			                    "invalid OSC bundle element size");
			return FALSE;
		}
		if (!process_message(self, message->buf, element_size, error))
			return FALSE;
		message->buf += element_size;
	}
	return TRUE;
}

static gboolean
process_message(OscTransport *self, const unsigned char *data, gsize size,
	GError **error)
{
	struct oscmsg message;
	OscEvent event;
	char *address, *types;

	memset(&message, 0, sizeof message);
	message.buf = (unsigned char *)data;
	message.end = message.buf + size;
	address = oscgetstr(&message);
	if (message.err || !address) {
		g_set_error(error, transport_error_quark(), 1, "invalid OSC address: %s",
		            message.err ? message.err : "missing address");
		return FALSE;
	}
	if (strcmp(address, "#bundle") == 0)
		return process_bundle(self, &message, error);
	if (address[0] != '/') {
		g_set_error_literal(error, transport_error_quark(), 1,
		                    "OSC address does not begin with '/'");
		return FALSE;
	}
	types = oscgetstr(&message);
	if (message.err || !types || types[0] != ',') {
		g_set_error(error, transport_error_quark(), 1, "invalid OSC type string: %s",
		            message.err ? message.err : "missing comma");
		return FALSE;
	}
	if (!g_atomic_int_get(&self->metering) &&
	    (g_str_has_suffix(address, "/level") ||
	     g_str_has_suffix(address, "/meter")))
		return TRUE;
	memset(&event, 0, sizeof event);
	event.address = address;
	event.received_at = g_get_monotonic_time();
	message.type = types + 1;
	while (*message.type && event.n_args < OSCMIX_OSC_MAX_ARGS) {
		OscArg *argument = &event.args[event.n_args];

		argument->type = *message.type;
		switch (*message.type) {
		case 'i': case 'T': case 'F':
			argument->type = 'i';
			argument->value.i = oscgetint(&message);
			break;
		case 'f':
			argument->value.f = oscgetfloat(&message);
			break;
		case 's': case 'N':
			argument->type = 's';
			argument->value.s = oscgetstr(&message);
			break;
		default:
			g_set_error(error, transport_error_quark(), 1,
			            "unsupported OSC argument type '%c'", *message.type);
			return FALSE;
		}
		if (message.err) {
			g_set_error(error, transport_error_quark(), 1,
			            "invalid OSC argument: %s", message.err);
			return FALSE;
		}
		++event.n_args;
	}
	g_mutex_lock(&self->activity_mutex);
	self->last_receive_time = event.received_at;
	g_mutex_unlock(&self->activity_mutex);
	mix_model_apply_event(self->model, &event);
	return TRUE;
}

gboolean
osc_transport_process_packet(OscTransport *self, const unsigned char *data,
	gsize size, GError **error)
{
	if (!data || size == 0) {
		g_set_error_literal(error, transport_error_quark(), 1, "empty OSC packet");
		return FALSE;
	}
	return process_message(self, data, size, error);
}

static gpointer
receive_thread(gpointer data)
{
	OscTransport *self = data;
	unsigned char buffer[65536];
	GError *error;
	gssize size;

	while (g_atomic_int_get(&self->running)) {
		error = NULL;
		if (!g_socket_condition_timed_wait(self->socket, G_IO_IN, 100000,
		                                   NULL, &error)) {
			if (error) {
				if (g_atomic_int_get(&self->running) &&
				    !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
					g_warning("OSC receive wait: %s", error->message);
				g_clear_error(&error);
			}
			continue;
		}
		size = g_socket_receive(self->socket, (char *)buffer, sizeof buffer,
		                        NULL, &error);
		if (size < 0) {
			if (g_atomic_int_get(&self->running))
				g_warning("OSC receive: %s", error->message);
			g_clear_error(&error);
			continue;
		}
		if (!osc_transport_process_packet(self, buffer, size, &error)) {
			g_warning("OSC packet ignored: %s", error->message);
			g_clear_error(&error);
		}
	}
	return NULL;
}

OscTransport *
osc_transport_new(MixModel *model, const char *send_host, guint16 send_port,
	const char *recv_host, guint16 recv_port, GError **error)
{
	OscTransport *self;
	GInetAddress *address;
	GSocketAddress *receive_address;

	self = g_new0(OscTransport, 1);
	self->model = model;
	g_atomic_int_set(&self->metering, TRUE);
	g_mutex_init(&self->send_mutex);
	g_mutex_init(&self->activity_mutex);
	self->socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
	                            G_SOCKET_PROTOCOL_UDP, error);
	if (!self->socket)
		goto fail;
	address = g_inet_address_new_from_string(recv_host);
	if (!address) {
		g_set_error(error, transport_error_quark(), 1,
		            "invalid receive address '%s'", recv_host);
		goto fail;
	}
	receive_address = g_inet_socket_address_new(address, recv_port);
	g_object_unref(address);
	if (!g_socket_bind(self->socket, receive_address, FALSE, error)) {
		g_object_unref(receive_address);
		goto fail;
	}
	g_object_unref(receive_address);
	address = g_inet_address_new_from_string(send_host);
	if (!address) {
		g_set_error(error, transport_error_quark(), 1,
		            "invalid send address '%s'", send_host);
		goto fail;
	}
	self->send_address = g_inet_socket_address_new(address, send_port);
	g_object_unref(address);
	return self;

fail:
	osc_transport_free(self);
	return NULL;
}

void
osc_transport_start(OscTransport *self)
{
	if (self->thread)
		return;
	g_atomic_int_set(&self->running, TRUE);
	self->thread = g_thread_new("oscmix-osc", receive_thread, self);
}

void
osc_transport_set_metering(OscTransport *self, gboolean enabled)
{
	g_atomic_int_set(&self->metering, !!enabled);
}

gint64
osc_transport_last_receive_time(OscTransport *self)
{
	gint64 last_receive_time;

	g_mutex_lock(&self->activity_mutex);
	last_receive_time = self->last_receive_time;
	g_mutex_unlock(&self->activity_mutex);
	return last_receive_time;
}

void
osc_transport_free(OscTransport *self)
{
	if (!self)
		return;
	if (self->thread) {
		g_atomic_int_set(&self->running, FALSE);
		g_thread_join(self->thread);
	}
	g_clear_object(&self->send_address);
	g_clear_object(&self->socket);
	g_mutex_clear(&self->activity_mutex);
	g_mutex_clear(&self->send_mutex);
	g_free(self);
}

static void
send_values(OscTransport *self, const char *address, const char *types,
	float float_value, int int_value)
{
	unsigned char buffer[512];
	struct oscmsg message;
	GError *error = NULL;

	memset(&message, 0, sizeof message);
	message.buf = buffer;
	message.end = buffer + sizeof buffer;
	oscputstr(&message, address);
	oscputstr(&message, types);
	message.type = types + 1;
	if (strchr(types, 'f'))
		oscputfloat(&message, float_value);
	if (strchr(types, 'i'))
		oscputint(&message, int_value);
	if (message.err) {
		g_warning("could not build OSC message: %s", message.err);
		return;
	}
	g_mutex_lock(&self->send_mutex);
	if (g_socket_send_to(self->socket, self->send_address, (char *)buffer,
	                     message.buf - buffer, NULL, &error) < 0) {
		g_warning("OSC send: %s", error->message);
		g_clear_error(&error);
	}
	g_mutex_unlock(&self->send_mutex);
}

void
osc_transport_send(OscTransport *self, const char *address)
{
	send_values(self, address, ",", 0.f, 0);
}

void
osc_transport_send_int(OscTransport *self, const char *address, int value)
{
	send_values(self, address, ",i", 0.f, value);
}

void
osc_transport_send_float(OscTransport *self, const char *address, float value)
{
	send_values(self, address, ",f", value, 0);
}

void
osc_transport_send_float_int(OscTransport *self, const char *address,
	float first, int second)
{
	send_values(self, address, ",fi", first, second);
}
