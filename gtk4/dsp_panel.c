#include <math.h>
#include <string.h>
#include "dsp_panel.h"

typedef enum {
	PARAM_SELECTED_CHANNEL,
	PARAM_SELECTED_OUTPUT,
	PARAM_GLOBAL
} ParamScope;

typedef enum {
	PARAM_SWITCH,
	PARAM_SCALE,
	PARAM_DROPDOWN,
	PARAM_KNOB
} ParamWidgetType;

typedef enum {
	DSP_PAGE_INSPECTOR,
	DSP_PAGE_INLINE,
	DSP_PAGE_WINDOW
} DspPageMode;

typedef struct {
	GtkAdjustment *adjustment;
	double reset_value;
	double drag_start_value;
	double drag_value;
	double drag_last_x;
	double drag_last_y;
	double red;
	double green;
	double blue;
	int digits;
	gboolean logarithmic;
	char unit[8];
} KnobData;

typedef struct {
	struct DspPanel *panel;
	GtkWidget *widget;
	GtkWidget *row;
	char *path;
	ParamScope scope;
	ParamWidgetType type;
	guint kind_mask;
	gboolean integer;
} ParamBinding;

struct DspPanel {
	MixModel *model;
	OscTransport *transport;
	GtkWidget *root;
	GtkStack *stack;
	GtkWidget *eq_page;
	GtkWidget *eq_graph;
	GtkWidget *eq_meter;
	GtkLabel *eq_title;
	GtkButton *eq_window_button;
	GtkWidget *dynamics_page;
	GtkWidget *dynamics_graph;
	GtkWidget *dynamics_meter;
	GtkLabel *dynamics_title;
	GtkButton *dynamics_window_button;
	GtkWidget *room_page;
	GtkButton *room_window_button;
	GtkWidget *room_graph;
	GtkToggleButton *room_left;
	GtkToggleButton *room_link;
	GtkToggleButton *room_right;
	GtkSwitch *room_band_enabled[9];
	GtkWidget *room_band_controls[9];
	GPtrArray *bindings;
	MixChannelKind kind;
	int channel;
	int selected_output;
	int room_pair;
	int room_peer;
	int room_target_output;
	int graph_drag_band;
	int graph_selected_band;
	float live_levels[2];
	float live_gain_reduction;
	gboolean live_stereo;
	double graph_drag_frequency;
	double graph_drag_gain;
	double graph_drag_q;
	double graph_drag_last_x;
	double graph_drag_last_y;
	gboolean updating;
	gboolean owns_root;
	gboolean expanded_controls;
	gboolean compact_controls;
	gboolean room_links[OSCMIX_UFXP_OUTPUTS / 2];
	gboolean room_right_selected[OSCMIX_UFXP_OUTPUTS / 2];
	gboolean room_band_active[OSCMIX_UFXP_OUTPUTS][9];
	double room_band_bypassed_gain[OSCMIX_UFXP_OUTPUTS][9];
	DspPanelDetachFunc detach_callback;
	gpointer detach_data;
};

#define KIND_MASK(kind) (1u << (kind))
#define MASK_IO (KIND_MASK(MIX_CHANNEL_INPUT) | KIND_MASK(MIX_CHANNEL_OUTPUT))
#define MASK_ALL (MASK_IO | KIND_MASK(MIX_CHANNEL_PLAYBACK))

static void
queue_dsp_graphs(DspPanel *self)
{
	if (self->eq_graph)
		gtk_widget_queue_draw(self->eq_graph);
	if (self->dynamics_graph)
		gtk_widget_queue_draw(self->dynamics_graph);
	if (self->room_graph)
		gtk_widget_queue_draw(self->room_graph);
}

static void
queue_live_displays(DspPanel *self)
{
	if (self->eq_meter && gtk_widget_get_mapped(self->eq_meter))
		gtk_widget_queue_draw(self->eq_meter);
	if (self->dynamics_meter && gtk_widget_get_mapped(self->dynamics_meter))
		gtk_widget_queue_draw(self->dynamics_meter);
	if (self->dynamics_graph && gtk_widget_get_mapped(self->dynamics_graph))
		gtk_widget_queue_draw(self->dynamics_graph);
}

static void
binding_free(gpointer data)
{
	ParamBinding *binding = data;

	g_free(binding->path);
	g_free(binding);
}

static gboolean
widget_is_dsp_parameter(DspPanel *self, GtkWidget *widget, GtkWidget *page)
{
	for (; widget && widget != page; widget = gtk_widget_get_parent(widget)) {
		if (widget == self->eq_meter || widget == self->dynamics_meter)
			return FALSE;
		if (GTK_IS_BUTTON(widget) || GTK_IS_SWITCH(widget) ||
		    GTK_IS_DROP_DOWN(widget) || GTK_IS_RANGE(widget) ||
		    GTK_IS_DRAWING_AREA(widget))
			return TRUE;
	}
	return FALSE;
}

static void
on_page_double_click(GtkGestureClick *gesture, int n_press, double x, double y,
	gpointer data)
{
	DspPanel *self = data;
	GtkWidget *page, *target;
	DspPanelPage panel_page;

	if (n_press != 2 || !self->detach_callback)
		return;
	page = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	target = gtk_widget_pick(page, x, y, GTK_PICK_DEFAULT);
	if (widget_is_dsp_parameter(self, target, page))
		return;
	panel_page = page == self->dynamics_page ? DSP_PANEL_PAGE_DYNAMICS :
	             page == self->room_page ? DSP_PANEL_PAGE_ROOM_EQ :
	                                      DSP_PANEL_PAGE_EQ;
	self->detach_callback(self, panel_page, self->detach_data);
}

static void
enable_page_double_click(DspPanel *self, GtkWidget *page, DspPageMode mode)
{
	GtkGesture *click;

	if (mode == DSP_PAGE_WINDOW)
		return;
	click = GTK_GESTURE(gtk_gesture_click_new());
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
	                                           GTK_PHASE_CAPTURE);
	g_signal_connect(click, "released", G_CALLBACK(on_page_double_click), self);
	gtk_widget_add_controller(page, GTK_EVENT_CONTROLLER(click));
}

static void
knob_data_free(gpointer data)
{
	KnobData *knob = data;

	g_clear_object(&knob->adjustment);
	g_free(knob);
}

static double
knob_fraction(KnobData *knob, double value)
{
	double lower = gtk_adjustment_get_lower(knob->adjustment);
	double upper = gtk_adjustment_get_upper(knob->adjustment);

	if (knob->logarithmic)
		return log(MAX(value, lower) / lower) / log(upper / lower);
	return (value - lower) / (upper - lower);
}

static double
knob_value(KnobData *knob, double fraction)
{
	double lower = gtk_adjustment_get_lower(knob->adjustment);
	double upper = gtk_adjustment_get_upper(knob->adjustment);
	double value;

	fraction = CLAMP(fraction, 0., 1.);
	if (knob->logarithmic)
		value = lower * pow(upper / lower, fraction);
	else
		value = lower + fraction * (upper - lower);
	return round(value / gtk_adjustment_get_step_increment(knob->adjustment)) *
	       gtk_adjustment_get_step_increment(knob->adjustment);
}

static void
draw_knob(GtkDrawingArea *area, cairo_t *cr, int width, int height,
	gpointer data)
{
	KnobData *knob = data;
	double angle, center_y, fraction, radius;
	char text[32];
	cairo_text_extents_t extents;

	(void)area;
	fraction = CLAMP(knob_fraction(knob,
	                gtk_adjustment_get_value(knob->adjustment)), 0., 1.);
	radius = MIN(width * .23, (height - 13) * .46);
	center_y = radius + 4.;
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_width(cr, 4.);
	cairo_set_source_rgba(cr, .18, .22, .27, 1.);
	cairo_arc(cr, width / 2., center_y, radius,
	          G_PI * .75, G_PI * 2.25);
	cairo_stroke(cr);
	cairo_set_source_rgba(cr, knob->red, knob->green, knob->blue, 1.);
	cairo_arc(cr, width / 2., center_y, radius,
	          G_PI * .75, G_PI * (.75 + 1.5 * fraction));
	cairo_stroke(cr);
	cairo_set_source_rgba(cr, .035, .045, .06, 1.);
	cairo_arc(cr, width / 2., center_y, MAX(2., radius - 4.), 0., 2. * G_PI);
	cairo_fill(cr);
	angle = G_PI * (.75 + 1.5 * fraction);
	cairo_set_source_rgba(cr, .78, .84, .90, 1.);
	cairo_set_line_width(cr, 2.);
	cairo_move_to(cr, width / 2. + cos(angle) * 5.,
	              center_y + sin(angle) * 5.);
	cairo_line_to(cr, width / 2. + cos(angle) * MAX(5., radius - 4.),
	              center_y + sin(angle) * MAX(5., radius - 4.));
	cairo_stroke(cr);
	if (strcmp(knob->unit, "oct") == 0)
		g_snprintf(text, sizeof text, "%.0f",
		           (gtk_adjustment_get_value(knob->adjustment) + 1.) * 6.);
	else if (strcmp(knob->unit, ":1") == 0)
		g_snprintf(text, sizeof text, "%.*f", knob->digits,
		           gtk_adjustment_get_value(knob->adjustment));
	else if (knob->digits == 0)
		g_snprintf(text, sizeof text, "%.0f%s",
		           gtk_adjustment_get_value(knob->adjustment), knob->unit);
	else
		g_snprintf(text, sizeof text, "%+.*f%s", knob->digits,
		           gtk_adjustment_get_value(knob->adjustment), knob->unit);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 11.);
	cairo_text_extents(cr, text, &extents);
	if (extents.width > width - 2.) {
		cairo_set_font_size(cr, MAX(7.5, 11. * (width - 2.) / extents.width));
		cairo_text_extents(cr, text, &extents);
	}
	cairo_set_source_rgba(cr, knob->red, knob->green, knob->blue, 1.);
	cairo_move_to(cr, (width - extents.width) / 2. - extents.x_bearing,
	              height - 3.);
	cairo_show_text(cr, text);
}

static void
on_knob_adjustment_changed(GtkAdjustment *adjustment, gpointer data)
{
	(void)adjustment;
	gtk_widget_queue_draw(GTK_WIDGET(data));
}

static void
on_knob_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data)
{
	KnobData *knob = data;

	(void)gesture;
	(void)x;
	(void)y;
	knob->drag_start_value = gtk_adjustment_get_value(knob->adjustment);
	knob->drag_value = knob->drag_start_value;
	knob->drag_last_x = 0.;
	knob->drag_last_y = 0.;
}

static void
on_knob_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y,
	gpointer data)
{
	KnobData *knob = data;
	double delta_x, delta_y, fraction;
	GdkModifierType modifiers;

	delta_x = offset_x - knob->drag_last_x;
	delta_y = offset_y - knob->drag_last_y;
	knob->drag_last_x = offset_x;
	knob->drag_last_y = offset_y;
	if (fabs(delta_x) > 96. || fabs(delta_y) > 96.)
		return;
	modifiers = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(gesture));
	if (modifiers & GDK_CONTROL_MASK) {
		delta_x *= .1;
		delta_y *= .1;
	}
	fraction = knob_fraction(knob, knob->drag_value) +
	           (delta_x - delta_y) / 180.;
	knob->drag_value = knob_value(knob, fraction);
	gtk_adjustment_set_value(knob->adjustment, knob->drag_value);
}

static gboolean
on_knob_scroll(GtkEventControllerScroll *controller, double dx, double dy,
	gpointer data)
{
	KnobData *knob = data;
	double delta, fraction;
	GdkModifierType modifiers;

	delta = fabs(dx) > fabs(dy) ? dx : -dy;
	if (delta == 0.)
		return TRUE;
	modifiers = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(controller));
	fraction = knob_fraction(knob, gtk_adjustment_get_value(knob->adjustment));
	/* GTK smooth scrolling can report several large, mixed-axis deltas for one
	 * physical wheel notch. Use direction only, so every normalized discrete
	 * event advances by one predictable visual step. */
	fraction += (delta > 0. ? 1. : -1.) *
	            ((modifiers & GDK_CONTROL_MASK) ? .00125 : .0125);
	gtk_adjustment_set_value(knob->adjustment, knob_value(knob, fraction));
	return TRUE;
}

static void
on_knob_click(GtkGestureClick *gesture, int n_press, double x, double y,
	gpointer data)
{
	KnobData *knob = data;

	(void)gesture;
	(void)x;
	(void)y;
	if (n_press == 2)
		gtk_adjustment_set_value(knob->adjustment, knob->reset_value);
}

static GtkWidget *
new_knob(double lower, double upper, double step, double reset_value,
	gboolean logarithmic, int digits, const char *unit,
	double red, double green, double blue)
{
	GtkWidget *area;
	GtkGesture *drag, *click;
	GtkEventController *scroll;
	KnobData *knob;

	area = gtk_drawing_area_new();
	gtk_widget_set_size_request(area, 38, 38);
	gtk_widget_set_cursor_from_name(area, "ns-resize");
	knob = g_new0(KnobData, 1);
	knob->adjustment = g_object_ref_sink(
		gtk_adjustment_new(reset_value, lower, upper, step, step * 10., 0.));
	knob->reset_value = reset_value;
	knob->logarithmic = logarithmic;
	knob->digits = digits;
	knob->red = red;
	knob->green = green;
	knob->blue = blue;
	g_strlcpy(knob->unit, unit ? unit : "", sizeof knob->unit);
	g_object_set_data_full(G_OBJECT(area), "oscmix-knob", knob, knob_data_free);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_knob, knob, NULL);
	g_signal_connect(knob->adjustment, "value-changed",
	                 G_CALLBACK(on_knob_adjustment_changed), area);
	drag = gtk_gesture_drag_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_knob_drag_begin), knob);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_knob_drag_update), knob);
	gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));
	click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
	/* Apply the double-click reset only after a genuine click has completed.
	 * Handling it on press made a quick click followed by a drag look like a
	 * double-click and caused an apparent jump to the reset value. */
	g_signal_connect(click, "released", G_CALLBACK(on_knob_click), knob);
	gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
	scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_knob_scroll), knob);
	gtk_widget_add_controller(area, scroll);
	return area;
}

static double
selected_parameter(DspPanel *self, const char *path, double fallback)
{
	MixParamValue value;
	char address[128];

	if (self->kind == MIX_CHANNEL_PLAYBACK)
		return fallback;
	g_snprintf(address, sizeof address, "/%s/%d/%s",
	           mix_channel_kind_osc_name(self->kind), self->channel + 1, path);
	if (!mix_model_get_parameter(self->model, address, &value))
		return fallback;
	return value.type == 'f' ? value.f : value.i;
}

static void
set_selected_parameter(DspPanel *self, const char *path, double value,
	gboolean integer)
{
	char address[128];

	if (self->kind == MIX_CHANNEL_PLAYBACK || self->channel < 0)
		return;
	g_snprintf(address, sizeof address, "/%s/%d/%s",
	           mix_channel_kind_osc_name(self->kind), self->channel + 1, path);
	if (integer) {
		mix_model_set_parameter_int(self->model, address, lround(value));
		osc_transport_send_int(self->transport, address, lround(value));
	} else {
		mix_model_set_parameter_float(self->model, address, value);
		osc_transport_send_float(self->transport, address, value);
	}
	queue_dsp_graphs(self);
}

static double
dynamics_output(double input, double exp_threshold, double exp_ratio,
	double comp_threshold, double comp_ratio, double gain)
{
	if (input < exp_threshold)
		return exp_threshold + (input - exp_threshold) * exp_ratio + gain;
	if (input > comp_threshold)
		return comp_threshold + (input - comp_threshold) / comp_ratio + gain;
	return input + gain;
}

static void
draw_level_lane(cairo_t *cr, double x, double top, double width, double height,
	double level)
{
	double bottom, level_y, orange_y, red_y;

	bottom = top + height;
	level = CLAMP(level, -60., 0.);
	level_y = top + -level / 60. * height;
	orange_y = top + 18. / 60. * height;
	red_y = top + 6. / 60. * height;
	cairo_set_source_rgba(cr, .035, .075, .09, 1.);
	cairo_rectangle(cr, x, top, width, height);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, .16, .86, .45, 1.);
	cairo_rectangle(cr, x, MAX(level_y, orange_y), width,
	                bottom - MAX(level_y, orange_y));
	cairo_fill(cr);
	if (level > -18.) {
		cairo_set_source_rgba(cr, 1., .55, .10, 1.);
		cairo_rectangle(cr, x, MAX(level_y, red_y), width,
		                orange_y - MAX(level_y, red_y));
		cairo_fill(cr);
	}
	if (level > -6.) {
		cairo_set_source_rgba(cr, .96, .18, .18, 1.);
		cairo_rectangle(cr, x, level_y, width, red_y - level_y);
		cairo_fill(cr);
	}
	cairo_set_source_rgba(cr, .38, .46, .52, .65);
	cairo_set_line_width(cr, 1.);
	cairo_rectangle(cr, x + .5, top + .5, width - 1., height - 1.);
	cairo_stroke(cr);
}

static void
draw_meter_scale(cairo_t *cr, double anchor_x, double top, double height,
	gboolean labels_right, gboolean compact, gboolean gain_reduction)
{
	static const int full_ticks[] = {0, 6, 18, 30, 42, 54, 60};
	static const int compact_ticks[] = {0, 12, 24, 36, 48, 60};
	static const int full_gr_ticks[] = {0, 2, 4, 6, 8, 10,
	                                    12, 14, 16, 18, 20};
	static const int compact_gr_ticks[] = {0, 5, 10, 15, 20};
	const int *ticks = gain_reduction
	                 ? (compact ? compact_gr_ticks : full_gr_ticks)
	                 : (compact ? compact_ticks : full_ticks);
	int count = gain_reduction
	          ? (compact ? G_N_ELEMENTS(compact_gr_ticks)
	                     : G_N_ELEMENTS(full_gr_ticks))
	          : (compact ? G_N_ELEMENTS(compact_ticks)
	                     : G_N_ELEMENTS(full_ticks));
	double range = gain_reduction ? 20. : 60.;
	cairo_text_extents_t extents;
	double baseline, y;
	char label[8];
	int i;

	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, compact ? 6.5 : 7.5);
	/* One-dB minor ticks make the dedicated 0…-20 dB GR scale precise even
	 * when the compact editor cannot fit a label at every major division. */
	if (gain_reduction) {
		cairo_set_source_rgba(cr, .28, .72, .90, .55);
		cairo_set_line_width(cr, 1.);
		for (i = 0; i <= 20; ++i) {
			y = top + i / range * height;
			cairo_move_to(cr, anchor_x, y + .5);
			cairo_line_to(cr, anchor_x + (labels_right ? 1.5 : -1.5), y + .5);
			cairo_stroke(cr);
		}
	}
	for (i = 0; i < count; ++i) {
		y = top + ticks[i] / range * height;
		g_snprintf(label, sizeof label, ticks[i] ? "-%d" : "0", ticks[i]);
		cairo_text_extents(cr, label, &extents);
		baseline = y - extents.height / 2. - extents.y_bearing;
		baseline = CLAMP(baseline,
		                 top - extents.y_bearing,
		                 top + height - extents.height - extents.y_bearing);
		if (gain_reduction)
			cairo_set_source_rgba(cr, .28, .72, .90, .92);
		else
			cairo_set_source_rgba(cr, .56, .64, .72, .92);
		cairo_set_line_width(cr, 1.);
		cairo_move_to(cr, anchor_x, y + .5);
		cairo_line_to(cr, anchor_x + (labels_right ? 3. : -3.), y + .5);
		cairo_stroke(cr);
		cairo_move_to(cr,
		              labels_right ? anchor_x + 5.
		                           : anchor_x - 5. - extents.width,
		              baseline);
		cairo_show_text(cr, label);
	}
}

static void
draw_meter_caption(cairo_t *cr, const char *text, double center_x, int height,
	gboolean gain_reduction)
{
	cairo_text_extents_t extents;

	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 8.);
	cairo_text_extents(cr, text, &extents);
	if (gain_reduction)
		cairo_set_source_rgba(cr, .30, .78, .98, .98);
	else
		cairo_set_source_rgba(cr, .68, .75, .82, .98);
	cairo_move_to(cr, center_x - extents.width / 2. - extents.x_bearing,
	              height - 4.);
	cairo_show_text(cr, text);
}

static void
draw_live_meter(GtkDrawingArea *area, cairo_t *cr, int width, int height,
	gpointer data)
{
	DspPanel *self = data;
	gboolean dynamics = GTK_WIDGET(area) == self->dynamics_meter;
	double gr_width, gr_x, input_end, lane_width, left, meter_height, top;
	gboolean compact;

	cairo_set_source_rgb(cr, .025, .038, .052);
	cairo_paint(cr);
	top = 5.;
	meter_height = MAX(12., height - top - 19.);
	compact = meter_height < 90.;
	left = 3.;
	if (self->live_stereo) {
		lane_width = 5.;
		draw_level_lane(cr, left, top, lane_width, meter_height,
		                self->live_levels[0]);
		draw_level_lane(cr, left + lane_width + 2., top, lane_width, meter_height,
		                self->live_levels[1]);
		input_end = left + lane_width * 2. + 2.;
	} else {
		lane_width = 9.;
		draw_level_lane(cr, left, top, lane_width, meter_height,
		                self->live_levels[0]);
		input_end = left + lane_width;
	}
	draw_meter_scale(cr, input_end + 2., top, meter_height,
	                 TRUE, compact, FALSE);
	draw_meter_caption(cr, "IN", (left + input_end) / 2., height, FALSE);
	if (dynamics) {
		double reduction_db = CLAMP(self->live_gain_reduction * 60., 0., 20.);
		double reduction_y = top + reduction_db / 20. * meter_height;

		gr_width = 8.;
		gr_x = width - gr_width - 3.;

		cairo_set_source_rgba(cr, .035, .16, .22, 1.);
		cairo_rectangle(cr, gr_x, top, gr_width, meter_height);
		cairo_fill(cr);
		if (self->live_gain_reduction > .001f) {
			cairo_set_source_rgba(cr, .16, .78, 1., 1.);
			cairo_rectangle(cr, gr_x, top, gr_width, reduction_y - top);
			cairo_fill(cr);
			cairo_rectangle(cr, gr_x - 1., CLAMP(reduction_y - 1., top,
			                top + meter_height - 2.), gr_width + 2., 2.);
			cairo_fill(cr);
		}
		cairo_set_source_rgba(cr, .38, .46, .52, .65);
		cairo_rectangle(cr, gr_x + .5, top + .5, gr_width - 1., meter_height - 1.);
		cairo_stroke(cr);
		draw_meter_scale(cr, gr_x - 2., top, meter_height,
		                 FALSE, compact, TRUE);
		draw_meter_caption(cr, "GR", gr_x + gr_width / 2., height, TRUE);
	}
}

static void
draw_dynamics_graph(GtkDrawingArea *area, cairo_t *cr, int width, int height,
	gpointer data)
{
	DspPanel *self = data;
	static const int input_ticks[] = {-60, -40, -20, 0};
	static const int output_ticks[] = {-60, -40, -20, 0};
	const double input_min = -60.;
	const double input_range = 60.;
	const double output_min = -60.;
	const double output_max = 0.;
	const double output_range = output_max - output_min;
	double comp_ratio, comp_threshold, exp_ratio, exp_threshold, gain;
	double graph_height, graph_width, left, marker_x, marker_y, top, position;
	double input[4], output[4], x[4], y[4];
	double alpha;
	int i, tick;
	char label[16];
	cairo_text_extents_t extents;

	(void)area;
	left = 34.;
	top = 8.;
	graph_width = MAX(1., width - left - 8.);
	graph_height = MAX(1., height - top - 22.);
	gain = CLAMP(selected_parameter(self, "dynamics/gain", 0.), -30., 30.);
	comp_threshold = CLAMP(selected_parameter(self, "dynamics/compthres", -35.),
	                       -60., 0.);
	comp_ratio = CLAMP(selected_parameter(self, "dynamics/compratio", 1.), 1., 10.);
	exp_threshold = CLAMP(selected_parameter(self, "dynamics/expthres", -52.),
	                      -99., comp_threshold);
	exp_ratio = CLAMP(selected_parameter(self, "dynamics/expratio", 1.), 1., 10.);
	alpha = selected_parameter(self, "dynamics", 0.) != 0. ? 1. : .38;

	cairo_set_source_rgb(cr, .025, .038, .052);
	cairo_paint(cr);
	cairo_save(cr);
	cairo_rectangle(cr, left, top, graph_width, graph_height);
	cairo_clip(cr);
	cairo_set_line_width(cr, 1.);
	for (i = -60; i <= 0; i += 10) {
		position = left + (i - input_min) / input_range * graph_width;
		cairo_set_source_rgba(cr, .30, .36, .42,
		                      i == -60 || i == 0 ? .28 : .15);
		cairo_move_to(cr, position, top);
		cairo_line_to(cr, position, top + graph_height);
		cairo_stroke(cr);
	}
	for (i = -60; i <= 0; i += 10) {
		position = top + (output_max - i) / output_range * graph_height;
		cairo_set_source_rgba(cr, .30, .36, .42,
		                      i % 20 == 0 ? .20 : .11);
		cairo_move_to(cr, left, position);
		cairo_line_to(cr, left + graph_width, position);
		cairo_stroke(cr);
	}

	/* Unity reference. */
	cairo_set_source_rgba(cr, .52, .58, .64, .26);
	cairo_set_line_width(cr, 1.);
	cairo_set_dash(cr, (double[]){3., 4.}, 2, 0.);
	cairo_move_to(cr, left,
	              top + (output_max - input_min) / output_range * graph_height);
	cairo_line_to(cr, left + graph_width,
	              top + output_max / output_range * graph_height);
	cairo_stroke(cr);
	cairo_set_dash(cr, NULL, 0, 0.);

	/* Start where the green segment enters the visible -60 dB range. This
	 * preserves a visible Expander Ratio slope without extending either axis. */
	input[0] = exp_threshold +
	           (output_min - gain - exp_threshold) / exp_ratio;
	input[0] = CLAMP(input[0], input_min, MAX(input_min, exp_threshold));
	input[1] = MAX(input_min, exp_threshold);
	input[2] = comp_threshold;
	input[3] = 0.;
	for (i = 0; i < 4; ++i) {
		output[i] = dynamics_output(input[i], exp_threshold, exp_ratio,
		                            comp_threshold, comp_ratio, gain);
		x[i] = left + (input[i] - input_min) / input_range * graph_width;
		y[i] = top + (output_max - CLAMP(output[i], output_min, output_max)) /
		       output_range * graph_height;
	}
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_width(cr, 2.2);
	cairo_set_source_rgba(cr, .20, .86, .43, alpha);
	cairo_move_to(cr, x[0], y[0]);
	cairo_line_to(cr, x[1], y[1]);
	cairo_stroke(cr);
	cairo_set_source_rgba(cr, .12, .66, .92, alpha);
	cairo_move_to(cr, x[1], y[1]);
	cairo_line_to(cr, x[2], y[2]);
	cairo_stroke(cr);
	cairo_set_source_rgba(cr, .95, .22, .27, alpha);
	cairo_move_to(cr, x[2], y[2]);
	cairo_line_to(cr, x[3], y[3]);
	cairo_stroke(cr);

	/* White squares mark the Expander and Compressor thresholds. */
	cairo_set_source_rgba(cr, .94, .96, .98, alpha);
	marker_x = CLAMP(x[1], left + 3., left + graph_width - 3.);
	marker_y = CLAMP(y[1], top + 3., top + graph_height - 3.);
	cairo_rectangle(cr, marker_x - 3., marker_y - 3., 6., 6.);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, .94, .96, .98, alpha);
	marker_x = CLAMP(x[2], left + 3., left + graph_width - 3.);
	marker_y = CLAMP(y[2], top + 3., top + graph_height - 3.);
	cairo_rectangle(cr, marker_x - 3., marker_y - 3., 6., 6.);
	cairo_fill(cr);
	/* The live point follows the pre-DSP input peak across the transfer
	 * function. It uses the same hardware meter sample as the adjacent VU. */
	{
		double live_input = MAX(self->live_levels[0],
		                        self->live_stereo ? self->live_levels[1] : input_min);

		if (live_input > input_min) {
			double live_output = dynamics_output(
				live_input, exp_threshold, exp_ratio,
				comp_threshold, comp_ratio, gain);
			marker_x = left + (CLAMP(live_input, input_min, 0.) - input_min) /
			           input_range * graph_width;
			marker_y = top +
			           (output_max - CLAMP(live_output, output_min, output_max)) /
			           output_range * graph_height;
			cairo_set_source_rgba(cr, .02, .04, .055, .94);
			cairo_arc(cr, marker_x, marker_y, 5., 0., 2. * G_PI);
			cairo_fill_preserve(cr);
			cairo_set_source_rgba(cr, .93, .98, 1., alpha);
			cairo_set_line_width(cr, 2.);
			cairo_stroke(cr);
		}
	}
	cairo_restore(cr);

	/* Horizontal input and vertical output levels, both in dBFS. */
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, height < 100 ? 8. : 9.5);
	cairo_set_source_rgba(cr, .67, .73, .79, .92);
	for (tick = 0; tick < (int)G_N_ELEMENTS(input_ticks); ++tick) {
		position = left + (input_ticks[tick] - input_min) / input_range * graph_width;
		g_snprintf(label, sizeof label, "%d", input_ticks[tick]);
		cairo_text_extents(cr, label, &extents);
		cairo_move_to(cr,
		              CLAMP(position - extents.width / 2. - extents.x_bearing,
		                    left, left + graph_width - extents.width),
		              height - 5.);
		cairo_show_text(cr, label);
	}
	for (tick = 0; tick < (int)G_N_ELEMENTS(output_ticks); ++tick) {
		position = top + (output_max - output_ticks[tick]) / output_range *
		           graph_height;
		g_snprintf(label, sizeof label, "%d", output_ticks[tick]);
		cairo_text_extents(cr, label, &extents);
		cairo_move_to(cr, left - extents.width - 5.,
		              CLAMP(position - extents.height / 2. - extents.y_bearing,
		                    top + extents.height, top + graph_height));
		cairo_show_text(cr, label);
	}
	cairo_set_source_rgba(cr, .32, .39, .46, .55);
	cairo_set_line_width(cr, 1.);
	cairo_rectangle(cr, left + .5, top + .5, graph_width - 1., graph_height - 1.);
	cairo_stroke(cr);
}

static void
graph_point_position(GtkWidget *graph, double frequency, double gain,
	double *x, double *y)
{
	double graph_width = MAX(1, gtk_widget_get_width(graph) - 38);
	double graph_height = MAX(1, gtk_widget_get_height(graph) - 28);

	*x = 30. + log(CLAMP(frequency, 20., 20000.) / 20.) /
	             log(1000.) * graph_width;
	*y = 6. + (24. - CLAMP(gain, -20., 20.)) / 48. * graph_height;
}

static int
graph_band_at(DspPanel *self, GtkWidget *graph, double x, double y)
{
	static const double defaults[] = {100., 1000., 5000.};
	double best_distance = 16., distance, point_x, point_y;
	double frequency, gain;
	char path[48];
	int band, nearest = -1;

	for (band = 0; band < 3; ++band) {
		g_snprintf(path, sizeof path, "eq/band%dgain", band + 1);
		gain = selected_parameter(self, path, 0.);
		g_snprintf(path, sizeof path, "eq/band%dfreq", band + 1);
		frequency = selected_parameter(self, path, defaults[band]);
		graph_point_position(graph, frequency, gain, &point_x, &point_y);
		distance = hypot(x - point_x, y - point_y);
		if (distance <= best_distance) {
			best_distance = distance;
			nearest = band;
		}
	}
	return nearest;
}

static void
graph_begin_band_drag(DspPanel *self, GtkWidget *graph, int band)
{
	static const double defaults[] = {100., 1000., 5000.};
	char path[48];

	self->graph_drag_band = band;
	self->graph_selected_band = band;
	g_snprintf(path, sizeof path, "eq/band%dfreq", band + 1);
	self->graph_drag_frequency = selected_parameter(self, path, defaults[band]);
	g_snprintf(path, sizeof path, "eq/band%dgain", band + 1);
	self->graph_drag_gain = selected_parameter(self, path, 0.);
	g_snprintf(path, sizeof path, "eq/band%dq", band + 1);
	self->graph_drag_q = selected_parameter(self, path, 1.);
	self->graph_drag_last_x = 0.;
	self->graph_drag_last_y = 0.;
	gtk_widget_set_cursor_from_name(graph, "grabbing");
	gtk_widget_queue_draw(graph);
}

static void
on_graph_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
	gpointer data)
{
	DspPanel *self = data;
	GtkWidget *graph;
	int band;

	(void)n_press;
	graph = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	band = graph_band_at(self, graph, x, y);
	if (band >= 0)
		graph_begin_band_drag(self, graph, band);
	else {
		self->graph_drag_band = -1;
		self->graph_selected_band = -1;
		gtk_widget_queue_draw(graph);
	}
}

static void
on_graph_released(GtkGestureClick *gesture, int n_press, double x, double y,
	gpointer data)
{
	DspPanel *self = data;
	GtkWidget *graph;
	char path[48];

	(void)x;
	(void)y;
	graph = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	if (n_press == 2 && self->graph_selected_band >= 0) {
		g_snprintf(path, sizeof path, "eq/band%dgain",
		           self->graph_selected_band + 1);
		set_selected_parameter(self, path, 0., FALSE);
	}
	self->graph_drag_band = -1;
	gtk_widget_set_cursor_from_name(graph, "crosshair");
	gtk_widget_queue_draw(graph);
}

static void
on_graph_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data)
{
	DspPanel *self = data;
	GtkWidget *graph;
	int band;

	graph = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	band = graph_band_at(self, graph, x, y);
	if (band < 0) {
		self->graph_drag_band = -1;
		return;
	}
	graph_begin_band_drag(self, graph, band);
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_graph_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y,
	gpointer data)
{
	DspPanel *self = data;
	GtkWidget *graph;
	double delta_x, delta_y, fraction, frequency, gain, graph_height, graph_width;
	GdkModifierType modifiers;
	char path[48];
	int band = self->graph_drag_band;

	if (band < 0)
		return;
	graph = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	graph_width = MAX(1, gtk_widget_get_width(graph) - 38);
	graph_height = MAX(1, gtk_widget_get_height(graph) - 28);
	delta_x = offset_x - self->graph_drag_last_x;
	delta_y = offset_y - self->graph_drag_last_y;
	self->graph_drag_last_x = offset_x;
	self->graph_drag_last_y = offset_y;
	if (fabs(delta_x) > 96. || fabs(delta_y) > 96.)
		return;
	modifiers = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(gesture));
	if (modifiers & GDK_CONTROL_MASK) {
		delta_x *= .1;
		delta_y *= .1;
	}
	fraction = log(CLAMP(self->graph_drag_frequency, 20., 20000.) / 20.) /
	           log(1000.) + delta_x / graph_width;
	frequency = 20. * pow(1000., CLAMP(fraction, 0., 1.));
	gain = CLAMP(self->graph_drag_gain - delta_y * 48. / graph_height,
	             -20., 20.);
	gain = round(gain * 10.) / 10.;
	self->graph_drag_frequency = frequency;
	self->graph_drag_gain = gain;
	g_snprintf(path, sizeof path, "eq/band%dfreq", band + 1);
	set_selected_parameter(self, path, frequency, TRUE);
	g_snprintf(path, sizeof path, "eq/band%dgain", band + 1);
	set_selected_parameter(self, path, gain, FALSE);
}

static void
on_graph_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y,
	gpointer data)
{
	DspPanel *self = data;
	GtkWidget *graph;

	(void)offset_x;
	(void)offset_y;
	graph = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	self->graph_drag_band = -1;
	gtk_widget_set_cursor_from_name(graph, "crosshair");
	gtk_widget_queue_draw(graph);
}

static gboolean
on_graph_scroll(GtkEventControllerScroll *controller, double dx, double dy,
	gpointer data)
{
	DspPanel *self = data;
	double delta, q, step;
	GdkModifierType modifiers;
	char path[48];
	int band = self->graph_selected_band;

	if (band < 0)
		return FALSE;
	delta = fabs(dx) > fabs(dy) ? -dx : -dy;
	g_snprintf(path, sizeof path, "eq/band%dq", band + 1);
	q = selected_parameter(self, path, self->graph_drag_q);
	if (delta == 0.)
		return TRUE;
	modifiers = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(controller));
	step = modifiers & GDK_CONTROL_MASK ? .1 : .2;
	q = CLAMP(round((q + (delta > 0. ? step : -step)) * 10.) / 10., .4, 9.9);
	self->graph_drag_q = q;
	set_selected_parameter(self, path, q, FALSE);
	return TRUE;
}

static double
band_response(double frequency, double center, double gain, double q,
	int type, int band)
{
	const double sample_rate = 48000.;
	double a0, a1, a2, alpha, amplitude, b0, b1, b2;
	double cosine, cosine_frequency, numerator, denominator, sine, square_amplitude;
	double omega;
	gboolean high_pass, high_shelf, low_pass, low_shelf;

	center = CLAMP(center, 20., 23000.);
	q = CLAMP(q, .4, 9.9);
	omega = 2. * G_PI * center / sample_rate;
	cosine = cos(omega);
	sine = sin(omega);
	amplitude = pow(10., gain / 40.);
	alpha = sine / (2. * q);
	low_shelf = band == 1 && type == 1;
	high_shelf = band == 3 && type == 1;
	high_pass = (band == 1 && type == 2) || (band == 3 && type == 3);
	low_pass = (band == 1 && type == 3) || (band == 3 && type == 2);
	if (low_shelf || high_shelf) {
		square_amplitude = sqrt(amplitude);
		if (low_shelf) {
			b0 = amplitude * ((amplitude + 1.) - (amplitude - 1.) * cosine +
			     2. * square_amplitude * alpha);
			b1 = 2. * amplitude * ((amplitude - 1.) - (amplitude + 1.) * cosine);
			b2 = amplitude * ((amplitude + 1.) - (amplitude - 1.) * cosine -
			     2. * square_amplitude * alpha);
			a0 = (amplitude + 1.) + (amplitude - 1.) * cosine +
			     2. * square_amplitude * alpha;
			a1 = -2. * ((amplitude - 1.) + (amplitude + 1.) * cosine);
			a2 = (amplitude + 1.) + (amplitude - 1.) * cosine -
			     2. * square_amplitude * alpha;
		} else {
			b0 = amplitude * ((amplitude + 1.) + (amplitude - 1.) * cosine +
			     2. * square_amplitude * alpha);
			b1 = -2. * amplitude * ((amplitude - 1.) + (amplitude + 1.) * cosine);
			b2 = amplitude * ((amplitude + 1.) + (amplitude - 1.) * cosine -
			     2. * square_amplitude * alpha);
			a0 = (amplitude + 1.) - (amplitude - 1.) * cosine +
			     2. * square_amplitude * alpha;
			a1 = 2. * ((amplitude - 1.) - (amplitude + 1.) * cosine);
			a2 = (amplitude + 1.) - (amplitude - 1.) * cosine -
			     2. * square_amplitude * alpha;
		}
	} else if (low_pass) {
		b0 = (1. - cosine) / 2.;
		b1 = 1. - cosine;
		b2 = b0;
		a0 = 1. + alpha;
		a1 = -2. * cosine;
		a2 = 1. - alpha;
	} else if (high_pass) {
		b0 = (1. + cosine) / 2.;
		b1 = -(1. + cosine);
		b2 = b0;
		a0 = 1. + alpha;
		a1 = -2. * cosine;
		a2 = 1. - alpha;
	} else {
		b0 = 1. + alpha * amplitude;
		b1 = -2. * cosine;
		b2 = 1. - alpha * amplitude;
		a0 = 1. + alpha / amplitude;
		a1 = -2. * cosine;
		a2 = 1. - alpha / amplitude;
	}
	omega = 2. * G_PI * frequency / sample_rate;
	cosine_frequency = cos(omega);
	numerator = b0 * b0 + b1 * b1 + b2 * b2 +
	            2. * (b0 * b1 + b1 * b2) * cosine_frequency +
	            2. * b0 * b2 * cos(2. * omega);
	denominator = a0 * a0 + a1 * a1 + a2 * a2 +
	              2. * (a0 * a1 + a1 * a2) * cosine_frequency +
	              2. * a0 * a2 * cos(2. * omega);
	return 10. * log10(MAX(1e-10, numerator / MAX(denominator, 1e-20)));
}

static double
lowcut_response(double frequency, double cutoff, int slope)
{
	static const double correction[] = {1., .655, .528, .457};
	double cutoff_squared, frequency_squared, response;
	int i, order;

	cutoff = CLAMP(cutoff, 20., 500.);
	order = CLAMP(slope, 0, 3);
	cutoff *= correction[order];
	cutoff_squared = cutoff * cutoff;
	frequency_squared = frequency * frequency;
	response = 1.;
	for (i = 0; i <= order; ++i)
		response *= frequency_squared / (frequency_squared + cutoff_squared);
	return 10. * log10(MAX(1e-10, response));
}

static void
graph_color(cairo_t *cr, double red, double green, double blue, double alpha)
{
	cairo_set_source_rgba(cr, red, green, blue, alpha);
}

static void
draw_response_curve(cairo_t *cr, int width, int height, const double gain[3],
	const double freq[3], const double q[3], const int type[3], int only_band,
	gboolean lowcut, double lowcut_freq, int lowcut_slope)
{
	double db, frequency, graph_height, graph_width, x, y;
	int band, pixel;

	graph_width = MAX(1, width - 38);
	graph_height = MAX(1, height - 28);
	for (pixel = 0; pixel <= (int)graph_width; ++pixel) {
		frequency = 20. * pow(1000., pixel / graph_width);
		db = 0.;
		if (only_band < 0 && lowcut)
			db += lowcut_response(frequency, lowcut_freq, lowcut_slope);
		for (band = 0; band < 3; ++band) {
			if (only_band != -2 && (only_band < 0 || only_band == band))
				db += band_response(frequency, freq[band], gain[band], q[band],
				                    type[band], band + 1);
		}
		db = CLAMP(db, -24., 24.);
		x = 30. + pixel;
		y = 6. + (24. - db) / 48. * graph_height;
		if (pixel == 0)
			cairo_move_to(cr, x, y);
		else
			cairo_line_to(cr, x, y);
	}
	cairo_stroke(cr);
}

static void
draw_eq_graph(GtkDrawingArea *area, cairo_t *cr, int width, int height,
	gpointer data)
{
	static const double colors[3][3] = {
		{.93, .20, .18}, {.18, .86, .34}, {.12, .62, .95}
	};
	static const double grid_freq[] = {20, 100, 1000, 10000, 20000};
	static const char *const grid_label[] = {"20", "100", "1k", "10k", "20k"};
	DspPanel *self = data;
	double gain[3], freq[3], q[3], graph_height, graph_width, x, y;
	double defaults[3] = {100., 1000., 5000.};
	gboolean enabled, lowcut;
	int band, type[3], slope, tick;
	char path[48];

	(void)area;
	graph_color(cr, .025, .035, .05, 1.);
	cairo_paint(cr);
	graph_width = MAX(1, width - 38);
	graph_height = MAX(1, height - 28);
	cairo_set_line_width(cr, 1.);
	graph_color(cr, .36, .43, .51, .22);
	for (tick = -20; tick <= 20; tick += 10) {
		y = 6. + (24. - tick) / 48. * graph_height;
		cairo_move_to(cr, 30., y);
		cairo_line_to(cr, 30. + graph_width, y);
	}
	for (tick = 0; tick < (int)G_N_ELEMENTS(grid_freq); ++tick) {
		x = 30. + log(grid_freq[tick] / 20.) / log(1000.) * graph_width;
		cairo_move_to(cr, x, 6.);
		cairo_line_to(cr, x, 6. + graph_height);
	}
	cairo_stroke(cr);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 10.5);
	graph_color(cr, .56, .64, .73, .85);
	for (tick = -20; tick <= 20; tick += 20) {
		char label[12];
		g_snprintf(label, sizeof label, "%+d", tick);
		y = 9. + (24. - tick) / 48. * graph_height;
		cairo_move_to(cr, 4., y);
		cairo_show_text(cr, label);
	}
	for (tick = 0; tick < (int)G_N_ELEMENTS(grid_freq); ++tick) {
		x = 30. + log(grid_freq[tick] / 20.) / log(1000.) * graph_width;
		cairo_move_to(cr, CLAMP(x - 8., 30., width - 24.), height - 5.);
		cairo_show_text(cr, grid_label[tick]);
	}
	for (band = 0; band < 3; ++band) {
		g_snprintf(path, sizeof path, "eq/band%dgain", band + 1);
		gain[band] = selected_parameter(self, path, 0.);
		g_snprintf(path, sizeof path, "eq/band%dfreq", band + 1);
		freq[band] = selected_parameter(self, path, defaults[band]);
		g_snprintf(path, sizeof path, "eq/band%dq", band + 1);
		q[band] = selected_parameter(self, path, 1.);
		type[band] = 0;
		if (band != 1) {
			g_snprintf(path, sizeof path, "eq/band%dtype", band + 1);
			type[band] = lround(selected_parameter(self, path, 0.));
		}
		graph_color(cr, colors[band][0], colors[band][1], colors[band][2], .62);
		cairo_set_line_width(cr, 1.15);
		draw_response_curve(cr, width, height, gain, freq, q, type, band,
		                    FALSE, 20., 0);
	}
	enabled = selected_parameter(self, "eq", 0.) != 0.;
	lowcut = selected_parameter(self, "lowcut", 0.) != 0.;
	slope = lround(selected_parameter(self, "lowcut/slope", 0.));
	graph_color(cr, .20, .78, .95, enabled || lowcut ? 1. : .38);
	cairo_set_line_width(cr, 2.2);
	draw_response_curve(cr, width, height, gain, freq, q, type,
	                    enabled ? -1 : -2,
	                    lowcut,
	                    selected_parameter(self, "lowcut/freq", 20.), slope);
	for (band = 0; band < 3; ++band) {
		char number[2] = {(char)('1' + band), '\0'};
		cairo_text_extents_t extents;

		graph_point_position(GTK_WIDGET(area), freq[band], gain[band], &x, &y);
		graph_color(cr, .02, .03, .045, 1.);
		cairo_new_path(cr);
		cairo_arc(cr, x, y,
		          self->graph_drag_band == band ? 9.5 :
		          self->graph_selected_band == band ? 9. : 7.5,
		          0., 2. * G_PI);
		cairo_fill_preserve(cr);
		graph_color(cr, colors[band][0], colors[band][1], colors[band][2], 1.);
		cairo_set_line_width(cr,
		                     self->graph_selected_band == band ? 3. : 2.);
		cairo_stroke(cr);
		cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
		                       CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 9.5);
		cairo_text_extents(cr, number, &extents);
		graph_color(cr, .88, .93, .98, 1.);
		cairo_move_to(cr, x - extents.width / 2. - extents.x_bearing,
		              y - extents.height / 2. - extents.y_bearing);
		cairo_show_text(cr, number);
		cairo_new_path(cr);
	}
}

static GtkWidget *
new_page(GtkWidget **content)
{
	GtkWidget *scroll, *box;

	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_widget_set_margin_top(box, 10);
	gtk_widget_set_margin_bottom(box, 10);
	gtk_widget_set_margin_start(box, 4);
	gtk_widget_set_margin_end(box, 8);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
	*content = box;
	return scroll;
}

static GtkWidget *
new_section(const char *title)
{
	GtkWidget *label;

	label = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(label), 0.f);
	gtk_widget_add_css_class(label, "section-title");
	gtk_widget_set_margin_top(label, 6);
	return label;
}

static void
binding_address(ParamBinding *binding, char *address, gsize size)
{
	DspPanel *self = binding->panel;

	if (binding->scope == PARAM_GLOBAL)
		g_strlcpy(address, binding->path, size);
	else if (binding->scope == PARAM_SELECTED_OUTPUT)
		g_snprintf(address, size, "/output/%d/%s",
		           self->room_target_output + 1,
		           binding->path);
	else
		g_snprintf(address, size, "/%s/%d/%s",
		           mix_channel_kind_osc_name(self->kind), self->channel + 1,
		           binding->path);
}

static gboolean
binding_is_available(ParamBinding *binding)
{
	return binding->scope != PARAM_SELECTED_CHANNEL ||
	       (binding->kind_mask & KIND_MASK(binding->panel->kind));
}

static gboolean
room_stereo_pair(DspPanel *self, int *left, int *right)
{
	MixRowSnapshot row;
	int first;

	first = self->selected_output & ~1;
	mix_model_snapshot_row(self->model, MIX_CHANNEL_OUTPUT, &row);
	if (first < 0 || first + 1 >= row.count ||
	    !row.channels[first].stereo || !row.channels[first + 1].stereo)
		return FALSE;
	if (left)
		*left = first;
	if (right)
		*right = first + 1;
	return TRUE;
}

static gboolean
room_link_peer(ParamBinding *binding, int *peer)
{
	DspPanel *self = binding->panel;

	if (binding->scope != PARAM_SELECTED_OUTPUT ||
	    !g_str_has_prefix(binding->path, "roomeq") ||
	    self->room_pair < 0 || self->room_peer < 0 ||
	    !self->room_links[self->room_pair])
		return FALSE;
	*peer = self->room_peer;
	return TRUE;
}

static void
set_binding_int(ParamBinding *binding, int value)
{
	DspPanel *self = binding->panel;
	char address[128];
	int peer;

	binding_address(binding, address, sizeof address);
	mix_model_set_parameter_int(self->model, address, value);
	osc_transport_send_int(self->transport, address, value);
	if (room_link_peer(binding, &peer)) {
		g_snprintf(address, sizeof address, "/output/%d/%s", peer + 1,
		           binding->path);
		mix_model_set_parameter_int(self->model, address, value);
		osc_transport_send_int(self->transport, address, value);
	}
}

static void
set_binding_float(ParamBinding *binding, double value)
{
	DspPanel *self = binding->panel;
	char address[128];
	int peer;

	binding_address(binding, address, sizeof address);
	mix_model_set_parameter_float(self->model, address, value);
	osc_transport_send_float(self->transport, address, value);
	if (room_link_peer(binding, &peer)) {
		g_snprintf(address, sizeof address, "/output/%d/%s", peer + 1,
		           binding->path);
		mix_model_set_parameter_float(self->model, address, value);
		osc_transport_send_float(self->transport, address, value);
	}
}

static void
on_switch_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	ParamBinding *binding = data;
	DspPanel *self = binding->panel;
	int value;

	(void)spec;
	if (self->updating || !binding_is_available(binding))
		return;
	value = gtk_switch_get_active(GTK_SWITCH(object));
	set_binding_int(binding, value);
	queue_dsp_graphs(self);
}

static void
on_scale_changed(GtkRange *range, gpointer data)
{
	ParamBinding *binding = data;
	DspPanel *self = binding->panel;
	double value;

	if (self->updating || !binding_is_available(binding))
		return;
	value = gtk_range_get_value(range);
	if (binding->integer)
		set_binding_int(binding, lround(value));
	else
		set_binding_float(binding, value);
	queue_dsp_graphs(self);
}

static void
on_knob_changed(GtkAdjustment *adjustment, gpointer data)
{
	ParamBinding *binding = data;
	DspPanel *self = binding->panel;
	double value;

	if (self->updating || !binding_is_available(binding))
		return;
	value = gtk_adjustment_get_value(adjustment);
	if (binding->integer)
		set_binding_int(binding, lround(value));
	else
		set_binding_float(binding, value);
	queue_dsp_graphs(self);
}

static void
on_dropdown_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	ParamBinding *binding = data;
	DspPanel *self = binding->panel;
	guint value;

	(void)spec;
	if (self->updating || !binding_is_available(binding))
		return;
	value = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
	if (value == GTK_INVALID_LIST_POSITION)
		return;
	set_binding_int(binding, value);
	queue_dsp_graphs(self);
}

static gboolean
on_eq_type_scroll(GtkEventControllerScroll *controller, double dx, double dy,
	gpointer data)
{
	GtkDropDown *dropdown = data;
	GListModel *model;
	double delta;
	guint count, selected;
	int next;

	(void)controller;
	delta = fabs(dx) > fabs(dy) ? dx : dy;
	if (delta == 0.)
		return TRUE;
	model = gtk_drop_down_get_model(dropdown);
	count = model ? g_list_model_get_n_items(model) : 0;
	selected = gtk_drop_down_get_selected(dropdown);
	if (count == 0 || selected == GTK_INVALID_LIST_POSITION)
		return TRUE;
	next = CLAMP((int)selected + (delta > 0. ? 1 : -1), 0, (int)count - 1);
	if ((guint)next != selected)
		gtk_drop_down_set_selected(dropdown, next);
	return TRUE;
}

static ParamBinding *
register_binding(DspPanel *self, GtkWidget *control, GtkWidget *row,
	const char *path, ParamScope scope, ParamWidgetType type, guint kind_mask)
{
	ParamBinding *binding;

	binding = g_new0(ParamBinding, 1);
	binding->panel = self;
	binding->widget = control;
	binding->row = row;
	binding->path = g_strdup(path);
	binding->scope = scope;
	binding->type = type;
	binding->kind_mask = kind_mask;
	g_ptr_array_add(self->bindings, binding);
	return binding;
}

static ParamBinding *
add_binding(DspPanel *self, GtkWidget *box, const char *label,
	const char *path, ParamScope scope, ParamWidgetType type,
	guint kind_mask, GtkWidget *control)
{
	GtkWidget *row, *text;

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	text = gtk_label_new(label);
	gtk_label_set_xalign(GTK_LABEL(text), 0.f);
	gtk_widget_set_hexpand(text, TRUE);
	gtk_box_append(GTK_BOX(row), text);
	gtk_widget_set_size_request(control, type == PARAM_SCALE ? 145 : -1, -1);
	gtk_box_append(GTK_BOX(row), control);
	gtk_box_append(GTK_BOX(box), row);
	return register_binding(self, control, row, path, scope, type, kind_mask);
}

static void
add_switch(DspPanel *self, GtkWidget *box, const char *label,
	const char *path, ParamScope scope, guint kind_mask)
{
	GtkWidget *control = gtk_switch_new();
	ParamBinding *binding = add_binding(self, box, label, path, scope,
	                                    PARAM_SWITCH, kind_mask, control);
	g_signal_connect(control, "notify::active", G_CALLBACK(on_switch_changed), binding);
}

static void
add_scale(DspPanel *self, GtkWidget *box, const char *label,
	const char *path, ParamScope scope, guint kind_mask,
	double lower, double upper, double step, gboolean integer)
{
	GtkWidget *control = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
	                                             lower, upper, step);
	ParamBinding *binding;

	gtk_scale_set_draw_value(GTK_SCALE(control), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(control), GTK_POS_RIGHT);
	binding = add_binding(self, box, label, path, scope, PARAM_SCALE,
	                      kind_mask, control);
	binding->integer = integer;
	g_signal_connect(control, "value-changed", G_CALLBACK(on_scale_changed), binding);
}

static void
add_dropdown(DspPanel *self, GtkWidget *box, const char *label,
	const char *path, ParamScope scope, guint kind_mask,
	const char *const *items, guint n_items)
{
	GtkStringList *strings = gtk_string_list_new(NULL);
	GtkWidget *control;
	ParamBinding *binding;
	guint i;

	for (i = 0; i < n_items; ++i)
		gtk_string_list_append(strings, items[i]);
	control = gtk_drop_down_new(G_LIST_MODEL(strings), NULL);
	binding = add_binding(self, box, label, path, scope, PARAM_DROPDOWN,
	                      kind_mask, control);
	g_signal_connect(control, "notify::selected", G_CALLBACK(on_dropdown_changed), binding);
}

static GtkWidget *
new_bound_switch(DspPanel *self, const char *path)
{
	GtkWidget *control;
	ParamBinding *binding;

	control = gtk_switch_new();
	binding = register_binding(self, control, control, path,
	                           PARAM_SELECTED_CHANNEL, PARAM_SWITCH, MASK_IO);
	g_signal_connect(control, "notify::active",
	                 G_CALLBACK(on_switch_changed), binding);
	return control;
}

static GtkWidget *
new_bound_dropdown(DspPanel *self,
	const char *path, const char *const *items, guint n_items)
{
	GtkStringList *strings = gtk_string_list_new(NULL);
	GtkWidget *control;
	GtkEventController *scroll;
	ParamBinding *binding;
	guint i;

	for (i = 0; i < n_items; ++i)
		gtk_string_list_append(strings, items[i]);
	control = gtk_drop_down_new(G_LIST_MODEL(strings), NULL);
	gtk_widget_set_hexpand(control, TRUE);
	binding = register_binding(self, control, control, path,
	                           PARAM_SELECTED_CHANNEL, PARAM_DROPDOWN, MASK_IO);
	g_signal_connect(control, "notify::selected",
	                 G_CALLBACK(on_dropdown_changed), binding);
	scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_eq_type_scroll), control);
	gtk_widget_add_controller(control, scroll);
	return control;
}

static GtkWidget *
new_bound_knob(DspPanel *self,
	const char *path, double lower, double upper, double step,
	double reset_value, gboolean logarithmic, int digits, const char *unit,
	double red, double green, double blue, gboolean integer)
{
	GtkWidget *control;
	KnobData *knob;
	ParamBinding *binding;

	control = new_knob(lower, upper, step, reset_value, logarithmic, digits,
	                   unit, red, green, blue);
	if (self->expanded_controls)
		gtk_widget_set_size_request(control, 58, 58);
	gtk_widget_set_halign(control, GTK_ALIGN_CENTER);
	binding = register_binding(self, control, control, path,
	                           PARAM_SELECTED_CHANNEL, PARAM_KNOB, MASK_IO);
	binding->integer = integer;
	knob = g_object_get_data(G_OBJECT(control), "oscmix-knob");
	g_signal_connect(knob->adjustment, "value-changed",
	                 G_CALLBACK(on_knob_changed), binding);
	return control;
}

static GtkWidget *
new_bound_output_dropdown(DspPanel *self,
	const char *path, const char *const *items, guint n_items)
{
	GtkStringList *strings = gtk_string_list_new(NULL);
	GtkWidget *control;
	GtkEventController *scroll;
	ParamBinding *binding;
	guint i;

	for (i = 0; i < n_items; ++i)
		gtk_string_list_append(strings, items[i]);
	control = gtk_drop_down_new(G_LIST_MODEL(strings), NULL);
	gtk_widget_set_hexpand(control, TRUE);
	binding = register_binding(self, control, control, path,
	                           PARAM_SELECTED_OUTPUT, PARAM_DROPDOWN, MASK_ALL);
	g_signal_connect(control, "notify::selected",
	                 G_CALLBACK(on_dropdown_changed), binding);
	scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_eq_type_scroll), control);
	gtk_widget_add_controller(control, scroll);
	return control;
}

static GtkWidget *
new_bound_output_knob(DspPanel *self,
	const char *path, double lower, double upper, double step,
	double reset_value, gboolean logarithmic, int digits, const char *unit,
	double red, double green, double blue, gboolean integer)
{
	GtkWidget *control;
	KnobData *knob;
	ParamBinding *binding;

	control = new_knob(lower, upper, step, reset_value, logarithmic, digits,
	                   unit, red, green, blue);
	gtk_widget_set_halign(control, GTK_ALIGN_CENTER);
	binding = register_binding(self, control, control, path,
	                           PARAM_SELECTED_OUTPUT, PARAM_KNOB, MASK_ALL);
	binding->integer = integer;
	knob = g_object_get_data(G_OBJECT(control), "oscmix-knob");
	g_signal_connect(knob->adjustment, "value-changed",
	                 G_CALLBACK(on_knob_changed), binding);
	return control;
}

static GtkWidget *
new_labeled_knob(DspPanel *self, const char *label, const char *path,
	double lower, double upper, double step, double reset_value,
	gboolean logarithmic, int digits, const char *unit,
	double red, double green, double blue, gboolean integer)
{
	GtkWidget *box, *text, *control;

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	text = gtk_label_new(label);
	gtk_widget_add_css_class(text, "eq-param-label");
	gtk_box_append(GTK_BOX(box), text);
	control = new_bound_knob(self, path, lower, upper, step, reset_value,
	                         logarithmic, digits, unit,
	                         red, green, blue, integer);
	if (self->compact_controls)
		gtk_widget_set_size_request(control, 28, 28);
	gtk_box_append(GTK_BOX(box), control);
	return box;
}

static GtkWidget *
new_captioned_knob(DspPanel *self, const char *label, const char *path,
	double lower, double upper, double step, double reset_value,
	gboolean logarithmic, int digits, const char *unit,
	double red, double green, double blue, gboolean integer)
{
	GtkWidget *box, *text, *control;

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	control = new_bound_knob(self, path, lower, upper, step, reset_value,
	                         logarithmic, digits, unit,
	                         red, green, blue, integer);
	if (self->compact_controls)
		gtk_widget_set_size_request(control, 28, 28);
	else if (!self->expanded_controls)
		gtk_widget_set_size_request(control, 34, 34);
	gtk_box_append(GTK_BOX(box), control);
	text = gtk_label_new(label);
	gtk_widget_add_css_class(text, "dynamics-param-label");
	gtk_box_append(GTK_BOX(box), text);
	return box;
}

static GtkWidget *
new_output_captioned_knob(DspPanel *self, const char *label, const char *path,
	double lower, double upper, double step, double reset_value,
	gboolean logarithmic, int digits, const char *unit,
	double red, double green, double blue, gboolean integer)
{
	GtkWidget *box, *text, *control;

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	control = new_bound_output_knob(self, path, lower, upper, step, reset_value,
	                                 logarithmic, digits, unit,
	                                 red, green, blue, integer);
	gtk_widget_set_size_request(control,
	                            self->expanded_controls ? 46 : 30,
	                            self->expanded_controls ? 46 : 30);
	gtk_box_append(GTK_BOX(box), control);
	text = gtk_label_new(label);
	gtk_widget_add_css_class(text, "dynamics-param-label");
	gtk_box_append(GTK_BOX(box), text);
	return box;
}

static void
build_eq_page(DspPanel *self, DspPageMode mode)
{
	static const char *const band1_types[] = {
		"Bell", "Low Shelf", "High Pass", "Low Pass"
	};
	static const char *const band3_types[] = {
		"Bell", "High Shelf", "Low Pass", "High Pass"
	};
	static const char *const compact_band1_types[] = {
		"Bell", "L Shelf", "HP", "LP"
	};
	static const char *const compact_band3_types[] = {
		"Bell", "H Shelf", "LP", "HP"
	};
	static const double colors[3][3] = {
		{.94, .25, .20}, {.25, .86, .35}, {.18, .64, .96}
	};
	GtkWidget *page, *header, *label, *control, *separator, *bands, *graph_row;
	GtkWidget *band_box, *band_header, *band_controls;
	GtkWidget *lowcut_header, *lowcut_grid;
	GtkGesture *click, *drag;
	GtkEventController *scroll;
	ParamBinding *binding;
	char path[48], text[16];
	int band;

	page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
	gtk_widget_set_margin_top(page, mode == DSP_PAGE_WINDOW ? 10 : 2);
	gtk_widget_set_margin_bottom(page, mode == DSP_PAGE_WINDOW ? 10 : 2);
	gtk_widget_set_margin_start(page, mode == DSP_PAGE_WINDOW ? 12 :
	                                 mode == DSP_PAGE_INLINE ? 2 : 5);
	gtk_widget_set_margin_end(page, mode == DSP_PAGE_WINDOW ? 12 :
	                               mode == DSP_PAGE_INLINE ? 2 : 5);
	gtk_widget_add_css_class(page, "channel-eq-panel");
	if (mode == DSP_PAGE_INLINE)
		gtk_widget_add_css_class(page, "compact-dsp-panel");
	if (mode == DSP_PAGE_WINDOW)
		gtk_widget_add_css_class(page, "expanded-eq-panel");
	self->eq_page = page;
	enable_page_double_click(self, page, mode);
	header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	self->eq_title = GTK_LABEL(gtk_label_new("Channel"));
	gtk_label_set_xalign(self->eq_title, 0.f);
	gtk_label_set_ellipsize(self->eq_title, PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(GTK_WIDGET(self->eq_title), TRUE);
	gtk_widget_add_css_class(GTK_WIDGET(self->eq_title), "eq-channel-title");
	gtk_box_append(GTK_BOX(header), GTK_WIDGET(self->eq_title));
	if (mode == DSP_PAGE_INSPECTOR) {
		self->eq_window_button = GTK_BUTTON(
			gtk_button_new_from_icon_name("window-new-symbolic"));
		gtk_widget_add_css_class(GTK_WIDGET(self->eq_window_button), "flat");
		gtk_widget_add_css_class(GTK_WIDGET(self->eq_window_button),
		                         "dsp-window-button");
		gtk_widget_set_tooltip_text(GTK_WIDGET(self->eq_window_button),
		                            "Show or hide the resizable EQ window");
		gtk_box_append(GTK_BOX(header), GTK_WIDGET(self->eq_window_button));
	}
	label = gtk_label_new("EQ");
	gtk_widget_add_css_class(label, "eq-master-label");
	gtk_box_append(GTK_BOX(header), label);
	control = gtk_switch_new();
	gtk_box_append(GTK_BOX(header), control);
	binding = register_binding(self, control, control, "eq",
	                           PARAM_SELECTED_CHANNEL, PARAM_SWITCH, MASK_IO);
	g_signal_connect(control, "notify::active",
	                 G_CALLBACK(on_switch_changed), binding);
	gtk_box_append(GTK_BOX(page), header);
	self->eq_graph = gtk_drawing_area_new();
	gtk_widget_set_size_request(self->eq_graph, -1,
	                            mode == DSP_PAGE_WINDOW ? 230 :
	                            mode == DSP_PAGE_INLINE ? 100 : 125);
	gtk_widget_set_vexpand(self->eq_graph, mode == DSP_PAGE_WINDOW);
	gtk_widget_set_hexpand(self->eq_graph, TRUE);
	gtk_widget_set_cursor_from_name(self->eq_graph, "crosshair");
	gtk_widget_add_css_class(self->eq_graph, "eq-graph");
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->eq_graph),
	                               draw_eq_graph, self, NULL);
	click = GTK_GESTURE(gtk_gesture_click_new());
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
	g_signal_connect(click, "pressed", G_CALLBACK(on_graph_pressed), self);
	g_signal_connect(click, "released", G_CALLBACK(on_graph_released), self);
	drag = GTK_GESTURE(gtk_gesture_drag_new());
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_graph_drag_begin), self);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_graph_drag_update), self);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_graph_drag_end), self);
	gtk_gesture_group(click, drag);
	gtk_widget_add_controller(self->eq_graph, GTK_EVENT_CONTROLLER(click));
	gtk_widget_add_controller(self->eq_graph, GTK_EVENT_CONTROLLER(drag));
	scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
		GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_graph_scroll), self);
	gtk_widget_add_controller(self->eq_graph, scroll);
	graph_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_vexpand(graph_row, mode == DSP_PAGE_WINDOW);
	gtk_box_append(GTK_BOX(graph_row), self->eq_graph);
	self->eq_meter = gtk_drawing_area_new();
	gtk_widget_set_size_request(self->eq_meter,
	                            mode == DSP_PAGE_INLINE ? 38 : 44, -1);
	gtk_widget_set_vexpand(self->eq_meter, TRUE);
	gtk_widget_set_tooltip_text(self->eq_meter,
	                            "Input level (dBFS, 0 to -60 dB)");
	gtk_widget_add_css_class(self->eq_meter, "dsp-live-meter");
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->eq_meter),
	                               draw_live_meter, self, NULL);
	gtk_box_append(GTK_BOX(graph_row), self->eq_meter);
	gtk_box_append(GTK_BOX(page), graph_row);
	bands = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
	                    mode == DSP_PAGE_WINDOW ? 12 : 3);
	gtk_box_set_homogeneous(GTK_BOX(bands), TRUE);
	gtk_widget_set_hexpand(bands, TRUE);
	for (band = 1; band <= 3; ++band) {
		band_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
		gtk_widget_set_hexpand(band_box, TRUE);
		gtk_widget_add_css_class(band_box, "eq-band-box");
		band_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
		                          mode == DSP_PAGE_INLINE ? 2 : 4);
		g_snprintf(text, sizeof text, "%d", band);
		label = gtk_label_new(text);
		gtk_widget_add_css_class(label, band == 1 ? "eq-band-1" :
		                                   band == 2 ? "eq-band-2" : "eq-band-3");
		gtk_widget_add_css_class(label, "eq-band-label");
		gtk_box_append(GTK_BOX(band_header), label);
		if (band == 1)
			control = new_bound_dropdown(self, "eq/band1type",
			                             mode == DSP_PAGE_INLINE ?
			                               compact_band1_types : band1_types,
			                             G_N_ELEMENTS(band1_types));
		else if (band == 3)
			control = new_bound_dropdown(self, "eq/band3type",
			                             mode == DSP_PAGE_INLINE ?
			                               compact_band3_types : band3_types,
			                             G_N_ELEMENTS(band3_types));
		else {
			control = gtk_label_new("Bell");
			gtk_widget_add_css_class(control, "eq-fixed-type");
			gtk_widget_set_hexpand(control, TRUE);
		}
		gtk_box_append(GTK_BOX(band_header), control);
		gtk_box_append(GTK_BOX(band_box), band_header);
		band_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
		                            mode == DSP_PAGE_WINDOW ? 8 : 1);
		gtk_box_set_homogeneous(GTK_BOX(band_controls), TRUE);
		g_snprintf(path, sizeof path, "eq/band%dgain", band);
		gtk_box_append(GTK_BOX(band_controls),
		               new_labeled_knob(self, "Gain", path,
		                 -20, 20, .1, 0, FALSE, 1, "",
		                 colors[band - 1][0], colors[band - 1][1],
		                 colors[band - 1][2], FALSE));
		g_snprintf(path, sizeof path, "eq/band%dfreq", band);
		gtk_box_append(GTK_BOX(band_controls),
		               new_labeled_knob(self, "Freq", path,
		                 20, 20000, 1,
		                 band == 1 ? 100 : band == 2 ? 1000 : 5000,
		                 TRUE, 0, "", colors[band - 1][0],
		                 colors[band - 1][1], colors[band - 1][2], TRUE));
		g_snprintf(path, sizeof path, "eq/band%dq", band);
		gtk_box_append(GTK_BOX(band_controls),
		               new_labeled_knob(self, "Q", path,
		                 .4, 9.9, .1, 1, FALSE, 1, "",
		                 colors[band - 1][0], colors[band - 1][1],
		                 colors[band - 1][2], FALSE));
		gtk_box_append(GTK_BOX(band_box), band_controls);
		gtk_box_append(GTK_BOX(bands), band_box);
	}
	gtk_box_append(GTK_BOX(page), bands);
	separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_append(GTK_BOX(page), separator);
	lowcut_grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(lowcut_grid),
	                            mode == DSP_PAGE_WINDOW ? 18 : 8);
	gtk_widget_set_hexpand(lowcut_grid, TRUE);
	gtk_widget_set_valign(lowcut_grid, GTK_ALIGN_CENTER);
	lowcut_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	label = gtk_label_new("Low Cut");
	gtk_label_set_xalign(GTK_LABEL(label), 0.f);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_widget_add_css_class(label, "eq-lowcut-title");
	gtk_box_append(GTK_BOX(lowcut_header), label);
	control = gtk_switch_new();
	gtk_box_append(GTK_BOX(lowcut_header), control);
	binding = register_binding(self, control, control, "lowcut",
	                           PARAM_SELECTED_CHANNEL, PARAM_SWITCH, MASK_IO);
	g_signal_connect(control, "notify::active",
	                 G_CALLBACK(on_switch_changed), binding);
	gtk_grid_attach(GTK_GRID(lowcut_grid), lowcut_header, 0, 0, 1, 2);
	control = new_bound_knob(self, "lowcut/slope", 0, 3, 1, 1,
	                         FALSE, 0, "oct", .94, .52, .28, TRUE);
	gtk_grid_attach(GTK_GRID(lowcut_grid), control, 1, 0, 1, 1);
	label = gtk_label_new("dB/oct");
	gtk_widget_add_css_class(label, "eq-row-label");
	gtk_grid_attach(GTK_GRID(lowcut_grid), label, 1, 1, 1, 1);
	control = new_bound_knob(self, "lowcut/freq", 20, 500, 1, 100,
	                         TRUE, 0, "", .94, .52, .28, TRUE);
	gtk_grid_attach(GTK_GRID(lowcut_grid), control, 2, 0, 1, 1);
	label = gtk_label_new("freq Hz");
	gtk_widget_add_css_class(label, "eq-row-label");
	gtk_grid_attach(GTK_GRID(lowcut_grid), label, 2, 1, 1, 1);
	gtk_box_append(GTK_BOX(page), lowcut_grid);
	gtk_stack_add_titled(self->stack, page, "eq", "EQ");
}

static void
build_dynamics_page(DspPanel *self, DspPageMode mode)
{
	static const double common_color[] = {.55, .72, .82};
	static const double compressor_color[] = {.95, .22, .27};
	static const double expander_color[] = {.20, .86, .43};
	static const double autolevel_color[] = {.55, .72, .82};
	GtkWidget *page, *header, *label, *control, *separator, *graph_row;
	GtkWidget *common_controls, *dynamics_groups;
	GtkWidget *compressor, *compressor_controls;
	GtkWidget *expander, *expander_controls;
	GtkWidget *autolevel_header, *autolevel_controls;

	page = gtk_box_new(GTK_ORIENTATION_VERTICAL,
	                   mode == DSP_PAGE_WINDOW ? 15 : 1);
	gtk_widget_set_margin_top(page, mode == DSP_PAGE_WINDOW ? 12 : 2);
	gtk_widget_set_margin_bottom(page, mode == DSP_PAGE_WINDOW ? 12 : 2);
	gtk_widget_set_margin_start(page, mode == DSP_PAGE_WINDOW ? 14 :
	                                 mode == DSP_PAGE_INLINE ? 2 : 4);
	gtk_widget_set_margin_end(page, mode == DSP_PAGE_WINDOW ? 14 :
	                               mode == DSP_PAGE_INLINE ? 2 : 4);
	gtk_widget_add_css_class(page, "channel-dynamics-panel");
	if (mode == DSP_PAGE_INLINE)
		gtk_widget_add_css_class(page, "compact-dsp-panel");
	if (mode == DSP_PAGE_WINDOW)
		gtk_widget_add_css_class(page, "expanded-dynamics-panel");
	self->dynamics_page = page;
	enable_page_double_click(self, page, mode);

	header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	control = new_bound_switch(self, "dynamics");
	gtk_widget_set_margin_start(control, 5);
	gtk_box_append(GTK_BOX(header), control);
	label = gtk_label_new("Dynamics");
	gtk_widget_add_css_class(label, "dynamics-master-label");
	gtk_box_append(GTK_BOX(header), label);
	self->dynamics_title = GTK_LABEL(gtk_label_new("Channel"));
	gtk_label_set_xalign(self->dynamics_title, 1.f);
	gtk_label_set_ellipsize(self->dynamics_title, PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(GTK_WIDGET(self->dynamics_title), TRUE);
	gtk_widget_add_css_class(GTK_WIDGET(self->dynamics_title),
	                         "dynamics-channel-title");
	gtk_box_append(GTK_BOX(header), GTK_WIDGET(self->dynamics_title));
	if (mode == DSP_PAGE_INSPECTOR) {
		self->dynamics_window_button = GTK_BUTTON(
			gtk_button_new_from_icon_name("window-new-symbolic"));
		gtk_widget_add_css_class(GTK_WIDGET(self->dynamics_window_button),
		                         "flat");
		gtk_widget_add_css_class(GTK_WIDGET(self->dynamics_window_button),
		                         "dsp-window-button");
		gtk_widget_set_tooltip_text(
			GTK_WIDGET(self->dynamics_window_button),
			"Show or hide the resizable Dynamics window");
		gtk_box_append(GTK_BOX(header),
		               GTK_WIDGET(self->dynamics_window_button));
	}
	gtk_box_append(GTK_BOX(page), header);

	self->dynamics_graph = gtk_drawing_area_new();
	gtk_widget_set_size_request(self->dynamics_graph, -1,
	                            mode == DSP_PAGE_WINDOW ? 190 :
	                            mode == DSP_PAGE_INSPECTOR ? 132 : 80);
	gtk_widget_set_vexpand(self->dynamics_graph, FALSE);
	gtk_widget_set_hexpand(self->dynamics_graph, TRUE);
	gtk_widget_add_css_class(self->dynamics_graph, "dynamics-graph");
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->dynamics_graph),
	                               draw_dynamics_graph, self, NULL);
	graph_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_append(GTK_BOX(graph_row), self->dynamics_graph);
	self->dynamics_meter = gtk_drawing_area_new();
	gtk_widget_set_size_request(self->dynamics_meter,
	                            mode == DSP_PAGE_INLINE ? 68 : 84, -1);
	gtk_widget_set_vexpand(self->dynamics_meter, TRUE);
	gtk_widget_set_tooltip_text(
		self->dynamics_meter,
		"Input level (0 to -60 dBFS) and gain reduction (0 to -20 dB)");
	gtk_widget_add_css_class(self->dynamics_meter, "dsp-live-meter");
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->dynamics_meter),
	                               draw_live_meter, self, NULL);
	gtk_box_append(GTK_BOX(graph_row), self->dynamics_meter);
	gtk_box_append(GTK_BOX(page), graph_row);

	common_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
	                             mode == DSP_PAGE_WINDOW ? 18 : 2);
	gtk_box_set_homogeneous(GTK_BOX(common_controls), TRUE);
	gtk_box_append(GTK_BOX(common_controls),
	               new_captioned_knob(self, "Gain", "dynamics/gain",
	                 -30, 30, .1, 0, FALSE, 1, "dB",
	                 common_color[0], common_color[1], common_color[2], FALSE));
	gtk_box_append(GTK_BOX(common_controls),
	               new_captioned_knob(self, "Attack", "dynamics/attack",
	                 0, 200, 1, 0, FALSE, 0, "ms",
	                 common_color[0], common_color[1], common_color[2], TRUE));
	gtk_box_append(GTK_BOX(common_controls),
	               new_captioned_knob(self, "Release", "dynamics/release",
	                 100, 999, 1, 100, FALSE, 0, "ms",
	                 common_color[0], common_color[1], common_color[2], TRUE));
	gtk_box_append(GTK_BOX(page), common_controls);

	dynamics_groups = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
	                             mode == DSP_PAGE_WINDOW ? 20 : 5);
	gtk_box_set_homogeneous(GTK_BOX(dynamics_groups), TRUE);
	compressor = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(compressor, "dynamics-group");
	label = gtk_label_new("Compressor");
	gtk_widget_add_css_class(label, "dynamics-group-title");
	gtk_widget_add_css_class(label, "dynamics-compressor-title");
	gtk_box_append(GTK_BOX(compressor), label);
	compressor_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
	                                  mode == DSP_PAGE_WINDOW ? 12 : 1);
	gtk_box_set_homogeneous(GTK_BOX(compressor_controls), TRUE);
	gtk_box_append(GTK_BOX(compressor_controls),
	               new_captioned_knob(self, "Threshold", "dynamics/compthres",
	                 -60, 0, .1, 0, FALSE, 1, "dB",
	                 compressor_color[0], compressor_color[1],
	                 compressor_color[2], FALSE));
	gtk_box_append(GTK_BOX(compressor_controls),
	               new_captioned_knob(self, "Ratio", "dynamics/compratio",
	                 1, 10, .1, 1, FALSE, 1, ":1",
	                 compressor_color[0], compressor_color[1],
	                 compressor_color[2], FALSE));
	gtk_box_append(GTK_BOX(compressor), compressor_controls);
	gtk_box_append(GTK_BOX(dynamics_groups), compressor);

	expander = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(expander, "dynamics-group");
	label = gtk_label_new("Expander");
	gtk_widget_add_css_class(label, "dynamics-group-title");
	gtk_widget_add_css_class(label, "dynamics-expander-title");
	gtk_box_append(GTK_BOX(expander), label);
	expander_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
	                                mode == DSP_PAGE_WINDOW ? 12 : 1);
	gtk_box_set_homogeneous(GTK_BOX(expander_controls), TRUE);
	gtk_box_append(GTK_BOX(expander_controls),
	               new_captioned_knob(self, "Threshold", "dynamics/expthres",
	                 -99, 20, .1, -99, FALSE, 1, "dB",
	                 expander_color[0], expander_color[1], expander_color[2],
	                 FALSE));
	gtk_box_append(GTK_BOX(expander_controls),
	               new_captioned_knob(self, "Ratio", "dynamics/expratio",
	                 1, 10, .1, 1, FALSE, 1, ":1",
	                 expander_color[0], expander_color[1], expander_color[2],
	                 FALSE));
	gtk_box_append(GTK_BOX(expander), expander_controls);
	gtk_box_append(GTK_BOX(dynamics_groups), expander);
	gtk_box_append(GTK_BOX(page), dynamics_groups);

	separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_append(GTK_BOX(page), separator);
	autolevel_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	control = new_bound_switch(self, "autolevel");
	gtk_widget_set_margin_start(control, 5);
	gtk_box_append(GTK_BOX(autolevel_header), control);
	label = gtk_label_new("Auto Level");
	gtk_label_set_xalign(GTK_LABEL(label), 0.f);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_widget_add_css_class(label, "dynamics-group-title");
	gtk_box_append(GTK_BOX(autolevel_header), label);
	gtk_box_append(GTK_BOX(page), autolevel_header);
	autolevel_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
	                                 mode == DSP_PAGE_WINDOW ? 18 : 2);
	gtk_box_set_homogeneous(GTK_BOX(autolevel_controls), TRUE);
	gtk_box_append(GTK_BOX(autolevel_controls),
	               new_captioned_knob(self, "Max Gain", "autolevel/maxgain",
	                 0, 18, .1, 0, FALSE, 1, "dB",
	                 autolevel_color[0], autolevel_color[1], autolevel_color[2],
	                 FALSE));
	gtk_box_append(GTK_BOX(autolevel_controls),
	               new_captioned_knob(self, "Headroom", "autolevel/headroom",
	                 3, 12, .1, 3, FALSE, 1, "dB",
	                 autolevel_color[0], autolevel_color[1], autolevel_color[2],
	                 FALSE));
	gtk_box_append(GTK_BOX(autolevel_controls),
	               new_captioned_knob(self, "Rise Time", "autolevel/risetime",
	                 .1, 9.9, .1, .1, FALSE, 1, "s",
	                 autolevel_color[0], autolevel_color[1], autolevel_color[2],
	                 FALSE));
	gtk_box_append(GTK_BOX(page), autolevel_controls);
	gtk_stack_add_titled(self->stack, page, "dynamics", "Dyn");
}

static void
build_fx_page(DspPanel *self)
{
	static const char *const reverb_types[] = {
		"Small Room", "Medium Room", "Large Room", "Walls", "Shorty",
		"Attack", "Swagger", "Old School", "Echoistic", "8plus9",
		"Grand Wide", "Thicker", "Envelope", "Gated", "Space"
	};
	static const char *const echo_types[] = {"Stereo Echo", "Stereo Cross", "Pong Echo"};
	GtkWidget *page, *box;

	page = new_page(&box);
	add_scale(self, box, "FX Send / Return", "fx", PARAM_SELECTED_CHANNEL,
	          MASK_IO, -65, 0, .1, FALSE);
	gtk_box_append(GTK_BOX(box), new_section("Reverb"));
	add_switch(self, box, "Enabled", "/reverb", PARAM_GLOBAL, MASK_ALL);
	add_dropdown(self, box, "Type", "/reverb/type", PARAM_GLOBAL, MASK_ALL,
	             reverb_types, G_N_ELEMENTS(reverb_types));
	add_scale(self, box, "Pre-delay", "/reverb/predelay", PARAM_GLOBAL,
	          MASK_ALL, 0, 500, 1, TRUE);
	add_scale(self, box, "Time", "/reverb/time", PARAM_GLOBAL,
	          MASK_ALL, .1, 10, .1, FALSE);
	add_scale(self, box, "Volume", "/reverb/volume", PARAM_GLOBAL,
	          MASK_ALL, -65, 6, .1, FALSE);
	add_scale(self, box, "Width", "/reverb/width", PARAM_GLOBAL,
	          MASK_ALL, 0, 1, .01, FALSE);
	gtk_box_append(GTK_BOX(box), new_section("Echo"));
	add_switch(self, box, "Enabled", "/echo", PARAM_GLOBAL, MASK_ALL);
	add_dropdown(self, box, "Type", "/echo/type", PARAM_GLOBAL, MASK_ALL,
	             echo_types, G_N_ELEMENTS(echo_types));
	add_scale(self, box, "Delay", "/echo/delay", PARAM_GLOBAL,
	          MASK_ALL, 0, 2, .001, FALSE);
	add_scale(self, box, "Feedback", "/echo/feedback", PARAM_GLOBAL,
	          MASK_ALL, 0, 100, 1, TRUE);
	add_scale(self, box, "Volume", "/echo/volume", PARAM_GLOBAL,
	          MASK_ALL, -65, 6, .1, FALSE);
	gtk_stack_add_titled(self->stack, page, "fx", "FX");
}

static const double ROOM_EQ_COLORS[9][3] = {
	{.94, .25, .20}, {.95, .52, .16}, {.86, .76, .20},
	{.35, .84, .30}, {.18, .76, .66}, {.18, .57, .94},
	{.42, .39, .94}, {.69, .35, .93}, {.93, .30, .66}
};

static double room_parameter(DspPanel *self, const char *path, double fallback);
static void set_room_parameter(DspPanel *self, const char *path,
	double value, gboolean integer);

static double
room_default_frequency(int band)
{
	return band == 0 ? 100. : band == 8 ? 10000. : 1000.;
}

static int
room_graph_band_at(DspPanel *self, GtkWidget *graph, double x, double y)
{
	double best_distance = 16., distance, point_x, point_y;
	double frequency, gain;
	int output = self->room_target_output >= 0
	           ? self->room_target_output : self->selected_output;
	char path[48];
	int band, nearest = -1;

	for (band = 0; band < 9; ++band) {
		if (!self->room_band_active[output][band])
			continue;
		g_snprintf(path, sizeof path, "roomeq/band%dgain", band + 1);
		gain = room_parameter(self, path, 0.);
		g_snprintf(path, sizeof path, "roomeq/band%dfreq", band + 1);
		frequency = room_parameter(self, path, room_default_frequency(band));
		graph_point_position(graph, frequency, gain, &point_x, &point_y);
		distance = hypot(x - point_x, y - point_y);
		if (distance <= best_distance) {
			best_distance = distance;
			nearest = band;
		}
	}
	return nearest;
}

static void
room_graph_begin_band_drag(DspPanel *self, GtkWidget *graph, int band)
{
	char path[48];

	self->graph_drag_band = band;
	self->graph_selected_band = band;
	g_snprintf(path, sizeof path, "roomeq/band%dfreq", band + 1);
	self->graph_drag_frequency =
		room_parameter(self, path, room_default_frequency(band));
	g_snprintf(path, sizeof path, "roomeq/band%dgain", band + 1);
	self->graph_drag_gain = room_parameter(self, path, 0.);
	g_snprintf(path, sizeof path, "roomeq/band%dq", band + 1);
	self->graph_drag_q = room_parameter(self, path, 1.);
	self->graph_drag_last_x = 0.;
	self->graph_drag_last_y = 0.;
	gtk_widget_set_cursor_from_name(graph, "grabbing");
	gtk_widget_queue_draw(graph);
}

static void
on_room_graph_pressed(GtkGestureClick *gesture, int n_press,
	double x, double y, gpointer data)
{
	DspPanel *self = data;
	GtkWidget *graph;
	int band;

	(void)n_press;
	graph = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	band = room_graph_band_at(self, graph, x, y);
	if (band >= 0)
		room_graph_begin_band_drag(self, graph, band);
	else {
		self->graph_drag_band = -1;
		self->graph_selected_band = -1;
		gtk_widget_queue_draw(graph);
	}
}

static void
on_room_graph_released(GtkGestureClick *gesture, int n_press,
	double x, double y, gpointer data)
{
	DspPanel *self = data;
	GtkWidget *graph;
	char path[48];

	(void)x;
	(void)y;
	graph = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	if (n_press == 2 && self->graph_selected_band >= 0) {
		g_snprintf(path, sizeof path, "roomeq/band%dgain",
		           self->graph_selected_band + 1);
		set_room_parameter(self, path, 0., FALSE);
	}
	self->graph_drag_band = -1;
	gtk_widget_set_cursor_from_name(graph, "crosshair");
	gtk_widget_queue_draw(graph);
}

static void
on_room_graph_drag_begin(GtkGestureDrag *gesture, double x, double y,
	gpointer data)
{
	DspPanel *self = data;
	GtkWidget *graph;
	int band;

	graph = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	band = room_graph_band_at(self, graph, x, y);
	if (band < 0) {
		self->graph_drag_band = -1;
		return;
	}
	room_graph_begin_band_drag(self, graph, band);
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_room_graph_drag_update(GtkGestureDrag *gesture,
	double offset_x, double offset_y, gpointer data)
{
	DspPanel *self = data;
	GtkWidget *graph;
	double delta_x, delta_y, fraction, frequency, gain;
	double graph_height, graph_width;
	GdkModifierType modifiers;
	char path[48];
	int band = self->graph_drag_band;

	if (band < 0)
		return;
	graph = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	graph_width = MAX(1, gtk_widget_get_width(graph) - 38);
	graph_height = MAX(1, gtk_widget_get_height(graph) - 28);
	delta_x = offset_x - self->graph_drag_last_x;
	delta_y = offset_y - self->graph_drag_last_y;
	self->graph_drag_last_x = offset_x;
	self->graph_drag_last_y = offset_y;
	if (fabs(delta_x) > 96. || fabs(delta_y) > 96.)
		return;
	modifiers = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(gesture));
	if (modifiers & GDK_CONTROL_MASK) {
		delta_x *= .1;
		delta_y *= .1;
	}
	fraction = log(CLAMP(self->graph_drag_frequency, 20., 20000.) / 20.) /
	           log(1000.) + delta_x / graph_width;
	frequency = 20. * pow(1000., CLAMP(fraction, 0., 1.));
	gain = CLAMP(self->graph_drag_gain - delta_y * 48. / graph_height,
	             -20., 20.);
	gain = round(gain * 10.) / 10.;
	self->graph_drag_frequency = frequency;
	self->graph_drag_gain = gain;
	g_snprintf(path, sizeof path, "roomeq/band%dfreq", band + 1);
	set_room_parameter(self, path, frequency, TRUE);
	g_snprintf(path, sizeof path, "roomeq/band%dgain", band + 1);
	set_room_parameter(self, path, gain, FALSE);
}

static void
on_room_graph_drag_end(GtkGestureDrag *gesture,
	double offset_x, double offset_y, gpointer data)
{
	DspPanel *self = data;
	GtkWidget *graph;

	(void)offset_x;
	(void)offset_y;
	graph = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	self->graph_drag_band = -1;
	gtk_widget_set_cursor_from_name(graph, "crosshair");
	gtk_widget_queue_draw(graph);
}

static gboolean
on_room_graph_scroll(GtkEventControllerScroll *controller,
	double dx, double dy, gpointer data)
{
	DspPanel *self = data;
	double delta, q, step;
	GdkModifierType modifiers;
	char path[48];
	int band = self->graph_selected_band;

	if (band < 0)
		return FALSE;
	delta = fabs(dx) > fabs(dy) ? -dx : -dy;
	if (delta == 0.)
		return TRUE;
	g_snprintf(path, sizeof path, "roomeq/band%dq", band + 1);
	q = room_parameter(self, path, self->graph_drag_q);
	modifiers = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(controller));
	step = modifiers & GDK_CONTROL_MASK ? .1 : .2;
	q = CLAMP(round((q + (delta > 0. ? step : -step)) * 10.) / 10., .4, 9.9);
	self->graph_drag_q = q;
	set_room_parameter(self, path, q, FALSE);
	return TRUE;
}

static void
draw_room_eq_graph(GtkDrawingArea *area, cairo_t *cr, int width, int height,
	gpointer data)
{
	static const double grid_freq[] = {20., 100., 1000., 10000., 20000.};
	DspPanel *self = data;
	double graph_width = MAX(1, width - 38), graph_height = MAX(1, height - 28);
	double db, frequency, gain[9], freq[9], q[9], x, y;
	gboolean enabled;
	int band, pixel, tick;
	int output = self->room_target_output >= 0
	           ? self->room_target_output : self->selected_output;
	char path[48], label[12];

	(void)area;
	graph_color(cr, .025, .035, .05, 1.);
	cairo_paint(cr);
	graph_color(cr, .36, .43, .51, .22);
	cairo_set_line_width(cr, 1.);
	for (tick = -20; tick <= 20; tick += 10) {
		y = 6. + (24. - tick) / 48. * graph_height;
		cairo_move_to(cr, 30., y);
		cairo_line_to(cr, 30. + graph_width, y);
	}
	for (tick = 0; tick < (int)G_N_ELEMENTS(grid_freq); ++tick) {
		x = 30. + log(grid_freq[tick] / 20.) / log(1000.) * graph_width;
		cairo_move_to(cr, x, 6.);
		cairo_line_to(cr, x, 6. + graph_height);
	}
	cairo_stroke(cr);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 9.5);
	graph_color(cr, .56, .64, .73, .85);
	for (tick = -20; tick <= 20; tick += 20) {
		g_snprintf(label, sizeof label, "%+d", tick);
		y = 9. + (24. - tick) / 48. * graph_height;
		cairo_move_to(cr, 4., y);
		cairo_show_text(cr, label);
	}
	for (tick = 0; tick < (int)G_N_ELEMENTS(grid_freq); ++tick) {
		x = 30. + log(grid_freq[tick] / 20.) / log(1000.) * graph_width;
		g_snprintf(label, sizeof label, grid_freq[tick] >= 1000. ? "%.0fk" : "%.0f",
		           grid_freq[tick] / (grid_freq[tick] >= 1000. ? 1000. : 1.));
		cairo_move_to(cr, CLAMP(x - 8., 30., width - 24.), height - 5.);
		cairo_show_text(cr, label);
	}
	enabled = room_parameter(self, "roomeq", 0.) != 0.;
	for (band = 0; band < 9; ++band) {
		g_snprintf(path, sizeof path, "roomeq/band%dgain", band + 1);
		gain[band] = room_parameter(self, path, 0.);
		g_snprintf(path, sizeof path, "roomeq/band%dfreq", band + 1);
		freq[band] = room_parameter(self, path, room_default_frequency(band));
		g_snprintf(path, sizeof path, "roomeq/band%dq", band + 1);
		q[band] = room_parameter(self, path, 1.);
	}
	if (enabled) {
		graph_color(cr, .38, .78, .98, 1.);
		cairo_set_line_width(cr, 2.1);
		for (pixel = 0; pixel <= (int)graph_width; ++pixel) {
			frequency = 20. * pow(1000., pixel / graph_width);
			db = 0.;
			for (band = 0; band < 9; ++band) {
			int type = 0, role = band == 0 ? 1 : band >= 7 ? 3 : 2;

				if (!self->room_band_active[output][band])
					continue;
				if (band == 0 || band >= 7) {
					g_snprintf(path, sizeof path, "roomeq/band%dtype", band + 1);
					type = lround(room_parameter(self, path, 0.));
				}
				db += band_response(frequency, freq[band], gain[band], q[band],
				                    type, role);
			}
			db = CLAMP(db, -24., 24.);
			x = 30. + pixel;
			y = 6. + (24. - db) / 48. * graph_height;
			if (pixel == 0)
				cairo_move_to(cr, x, y);
			else
				cairo_line_to(cr, x, y);
		}
		cairo_stroke(cr);
	}
	for (band = 0; band < 9; ++band) {
		char number[3];
		cairo_text_extents_t extents;
		double alpha = self->room_band_active[output][band]
		             ? (enabled ? 1. : .62) : .28;

		graph_point_position(GTK_WIDGET(area), freq[band], gain[band], &x, &y);
		g_snprintf(number, sizeof number, "%d", band + 1);
		graph_color(cr, .02, .03, .045, .96);
		cairo_new_path(cr);
		cairo_arc(cr, x, y,
		          self->graph_drag_band == band ? 9.5 :
		          self->graph_selected_band == band ? 9. : 7.5,
		          0., 2. * G_PI);
		cairo_fill_preserve(cr);
		graph_color(cr, ROOM_EQ_COLORS[band][0], ROOM_EQ_COLORS[band][1],
		            ROOM_EQ_COLORS[band][2], alpha);
		cairo_set_line_width(cr,
		                     self->graph_selected_band == band ? 3. : 2.);
		cairo_stroke(cr);
		cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
		                       CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 8.5);
		cairo_text_extents(cr, number, &extents);
		graph_color(cr, .88, .93, .98, alpha);
		cairo_move_to(cr, x - extents.width / 2. - extents.x_bearing,
		              y - extents.height / 2. - extents.y_bearing);
		cairo_show_text(cr, number);
		cairo_new_path(cr);
	}
}

static void
copy_room_eq_to_peer(DspPanel *self)
{
	MixParamValue value;
	ParamBinding *binding;
	char address[128];
	int peer;
	guint i;

	if (self->room_peer < 0)
		return;
	peer = self->room_peer;
	for (i = 0; i < self->bindings->len; ++i) {
		binding = g_ptr_array_index(self->bindings, i);
		if (binding->scope != PARAM_SELECTED_OUTPUT ||
		    !g_str_has_prefix(binding->path, "roomeq"))
			continue;
		binding_address(binding, address, sizeof address);
		if (!mix_model_get_parameter(self->model, address, &value))
			continue;
		g_snprintf(address, sizeof address, "/output/%d/%s", peer + 1,
		           binding->path);
		if (value.type == 'f') {
			mix_model_set_parameter_float(self->model, address, value.f);
			osc_transport_send_float(self->transport, address, value.f);
		} else {
			mix_model_set_parameter_int(self->model, address, value.i);
			osc_transport_send_int(self->transport, address, value.i);
		}
	}
}

static double
room_parameter(DspPanel *self, const char *path, double fallback)
{
	MixParamValue value;
	char address[128];
	int output = self->room_target_output >= 0
	           ? self->room_target_output : self->selected_output;

	g_snprintf(address, sizeof address, "/output/%d/%s", output + 1, path);
	if (!mix_model_get_parameter(self->model, address, &value))
		return fallback;
	return value.type == 'f' ? value.f : value.i;
}

static void
set_room_parameter(DspPanel *self, const char *path, double value,
	gboolean integer)
{
	char address[128];
	int output = self->room_target_output >= 0
	           ? self->room_target_output : self->selected_output;

	g_snprintf(address, sizeof address, "/output/%d/%s", output + 1, path);
	if (integer) {
		mix_model_set_parameter_int(self->model, address, lround(value));
		osc_transport_send_int(self->transport, address, lround(value));
	} else {
		mix_model_set_parameter_float(self->model, address, value);
		osc_transport_send_float(self->transport, address, value);
	}
	if (self->room_pair >= 0 && self->room_peer >= 0 &&
	    self->room_links[self->room_pair]) {
		g_snprintf(address, sizeof address, "/output/%d/%s",
		           self->room_peer + 1, path);
		if (integer) {
			mix_model_set_parameter_int(self->model, address, lround(value));
			osc_transport_send_int(self->transport, address, lround(value));
		} else {
			mix_model_set_parameter_float(self->model, address, value);
			osc_transport_send_float(self->transport, address, value);
		}
	}
	queue_dsp_graphs(self);
}

static void
set_room_band_gain(DspPanel *self, int band, double gain)
{
	char path[48];

	g_snprintf(path, sizeof path, "roomeq/band%dgain", band + 1);
	set_room_parameter(self, path, gain, FALSE);
}

static void
on_room_band_enabled_changed(GObject *object, GParamSpec *spec, gpointer data)
{
	DspPanel *self = data;
	int band = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(object), "room-band"));
	int output;
	gboolean active;
	char path[48];

	(void)spec;
	if (self->updating || band < 0 || band >= 9)
		return;
	output = self->room_target_output >= 0
	       ? self->room_target_output : self->selected_output;
	active = gtk_switch_get_active(GTK_SWITCH(object));
	if (!active) {
		g_snprintf(path, sizeof path, "roomeq/band%dgain", band + 1);
		self->room_band_bypassed_gain[output][band] =
			room_parameter(self, path, 0.);
		set_room_band_gain(self, band, 0.);
	} else {
		set_room_band_gain(self, band, self->room_band_bypassed_gain[output][band]);
	}
	self->room_band_active[output][band] = active;
	if (self->room_pair >= 0 && self->room_peer >= 0 &&
	    self->room_links[self->room_pair])
		self->room_band_active[self->room_peer][band] = active;
	gtk_widget_set_sensitive(self->room_band_controls[band], active);
	queue_dsp_graphs(self);
}

static void
on_room_link_toggled(GtkToggleButton *button, gpointer data)
{
	DspPanel *self = data;
	gboolean active;

	if (self->updating || self->room_pair < 0)
		return;
	active = gtk_toggle_button_get_active(button);
	self->room_links[self->room_pair] = active;
	if (active)
		copy_room_eq_to_peer(self);
	dsp_panel_sync(self, self->kind, self->channel, self->selected_output);
}

static void
on_room_side_toggled(GtkToggleButton *button, gpointer data)
{
	DspPanel *self = data;

	if (self->updating || self->room_pair < 0 ||
	    !gtk_toggle_button_get_active(button) ||
	    self->room_links[self->room_pair])
		return;
	self->room_right_selected[self->room_pair] = button == self->room_right;
	dsp_panel_sync(self, self->kind, self->channel, self->selected_output);
}

static void
build_room_page(DspPanel *self, DspPageMode mode)
{
	static const char *const first_types[] = {"Peak", "Low Shelf", "High Pass", "Low Pass"};
	static const char *const last_types[] = {"Peak", "High Shelf", "Low Pass", "High Pass"};
	static const char *const crossfeed_levels[] = {
		"Off", "1", "2", "3", "4", "5"
	};
	GtkWidget *page, *box, *link_controls, *link_row, *link_label;
	GtkWidget *header, *label, *control, *bands, *band_box, *band_header;
	GtkWidget *band_controls, *graph_row, *utility_controls;
	char title[32], path[48];
	int band;

	page = new_page(&box);
	self->room_page = page;
	enable_page_double_click(self, page, mode);
	if (mode == DSP_PAGE_WINDOW)
		gtk_widget_add_css_class(page, "expanded-room-panel");
	link_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	link_label = gtk_label_new("Paired channels");
	gtk_label_set_xalign(GTK_LABEL(link_label), 0.f);
	gtk_widget_set_hexpand(link_label, TRUE);
	gtk_box_append(GTK_BOX(link_row), link_label);
	link_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(link_controls, "linked");
	self->room_left = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("L"));
	self->room_link = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Link"));
	self->room_right = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("R"));
	gtk_toggle_button_set_group(self->room_right, self->room_left);
	gtk_toggle_button_set_active(self->room_left, TRUE);
	gtk_widget_set_tooltip_text(GTK_WIDGET(self->room_link),
	                            "Apply Room EQ to both paired outputs");
	gtk_widget_set_tooltip_text(GTK_WIDGET(self->room_left),
	                            "Edit the left channel while Link is disengaged");
	gtk_widget_set_tooltip_text(GTK_WIDGET(self->room_right),
	                            "Edit the right channel while Link is disengaged");
	g_signal_connect(self->room_link, "toggled",
	                 G_CALLBACK(on_room_link_toggled), self);
	g_signal_connect(self->room_left, "toggled",
	                 G_CALLBACK(on_room_side_toggled), self);
	g_signal_connect(self->room_right, "toggled",
	                 G_CALLBACK(on_room_side_toggled), self);
	gtk_box_append(GTK_BOX(link_controls), GTK_WIDGET(self->room_left));
	gtk_box_append(GTK_BOX(link_controls), GTK_WIDGET(self->room_link));
	gtk_box_append(GTK_BOX(link_controls), GTK_WIDGET(self->room_right));
	gtk_box_append(GTK_BOX(link_row), link_controls);
	gtk_box_append(GTK_BOX(box), link_row);
	header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	label = gtk_label_new("Room EQ");
	gtk_label_set_xalign(GTK_LABEL(label), 0.f);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_widget_add_css_class(label, "eq-master-label");
	gtk_box_append(GTK_BOX(header), label);
	if (mode == DSP_PAGE_INSPECTOR) {
		self->room_window_button = GTK_BUTTON(
			gtk_button_new_from_icon_name("window-new-symbolic"));
		gtk_widget_add_css_class(GTK_WIDGET(self->room_window_button), "flat");
		gtk_widget_add_css_class(GTK_WIDGET(self->room_window_button),
		                         "dsp-window-button");
		gtk_widget_set_tooltip_text(
			GTK_WIDGET(self->room_window_button),
			"Show or hide the resizable Room EQ window");
		gtk_box_append(GTK_BOX(header), GTK_WIDGET(self->room_window_button));
	}
	control = gtk_switch_new();
	gtk_box_append(GTK_BOX(header), control);
	{
		ParamBinding *binding = register_binding(self, control, control, "roomeq",
		                                           PARAM_SELECTED_OUTPUT,
		                                           PARAM_SWITCH, MASK_ALL);
		g_signal_connect(control, "notify::active",
		                 G_CALLBACK(on_switch_changed), binding);
	}
	gtk_box_append(GTK_BOX(box), header);
	self->room_graph = gtk_drawing_area_new();
	gtk_widget_set_size_request(self->room_graph, -1,
	                            mode == DSP_PAGE_WINDOW ? 240 : 104);
	gtk_widget_set_hexpand(self->room_graph, TRUE);
	gtk_widget_set_cursor_from_name(self->room_graph, "crosshair");
	gtk_widget_add_css_class(self->room_graph, "eq-graph");
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self->room_graph),
	                               draw_room_eq_graph, self, NULL);
	{
		GtkGesture *click = GTK_GESTURE(gtk_gesture_click_new());
		GtkGesture *drag = GTK_GESTURE(gtk_gesture_drag_new());
		GtkEventController *scroll;

		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
		                              GDK_BUTTON_PRIMARY);
		g_signal_connect(click, "pressed",
		                 G_CALLBACK(on_room_graph_pressed), self);
		g_signal_connect(click, "released",
		                 G_CALLBACK(on_room_graph_released), self);
		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag),
		                              GDK_BUTTON_PRIMARY);
		g_signal_connect(drag, "drag-begin",
		                 G_CALLBACK(on_room_graph_drag_begin), self);
		g_signal_connect(drag, "drag-update",
		                 G_CALLBACK(on_room_graph_drag_update), self);
		g_signal_connect(drag, "drag-end",
		                 G_CALLBACK(on_room_graph_drag_end), self);
		gtk_gesture_group(click, drag);
		gtk_widget_add_controller(self->room_graph,
		                          GTK_EVENT_CONTROLLER(click));
		gtk_widget_add_controller(self->room_graph,
		                          GTK_EVENT_CONTROLLER(drag));
		scroll = gtk_event_controller_scroll_new(
			GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
			GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
		g_signal_connect(scroll, "scroll",
		                 G_CALLBACK(on_room_graph_scroll), self);
		gtk_widget_add_controller(self->room_graph, scroll);
	}
	graph_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(graph_row), self->room_graph);
	gtk_box_append(GTK_BOX(box), graph_row);
	utility_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
	                               mode == DSP_PAGE_WINDOW ? 18 : 8);
	gtk_box_set_homogeneous(GTK_BOX(utility_controls), TRUE);
	gtk_box_append(GTK_BOX(utility_controls),
	               new_output_captioned_knob(self, "Delay", "roomeq/delay",
	                 0, .425, .001, 0, FALSE, 3, "ms", .35, .74, .95, FALSE));
	gtk_box_append(GTK_BOX(utility_controls),
	               new_output_captioned_knob(self, "Vol. Cal", "volumecal",
	                 -24, 3, .1, 0, FALSE, 1, "dB", .94, .62, .26, FALSE));
	control = new_bound_output_dropdown(self, "crossfeed", crossfeed_levels,
	                                   G_N_ELEMENTS(crossfeed_levels));
	gtk_widget_set_tooltip_text(control, "Crossfeed strength");
	gtk_box_append(GTK_BOX(utility_controls), control);
	gtk_box_append(GTK_BOX(box), utility_controls);
	bands = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(bands),
	                         mode == DSP_PAGE_WINDOW ? 10 : 4);
	gtk_grid_set_column_spacing(GTK_GRID(bands),
	                            mode == DSP_PAGE_WINDOW ? 10 : 2);
	gtk_grid_set_column_homogeneous(GTK_GRID(bands), TRUE);
	gtk_widget_set_hexpand(bands, TRUE);
	for (band = 1; band <= 9; ++band) {
		band_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_widget_add_css_class(band_box, "eq-band-box");
		band_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
		g_snprintf(title, sizeof title, "%d", band);
		label = gtk_label_new(title);
		gtk_widget_add_css_class(label, "eq-band-label");
		g_snprintf(title, sizeof title, "Room EQ band %d", band);
		gtk_widget_set_tooltip_text(label, title);
		gtk_box_append(GTK_BOX(band_header), label);
		self->room_band_enabled[band - 1] = GTK_SWITCH(gtk_switch_new());
		g_object_set_data(G_OBJECT(self->room_band_enabled[band - 1]), "room-band",
		                  GINT_TO_POINTER(band - 1));
		g_signal_connect(self->room_band_enabled[band - 1], "notify::active",
		                 G_CALLBACK(on_room_band_enabled_changed), self);
		gtk_box_append(GTK_BOX(band_header),
		               GTK_WIDGET(self->room_band_enabled[band - 1]));
		if (band == 1) {
			control = new_bound_output_dropdown(self, "roomeq/band1type", first_types,
			                                   G_N_ELEMENTS(first_types));
			gtk_widget_set_size_request(control,
			                            mode == DSP_PAGE_WINDOW ? 100 : 38, -1);
			gtk_box_append(GTK_BOX(band_header), control);
		} else if (band >= 8) {
			g_snprintf(path, sizeof path, "roomeq/band%dtype", band);
			control = new_bound_output_dropdown(self, path, last_types,
			                                   G_N_ELEMENTS(last_types));
			gtk_widget_set_size_request(control,
			                            mode == DSP_PAGE_WINDOW ? 100 : 38, -1);
			gtk_box_append(GTK_BOX(band_header), control);
		}
		gtk_box_append(GTK_BOX(band_box), band_header);
		band_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		self->room_band_controls[band - 1] = band_controls;
		g_snprintf(path, sizeof path, "roomeq/band%dfreq", band);
		gtk_box_append(GTK_BOX(band_controls),
		               new_output_captioned_knob(self, "Freq", path, 20, 20000, 1,
		                 band == 1 ? 100 : band == 9 ? 10000 : 1000, TRUE, 0, "",
		                 ROOM_EQ_COLORS[band - 1][0], ROOM_EQ_COLORS[band - 1][1],
		                 ROOM_EQ_COLORS[band - 1][2], TRUE));
		g_snprintf(path, sizeof path, "roomeq/band%dgain", band);
		gtk_box_append(GTK_BOX(band_controls),
		               new_output_captioned_knob(self, "Gain", path, -20, 20, .1, 0,
		                 FALSE, 1, "", ROOM_EQ_COLORS[band - 1][0],
		                 ROOM_EQ_COLORS[band - 1][1],
		                 ROOM_EQ_COLORS[band - 1][2], FALSE));
		g_snprintf(path, sizeof path, "roomeq/band%dq", band);
		gtk_box_append(GTK_BOX(band_controls),
		               new_output_captioned_knob(self, "Q", path, .4, 9.9, .1, 1,
		                 FALSE, 1, "", ROOM_EQ_COLORS[band - 1][0],
		                 ROOM_EQ_COLORS[band - 1][1],
		                 ROOM_EQ_COLORS[band - 1][2], FALSE));
		gtk_box_append(GTK_BOX(band_box), band_controls);
		gtk_grid_attach(GTK_GRID(bands), band_box, (band - 1) % 3,
		                (band - 1) / 3, 1, 1);
	}
	gtk_box_append(GTK_BOX(box), bands);
	gtk_stack_add_titled(self->stack, page, "room", "Room EQ");
}

static void
on_command_clicked(GtkButton *button, gpointer data)
{
	const char *address = g_object_get_data(G_OBJECT(button), "osc-address");
	DspPanel *self = data;

	osc_transport_send(self->transport, address);
}

static void
add_command_button(DspPanel *self, GtkWidget *box, const char *label,
	const char *address)
{
	GtkWidget *button = gtk_button_new_with_label(label);

	g_object_set_data_full(G_OBJECT(button), "osc-address", g_strdup(address), g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(on_command_clicked), self);
	gtk_box_append(GTK_BOX(box), button);
}

static void
build_system_page(DspPanel *self)
{
	static const char *const clock_sources[] = {
		"Internal", "Word Clock", "AES", "Opt. 1", "Opt. 2", "MADI Opt.", "MADI Coax."
	};
	static const char *const cc_mixes[] = {
		"TotalMix App", "6 channels + phones", "8 channels", "20 channels"
	};
	static const char *const standalone_midi[] = {
		"Off", "MIDI 1", "MIDI 2", "MADI Opt.", "MADI Coax."
	};
	static const char *const standalone_arc[] = {"Volume", "1 s operation", "Normal"};
	static const char *const lock_keys[] = {"Off", "Keys", "All"};
	GtkWidget *page, *box;

	page = new_page(&box);
	gtk_box_append(GTK_BOX(box), new_section("Control Room"));
	add_switch(self, box, "Main Mono", "/controlroom/mainmono", PARAM_GLOBAL, MASK_ALL);
	add_switch(self, box, "Mute Enable", "/controlroom/muteenable", PARAM_GLOBAL, MASK_ALL);
	add_switch(self, box, "Dim", "/controlroom/dim", PARAM_GLOBAL, MASK_ALL);
	add_scale(self, box, "Dim reduction", "/controlroom/dimreduction", PARAM_GLOBAL,
	          MASK_ALL, -65, 0, .1, FALSE);
	add_scale(self, box, "Recall Volume", "/controlroom/recallvolume", PARAM_GLOBAL,
	          MASK_ALL, -65, 0, .1, FALSE);
	gtk_box_append(GTK_BOX(box), new_section("Clock"));
	add_dropdown(self, box, "Source", "/clock/source", PARAM_GLOBAL, MASK_ALL,
	             clock_sources, G_N_ELEMENTS(clock_sources));
	add_switch(self, box, "Word Clock Out", "/clock/wckout", PARAM_GLOBAL, MASK_ALL);
	add_switch(self, box, "Single Speed", "/clock/wcksingle", PARAM_GLOBAL, MASK_ALL);
	add_switch(self, box, "Termination", "/clock/wckterm", PARAM_GLOBAL, MASK_ALL);
	gtk_box_append(GTK_BOX(box), new_section("Standalone mode"));
	add_dropdown(self, box, "CC Mix", "/hardware/ccmix", PARAM_GLOBAL, MASK_ALL,
	             cc_mixes, G_N_ELEMENTS(cc_mixes));
	add_dropdown(self, box, "Standalone MIDI", "/hardware/standalonemidi",
	             PARAM_GLOBAL, MASK_ALL, standalone_midi,
	             G_N_ELEMENTS(standalone_midi));
	add_dropdown(self, box, "Standalone ARC", "/hardware/standalonearc",
	             PARAM_GLOBAL, MASK_ALL, standalone_arc,
	             G_N_ELEMENTS(standalone_arc));
	add_dropdown(self, box, "Lock keys", "/hardware/lockkeys",
	             PARAM_GLOBAL, MASK_ALL, lock_keys, G_N_ELEMENTS(lock_keys));
	gtk_box_append(GTK_BOX(box), new_section("DuRec"));
	add_command_button(self, box, "Record", "/durec/record");
	add_command_button(self, box, "Play / Pause", "/durec/play");
	add_command_button(self, box, "Stop", "/durec/stop");
	gtk_stack_add_titled(self->stack, page, "system", "System");
}

static DspPanel *
new_panel(MixModel *model, OscTransport *transport)
{
	DspPanel *self;
	int i;

	self = g_new0(DspPanel, 1);
	self->model = model;
	self->transport = transport;
	self->graph_drag_band = -1;
	self->graph_selected_band = -1;
	self->live_levels[0] = -65.f;
	self->live_levels[1] = -65.f;
	self->room_pair = -1;
	self->room_peer = -1;
	self->room_target_output = -1;
	for (i = 0; i < OSCMIX_UFXP_OUTPUTS / 2; ++i)
		self->room_links[i] = TRUE;
	for (i = 0; i < OSCMIX_UFXP_OUTPUTS; ++i) {
		int band;

		for (band = 0; band < 9; ++band)
			self->room_band_active[i][band] = TRUE;
	}
	self->bindings = g_ptr_array_new_with_free_func(binding_free);
	self->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_vexpand(self->root, TRUE);
	self->stack = GTK_STACK(gtk_stack_new());
	gtk_stack_set_transition_type(self->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_widget_set_vexpand(GTK_WIDGET(self->stack), TRUE);
	return self;
}

DspPanel *
dsp_panel_new(MixModel *model, OscTransport *transport)
{
	DspPanel *self;
	GtkWidget *switcher;

	self = new_panel(model, transport);
	switcher = gtk_stack_switcher_new();
	gtk_widget_add_css_class(switcher, "dsp-tabs");
	gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), self->stack);
	gtk_box_append(GTK_BOX(self->root), switcher);
	gtk_box_append(GTK_BOX(self->root), GTK_WIDGET(self->stack));
	build_eq_page(self, DSP_PAGE_INSPECTOR);
	build_dynamics_page(self, DSP_PAGE_INSPECTOR);
	build_fx_page(self);
	build_room_page(self, DSP_PAGE_INSPECTOR);
	build_system_page(self);
	return self;
}

DspPanel *
dsp_panel_new_eq(MixModel *model, OscTransport *transport)
{
	DspPanel *self;

	self = new_panel(model, transport);
	self->compact_controls = TRUE;
	g_object_ref_sink(self->root);
	self->owns_root = TRUE;
	gtk_widget_set_overflow(self->root, GTK_OVERFLOW_HIDDEN);
	gtk_box_append(GTK_BOX(self->root), GTK_WIDGET(self->stack));
	build_eq_page(self, DSP_PAGE_INLINE);
	gtk_stack_set_visible_child_name(self->stack, "eq");
	return self;
}

DspPanel *
dsp_panel_new_eq_window(MixModel *model, OscTransport *transport)
{
	DspPanel *self;

	self = new_panel(model, transport);
	self->expanded_controls = TRUE;
	g_object_ref_sink(self->root);
	self->owns_root = TRUE;
	gtk_widget_set_overflow(self->root, GTK_OVERFLOW_HIDDEN);
	gtk_box_append(GTK_BOX(self->root), GTK_WIDGET(self->stack));
	build_eq_page(self, DSP_PAGE_WINDOW);
	gtk_stack_set_visible_child_name(self->stack, "eq");
	return self;
}

DspPanel *
dsp_panel_new_dynamics(MixModel *model, OscTransport *transport)
{
	DspPanel *self;

	self = new_panel(model, transport);
	self->compact_controls = TRUE;
	g_object_ref_sink(self->root);
	self->owns_root = TRUE;
	gtk_widget_set_overflow(self->root, GTK_OVERFLOW_HIDDEN);
	gtk_box_append(GTK_BOX(self->root), GTK_WIDGET(self->stack));
	build_dynamics_page(self, DSP_PAGE_INLINE);
	gtk_stack_set_visible_child_name(self->stack, "dynamics");
	return self;
}

DspPanel *
dsp_panel_new_dynamics_window(MixModel *model, OscTransport *transport)
{
	DspPanel *self;

	self = new_panel(model, transport);
	self->expanded_controls = TRUE;
	g_object_ref_sink(self->root);
	self->owns_root = TRUE;
	gtk_widget_set_overflow(self->root, GTK_OVERFLOW_HIDDEN);
	gtk_box_append(GTK_BOX(self->root), GTK_WIDGET(self->stack));
	build_dynamics_page(self, DSP_PAGE_WINDOW);
	gtk_stack_set_visible_child_name(self->stack, "dynamics");
	return self;
}

DspPanel *
dsp_panel_new_room_window(MixModel *model, OscTransport *transport)
{
	DspPanel *self;

	self = new_panel(model, transport);
	self->expanded_controls = TRUE;
	g_object_ref_sink(self->root);
	self->owns_root = TRUE;
	gtk_box_append(GTK_BOX(self->root), GTK_WIDGET(self->stack));
	build_room_page(self, DSP_PAGE_WINDOW);
	gtk_stack_set_visible_child_name(self->stack, "room");
	return self;
}

GtkWidget *
dsp_panel_get_widget(DspPanel *self)
{
	return self->root;
}

GtkButton *
dsp_panel_get_eq_window_button(DspPanel *self)
{
	return self->eq_window_button;
}

GtkButton *
dsp_panel_get_dynamics_window_button(DspPanel *self)
{
	return self->dynamics_window_button;
}

GtkButton *
dsp_panel_get_room_window_button(DspPanel *self)
{
	return self->room_window_button;
}

void
dsp_panel_sync(DspPanel *self, MixChannelKind kind, int channel,
	int selected_output)
{
	MixRowSnapshot row;
	MixParamValue value;
	ParamBinding *binding;
	char address[128];
	char title[144];
	guint i;
	int left = 0, right = 0;
	double number;
	gboolean room_active, room_paired;

	self->kind = kind;
	self->channel = channel;
	self->selected_output = selected_output;
	self->updating = TRUE;
	if (self->room_link) {
		room_paired = room_stereo_pair(self, &left, &right);
		self->room_pair = room_paired ? left / 2 : -1;
		self->room_target_output = room_paired &&
		                           self->room_right_selected[self->room_pair]
		                         ? right : room_paired ? left : selected_output;
		self->room_peer = room_paired && self->room_target_output == right
		                ? left : room_paired ? right : -1;
		room_active = room_paired && self->room_links[self->room_pair];
		gtk_widget_set_sensitive(GTK_WIDGET(self->room_link), room_paired);
		if (gtk_toggle_button_get_active(self->room_link) != room_active)
			gtk_toggle_button_set_active(self->room_link, room_active);
		gtk_widget_set_visible(GTK_WIDGET(self->room_left), room_paired);
		gtk_widget_set_visible(GTK_WIDGET(self->room_right), room_paired);
		gtk_widget_set_sensitive(GTK_WIDGET(self->room_left),
		                         room_paired && !room_active);
		gtk_widget_set_sensitive(GTK_WIDGET(self->room_right),
		                         room_paired && !room_active);
		if (room_paired) {
			GtkToggleButton *side = self->room_target_output == right
			                      ? self->room_right : self->room_left;
			if (!gtk_toggle_button_get_active(side))
				gtk_toggle_button_set_active(side, TRUE);
		}
	}
	if (self->eq_title || self->dynamics_title) {
		mix_model_snapshot_row(self->model, kind, &row);
		if (channel >= 0 && channel < row.count) {
			mix_row_format_channel_name(&row, channel, title, sizeof title);
			if (self->eq_title)
				gtk_label_set_text(self->eq_title, title);
			if (self->dynamics_title)
				gtk_label_set_text(self->dynamics_title, title);
		}
	}
	for (i = 0; i < self->bindings->len; ++i) {
		binding = g_ptr_array_index(self->bindings, i);
		if (gtk_widget_get_sensitive(binding->row) != binding_is_available(binding))
			gtk_widget_set_sensitive(binding->row, binding_is_available(binding));
		if (!binding_is_available(binding))
			continue;
		binding_address(binding, address, sizeof address);
		if (!mix_model_get_parameter(self->model, address, &value))
			continue;
		if (binding->type == PARAM_SWITCH) {
			gboolean active = value.type == 'f' ? value.f != 0.f : value.i != 0;
			if (gtk_switch_get_active(GTK_SWITCH(binding->widget)) != active)
				gtk_switch_set_active(GTK_SWITCH(binding->widget), active);
		} else if (binding->type == PARAM_DROPDOWN) {
			guint selected = value.type == 'f' ? lroundf(value.f) : value.i;
			if (gtk_drop_down_get_selected(GTK_DROP_DOWN(binding->widget)) != selected)
				gtk_drop_down_set_selected(GTK_DROP_DOWN(binding->widget), selected);
		} else if (binding->type == PARAM_KNOB) {
			KnobData *knob = g_object_get_data(G_OBJECT(binding->widget),
			                                   "oscmix-knob");
			number = value.type == 'f' ? value.f : value.i;
			if (fabs(gtk_adjustment_get_value(knob->adjustment) - number) > .0001)
				gtk_adjustment_set_value(knob->adjustment, number);
		} else {
			number = value.type == 'f' ? value.f : value.i;
			if (fabs(gtk_range_get_value(GTK_RANGE(binding->widget)) - number) > .0001)
				gtk_range_set_value(GTK_RANGE(binding->widget), number);
		}
	}
	if (self->room_graph) {
		int output = self->room_target_output >= 0
		           ? self->room_target_output : selected_output;
		int band;

		for (band = 0; band < 9; ++band) {
			gboolean active = self->room_band_active[output][band];

			if (gtk_switch_get_active(self->room_band_enabled[band]) != active)
				gtk_switch_set_active(self->room_band_enabled[band], active);
			gtk_widget_set_sensitive(self->room_band_controls[band], active);
		}
	}
	self->updating = FALSE;
	queue_dsp_graphs(self);
}

void
dsp_panel_update_meters(DspPanel *self)
{
	MixRowSnapshot row;
	float level_left, level_right, gain_reduction;
	int first;
	gboolean changed, dynamics, stereo;

	if (!self || self->kind == MIX_CHANNEL_PLAYBACK || self->channel < 0)
		return;
	mix_model_snapshot_row(self->model, self->kind, &row);
	if (self->channel >= row.count)
		return;
	first = self->channel;
	if ((first & 1) && row.channels[first].stereo)
		--first;
	stereo = row.channels[first].stereo && first + 1 < row.count;
	level_left = row.channels[first].level;
	level_right = stereo ? row.channels[first + 1].level : -65.f;
	dynamics = row.channels[first].dynamics ||
	           (stereo && row.channels[first + 1].dynamics);
	gain_reduction = dynamics ? row.channels[first].gain_reduction : 0.f;
	if (dynamics && stereo)
		gain_reduction = MAX(gain_reduction,
		                     row.channels[first + 1].gain_reduction);
	changed = fabsf(level_left - self->live_levels[0]) > .01f ||
	          fabsf(level_right - self->live_levels[1]) > .01f ||
	          fabsf(gain_reduction - self->live_gain_reduction) > .001f ||
	          stereo != self->live_stereo;
	if (!changed)
		return;
	self->live_levels[0] = level_left;
	self->live_levels[1] = level_right;
	self->live_gain_reduction = gain_reduction;
	self->live_stereo = stereo;
	queue_live_displays(self);
}

void
dsp_panel_set_detach_handler(DspPanel *self, DspPanelDetachFunc callback,
	gpointer user_data)
{
	self->detach_callback = callback;
	self->detach_data = user_data;
}

void
dsp_panel_show_eq(DspPanel *self)
{
	if (!self->eq_page)
		return;
	gtk_stack_set_visible_child_name(self->stack, "eq");
	if (self->eq_graph)
		gtk_widget_queue_draw(self->eq_graph);
}

void
dsp_panel_show_dynamics(DspPanel *self)
{
	if (!self->dynamics_page)
		return;
	gtk_stack_set_visible_child_name(self->stack, "dynamics");
	if (self->dynamics_graph)
		gtk_widget_queue_draw(self->dynamics_graph);
}

void
dsp_panel_show_room(DspPanel *self)
{
	if (!self->room_page)
		return;
	gtk_stack_set_visible_child_name(self->stack, "room");
	if (self->room_graph)
		gtk_widget_queue_draw(self->room_graph);
}

void
dsp_panel_free(DspPanel *self)
{
	if (!self)
		return;
	if (self->owns_root) {
		if (gtk_widget_get_parent(self->root))
			gtk_widget_unparent(self->root);
		g_clear_object(&self->root);
	}
	g_ptr_array_unref(self->bindings);
	g_free(self);
}
