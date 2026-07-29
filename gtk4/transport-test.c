#include <glib.h>
#include <string.h>
#include "transport.h"
#include "../intpack.h"
#include "../osc.h"

static gsize
make_string_message(unsigned char *buffer, gsize capacity,
	const char *address, const char *value)
{
	struct oscmsg message = {0};

	message.buf = buffer;
	message.end = buffer + capacity;
	oscputstr(&message, address);
	oscputstr(&message, ",s");
	message.type = "s";
	oscputstr(&message, value);
	g_assert_null(message.err);
	return message.buf - buffer;
}

static gsize
make_float_message(unsigned char *buffer, gsize capacity,
	const char *address, float value)
{
	struct oscmsg message = {0};

	message.buf = buffer;
	message.end = buffer + capacity;
	oscputstr(&message, address);
	oscputstr(&message, ",f");
	message.type = "f";
	oscputfloat(&message, value);
	g_assert_null(message.err);
	return message.buf - buffer;
}

static gsize
make_int_message(unsigned char *buffer, gsize capacity,
	const char *address, int value)
{
	struct oscmsg message = {0};

	message.buf = buffer;
	message.end = buffer + capacity;
	oscputstr(&message, address);
	oscputstr(&message, ",i");
	message.type = "i";
	oscputint(&message, value);
	g_assert_null(message.err);
	return message.buf - buffer;
}

static void
test_transport_bundle(void)
{
	unsigned char first[128], second[128], bundle[512];
	struct oscmsg message = {0};
	MixChannelState selected;
	MixChannelKind kind;
	MixModel *model;
	OscTransport *transport;
	GError *error = NULL;
	gboolean connected;
	char device_id[32];
	gsize first_size, second_size, bundle_size;
	int index, selected_output;

	model = mix_model_new();
	transport = osc_transport_new(model, "127.0.0.1", 9,
	                              "127.0.0.1", 0, &error);
	g_assert_no_error(error);
	g_assert_nonnull(transport);
	g_assert_cmpint(osc_transport_last_receive_time(transport), ==, 0);
	first_size = make_string_message(first, sizeof first, "/device/id", "ffufxp");
	second_size = make_float_message(second, sizeof second, "/input/12/gain", 75.f);
	message.buf = bundle;
	message.end = bundle + sizeof bundle;
	oscputstr(&message, "#bundle");
	memset(message.buf, 0, 8);
	message.buf += 8;
	putbe32(message.buf, first_size);
	message.buf += 4;
	memcpy(message.buf, first, first_size);
	message.buf += first_size;
	putbe32(message.buf, second_size);
	message.buf += 4;
	memcpy(message.buf, second, second_size);
	message.buf += second_size;
	bundle_size = message.buf - bundle;
	g_assert_true(osc_transport_process_packet(transport, bundle, bundle_size, &error));
	g_assert_no_error(error);
	mix_model_select_channel(model, MIX_CHANNEL_INPUT, 11);
	mix_model_snapshot_selected(model, &kind, &index, &selected, &selected_output,
	                            &connected, device_id, sizeof device_id);
	g_assert_true(connected);
	g_assert_cmpstr(device_id, ==, "ffufxp");
	g_assert_cmpfloat(selected.gain, ==, 75.f);
	g_assert_cmpint(osc_transport_last_receive_time(transport), >, 0);
	g_assert_cmpuint(mix_model_connection_generation(model), ==, 0);
	second_size = make_int_message(second, sizeof second, "/device/session", 1234);
	g_assert_true(osc_transport_process_packet(transport, second, second_size,
	                                           &error));
	g_assert_no_error(error);
	g_assert_cmpuint(mix_model_connection_generation(model), ==, 1);
	g_assert_true(osc_transport_process_packet(transport, second, second_size,
	                                           &error));
	g_assert_no_error(error);
	g_assert_cmpuint(mix_model_connection_generation(model), ==, 1);

	second_size = make_int_message(second, sizeof second,
	                               "/input/12/dynamics/meter", 64);
	g_assert_true(osc_transport_process_packet(transport, second, second_size,
	                                           &error));
	g_assert_no_error(error);
	mix_model_snapshot_selected(model, &kind, &index, &selected, &selected_output,
	                            &connected, device_id, sizeof device_id);
	g_assert_cmpfloat_with_epsilon(selected.gain_reduction, 64.f / 255.f,
	                               .0001f);
	osc_transport_set_metering(transport, FALSE);
	second_size = make_int_message(second, sizeof second,
	                               "/input/12/dynamics/meter", 255);
	g_assert_true(osc_transport_process_packet(transport, second, second_size,
	                                           &error));
	g_assert_no_error(error);
	mix_model_snapshot_selected(model, &kind, &index, &selected, &selected_output,
	                            &connected, device_id, sizeof device_id);
	g_assert_cmpfloat_with_epsilon(selected.gain_reduction, 64.f / 255.f,
	                               .0001f);
	mix_model_mark_disconnected(model);
	mix_model_snapshot_selected(model, &kind, &index, &selected, &selected_output,
	                            &connected, device_id, sizeof device_id);
	g_assert_false(connected);
	g_assert_cmpstr(device_id, ==, "");

	g_assert_false(osc_transport_process_packet(transport,
	                                            (const unsigned char *)"bad", 3,
	                                            &error));
	g_assert_error(error, g_quark_from_static_string("oscmix-gtk4-transport-error"), 1);
	g_clear_error(&error);
	osc_transport_free(transport);
	mix_model_free(model);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/transport/bundle", test_transport_bundle);
	return g_test_run();
}
