#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include "dsp_panel.h"
#include "mixer_row.h"
#include "model.h"
#include "transport.h"

typedef struct {
	char *send_host;
	char *recv_host;
	int send_port;
	int recv_port;
} AppConfig;

typedef struct {
	GtkApplication *application;
	GtkWindow *window;
	GtkPaned *main_paned;
	GtkWidget *inspector;
	GtkToggleButton *inspector_toggle;
	int inspector_width;
	MixModel *model;
	OscTransport *transport;
	GtkLabel *status;
	GtkLabel *selection;
	GtkDropDown *output;
	GtkWidget *gain_box;
	GtkScale *volume;
	GtkScale *pan;
	GtkScale *gain;
	GtkSwitch *mute;
	GtkSwitch *stereo;
	GtkSwitch *phantom;
	GtkSwitch *hiz;
	GtkWidget *phantom_row;
	GtkWidget *hiz_row;
	MixChannelKind displayed_kind;
	int displayed_channel;
	int displayed_output;
	gboolean updating;
	gboolean refreshed;
	gint64 last_probe;
	char renderer[64];
	char status_text[160];
	char selection_text[128];
	DspPanel *dsp_panel;
	DspPanel *eq_panel;
	DspPanel *dynamics_panel;
	DspPanel *eq_window_panel;
	DspPanel *dynamics_window_panel;
	DspPanel *room_window_panel;
	GtkWindow *eq_window;
	GtkWindow *dynamics_window;
	GtkWindow *room_window;
	OscmixMixerRow *eq_anchor_row;
	OscmixMixerRow *dynamics_anchor_row;
	MixChannelKind eq_kind;
	MixChannelKind dynamics_kind;
	int eq_channel;
	int dynamics_channel;
	gboolean metering_enabled;
	guint64 last_control_generation;
	guint64 last_connection_generation;
	char *route_state_path;
} AppUi;

static AppConfig config = {
	.send_host = NULL,
	.recv_host = NULL,
	.send_port = 7222,
	.recv_port = 8222,
};

static gboolean
quit_test_application(gpointer data)
{
	g_message("test timeout: quitting GTK 4 prototype");
	g_application_quit(G_APPLICATION(data));
	return G_SOURCE_REMOVE;
}

static void
send_channel_int(AppUi *ui, MixChannelKind kind, int channel,
	const char *leaf, int value)
{
	char address[96];

	g_snprintf(address, sizeof address, "/%s/%d/%s",
	           mix_channel_kind_osc_name(kind), channel + 1, leaf);
	osc_transport_send_int(ui->transport, address, value);
}

static void
send_selected_volume(AppUi *ui, MixChannelKind kind, int channel,
	int selected_output, float volume, int pan)
{
	char address[96];

	if (kind == MIX_CHANNEL_OUTPUT) {
		g_snprintf(address, sizeof address, "/output/%d/volume", channel + 1);
		osc_transport_send_float(ui->transport, address, volume);
	} else {
		g_snprintf(address, sizeof address, "/mix/%d/%s/%d", selected_output + 1,
		           mix_channel_kind_osc_name(kind), channel + 1);
		osc_transport_send_float_int(ui->transport, address, volume, pan);
	}
}

static void
selected_state(AppUi *ui, MixChannelKind *kind, int *channel,
	MixChannelState *state, int *selected_output)
{
	gboolean connected;
	char device_id[32];

	mix_model_snapshot_selected(ui->model, kind, channel, state,
	                            selected_output, &connected,
	                            device_id, sizeof device_id);
}

static void
on_inspector_toggled(GtkToggleButton *button, gpointer data)
{
	AppUi *ui = data;
	gboolean visible;
	int pane_width, width;

	visible = gtk_toggle_button_get_active(button);
	if (!visible) {
		width = gtk_widget_get_width(ui->inspector);
		if (width > 0)
			ui->inspector_width = width;
	}
	gtk_widget_set_visible(ui->inspector, visible);
	if (visible && ui->inspector_width > 0) {
		pane_width = gtk_widget_get_width(GTK_WIDGET(ui->main_paned));
		width = gtk_window_is_fullscreen(ui->window) ? pane_width / 5
		                                              : ui->inspector_width;
		if (pane_width > 0)
			gtk_paned_set_position(ui->main_paned,
			                       pane_width - MIN(ui->inspector_width, width));
	}
	gtk_widget_set_tooltip_text(GTK_WIDGET(button),
	                            visible ? "Hide inspector" : "Show inspector");
}

static void
constrain_inspector_width(AppUi *ui)
{
	int pane_width, width, position;

	if (!ui->inspector || !gtk_widget_get_visible(ui->inspector))
		return;
	pane_width = gtk_widget_get_width(GTK_WIDGET(ui->main_paned));
	if (pane_width <= 0)
		return;
	width = ui->inspector_width;
	if (width <= 0)
		width = pane_width / 5;
	if (gtk_window_is_fullscreen(ui->window))
		width = MIN(width, pane_width / 5);
	position = pane_width - width;
	if (gtk_paned_get_position(ui->main_paned) != position)
		gtk_paned_set_position(ui->main_paned, position);
}

static void
on_paned_position_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	AppUi *ui = data;
	int pane_width, width;

	(void)spec;
	pane_width = gtk_widget_get_width(GTK_WIDGET(object));
	if (pane_width <= 0)
		return;
	width = pane_width - gtk_paned_get_position(GTK_PANED(object));
	if (gtk_window_is_fullscreen(ui->window) && width > pane_width / 5) {
		gtk_paned_set_position(GTK_PANED(object), pane_width - pane_width / 5);
		return;
	}
	if (width > 0)
		ui->inspector_width = width;
}

static void
on_volume_changed(GtkRange *range, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	int channel, selected_output;
	float value;

	if (ui->updating)
		return;
	selected_state(ui, &kind, &channel, &state, &selected_output);
	value = gtk_range_get_value(range);
	mix_model_set_volume(ui->model, kind, channel, value);
	send_selected_volume(ui, kind, channel, selected_output, value, state.pan);
}

static void
on_pan_changed(GtkRange *range, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	int channel, selected_output, pan;
	char address[96];

	if (ui->updating)
		return;
	selected_state(ui, &kind, &channel, &state, &selected_output);
	pan = lround(gtk_range_get_value(range));
	mix_model_set_pan(ui->model, kind, channel, pan);
	if (kind == MIX_CHANNEL_OUTPUT) {
		g_snprintf(address, sizeof address, "/output/%d/pan", channel + 1);
		osc_transport_send_int(ui->transport, address, pan);
	} else {
		send_selected_volume(ui, kind, channel, selected_output, state.volume, pan);
	}
}

static void
on_pan_double_click(GtkGestureClick *gesture, int n_press, double x, double y,
	gpointer data)
{
	AppUi *ui = data;

	(void)x;
	(void)y;
	if (n_press != 2)
		return;
	gtk_range_set_value(GTK_RANGE(ui->pan), 0.);
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_mute_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	int channel, selected_output;
	gboolean value;

	(void)spec;
	if (ui->updating)
		return;
	selected_state(ui, &kind, &channel, &state, &selected_output);
	value = gtk_switch_get_active(GTK_SWITCH(object));
	mix_model_set_mute(ui->model, kind, channel, value);
	send_channel_int(ui, kind, channel, "mute", value);
}

static void
on_stereo_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	int channel, selected_output;
	gboolean value;

	(void)spec;
	if (ui->updating)
		return;
	selected_state(ui, &kind, &channel, &state, &selected_output);
	value = gtk_switch_get_active(GTK_SWITCH(object));
	mix_model_set_stereo(ui->model, kind, channel, value);
	send_channel_int(ui, kind, channel, "stereo", value);
}

static void
on_gain_changed(GtkRange *range, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	int channel, selected_output;
	float value;
	char address[64];

	if (ui->updating)
		return;
	selected_state(ui, &kind, &channel, &state, &selected_output);
	if (kind != MIX_CHANNEL_INPUT)
		return;
	value = gtk_range_get_value(range);
	if (state.gain_gap_end > 0 && value > 0 && value < state.gain_gap_end) {
		value = state.gain <= 0 ? state.gain_gap_end : 0;
		ui->updating = TRUE;
		gtk_range_set_value(range, value);
		ui->updating = FALSE;
	}
	mix_model_set_gain(ui->model, channel, value);
	g_snprintf(address, sizeof address, "/input/%d/gain", channel + 1);
	osc_transport_send_float(ui->transport, address, value);
}

static void
on_phantom_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	int channel, selected_output;
	gboolean value;

	(void)spec;
	if (ui->updating)
		return;
	selected_state(ui, &kind, &channel, &state, &selected_output);
	if (kind != MIX_CHANNEL_INPUT)
		return;
	value = gtk_switch_get_active(GTK_SWITCH(object));
	mix_model_set_phantom(ui->model, channel, value);
	send_channel_int(ui, kind, channel, "48v", value);
}

static void
on_hiz_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	int channel, selected_output;
	gboolean value;

	(void)spec;
	if (ui->updating)
		return;
	selected_state(ui, &kind, &channel, &state, &selected_output);
	if (kind != MIX_CHANNEL_INPUT)
		return;
	value = gtk_switch_get_active(GTK_SWITCH(object));
	mix_model_set_hiz(ui->model, channel, value);
	send_channel_int(ui, kind, channel, "hi-z", value);
}

static void
on_output_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	AppUi *ui = data;
	guint selected;

	(void)spec;
	if (ui->updating)
		return;
	selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
	if (selected != GTK_INVALID_LIST_POSITION)
		mix_model_select_output(ui->model, selected);
}

static GtkWidget *
labelled_control(const char *label, GtkWidget *control)
{
	GtkWidget *box, *text;

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
	text = gtk_label_new(label);
	gtk_label_set_xalign(GTK_LABEL(text), 0.f);
	gtk_widget_add_css_class(text, "control-label");
	gtk_box_append(GTK_BOX(box), text);
	gtk_box_append(GTK_BOX(box), control);
	return box;
}

static GtkWidget *
switch_row(const char *label, GtkSwitch **out_switch)
{
	GtkWidget *box, *text, *control;

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	text = gtk_label_new(label);
	gtk_label_set_xalign(GTK_LABEL(text), 0.f);
	gtk_widget_set_hexpand(text, TRUE);
	control = gtk_switch_new();
	gtk_box_append(GTK_BOX(box), text);
	gtk_box_append(GTK_BOX(box), control);
	*out_switch = GTK_SWITCH(control);
	return box;
}

static void
release_inline_editor(AppUi *ui)
{
	if (ui->eq_anchor_row)
		oscmix_mixer_row_release_editor(ui->eq_anchor_row);
	if (ui->dynamics_anchor_row &&
	    ui->dynamics_anchor_row != ui->eq_anchor_row)
		oscmix_mixer_row_release_editor(ui->dynamics_anchor_row);
	ui->eq_anchor_row = NULL;
	ui->eq_channel = -1;
	ui->dynamics_anchor_row = NULL;
	ui->dynamics_channel = -1;
}

static void
on_eq_requested(OscmixMixerRow *row, int kind, int channel, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind selected_kind;
	int selected_channel, selected_output;
	gboolean already_open;

	mix_model_select_channel(ui->model, kind, channel);
	if (kind == MIX_CHANNEL_OUTPUT)
		mix_model_select_output(ui->model, channel);
	already_open = ui->eq_anchor_row == row &&
	               ui->eq_kind == (MixChannelKind)kind &&
	               ui->eq_channel == channel;
	release_inline_editor(ui);
	if (already_open)
		return;
	ui->eq_kind = kind;
	ui->eq_channel = channel;
	selected_state(ui, &selected_kind, &selected_channel, &state,
	               &selected_output);
	dsp_panel_sync(ui->eq_panel, kind, channel, selected_output);
	dsp_panel_show_eq(ui->eq_panel);
	oscmix_mixer_row_set_editor(row, dsp_panel_get_widget(ui->eq_panel), channel);
	ui->eq_anchor_row = row;
	ui->last_control_generation = G_MAXUINT64;
}

static void
on_dynamics_requested(OscmixMixerRow *row, int kind, int channel,
	gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind selected_kind;
	int selected_channel, selected_output;
	gboolean already_open;

	mix_model_select_channel(ui->model, kind, channel);
	if (kind == MIX_CHANNEL_OUTPUT)
		mix_model_select_output(ui->model, channel);
	already_open = ui->dynamics_anchor_row == row &&
	               ui->dynamics_kind == (MixChannelKind)kind &&
	               ui->dynamics_channel == channel;
	release_inline_editor(ui);
	if (already_open)
		return;
	ui->dynamics_kind = kind;
	ui->dynamics_channel = channel;
	selected_state(ui, &selected_kind, &selected_channel, &state,
	               &selected_output);
	dsp_panel_sync(ui->dynamics_panel, kind, channel, selected_output);
	dsp_panel_show_dynamics(ui->dynamics_panel);
	oscmix_mixer_row_set_editor(
		row, dsp_panel_get_widget(ui->dynamics_panel), channel);
	ui->dynamics_anchor_row = row;
	ui->last_control_generation = G_MAXUINT64;
}

static void
destroy_eq_window(AppUi *ui)
{
	if (!ui->eq_window)
		return;
	if (ui->eq_window_panel) {
		if (gtk_window_get_child(ui->eq_window) ==
		    dsp_panel_get_widget(ui->eq_window_panel))
			gtk_window_set_child(ui->eq_window, NULL);
		dsp_panel_free(ui->eq_window_panel);
		ui->eq_window_panel = NULL;
	}
	if (!gtk_widget_in_destruction(GTK_WIDGET(ui->eq_window)))
		gtk_window_destroy(ui->eq_window);
	g_clear_object(&ui->eq_window);
}

static void
destroy_dynamics_window(AppUi *ui)
{
	if (!ui->dynamics_window)
		return;
	if (ui->dynamics_window_panel) {
		if (gtk_window_get_child(ui->dynamics_window) ==
		    dsp_panel_get_widget(ui->dynamics_window_panel))
			gtk_window_set_child(ui->dynamics_window, NULL);
		dsp_panel_free(ui->dynamics_window_panel);
		ui->dynamics_window_panel = NULL;
	}
	if (!gtk_widget_in_destruction(GTK_WIDGET(ui->dynamics_window)))
		gtk_window_destroy(ui->dynamics_window);
	g_clear_object(&ui->dynamics_window);
}

static void
destroy_room_window(AppUi *ui)
{
	if (!ui->room_window)
		return;
	if (ui->room_window_panel) {
		if (gtk_window_get_child(ui->room_window) ==
		    dsp_panel_get_widget(ui->room_window_panel))
			gtk_window_set_child(ui->room_window, NULL);
		dsp_panel_free(ui->room_window_panel);
		ui->room_window_panel = NULL;
	}
	if (!gtk_widget_in_destruction(GTK_WIDGET(ui->room_window)))
		gtk_window_destroy(ui->room_window);
	g_clear_object(&ui->room_window);
}

static gboolean
on_main_close_request(GtkWindow *window, gpointer data)
{
	(void)window;
	destroy_eq_window(data);
	destroy_dynamics_window(data);
	destroy_room_window(data);
	return FALSE;
}

static void
on_open_eq_window(GtkButton *button, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	MixRowSnapshot row;
	GtkWidget *window;
	char channel_name[72], title[160];
	int channel, selected_output;

	(void)button;
	if (ui->eq_window &&
	    gtk_widget_get_visible(GTK_WIDGET(ui->eq_window))) {
		gtk_widget_set_visible(GTK_WIDGET(ui->eq_window), FALSE);
		return;
	}
	selected_state(ui, &kind, &channel, &state, &selected_output);
	if (kind == MIX_CHANNEL_PLAYBACK)
		return;
	if (!ui->eq_window) {
		window = gtk_application_window_new(ui->application);
		ui->eq_window = GTK_WINDOW(g_object_ref(window));
		gtk_window_set_default_size(ui->eq_window, 760, 560);
		gtk_window_set_resizable(ui->eq_window, TRUE);
		gtk_window_set_transient_for(ui->eq_window, ui->window);
		gtk_window_set_destroy_with_parent(ui->eq_window, FALSE);
		gtk_window_set_hide_on_close(ui->eq_window, TRUE);
		ui->eq_window_panel = dsp_panel_new_eq_window(ui->model, ui->transport);
		gtk_window_set_child(ui->eq_window,
		                     dsp_panel_get_widget(ui->eq_window_panel));
	}
	mix_model_snapshot_row(ui->model, kind, &row);
	mix_row_format_channel_name(&row, channel, channel_name,
	                            sizeof channel_name);
	g_snprintf(title, sizeof title, "EQ — %s", channel_name);
	gtk_window_set_title(ui->eq_window, title);
	dsp_panel_sync(ui->eq_window_panel, kind, channel, selected_output);
	dsp_panel_show_eq(ui->eq_window_panel);
	gtk_window_present(ui->eq_window);
}

static void
on_open_dynamics_window(GtkButton *button, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	MixRowSnapshot row;
	GtkWidget *window;
	char channel_name[72], title[160];
	int channel, selected_output;

	(void)button;
	if (ui->dynamics_window &&
	    gtk_widget_get_visible(GTK_WIDGET(ui->dynamics_window))) {
		gtk_widget_set_visible(GTK_WIDGET(ui->dynamics_window), FALSE);
		return;
	}
	selected_state(ui, &kind, &channel, &state, &selected_output);
	if (kind == MIX_CHANNEL_PLAYBACK)
		return;
	if (!ui->dynamics_window) {
		window = gtk_application_window_new(ui->application);
		ui->dynamics_window = GTK_WINDOW(g_object_ref(window));
		/* Match the narrow, vertical proportions of the hardware-style editor. */
		gtk_window_set_default_size(ui->dynamics_window, 340, 600);
		gtk_window_set_resizable(ui->dynamics_window, TRUE);
		gtk_window_set_transient_for(ui->dynamics_window, ui->window);
		gtk_window_set_destroy_with_parent(ui->dynamics_window, FALSE);
		gtk_window_set_hide_on_close(ui->dynamics_window, TRUE);
		ui->dynamics_window_panel = dsp_panel_new_dynamics_window(
			ui->model, ui->transport);
		gtk_window_set_child(
			ui->dynamics_window,
			dsp_panel_get_widget(ui->dynamics_window_panel));
	}
	mix_model_snapshot_row(ui->model, kind, &row);
	mix_row_format_channel_name(&row, channel, channel_name,
	                            sizeof channel_name);
	g_snprintf(title, sizeof title, "Dynamics — %s", channel_name);
	gtk_window_set_title(ui->dynamics_window, title);
	dsp_panel_sync(ui->dynamics_window_panel, kind, channel, selected_output);
	dsp_panel_show_dynamics(ui->dynamics_window_panel);
	gtk_window_present(ui->dynamics_window);
}

static void
on_open_room_window(GtkButton *button, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	MixRowSnapshot row;
	GtkWidget *window;
	char output_name[72], title[160];
	int channel, selected_output;

	(void)button;
	if (ui->room_window &&
	    gtk_widget_get_visible(GTK_WIDGET(ui->room_window))) {
		gtk_widget_set_visible(GTK_WIDGET(ui->room_window), FALSE);
		return;
	}
	selected_state(ui, &kind, &channel, &state, &selected_output);
	if (!ui->room_window) {
		window = gtk_application_window_new(ui->application);
		ui->room_window = GTK_WINDOW(g_object_ref(window));
		gtk_window_set_default_size(ui->room_window, 720, 760);
		gtk_window_set_resizable(ui->room_window, TRUE);
		gtk_window_set_transient_for(ui->room_window, ui->window);
		gtk_window_set_destroy_with_parent(ui->room_window, FALSE);
		gtk_window_set_hide_on_close(ui->room_window, TRUE);
		ui->room_window_panel = dsp_panel_new_room_window(
			ui->model, ui->transport);
		gtk_window_set_child(
			ui->room_window,
			dsp_panel_get_widget(ui->room_window_panel));
	}
	mix_model_snapshot_row(ui->model, MIX_CHANNEL_OUTPUT, &row);
	mix_row_format_channel_name(&row, selected_output, output_name,
	                            sizeof output_name);
	g_snprintf(title, sizeof title, "Room EQ — %s", output_name);
	gtk_window_set_title(ui->room_window, title);
	dsp_panel_sync(ui->room_window_panel, kind, channel, selected_output);
	dsp_panel_show_room(ui->room_window_panel);
	gtk_window_present(ui->room_window);
}

static void
on_dsp_detach_requested(DspPanel *panel, DspPanelPage page, gpointer data)
{
	(void)panel;
	if (page == DSP_PANEL_PAGE_DYNAMICS)
		on_open_dynamics_window(NULL, data);
	else if (page == DSP_PANEL_PAGE_ROOM_EQ)
		on_open_room_window(NULL, data);
	else
		on_open_eq_window(NULL, data);
}

static GtkWidget *
make_row_with_scroll(AppUi *ui, MixChannelKind kind)
{
	GtkWidget *box, *row, *scrollbar;

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	row = oscmix_mixer_row_new(ui->model, ui->transport, kind);
	g_signal_connect(row, "eq-requested", G_CALLBACK(on_eq_requested), ui);
	g_signal_connect(row, "dynamics-requested",
	                 G_CALLBACK(on_dynamics_requested), ui);
	gtk_widget_set_vexpand(row, TRUE);
	scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL,
	                              oscmix_mixer_row_get_adjustment(OSCMIX_MIXER_ROW(row)));
	gtk_box_append(GTK_BOX(box), row);
	gtk_box_append(GTK_BOX(box), scrollbar);
	return box;
}

static GtkWidget *
build_inspector(AppUi *ui)
{
	GtkWidget *scroll, *panel, *title, *separator;
	GtkGesture *pan_click;

	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
	                               GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll),
	                                                FALSE);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll),
	                                                 FALSE);
	gtk_widget_set_size_request(scroll, 0, -1);
	gtk_widget_add_css_class(scroll, "inspector");
	panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
	gtk_widget_add_css_class(panel, "inspector-content");
	title = gtk_label_new("Selected channel");
	gtk_widget_add_css_class(title, "section-title");
	gtk_label_set_xalign(GTK_LABEL(title), 0.f);
	gtk_box_append(GTK_BOX(panel), title);
	ui->selection = GTK_LABEL(gtk_label_new("Input 1"));
	gtk_label_set_xalign(ui->selection, 0.f);
	gtk_widget_add_css_class(GTK_WIDGET(ui->selection), "channel-title");
	gtk_box_append(GTK_BOX(panel), GTK_WIDGET(ui->selection));
	separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_append(GTK_BOX(panel), separator);

	ui->volume = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
	                                                -65, 6, .1));
	gtk_scale_set_draw_value(ui->volume, TRUE);
	gtk_scale_add_mark(ui->volume, 0, GTK_POS_TOP, "0 dB");
	gtk_box_append(GTK_BOX(panel), labelled_control("Level", GTK_WIDGET(ui->volume)));
	g_signal_connect(ui->volume, "value-changed", G_CALLBACK(on_volume_changed), ui);

	ui->pan = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
	                                             -100, 100, 1));
	gtk_scale_set_draw_value(ui->pan, TRUE);
	gtk_scale_add_mark(ui->pan, 0, GTK_POS_TOP, "Center");
	gtk_box_append(GTK_BOX(panel), labelled_control("Pan", GTK_WIDGET(ui->pan)));
	g_signal_connect(ui->pan, "value-changed", G_CALLBACK(on_pan_changed), ui);
	pan_click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pan_click), GDK_BUTTON_PRIMARY);
	g_signal_connect(pan_click, "pressed", G_CALLBACK(on_pan_double_click), ui);
	gtk_widget_add_controller(GTK_WIDGET(ui->pan), GTK_EVENT_CONTROLLER(pan_click));

	gtk_box_append(GTK_BOX(panel), switch_row("Mute", &ui->mute));
	gtk_box_append(GTK_BOX(panel), switch_row("Stereo", &ui->stereo));
	g_signal_connect(ui->mute, "notify::active", G_CALLBACK(on_mute_changed), ui);
	g_signal_connect(ui->stereo, "notify::active", G_CALLBACK(on_stereo_changed), ui);

	ui->gain_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	ui->gain = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 75, 1));
	gtk_scale_set_draw_value(ui->gain, TRUE);
	gtk_box_append(GTK_BOX(ui->gain_box), labelled_control("Preamp gain", GTK_WIDGET(ui->gain)));
	ui->phantom_row = switch_row("48 V phantom power", &ui->phantom);
	ui->hiz_row = switch_row("Instrument Hi-Z", &ui->hiz);
	gtk_box_append(GTK_BOX(ui->gain_box), ui->phantom_row);
	gtk_box_append(GTK_BOX(ui->gain_box), ui->hiz_row);
	g_signal_connect(ui->gain, "value-changed", G_CALLBACK(on_gain_changed), ui);
	g_signal_connect(ui->phantom, "notify::active", G_CALLBACK(on_phantom_changed), ui);
	g_signal_connect(ui->hiz, "notify::active", G_CALLBACK(on_hiz_changed), ui);
	gtk_box_append(GTK_BOX(panel), ui->gain_box);

	ui->dsp_panel = dsp_panel_new(ui->model, ui->transport);
	dsp_panel_set_detach_handler(ui->dsp_panel,
	                             on_dsp_detach_requested, ui);
	g_signal_connect(dsp_panel_get_eq_window_button(ui->dsp_panel), "clicked",
	                 G_CALLBACK(on_open_eq_window), ui);
	g_signal_connect(
		dsp_panel_get_dynamics_window_button(ui->dsp_panel), "clicked",
		G_CALLBACK(on_open_dynamics_window), ui);
	g_signal_connect(
		dsp_panel_get_room_window_button(ui->dsp_panel), "clicked",
		G_CALLBACK(on_open_room_window), ui);
	gtk_box_append(GTK_BOX(panel), dsp_panel_get_widget(ui->dsp_panel));
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), panel);
	return scroll;
}

static void
sync_output_names(AppUi *ui)
{
	GtkStringList *names;
	MixRowSnapshot row;
	char display_name[72];
	int index;

	names = GTK_STRING_LIST(gtk_drop_down_get_model(ui->output));
	mix_model_snapshot_row(ui->model, MIX_CHANNEL_OUTPUT, &row);
	for (index = 0; index < row.count; ++index) {
		const char *current;
		const char *replacement[2];

		mix_row_format_channel_name(&row, index, display_name,
		                            sizeof display_name);
		current = gtk_string_list_get_string(names, index);
		if (g_strcmp0(current, display_name) == 0)
			continue;
		replacement[0] = display_name;
		replacement[1] = NULL;
		gtk_string_list_splice(names, index, 1, replacement);
	}
}

static gboolean
sync_ui(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
	AppUi *ui = data;
	MixChannelState state;
	MixChannelKind kind;
	MixRowSnapshot row;
	gboolean connected;
	char channel_name[72], device_id[32], title[128], status[160];
	int channel, selected_output;
	gint64 now;
	gint64 last_receive_time;
	guint64 control_generation;
	guint64 connection_generation;

	(void)widget;
	(void)clock;
	mix_model_snapshot_selected(ui->model, &kind, &channel, &state,
	                            &selected_output, &connected,
	                            device_id, sizeof device_id);
	control_generation = mix_model_control_generation(ui->model);
	connection_generation = mix_model_connection_generation(ui->model);
	now = g_get_monotonic_time();
	if (now - ui->last_probe >= 500000) {
		osc_transport_send(ui->transport, "/device/info");
		ui->last_probe = now;
	}
	constrain_inspector_width(ui);
	last_receive_time = osc_transport_last_receive_time(ui->transport);
	if (connected && last_receive_time > 0 &&
	    now - last_receive_time >= 2000000) {
		mix_model_mark_disconnected(ui->model);
		ui->refreshed = FALSE;
		connected = FALSE;
		device_id[0] = '\0';
		control_generation = mix_model_control_generation(ui->model);
		connection_generation = mix_model_connection_generation(ui->model);
	}
	if (connection_generation != ui->last_connection_generation) {
		ui->refreshed = FALSE;
		ui->last_connection_generation = connection_generation;
	}
	if (connected && !ui->refreshed) {
		osc_transport_send_int(ui->transport, "/metering", ui->metering_enabled);
		osc_transport_send(ui->transport, "/device/channels");
		osc_transport_send(ui->transport, "/device/names");
		osc_transport_send(ui->transport, "/refresh");
		ui->refreshed = TRUE;
	}
	if (control_generation != ui->last_control_generation) {
		gboolean paired;
		int first;

		ui->updating = TRUE;
		sync_output_names(ui);
		mix_model_snapshot_row(ui->model, kind, &row);
		paired = mix_row_format_channel_name(&row, channel, channel_name,
		                                     sizeof channel_name);
		first = paired ? channel & ~1 : channel;
		if (paired)
			g_snprintf(title, sizeof title, "%s %d/%d — %s",
			           mix_channel_kind_singular_name(kind), first + 1,
			           first + 2, channel_name);
		else
			g_snprintf(title, sizeof title, "%s %d — %s",
			           mix_channel_kind_singular_name(kind), channel + 1,
			           channel_name);
		if (ui->displayed_kind != kind || ui->displayed_channel != channel ||
		    strcmp(ui->selection_text, title) != 0) {
			gtk_label_set_text(ui->selection, title);
			g_strlcpy(ui->selection_text, title, sizeof ui->selection_text);
			ui->displayed_kind = kind;
			ui->displayed_channel = channel;
		}
		if (ui->displayed_output != selected_output) {
			gtk_drop_down_set_selected(ui->output, selected_output);
			ui->displayed_output = selected_output;
		}
		if (fabs(gtk_range_get_value(GTK_RANGE(ui->volume)) - state.volume) > .001)
			gtk_range_set_value(GTK_RANGE(ui->volume), state.volume);
		if (fabs(gtk_range_get_value(GTK_RANGE(ui->pan)) - state.pan) > .001)
			gtk_range_set_value(GTK_RANGE(ui->pan), state.pan);
		if (gtk_switch_get_active(ui->mute) != state.mute)
			gtk_switch_set_active(ui->mute, state.mute);
		if (gtk_switch_get_active(ui->stereo) != state.stereo)
			gtk_switch_set_active(ui->stereo, state.stereo);
		gtk_widget_set_visible(ui->gain_box,
		                       kind == MIX_CHANNEL_INPUT &&
		                       (state.flags & INPUT_HAS_GAIN));
		if (kind == MIX_CHANNEL_INPUT) {
			GtkAdjustment *gain_adjustment = gtk_range_get_adjustment(GTK_RANGE(ui->gain));
			if (gtk_adjustment_get_lower(gain_adjustment) != state.gain_min ||
			    gtk_adjustment_get_upper(gain_adjustment) != state.gain_max) {
				gtk_range_set_range(GTK_RANGE(ui->gain), state.gain_min, state.gain_max);
				gtk_range_set_increments(GTK_RANGE(ui->gain), state.gain_max > 24 ? 1 : .5,
				                         state.gain_max > 24 ? 5 : 2);
			}
			if (fabs(gtk_range_get_value(GTK_RANGE(ui->gain)) - state.gain) > .001)
				gtk_range_set_value(GTK_RANGE(ui->gain), state.gain);
			if (gtk_switch_get_active(ui->phantom) != state.phantom)
				gtk_switch_set_active(ui->phantom, state.phantom);
			if (gtk_switch_get_active(ui->hiz) != state.hiz)
				gtk_switch_set_active(ui->hiz, state.hiz);
			gtk_widget_set_visible(ui->phantom_row, state.flags & INPUT_HAS_48V);
			gtk_widget_set_visible(ui->hiz_row, state.flags & INPUT_HAS_HIZ);
		}
		ui->updating = FALSE;
		dsp_panel_sync(ui->dsp_panel, kind, channel, selected_output);
		gtk_widget_set_sensitive(
			GTK_WIDGET(dsp_panel_get_eq_window_button(ui->dsp_panel)),
			kind != MIX_CHANNEL_PLAYBACK);
		gtk_widget_set_sensitive(
			GTK_WIDGET(dsp_panel_get_dynamics_window_button(ui->dsp_panel)),
			kind != MIX_CHANNEL_PLAYBACK);
		if (ui->eq_anchor_row)
			dsp_panel_sync(ui->eq_panel, ui->eq_kind, ui->eq_channel,
			               selected_output);
		if (ui->dynamics_anchor_row)
			dsp_panel_sync(ui->dynamics_panel, ui->dynamics_kind,
			               ui->dynamics_channel, selected_output);
		if (ui->eq_window_panel &&
		    gtk_widget_get_visible(GTK_WIDGET(ui->eq_window))) {
			g_snprintf(title, sizeof title, "EQ — %s", channel_name);
			gtk_window_set_title(ui->eq_window, title);
			dsp_panel_sync(ui->eq_window_panel, kind, channel, selected_output);
		}
		if (ui->dynamics_window_panel &&
		    gtk_widget_get_visible(GTK_WIDGET(ui->dynamics_window))) {
			g_snprintf(title, sizeof title, "Dynamics — %s", channel_name);
			gtk_window_set_title(ui->dynamics_window, title);
			dsp_panel_sync(ui->dynamics_window_panel, kind, channel,
			               selected_output);
		}
		if (ui->room_window_panel &&
		    gtk_widget_get_visible(GTK_WIDGET(ui->room_window))) {
			MixRowSnapshot output_row;
			char output_name[72];

			mix_model_snapshot_row(ui->model, MIX_CHANNEL_OUTPUT, &output_row);
			mix_row_format_channel_name(&output_row, selected_output,
			                            output_name, sizeof output_name);
			g_snprintf(title, sizeof title, "Room EQ — %s", output_name);
			gtk_window_set_title(ui->room_window, title);
			dsp_panel_sync(ui->room_window_panel, kind, channel,
			               selected_output);
		}
		ui->last_control_generation = control_generation;
	}
	/* Meter packets deliberately do not rebuild control bindings. Update only
	 * the small live displays that are currently present. */
	dsp_panel_update_meters(ui->dsp_panel);
	if (ui->eq_anchor_row)
		dsp_panel_update_meters(ui->eq_panel);
	if (ui->dynamics_anchor_row)
		dsp_panel_update_meters(ui->dynamics_panel);
	if (ui->eq_window_panel && ui->eq_window &&
	    gtk_widget_get_visible(GTK_WIDGET(ui->eq_window)))
		dsp_panel_update_meters(ui->eq_window_panel);
	if (ui->dynamics_window_panel && ui->dynamics_window &&
	    gtk_widget_get_visible(GTK_WIDGET(ui->dynamics_window)))
		dsp_panel_update_meters(ui->dynamics_window_panel);
	if (connected)
		g_snprintf(status, sizeof status, "UFX+ connected • %s",
		           ui->renderer[0] ? ui->renderer : "detecting renderer");
	else if (device_id[0])
		g_snprintf(status, sizeof status, "Unsupported device: %s", device_id);
	else
		g_snprintf(status, sizeof status, "Connecting to oscmix… • %s",
		           ui->renderer[0] ? ui->renderer : "GTK 4");
	if (strcmp(ui->status_text, status) != 0) {
		gtk_label_set_text(ui->status, status);
		g_strlcpy(ui->status_text, status, sizeof ui->status_text);
	}
	return G_SOURCE_CONTINUE;
}

static void
set_metering_enabled(AppUi *ui, gboolean enabled)
{
	enabled = !!enabled;
	if (ui->metering_enabled == enabled)
		return;
	ui->metering_enabled = enabled;
	osc_transport_set_metering(ui->transport, enabled);
	osc_transport_send_int(ui->transport, "/metering", enabled);
}

static void
on_toplevel_state_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	AppUi *ui = data;
	GdkToplevelState state;

	(void)spec;
	state = gdk_toplevel_get_state(GDK_TOPLEVEL(object));
	set_metering_enabled(ui, !(state & GDK_TOPLEVEL_STATE_MINIMIZED));
}

static void
on_window_map(GtkWidget *widget, gpointer data)
{
	AppUi *ui = data;
	GtkNative *native;
	GskRenderer *renderer;

	native = gtk_widget_get_native(widget);
	renderer = native ? gtk_native_get_renderer(native) : NULL;
	if (renderer) {
		g_strlcpy(ui->renderer, G_OBJECT_TYPE_NAME(renderer), sizeof ui->renderer);
		g_message("GTK 4 renderer: %s", ui->renderer);
	}
	if (native) {
		GdkSurface *surface = gtk_native_get_surface(native);
		if (GDK_IS_TOPLEVEL(surface) &&
		    !g_object_get_data(G_OBJECT(surface), "oscmix-state-handler")) {
			g_signal_connect(surface, "notify::state",
			                 G_CALLBACK(on_toplevel_state_changed), ui);
			g_object_set_data(G_OBJECT(surface), "oscmix-state-handler", GINT_TO_POINTER(1));
		}
	}
	set_metering_enabled(ui, TRUE);
}

static void
on_window_unmap(GtkWidget *widget, gpointer data)
{
	AppUi *ui = data;

	(void)widget;
	set_metering_enabled(ui, FALSE);
}

static void
app_ui_free(gpointer data)
{
	AppUi *ui = data;
	GError *error = NULL;
	char *state_dir;

	/* Solo is an ephemeral monitoring state. Always restore every submix
	 * before the dedicated backend is stopped by AppRun. */
	osc_transport_send(ui->transport, "/solo/clear");
	g_usleep(20000);
	state_dir = g_path_get_dirname(ui->route_state_path);
	if (g_mkdir_with_parents(state_dir, 0700) != 0) {
		g_warning("could not create mixer state directory: %s", state_dir);
	} else if (!mix_model_save_routes(ui->model, ui->route_state_path, &error)) {
		g_warning("could not save mixer matrix: %s", error->message);
		g_clear_error(&error);
	}
	g_free(state_dir);
	destroy_eq_window(ui);
	destroy_dynamics_window(ui);
	destroy_room_window(ui);
	dsp_panel_free(ui->eq_panel);
	dsp_panel_free(ui->dynamics_panel);
	dsp_panel_free(ui->dsp_panel);
	osc_transport_free(ui->transport);
	mix_model_free(ui->model);
	g_free(ui->route_state_path);
	g_free(ui);
}

static void
show_startup_error(GtkApplication *application, const char *message)
{
	GtkWidget *window, *box, *label;

	window = gtk_application_window_new(application);
	gtk_window_set_title(GTK_WINDOW(window), "oscmix GTK4 — error");
	gtk_window_set_default_size(GTK_WINDOW(window), 520, 180);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
	gtk_widget_set_margin_top(box, 24);
	gtk_widget_set_margin_bottom(box, 24);
	gtk_widget_set_margin_start(box, 24);
	gtk_widget_set_margin_end(box, 24);
	label = gtk_label_new(message);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_window_set_child(GTK_WINDOW(window), box);
	gtk_box_append(GTK_BOX(box), label);
	gtk_window_present(GTK_WINDOW(window));
}

static void
activate(GtkApplication *application, gpointer data)
{
	AppUi *ui;
	GtkWidget *window, *root, *header, *title, *main_box, *rows, *rows_view, *panel;
	GtkWidget *inspector_icon;
	GtkStringList *outputs;
	MixRowSnapshot output_snapshot;
	GError *error = NULL;
	int i;
	const char *test_quit;

	(void)data;
	ui = g_new0(AppUi, 1);
	ui->application = application;
	ui->displayed_kind = MIX_CHANNEL_COUNT;
	ui->displayed_channel = -1;
	ui->displayed_output = -1;
	ui->eq_channel = -1;
	ui->last_control_generation = G_MAXUINT64;
	ui->model = mix_model_new();
	ui->route_state_path = g_build_filename(g_get_user_state_dir(), "oscmix",
	                                        "ufxplus-matrix.ini", NULL);
	if (!mix_model_load_routes(ui->model, ui->route_state_path, &error)) {
		if (error && !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			g_warning("could not restore mixer matrix: %s", error->message);
		g_clear_error(&error);
	}
	ui->transport = osc_transport_new(ui->model,
	                                  config.send_host, config.send_port,
	                                  config.recv_host, config.recv_port, &error);
	if (!ui->transport) {
		show_startup_error(application, error->message);
		g_clear_error(&error);
		mix_model_free(ui->model);
		g_free(ui->route_state_path);
		g_free(ui);
		return;
	}
	osc_transport_start(ui->transport);
	g_object_set_data_full(G_OBJECT(application), "oscmix-ui", ui, app_ui_free);

	window = gtk_application_window_new(application);
	ui->window = GTK_WINDOW(window);
	gtk_window_set_title(ui->window, "oscmix GTK4 — RME Fireface UFX+");
	gtk_window_set_default_size(ui->window, 1440, 940);
	g_signal_connect(window, "close-request",
	                 G_CALLBACK(on_main_close_request), ui);
	g_signal_connect(window, "map", G_CALLBACK(on_window_map), ui);
	g_signal_connect(window, "unmap", G_CALLBACK(on_window_unmap), ui);

	header = gtk_header_bar_new();
	title = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(title), gtk_label_new("oscmix GTK4 — UFX+"));
	ui->status = GTK_LABEL(gtk_label_new("Connecting to oscmix…"));
	gtk_widget_add_css_class(GTK_WIDGET(ui->status), "status");
	gtk_box_append(GTK_BOX(title), GTK_WIDGET(ui->status));
	gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);

	outputs = gtk_string_list_new(NULL);
	mix_model_snapshot_row(ui->model, MIX_CHANNEL_OUTPUT, &output_snapshot);
	for (i = 0; i < output_snapshot.count; ++i) {
		char output_name[72];

		mix_row_format_channel_name(&output_snapshot, i, output_name,
		                            sizeof output_name);
		gtk_string_list_append(outputs, output_name);
	}
	ui->output = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(outputs), NULL));
	gtk_widget_set_tooltip_text(GTK_WIDGET(ui->output), "Active submix output");
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), GTK_WIDGET(ui->output));
	g_signal_connect(ui->output, "notify::selected", G_CALLBACK(on_output_changed), ui);
	ui->inspector_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
	inspector_icon = gtk_image_new_from_icon_name("sidebar-show-right-symbolic");
	gtk_button_set_child(GTK_BUTTON(ui->inspector_toggle), inspector_icon);
	gtk_widget_add_css_class(GTK_WIDGET(ui->inspector_toggle), "flat");
	gtk_widget_add_css_class(GTK_WIDGET(ui->inspector_toggle), "inspector-toggle");
	gtk_widget_set_tooltip_text(GTK_WIDGET(ui->inspector_toggle), "Hide inspector");
	gtk_toggle_button_set_active(ui->inspector_toggle, TRUE);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header),
	                        GTK_WIDGET(ui->inspector_toggle));

	root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(root), header);
	main_box = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	ui->main_paned = GTK_PANED(main_box);
	gtk_widget_set_vexpand(main_box, TRUE);
	gtk_box_append(GTK_BOX(root), main_box);
	rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
	gtk_widget_set_hexpand(rows, TRUE);
	gtk_box_append(GTK_BOX(rows), make_row_with_scroll(ui, MIX_CHANNEL_INPUT));
	gtk_box_append(GTK_BOX(rows), make_row_with_scroll(ui, MIX_CHANNEL_PLAYBACK));
	gtk_box_append(GTK_BOX(rows), make_row_with_scroll(ui, MIX_CHANNEL_OUTPUT));
	/* The mixer rows have their own horizontal scrollbars. A viewport prevents
	 * their natural all-channel width from starving the inspector in GtkPaned. */
	rows_view = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rows_view),
	                               GTK_POLICY_NEVER, GTK_POLICY_NEVER);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(rows_view),
	                                                FALSE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rows_view), rows);
	panel = build_inspector(ui);
	ui->inspector = panel;
	ui->eq_panel = dsp_panel_new_eq(ui->model, ui->transport);
	ui->dynamics_panel = dsp_panel_new_dynamics(ui->model, ui->transport);
	dsp_panel_set_detach_handler(ui->eq_panel,
	                             on_dsp_detach_requested, ui);
	dsp_panel_set_detach_handler(ui->dynamics_panel,
	                             on_dsp_detach_requested, ui);
	gtk_paned_set_start_child(GTK_PANED(main_box), rows_view);
	gtk_paned_set_end_child(GTK_PANED(main_box), panel);
	/* The inspector can be dragged narrower by the user. Its width is capped
	 * separately in constrain_inspector_width(), never hard-fixed. */
	gtk_paned_set_resize_start_child(GTK_PANED(main_box), TRUE);
	gtk_paned_set_resize_end_child(GTK_PANED(main_box), TRUE);
	/* Mixer rows already provide horizontal scrolling. Let them shrink before
	 * sacrificing the inspector to their natural full-channel width. */
	gtk_paned_set_shrink_start_child(GTK_PANED(main_box), TRUE);
	gtk_paned_set_shrink_end_child(GTK_PANED(main_box), TRUE);
	gtk_paned_set_position(GTK_PANED(main_box), 1152);
	ui->inspector_width = 288;
	g_signal_connect(main_box, "notify::position",
	                 G_CALLBACK(on_paned_position_changed), ui);
	g_signal_connect(ui->inspector_toggle, "toggled",
	                 G_CALLBACK(on_inspector_toggled), ui);
	gtk_window_set_child(ui->window, root);
	gtk_widget_add_tick_callback(window, sync_ui, ui, NULL);
	gtk_window_present(ui->window);

	test_quit = g_getenv("OSCMIX_GTK4_TEST_QUIT_MS");
	if (test_quit && atoi(test_quit) > 0) {
		g_message("test timeout armed: %s ms", test_quit);
		g_timeout_add(atoi(test_quit), quit_test_application, application);
	}
}

static void
load_css(GtkApplication *application, gpointer data)
{
	static const char css[] =
		"window { background: #09101b; color: #e1e9f4; }"
		"headerbar { background: #111b2b; color: #eef7ff; min-height: 48px; }"
		".status { color: #7d91a8; font-size: 0.82em; }"
		".inspector { background: #101a29; }"
		".inspector-content { padding: 9px; }"
		".section-title { color: #7f94ad; font-weight: 600; }"
		".channel-title { color: #f2f8ff; font-size: 1.15em; font-weight: 700; }"
		".control-label { color: #9bacc0; font-size: 0.9em; }"
		".coming-soon { color: #60738a; margin-top: 18px; }"
		".dsp-tabs button { min-width: 0; padding-left: 7px; padding-right: 7px; }"
		".dsp-window-button { min-width: 24px; min-height: 24px; padding: 2px; }"
		".channel-eq-panel { background: #101720; color: #d9e1e9; border-left: 1px solid #35414e; border-right: 1px solid #35414e; }"
		".eq-channel-title { color: #f0f4f7; font-size: 1.1em; font-weight: 700; }"
		".eq-master-label, .eq-lowcut-title { color: #d9e1e9; font-weight: 600; }"
		".eq-row-label { color: #8795a3; font-size: .88em; }"
		".eq-param-label { color: #9aa8b5; font-size: .9em; }"
		".eq-band-label { font-size: 1.05em; font-weight: 700; }"
		".eq-band-1 { color: #ef4033; } .eq-band-2 { color: #40dc59; } .eq-band-3 { color: #2ea3f5; }"
		".eq-fixed-type { color: #8b98a4; font-size: .9em; font-style: italic; }"
		".eq-band-box { padding: 0 1px; border-right: 1px solid rgba(90,105,120,.25); }"
		".channel-eq-panel dropdown, .channel-eq-panel dropdown button { min-width: 0; min-height: 22px; padding: 0 3px; font-size: .88em; }"
		".channel-eq-panel switch { min-height: 18px; }"
		".compact-dsp-panel dropdown, .compact-dsp-panel dropdown button { min-width: 0; min-height: 20px; padding: 0 1px; font-size: .78em; }"
		".compact-dsp-panel .eq-param-label, .compact-dsp-panel .eq-row-label, .compact-dsp-panel .dynamics-param-label { font-size: .76em; }"
		".compact-dsp-panel .eq-band-box, .compact-dsp-panel .dynamics-group { padding-left: 0; padding-right: 0; }"
		".expanded-eq-panel { padding: 8px; }"
		".expanded-eq-panel .eq-param-label, .expanded-eq-panel .eq-row-label { font-size: 1em; }"
		".expanded-eq-panel dropdown, .expanded-eq-panel dropdown button { min-height: 30px; font-size: 1em; }"
		".expanded-eq-panel .eq-band-box { padding: 4px 8px; }"
		".channel-dynamics-panel { background: #101720; color: #d9e1e9; border-left: 1px solid #35414e; border-right: 1px solid #35414e; }"
		".dynamics-channel-title { color: #7f8d9b; font-size: .88em; font-weight: 600; }"
		".dynamics-master-label, .dynamics-group-title { color: #d9e1e9; font-weight: 600; }"
		".dynamics-compressor-title { color: #f23845; }"
		".dynamics-expander-title { color: #35dc70; }"
		".dynamics-param-label { color: #8795a3; font-size: .84em; }"
		".dynamics-group { padding: 1px 4px; }"
		".channel-dynamics-panel switch { min-height: 18px; }"
		".expanded-dynamics-panel { padding: 8px; }"
		".expanded-dynamics-panel .dynamics-param-label { font-size: 1em; }"
		"scrollbar { min-height: 8px; }";
	GtkCssProvider *provider;

	(void)application;
	(void)data;
	provider = gtk_css_provider_new();
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_css_provider_load_from_data(provider, css, -1);
	G_GNUC_END_IGNORE_DEPRECATIONS
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
	                                           GTK_STYLE_PROVIDER(provider),
	                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);
}

int
main(int argc, char **argv)
{
	GtkApplication *application;
	GApplicationFlags application_flags;
	GOptionContext *options;
	GError *error = NULL;
	int result;
	static GOptionEntry entries[] = {
		{"send-host", 0, 0, G_OPTION_ARG_STRING, &config.send_host,
		 "OSC engine host", "HOST"},
		{"send-port", 0, 0, G_OPTION_ARG_INT, &config.send_port,
		 "OSC engine receive port", "PORT"},
		{"recv-host", 0, 0, G_OPTION_ARG_STRING, &config.recv_host,
		 "Local OSC bind address", "HOST"},
		{"recv-port", 0, 0, G_OPTION_ARG_INT, &config.recv_port,
		 "Local OSC receive port", "PORT"},
		{NULL}
	};

	config.send_host = g_strdup("127.0.0.1");
	config.recv_host = g_strdup("127.0.0.1");
	options = g_option_context_new("— professional RME Fireface UFX+ remote");
	g_option_context_add_main_entries(options, entries, NULL);
	if (!g_option_context_parse(options, &argc, &argv, &error)) {
		g_printerr("%s\n", error->message);
		g_clear_error(&error);
		g_option_context_free(options);
		return 2;
	}
	g_option_context_free(options);
	if (config.send_port < 1 || config.send_port > 65535 ||
	    config.recv_port < 1 || config.recv_port > 65535) {
		g_printerr("OSC ports must be between 1 and 65535\n");
		return 2;
	}
#if GLIB_CHECK_VERSION(2, 74, 0)
	application_flags = G_APPLICATION_DEFAULT_FLAGS;
#else
	application_flags = G_APPLICATION_FLAGS_NONE;
#endif
	/* Each launcher owns its OSC ports and backend process, so it must also own
	 * a distinct UI process instead of forwarding activation over D-Bus. */
	application_flags |= G_APPLICATION_NON_UNIQUE;
	application = gtk_application_new("io.github.charlysound.OscmixGtk4Prototype",
	                                  application_flags);
	g_signal_connect(application, "startup", G_CALLBACK(load_css), NULL);
	g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
	result = g_application_run(G_APPLICATION(application), argc, argv);
	g_object_unref(application);
	g_free(config.send_host);
	g_free(config.recv_host);
	return result;
}
