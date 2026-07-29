#ifndef OSCMIX_GTK4_DSP_PANEL_H
#define OSCMIX_GTK4_DSP_PANEL_H

#include <gtk/gtk.h>
#include "model.h"
#include "transport.h"

typedef struct DspPanel DspPanel;
typedef enum {
	DSP_PANEL_PAGE_EQ,
	DSP_PANEL_PAGE_DYNAMICS,
	DSP_PANEL_PAGE_ROOM_EQ
} DspPanelPage;

typedef void (*DspPanelDetachFunc)(DspPanel *self, DspPanelPage page,
	gpointer user_data);

DspPanel *dsp_panel_new(MixModel *model, OscTransport *transport);
DspPanel *dsp_panel_new_eq(MixModel *model, OscTransport *transport);
DspPanel *dsp_panel_new_eq_window(MixModel *model, OscTransport *transport);
DspPanel *dsp_panel_new_dynamics(MixModel *model, OscTransport *transport);
DspPanel *dsp_panel_new_dynamics_window(MixModel *model,
	OscTransport *transport);
DspPanel *dsp_panel_new_room_window(MixModel *model,
	OscTransport *transport);
GtkWidget *dsp_panel_get_widget(DspPanel *self);
GtkButton *dsp_panel_get_eq_window_button(DspPanel *self);
GtkButton *dsp_panel_get_dynamics_window_button(DspPanel *self);
GtkButton *dsp_panel_get_room_window_button(DspPanel *self);
void dsp_panel_sync(DspPanel *self, MixChannelKind kind, int channel,
	int selected_output);
void dsp_panel_update_meters(DspPanel *self);
void dsp_panel_set_detach_handler(DspPanel *self,
	DspPanelDetachFunc callback, gpointer user_data);
void dsp_panel_show_eq(DspPanel *self);
void dsp_panel_show_dynamics(DspPanel *self);
void dsp_panel_show_room(DspPanel *self);
void dsp_panel_free(DspPanel *self);

#endif
