#ifndef OSCMIX_GTK4_TRANSPORT_H
#define OSCMIX_GTK4_TRANSPORT_H

#include <gio/gio.h>
#include "model.h"

typedef struct OscTransport OscTransport;

OscTransport *osc_transport_new(MixModel *model, const char *send_host,
	guint16 send_port, const char *recv_host, guint16 recv_port, GError **error);
void osc_transport_start(OscTransport *self);
void osc_transport_free(OscTransport *self);
void osc_transport_set_metering(OscTransport *self, gboolean enabled);
gint64 osc_transport_last_receive_time(OscTransport *self);

gboolean osc_transport_process_packet(OscTransport *self,
	const unsigned char *data, gsize size, GError **error);

void osc_transport_send(OscTransport *self, const char *address);
void osc_transport_send_int(OscTransport *self, const char *address, int value);
void osc_transport_send_float(OscTransport *self, const char *address, float value);
void osc_transport_send_float_int(OscTransport *self, const char *address,
	float first, int second);

#endif
