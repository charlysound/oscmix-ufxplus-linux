#include <math.h>
#include <string.h>
#include "mixer_row.h"

#define TITLE_WIDTH 92.f
#define STRIP_WIDTH 96.f
#define STRIP_GAP 3.f
#define PAN_TOP 30.f
#define PAN_HEIGHT 20.f
#define BUTTON_TOP 54.f
#define BUTTON_HEIGHT 21.f
#define FADER_TOP 82.f
#define FADER_BOTTOM 43.f
#define FADER_TRACK_X 12.f
#define FADER_TRACK_WIDTH 4.f
#define FADER_CAP_WIDTH 16.f
#define FADER_CAP_HEIGHT 28.f
#define DSP_BUTTON_X 64.f
#define DSP_BUTTON_WIDTH 28.f
#define DSP_BUTTON_HEIGHT 20.f
#define DSP_EDITOR_WIDTH 280.f
#define METER_SCALE_COUNT 7

typedef enum {
	DRAG_NONE,
	DRAG_VOLUME,
	DRAG_PAN
} DragMode;

struct _OscmixMixerRow {
	GtkWidget parent;
	MixModel *model;
	OscTransport *transport;
	MixChannelKind kind;
	GtkAdjustment *adjustment;
	PangoLayout *name_layouts[OSCMIX_UFXP_OUTPUTS];
	char cached_names[OSCMIX_UFXP_OUTPUTS][72];
	PangoLayout *db_layouts[OSCMIX_UFXP_OUTPUTS];
	PangoLayout *pan_layouts[OSCMIX_UFXP_OUTPUTS];
	PangoLayout *mute_layout;
	PangoLayout *solo_layout;
	PangoLayout *eq_layout;
	PangoLayout *dynamics_layout;
	PangoLayout *stereo_layout;
	PangoLayout *title_layout;
	PangoLayout *meter_scale_layouts[METER_SCALE_COUNT];
	int cached_db[OSCMIX_UFXP_OUTPUTS];
	int cached_pan[OSCMIX_UFXP_OUTPUTS];
	float display_levels[OSCMIX_UFXP_OUTPUTS];
	float display_gain_reduction[OSCMIX_UFXP_OUTPUTS];
	int drag_channel;
	DragMode drag_mode;
	float drag_start_volume;
	int drag_start_pan;
	float drag_volume;
	int drag_pan;
	double drag_last_x;
	double drag_last_y;
	gint64 last_frame_time;
	guint64 last_generation;
	int last_visible_count;
	GtkWidget *editor;
	int editor_channel;
	gboolean reveal_editor;
};

G_DEFINE_TYPE(OscmixMixerRow, oscmix_mixer_row, GTK_TYPE_WIDGET)

enum {
	EQ_REQUESTED,
	DYNAMICS_REQUESTED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

static const GdkRGBA COLOR_ROW_BG = {0.035, 0.047, 0.071, 1.0};
static const GdkRGBA COLOR_STRIP = {0.075, 0.094, 0.132, 1.0};
static const GdkRGBA COLOR_STRIP_SELECTED = {0.11, 0.16, 0.22, 1.0};
static const GdkRGBA COLOR_TEXT = {0.88, 0.92, 0.97, 1.0};
static const GdkRGBA COLOR_MUTED = {0.49, 0.54, 0.61, 1.0};
static const GdkRGBA COLOR_ACCENT = {0.0, 0.72, 0.94, 1.0};
static const GdkRGBA COLOR_METER = {0.16, 0.86, 0.45, 1.0};
static const GdkRGBA COLOR_METER_ORANGE = {1.0, 0.55, 0.1, 1.0};
static const GdkRGBA COLOR_METER_RED = {0.96, 0.18, 0.18, 1.0};
static const GdkRGBA COLOR_GR_TRACK = {0.035, 0.16, 0.22, 1.0};
static const GdkRGBA COLOR_GR_METER = {0.16, 0.78, 1.0, 1.0};
static const GdkRGBA COLOR_MUTE = {0.86, 0.18, 0.18, 1.0};
static const GdkRGBA COLOR_SOLO = {0.96, 0.43, 0.04, 1.0};
static const GdkRGBA COLOR_EQ = {0.72, 0.49, 0.02, 1.0};
static const GdkRGBA COLOR_EQ_TEXT = {1.0, 0.76, 0.08, 1.0};
static const GdkRGBA COLOR_TRACK = {0.025, 0.03, 0.045, 1.0};
static const GdkRGBA COLOR_FADER_BORDER = {0.12, 0.14, 0.16, 1.0};
static const GdkRGBA COLOR_FADER_GROOVE = {0.055, 0.065, 0.075, 1.0};
static const GdkRGBA COLOR_FADER_SHADOW = {0.0, 0.0, 0.0, 0.55};
static const GdkRGBA COLOR_FADER_HIGHLIGHT = {0.88, 0.91, 0.93, 0.75};

static const float meter_scale_db[METER_SCALE_COUNT] = {
	0.f, -6.f, -12.f, -18.f, -30.f, -42.f, -54.f
};
static const char *meter_scale_labels[METER_SCALE_COUNT] = {
	"0", "-6", "-12", "-18", "-30", "-42", "-54"
};

static void
append_rect(GtkSnapshot *snapshot, const GdkRGBA *color,
	float x, float y, float width, float height)
{
	graphene_rect_t rect;

	if (width <= 0.f || height <= 0.f)
		return;
	gtk_snapshot_append_color(snapshot, color,
	                          graphene_rect_init(&rect, x, y, width, height));
}

static void
append_rounded_rect(GtkSnapshot *snapshot, const GdkRGBA *color,
	float x, float y, float width, float height, float radius)
{
	graphene_rect_t bounds;
	GskRoundedRect rounded;

	if (width <= 0.f || height <= 0.f)
		return;
	graphene_rect_init(&bounds, x, y, width, height);
	gsk_rounded_rect_init_from_rect(&rounded, &bounds, radius);
	gtk_snapshot_push_rounded_clip(snapshot, &rounded);
	gtk_snapshot_append_color(snapshot, color, &bounds);
	gtk_snapshot_pop(snapshot);
}

static void
append_layout(GtkSnapshot *snapshot, PangoLayout *layout, const GdkRGBA *color,
	float x, float y)
{
	gtk_snapshot_save(snapshot);
	gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(x, y));
	gtk_snapshot_append_layout(snapshot, layout, color);
	gtk_snapshot_restore(snapshot);
}

static PangoLayout *
channel_name_layout(OscmixMixerRow *self, int index, const char *name)
{
	PangoLayout *layout;

	if (!self->name_layouts[index] ||
	    strcmp(self->cached_names[index], name) != 0) {
		g_clear_object(&self->name_layouts[index]);
		g_strlcpy(self->cached_names[index], name,
		            sizeof self->cached_names[index]);
		layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), name);
		pango_layout_set_width(layout, (STRIP_WIDTH - 10.f) * PANGO_SCALE);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
		pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
		self->name_layouts[index] = layout;
	}
	return self->name_layouts[index];
}

static PangoLayout *
channel_db_layout(OscmixMixerRow *self, int index, float db)
{
	char text[24];
	int tenth;

	tenth = db <= -64.95f ? -650 : lroundf(db * 10.f);
	if (!self->db_layouts[index] || self->cached_db[index] != tenth) {
		g_clear_object(&self->db_layouts[index]);
		self->cached_db[index] = tenth;
		if (tenth == -650)
			g_strlcpy(text, "−∞", sizeof text);
		else
			g_snprintf(text, sizeof text, "%.1f dB", db);
		self->db_layouts[index] = gtk_widget_create_pango_layout(GTK_WIDGET(self), text);
	}
	return self->db_layouts[index];
}

static PangoLayout *
channel_pan_layout(OscmixMixerRow *self, int index, int pan)
{
	char text[24];

	if (!self->pan_layouts[index] || self->cached_pan[index] != pan) {
		g_clear_object(&self->pan_layouts[index]);
		self->cached_pan[index] = pan;
		if (pan < 0)
			g_snprintf(text, sizeof text, "L%d", -pan);
		else if (pan > 0)
			g_snprintf(text, sizeof text, "R%d", pan);
		else
			g_strlcpy(text, "C", sizeof text);
		self->pan_layouts[index] =
			gtk_widget_create_pango_layout(GTK_WIDGET(self), text);
	}
	return self->pan_layouts[index];
}

static gboolean
channel_is_paired(const MixRowSnapshot *row, int index)
{
	return !(index & 1) && index + 1 < row->count &&
	       row->channels[index].stereo && row->channels[index + 1].stereo;
}

static int
build_visible_channels(const MixRowSnapshot *row,
	int visible[OSCMIX_UFXP_OUTPUTS])
{
	int count = 0, index;

	for (index = 0; index < row->count; ++index) {
		visible[count++] = index;
		if (channel_is_paired(row, index))
			++index;
	}
	return count;
}

static int
editor_insert_position(OscmixMixerRow *self, const MixRowSnapshot *row,
	const int visible[OSCMIX_UFXP_OUTPUTS], int visible_count)
{
	int position, channel;

	if (!self->editor)
		return -1;
	for (position = 0; position < visible_count; ++position) {
		channel = visible[position];
		if (channel == self->editor_channel ||
		    (channel_is_paired(row, channel) && channel + 1 == self->editor_channel))
			return position + 1;
	}
	return -1;
}

static double
strip_content_x(int position, int insert_position)
{
	return position * STRIP_WIDTH +
	       (insert_position >= 0 && position >= insert_position
	        ? DSP_EDITOR_WIDTH : 0.);
}

static float
volume_to_y(float volume, float height)
{
	float travel, usable, normalized;

	usable = MAX(20.f, height - FADER_TOP - FADER_BOTTOM);
	normalized = CLAMP((volume + 65.f) / 71.f, 0.f, 1.f);
	travel = MAX(1.f, usable - FADER_CAP_HEIGHT);
	return FADER_TOP + FADER_CAP_HEIGHT / 2.f + (1.f - normalized) * travel;
}

static float
fader_cap_left(void)
{
	return FADER_TRACK_X + FADER_TRACK_WIDTH / 2.f - FADER_CAP_WIDTH / 2.f;
}

static gboolean
point_in_fader(double x)
{
	return x >= fader_cap_left() - 2.f &&
	       x <= fader_cap_left() + FADER_CAP_WIDTH + 2.f;
}

static void
draw_fader_cap(GtkSnapshot *snapshot, float strip_x, float center_y)
{
	graphene_point_t gradient_end, gradient_start;
	graphene_rect_t bounds;
	GskColorStop metal_stops[] = {
		{0.f, {0.58, 0.61, 0.63, 1.0}},
		{0.18f, {0.80, 0.82, 0.84, 1.0}},
		{0.52f, {0.69, 0.71, 0.73, 1.0}},
		{0.82f, {0.56, 0.58, 0.60, 1.0}},
		{1.f, {0.43, 0.45, 0.47, 1.0}}
	};
	GskColorStop sheen_stops[] = {
		{0.f, {0.06, 0.07, 0.08, 0.22}},
		{0.30f, {1.0, 1.0, 1.0, 0.10}},
		{0.58f, {1.0, 1.0, 1.0, 0.18}},
		{1.f, {0.04, 0.05, 0.06, 0.24}}
	};
	GdkRGBA border_colors[4] = {
		COLOR_FADER_BORDER, COLOR_FADER_BORDER,
		COLOR_FADER_BORDER, COLOR_FADER_BORDER
	};
	float border_widths[4] = {1.f, 1.f, 1.f, 1.f};
	GskRoundedRect rounded;
	float cap_x, cap_y;

	cap_x = strip_x + fader_cap_left();
	cap_y = center_y - FADER_CAP_HEIGHT / 2.f;
	append_rounded_rect(snapshot, &COLOR_FADER_SHADOW, cap_x + 1.f, cap_y + 2.f,
	                    FADER_CAP_WIDTH, FADER_CAP_HEIGHT, 3.f);
	graphene_rect_init(&bounds, cap_x, cap_y, FADER_CAP_WIDTH, FADER_CAP_HEIGHT);
	gsk_rounded_rect_init_from_rect(&rounded, &bounds, 3.f);
	gtk_snapshot_push_rounded_clip(snapshot, &rounded);
	gradient_start = GRAPHENE_POINT_INIT(cap_x, cap_y);
	gradient_end = GRAPHENE_POINT_INIT(cap_x, cap_y + FADER_CAP_HEIGHT);
	gtk_snapshot_append_linear_gradient(snapshot, &bounds, &gradient_start,
	                                    &gradient_end, metal_stops,
	                                    G_N_ELEMENTS(metal_stops));
	gradient_end = GRAPHENE_POINT_INIT(cap_x + FADER_CAP_WIDTH, cap_y);
	gtk_snapshot_append_linear_gradient(snapshot, &bounds, &gradient_start,
	                                    &gradient_end, sheen_stops,
	                                    G_N_ELEMENTS(sheen_stops));
	gtk_snapshot_pop(snapshot);
	gtk_snapshot_append_border(snapshot, &rounded, border_widths, border_colors);
	append_rect(snapshot, &COLOR_FADER_HIGHLIGHT, cap_x + 3.f, cap_y + 3.f,
	            FADER_CAP_WIDTH - 6.f, 1.f);
	append_rect(snapshot, &COLOR_FADER_GROOVE, cap_x + 2.f, center_y - 1.5f,
	            FADER_CAP_WIDTH - 4.f, 3.f);
	append_rect(snapshot, &COLOR_FADER_HIGHLIGHT, cap_x + 4.f, center_y + 1.5f,
	            FADER_CAP_WIDTH - 8.f, 1.f);
}

static float
meter_to_y(float level, float height)
{
	float usable, normalized;

	usable = MAX(20.f, height - FADER_TOP - FADER_BOTTOM);
	normalized = CLAMP((level + 60.f) / 60.f, 0.f, 1.f);
	return FADER_TOP + (1.f - normalized) * usable;
}

static void
draw_meter_scale(OscmixMixerRow *self, GtkSnapshot *snapshot, float meter_x,
	float height, gboolean dimmed)
{
	const GdkRGBA *color;
	PangoLayout *layout;
	float y;
	int i, text_width, text_height;

	for (i = 0; i < METER_SCALE_COUNT; ++i) {
		y = meter_to_y(meter_scale_db[i], height);
		if (dimmed)
			color = &COLOR_MUTED;
		else if (meter_scale_db[i] >= -6.f)
			color = &COLOR_METER_RED;
		else if (meter_scale_db[i] >= -18.f)
			color = &COLOR_METER_ORANGE;
		else
			color = &COLOR_MUTED;
		append_rect(snapshot, color, meter_x - 4.f, y, 3.f, 1.f);
		layout = self->meter_scale_layouts[i];
		pango_layout_get_pixel_size(layout, &text_width, &text_height);
		append_layout(snapshot, layout, color, meter_x - 6.f - text_width,
		              y - text_height / 2.f);
	}
}

static void
draw_meter(GtkSnapshot *snapshot, float x, float width, float height,
	float level, gboolean dimmed)
{
	float bottom, level_y, orange_y, red_y;

	bottom = FADER_TOP + MAX(20.f, height - FADER_TOP - FADER_BOTTOM);
	level_y = meter_to_y(level, height);
	if (dimmed) {
		append_rect(snapshot, &COLOR_MUTED, x, level_y, width, bottom - level_y);
		return;
	}

	orange_y = meter_to_y(-18.f, height);
	red_y = meter_to_y(-6.f, height);
	append_rect(snapshot, &COLOR_METER, x, MAX(level_y, orange_y), width,
	            bottom - MAX(level_y, orange_y));
	if (level > -18.f)
		append_rect(snapshot, &COLOR_METER_ORANGE, x, MAX(level_y, red_y), width,
		            orange_y - MAX(level_y, red_y));
	if (level > -6.f)
		append_rect(snapshot, &COLOR_METER_RED, x, level_y, width,
		            red_y - level_y);
}

static void
draw_gain_reduction(GtkSnapshot *snapshot, float x, float width, float height,
	float reduction, gboolean dimmed)
{
	float bottom, marker_y, usable;
	const GdkRGBA *color;

	usable = MAX(20.f, height - FADER_TOP - FADER_BOTTOM);
	bottom = FADER_TOP + usable;
	reduction = CLAMP(reduction, 0.f, 1.f);
	color = dimmed ? &COLOR_MUTED : &COLOR_GR_METER;

	/* TotalMix shows gain reduction as small light-blue lines inside the
	 * channel meter.  Keep a narrow lane visible while Dynamics is enabled,
	 * then grow it downwards as attenuation increases. */
	append_rect(snapshot, &COLOR_GR_TRACK, x + width - 2.f, FADER_TOP, 2.f, usable);
	if (reduction <= .001f)
		return;
	marker_y = FADER_TOP + reduction * usable;
	append_rect(snapshot, color, x + width - 2.f, FADER_TOP, 2.f,
	            marker_y - FADER_TOP);
	append_rect(snapshot, color, x, CLAMP(marker_y - 1.f, FADER_TOP, bottom - 2.f),
	            width, 2.f);
}

static float
dsp_button_y(float height, gboolean dynamics)
{
	float usable = MAX(20.f, height - FADER_TOP - FADER_BOTTOM);
	float center = FADER_TOP + usable * .58f;

	return dynamics ? center + 3.f : center - DSP_BUTTON_HEIGHT - 3.f;
}

static gboolean
point_in_eq_button(double x, double y, float height)
{
	float button_y = dsp_button_y(height, FALSE);

	return x >= DSP_BUTTON_X && x <= DSP_BUTTON_X + DSP_BUTTON_WIDTH &&
	       y >= button_y && y <= button_y + DSP_BUTTON_HEIGHT;
}

static gboolean
point_in_dynamics_button(double x, double y, float height)
{
	float button_y = dsp_button_y(height, TRUE);

	return x >= DSP_BUTTON_X && x <= DSP_BUTTON_X + DSP_BUTTON_WIDTH &&
	       y >= button_y && y <= button_y + DSP_BUTTON_HEIGHT;
}

static int
channel_at(OscmixMixerRow *self, double x, double *strip_x)
{
	MixRowSnapshot row;
	int visible[OSCMIX_UFXP_OUTPUTS];
	int insert_position, visible_count, position;
	double content_x;

	if (x < TITLE_WIDTH)
		return -1;
	mix_model_snapshot_row(self->model, self->kind, &row);
	visible_count = build_visible_channels(&row, visible);
	content_x = x - TITLE_WIDTH + gtk_adjustment_get_value(self->adjustment);
	insert_position = editor_insert_position(self, &row, visible, visible_count);
	if (insert_position >= 0) {
		double editor_x = insert_position * STRIP_WIDTH;
		if (content_x >= editor_x && content_x < editor_x + DSP_EDITOR_WIDTH)
			return -1;
		if (content_x >= editor_x + DSP_EDITOR_WIDTH)
			content_x -= DSP_EDITOR_WIDTH;
	}
	position = floor(content_x / STRIP_WIDTH);
	if (strip_x)
		*strip_x = content_x - position * STRIP_WIDTH;
	return position >= 0 && position < visible_count ? visible[position] : -1;
}

static void
send_volume(OscmixMixerRow *self, int channel, float volume, int pan,
	int selected_output)
{
	char address[96];

	if (self->kind == MIX_CHANNEL_OUTPUT) {
		g_snprintf(address, sizeof address, "/output/%d/volume", channel + 1);
		osc_transport_send_float(self->transport, address, volume);
	} else {
		g_snprintf(address, sizeof address, "/mix/%d/%s/%d", selected_output + 1,
		           mix_channel_kind_osc_name(self->kind), channel + 1);
		osc_transport_send_float_int(self->transport, address, volume, pan);
	}
}

static void
send_mute(OscmixMixerRow *self, int channel, gboolean mute)
{
	char address[64];

	g_snprintf(address, sizeof address, "/%s/%d/mute",
	           mix_channel_kind_osc_name(self->kind), channel + 1);
	osc_transport_send_int(self->transport, address, mute);
}

static void
send_pan(OscmixMixerRow *self, int channel, float volume, int pan,
	int selected_output)
{
	char address[96];

	if (self->kind == MIX_CHANNEL_OUTPUT) {
		g_snprintf(address, sizeof address, "/output/%d/pan", channel + 1);
		osc_transport_send_int(self->transport, address, pan);
	} else {
		send_volume(self, channel, volume, pan, selected_output);
	}
}

static void
send_solo(OscmixMixerRow *self, int channel, int selected_output,
	gboolean solo)
{
	char address[96];

	if (self->kind == MIX_CHANNEL_OUTPUT)
		return;
	g_snprintf(address, sizeof address, "/solo/%d/%s/%d", selected_output + 1,
	           mix_channel_kind_osc_name(self->kind), channel + 1);
	osc_transport_send_int(self->transport, address, solo);
}

static void
draw_strip(OscmixMixerRow *self, GtkSnapshot *snapshot,
	const MixRowSnapshot *row, int index, float x, float height)
{
	const MixChannelState *channel = &row->channels[index];
	const GdkRGBA *background;
	PangoLayout *layout;
	char name[64];
	float fader_y, usable, marker_x;
	int text_width, text_height;
	gboolean selected, paired, muted, solo, dimmed, eq, dynamics, dsp_available;

	paired = channel_is_paired(row, index);
	selected = row->selected_kind == (int)self->kind &&
	           (row->selected_channel == index ||
	            (paired && row->selected_channel == index + 1));
	if (self->kind == MIX_CHANNEL_OUTPUT &&
	    (row->selected_output == index || (paired && row->selected_output == index + 1)))
		selected = TRUE;
	muted = channel->mute || (paired && row->channels[index + 1].mute);
	solo = channel->solo || (paired && row->channels[index + 1].solo);
	eq = channel->eq || (paired && row->channels[index + 1].eq);
	dynamics = channel->dynamics ||
	           (paired && row->channels[index + 1].dynamics);
	dsp_available = self->kind != MIX_CHANNEL_PLAYBACK;
	dimmed = muted || (row->any_solo && !solo);
	background = selected ? &COLOR_STRIP_SELECTED : &COLOR_STRIP;
	append_rect(snapshot, background, x + STRIP_GAP, 4.f,
	            STRIP_WIDTH - STRIP_GAP * 2.f, height - 8.f);
	if (selected)
		append_rect(snapshot, &COLOR_ACCENT, x + STRIP_GAP, 4.f, 3.f, height - 8.f);

	mix_row_format_channel_name(row, index, name, sizeof name);
	layout = channel_name_layout(self, index, name);
	pango_layout_get_pixel_size(layout, &text_width, &text_height);
	append_layout(snapshot, layout, dimmed ? &COLOR_MUTED : &COLOR_TEXT,
	              x + (STRIP_WIDTH - text_width) / 2.f, 10.f);

	append_rect(snapshot, &COLOR_TRACK, x + 10.f, PAN_TOP,
	            STRIP_WIDTH - 20.f, PAN_HEIGHT);
	marker_x = x + 12.f + (channel->pan + 100.f) / 200.f * (STRIP_WIDTH - 26.f);
	append_rect(snapshot, &COLOR_ACCENT, marker_x, PAN_TOP + 2.f, 2.f,
	            PAN_HEIGHT - 4.f);
	layout = channel_pan_layout(self, index, channel->pan);
	pango_layout_get_pixel_size(layout, &text_width, &text_height);
	append_layout(snapshot, layout, &COLOR_TEXT,
	              x + (STRIP_WIDTH - text_width) / 2.f,
	              PAN_TOP + (PAN_HEIGHT - text_height) / 2.f);

	append_rect(snapshot, muted ? &COLOR_MUTE : &COLOR_TRACK,
	            x + 10.f, BUTTON_TOP, 35.f, BUTTON_HEIGHT);
	append_rect(snapshot, solo ? &COLOR_SOLO : &COLOR_TRACK,
	            x + 49.f, BUTTON_TOP, 35.f, BUTTON_HEIGHT);
	layout = self->mute_layout;
	pango_layout_get_pixel_size(layout, &text_width, &text_height);
	append_layout(snapshot, layout, &COLOR_TEXT,
	              x + 10.f + (35.f - text_width) / 2.f,
	              BUTTON_TOP + (BUTTON_HEIGHT - text_height) / 2.f);
	layout = self->solo_layout;
	pango_layout_get_pixel_size(layout, &text_width, &text_height);
	append_layout(snapshot, layout,
	              self->kind == MIX_CHANNEL_OUTPUT ? &COLOR_MUTED : &COLOR_TEXT,
	              x + 49.f + (35.f - text_width) / 2.f,
	              BUTTON_TOP + (BUTTON_HEIGHT - text_height) / 2.f);

	usable = MAX(20.f, height - FADER_TOP - FADER_BOTTOM);
	append_rect(snapshot, &COLOR_TRACK, x + FADER_TRACK_X, FADER_TOP,
	            FADER_TRACK_WIDTH, usable);
	draw_meter_scale(self, snapshot, x + (paired ? 43.f : 48.f), height, dimmed);
	fader_y = volume_to_y(channel->volume, height);
	draw_fader_cap(snapshot, x, fader_y);

	draw_meter(snapshot, x + (paired ? 43.f : 48.f), paired ? 8.f : 10.f,
	           height, self->display_levels[index], dimmed);
	if (dynamics)
		draw_gain_reduction(snapshot, x + (paired ? 43.f : 48.f),
		                    paired ? 8.f : 10.f, height,
		                    self->display_gain_reduction[index], dimmed);
	if (paired)
		draw_meter(snapshot, x + 54.f, 8.f, height,
		           self->display_levels[index + 1], dimmed);
	if (paired && dynamics)
		draw_gain_reduction(snapshot, x + 54.f, 8.f, height,
		                    self->display_gain_reduction[index + 1], dimmed);

	if (dsp_available) {
		append_rect(snapshot, eq ? &COLOR_EQ : &COLOR_TRACK,
		            x + DSP_BUTTON_X, dsp_button_y(height, FALSE),
		            DSP_BUTTON_WIDTH, DSP_BUTTON_HEIGHT);
		layout = self->eq_layout;
		pango_layout_get_pixel_size(layout, &text_width, &text_height);
		append_layout(snapshot, layout, eq ? &COLOR_TEXT : &COLOR_EQ_TEXT,
		              x + DSP_BUTTON_X + (DSP_BUTTON_WIDTH - text_width) / 2.f,
		              dsp_button_y(height, FALSE) +
		              (DSP_BUTTON_HEIGHT - text_height) / 2.f);
		append_rect(snapshot, dynamics ? &COLOR_ACCENT : &COLOR_TRACK,
		            x + DSP_BUTTON_X, dsp_button_y(height, TRUE),
		            DSP_BUTTON_WIDTH, DSP_BUTTON_HEIGHT);
		layout = self->dynamics_layout;
		pango_layout_get_pixel_size(layout, &text_width, &text_height);
		append_layout(snapshot, layout,
		              dynamics ? &COLOR_TEXT : &COLOR_ACCENT,
		              x + DSP_BUTTON_X + (DSP_BUTTON_WIDTH - text_width) / 2.f,
		              dsp_button_y(height, TRUE) +
		              (DSP_BUTTON_HEIGHT - text_height) / 2.f);
	}

	layout = channel_db_layout(self, index, channel->volume);
	pango_layout_get_pixel_size(layout, &text_width, &text_height);
	append_layout(snapshot, layout, &COLOR_TEXT,
	              paired ? x + 7.f : x + (STRIP_WIDTH - text_width) / 2.f,
	              height - text_height - 8.f);
	if (paired) {
		layout = self->stereo_layout;
		pango_layout_get_pixel_size(layout, &text_width, &text_height);
		append_layout(snapshot, layout, dimmed ? &COLOR_MUTED : &COLOR_ACCENT,
		              x + STRIP_WIDTH - text_width - 7.f,
		              height - text_height - 3.f);
	}
}

static void
oscmix_mixer_row_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
	OscmixMixerRow *self = OSCMIX_MIXER_ROW(widget);
	MixRowSnapshot row;
	PangoLayout *title;
	int visible[OSCMIX_UFXP_OUTPUTS];
	double scroll;
	float width, height, x;
	int insert_position, position, visible_count, text_width, text_height;

	width = gtk_widget_get_width(widget);
	height = gtk_widget_get_height(widget);
	append_rect(snapshot, &COLOR_ROW_BG, 0.f, 0.f, width, height);
	mix_model_snapshot_row(self->model, self->kind, &row);
	visible_count = build_visible_channels(&row, visible);
	insert_position = editor_insert_position(self, &row, visible, visible_count);
	scroll = gtk_adjustment_get_value(self->adjustment);
	for (position = 0; position < visible_count; ++position) {
		x = TITLE_WIDTH + strip_content_x(position, insert_position) - scroll;
		if (x + STRIP_WIDTH < TITLE_WIDTH || x > width)
			continue;
		draw_strip(self, snapshot, &row, visible[position], x, height);
	}
	if (self->editor)
		gtk_widget_snapshot_child(widget, self->editor, snapshot);
	append_rect(snapshot, &COLOR_ROW_BG, 0.f, 0.f, TITLE_WIDTH, height);
	append_rect(snapshot, &COLOR_ACCENT, TITLE_WIDTH - 3.f, 0.f, 3.f, height);
	if (!self->title_layout)
		self->title_layout = gtk_widget_create_pango_layout(widget,
		                                                   mix_channel_kind_name(self->kind));
	title = self->title_layout;
	pango_layout_get_pixel_size(title, &text_width, &text_height);
	append_layout(snapshot, title, &COLOR_TEXT, 8.f, (height - text_height) / 2.f);
}

static void
oscmix_mixer_row_measure(GtkWidget *widget, GtkOrientation orientation,
	int for_size, int *minimum, int *natural, int *minimum_baseline,
	int *natural_baseline)
{
	(void)widget;
	(void)for_size;
	if (orientation == GTK_ORIENTATION_HORIZONTAL) {
		*minimum = 360;
		*natural = 1000;
	} else {
		*minimum = 170;
		*natural = 230;
	}
	*minimum_baseline = *natural_baseline = -1;
}

static void
oscmix_mixer_row_size_allocate(GtkWidget *widget, int width, int height,
	int baseline)
{
	OscmixMixerRow *self = OSCMIX_MIXER_ROW(widget);
	MixRowSnapshot row;
	int visible[OSCMIX_UFXP_OUTPUTS];
	int insert_position, visible_count;
	double editor_content_x, editor_x, page, scroll, upper;
	GskTransform *transform;
	graphene_point_t point;

	GTK_WIDGET_CLASS(oscmix_mixer_row_parent_class)->size_allocate(widget, width,
	                                                             height, baseline);
	mix_model_snapshot_row(self->model, self->kind, &row);
	visible_count = build_visible_channels(&row, visible);
	insert_position = editor_insert_position(self, &row, visible, visible_count);
	page = MAX(1, width - TITLE_WIDTH);
	upper = visible_count * STRIP_WIDTH +
	        (insert_position >= 0 ? DSP_EDITOR_WIDTH : 0.);
	gtk_adjustment_configure(self->adjustment,
	                         MIN(gtk_adjustment_get_value(self->adjustment),
	                             MAX(0., upper - page)),
	                         0, upper, STRIP_WIDTH, page * .9, page);
	if (self->editor && insert_position >= 0) {
		editor_content_x = insert_position * STRIP_WIDTH;
		scroll = gtk_adjustment_get_value(self->adjustment);
		if (self->reveal_editor) {
			if (editor_content_x < scroll)
				scroll = editor_content_x;
			else if (editor_content_x + DSP_EDITOR_WIDTH > scroll + page)
				scroll = editor_content_x + DSP_EDITOR_WIDTH - page;
			gtk_adjustment_set_value(self->adjustment,
			                         CLAMP(scroll, 0., MAX(0., upper - page)));
			self->reveal_editor = FALSE;
		}
		editor_x = TITLE_WIDTH + editor_content_x -
		           gtk_adjustment_get_value(self->adjustment);
		point = GRAPHENE_POINT_INIT(editor_x, 4.);
		transform = gsk_transform_translate(NULL, &point);
		gtk_widget_allocate(self->editor, DSP_EDITOR_WIDTH,
		                    MAX(1, height - 8), baseline, transform);
	}
}

static gboolean
on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
	OscmixMixerRow *self = OSCMIX_MIXER_ROW(widget);
	MixRowSnapshot row;
	int visible[OSCMIX_UFXP_OUTPUTS];
	gint64 now;
	float dt, fall;
	gboolean dirty;
	int i, visible_count;

	(void)data;
	GtkNative *native = gtk_widget_get_native(widget);
	GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
	gboolean minimized = GDK_IS_TOPLEVEL(surface) &&
	                     (gdk_toplevel_get_state(GDK_TOPLEVEL(surface)) &
	                      GDK_TOPLEVEL_STATE_MINIMIZED);
	if (!gtk_widget_get_mapped(widget) || minimized) {
		self->last_frame_time = 0;
		return G_SOURCE_CONTINUE;
	}
	now = gdk_frame_clock_get_frame_time(clock);
	dt = self->last_frame_time ? (now - self->last_frame_time) / 1000000.f : 1.f / 60.f;
	self->last_frame_time = now;
	fall = 28.f * MIN(dt, .1f);
	mix_model_snapshot_row(self->model, self->kind, &row);
	visible_count = build_visible_channels(&row, visible);
	if (visible_count != self->last_visible_count) {
		self->last_visible_count = visible_count;
		gtk_widget_queue_resize(widget);
	}
	dirty = row.generation != self->last_generation;
	self->last_generation = row.generation;
	for (i = 0; i < row.count; ++i) {
		float previous = self->display_levels[i];
		float previous_gr = self->display_gain_reduction[i];
		gboolean dynamics = row.channels[i].dynamics;
		float target_gr;
		if (row.channels[i].stereo) {
			int left = i & ~1;
			dynamics = row.channels[left].dynamics ||
			           (left + 1 < row.count && row.channels[left + 1].dynamics);
		}
		target_gr = dynamics ? row.channels[i].gain_reduction : 0.f;
		if (row.channels[i].level >= self->display_levels[i])
			self->display_levels[i] = row.channels[i].level;
		else
			self->display_levels[i] = MAX(row.channels[i].level,
			                                  self->display_levels[i] - fall);
		if (fabsf(previous - self->display_levels[i]) > .01f)
			dirty = TRUE;
		/* The hardware/OSC stream already contains the compressor envelope.
		 * Do not add a second release stage in the UI. */
		self->display_gain_reduction[i] = target_gr;
		if (fabsf(previous_gr - self->display_gain_reduction[i]) > .001f)
			dirty = TRUE;
	}
	if (dirty)
		gtk_widget_queue_draw(widget);
	return G_SOURCE_CONTINUE;
}

static void
on_adjustment_changed(GtkAdjustment *adjustment, gpointer data)
{
	(void)adjustment;
	gtk_widget_queue_allocate(GTK_WIDGET(data));
	gtk_widget_queue_draw(GTK_WIDGET(data));
}

static gboolean
on_scroll(GtkEventControllerScroll *controller, double dx, double dy,
	gpointer data)
{
	OscmixMixerRow *self = data;
	double value, delta;

	(void)controller;
	delta = fabs(dx) > fabs(dy) ? dx : dy;
	value = gtk_adjustment_get_value(self->adjustment) + delta * STRIP_WIDTH * 2.;
	gtk_adjustment_set_value(self->adjustment, value);
	return TRUE;
}

static void
on_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
	OscmixMixerRow *self = data;
	MixRowSnapshot row;
	MixChannelState *channel;
	int index;
	double local_x;

	(void)gesture;
	(void)n_press;
	index = channel_at(self, x, &local_x);
	if (index < 0)
		return;
	mix_model_select_channel(self->model, self->kind, index);
	if (self->kind == MIX_CHANNEL_OUTPUT)
		mix_model_select_output(self->model, index);
	mix_model_snapshot_row(self->model, self->kind, &row);
	channel = &row.channels[index];
	if (point_in_eq_button(local_x, y,
	                       gtk_widget_get_height(GTK_WIDGET(self))) &&
	    self->kind != MIX_CHANNEL_PLAYBACK) {
		g_signal_emit(self, signals[EQ_REQUESTED], 0, self->kind, index);
	} else if (point_in_dynamics_button(
	               local_x, y, gtk_widget_get_height(GTK_WIDGET(self))) &&
	           self->kind != MIX_CHANNEL_PLAYBACK) {
		g_signal_emit(self, signals[DYNAMICS_REQUESTED], 0, self->kind, index);
	} else if (y >= BUTTON_TOP && y <= BUTTON_TOP + BUTTON_HEIGHT &&
	    local_x >= 10.f && local_x <= 45.f) {
		mix_model_set_mute(self->model, self->kind, index, !channel->mute);
		send_mute(self, index, !channel->mute);
	} else if (y >= BUTTON_TOP && y <= BUTTON_TOP + BUTTON_HEIGHT &&
	           local_x >= 49.f && local_x <= 84.f &&
	           self->kind != MIX_CHANNEL_OUTPUT) {
		mix_model_set_solo(self->model, self->kind, index, !channel->solo);
		send_solo(self, index, row.selected_output, !channel->solo);
	}
}

static void
on_click_released(GtkGestureClick *gesture, int n_press, double x, double y,
	gpointer data)
{
	OscmixMixerRow *self = data;
	MixRowSnapshot row;
	MixChannelState *channel;
	double local_x;
	int index;

	(void)gesture;
	if (n_press != 2)
		return;
	index = channel_at(self, x, &local_x);
	if (index < 0)
		return;
	mix_model_snapshot_row(self->model, self->kind, &row);
	channel = &row.channels[index];
	if (y >= PAN_TOP && y <= PAN_TOP + PAN_HEIGHT &&
	    local_x >= 10.f && local_x <= 84.f) {
		mix_model_set_pan(self->model, self->kind, index, 0);
		send_pan(self, index, channel->volume, 0, row.selected_output);
	} else if (y >= FADER_TOP &&
	           y <= gtk_widget_get_height(GTK_WIDGET(self)) - FADER_BOTTOM &&
	           point_in_fader(local_x)) {
		mix_model_set_volume(self->model, self->kind, index, 0.f);
		send_volume(self, index, 0.f, channel->pan, row.selected_output);
	}
}

static void
on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data)
{
	OscmixMixerRow *self = data;
	MixRowSnapshot row;
	int index;
	double local_x;

	(void)gesture;
	self->drag_channel = -1;
	self->drag_mode = DRAG_NONE;
	index = channel_at(self, x, &local_x);
	if (index < 0)
		return;
	if (self->kind != MIX_CHANNEL_PLAYBACK &&
	    point_in_eq_button(local_x, y,
	                       gtk_widget_get_height(GTK_WIDGET(self))))
		return;
	if (y >= PAN_TOP && y <= PAN_TOP + PAN_HEIGHT &&
	    local_x >= 10.f && local_x <= 84.f)
		self->drag_mode = DRAG_PAN;
	else if (y >= FADER_TOP &&
	         y <= gtk_widget_get_height(GTK_WIDGET(self)) - FADER_BOTTOM &&
	         point_in_fader(local_x))
		self->drag_mode = DRAG_VOLUME;
	else
		return;
	mix_model_select_channel(self->model, self->kind, index);
	if (self->kind == MIX_CHANNEL_OUTPUT)
		mix_model_select_output(self->model, index);
	mix_model_snapshot_row(self->model, self->kind, &row);
	self->drag_channel = index;
	self->drag_start_volume = row.channels[index].volume;
	self->drag_start_pan = row.channels[index].pan;
	self->drag_volume = self->drag_start_volume;
	self->drag_pan = self->drag_start_pan;
	self->drag_last_x = 0.;
	self->drag_last_y = 0.;
}

static void
on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y,
	gpointer data)
{
	OscmixMixerRow *self = data;
	MixRowSnapshot row;
	double delta_x, delta_y;
	float volume;
	int pan;

	(void)gesture;
	if (self->drag_channel < 0)
		return;
	delta_x = offset_x - self->drag_last_x;
	delta_y = offset_y - self->drag_last_y;
	self->drag_last_x = offset_x;
	self->drag_last_y = offset_y;
	if (fabs(delta_x) > 96. || fabs(delta_y) > 96.)
		return;
	mix_model_snapshot_row(self->model, self->kind, &row);
	if (self->drag_mode == DRAG_PAN) {
		pan = CLAMP(self->drag_pan + lround(delta_x * 1.5), -100, 100);
		self->drag_pan = pan;
		mix_model_set_pan(self->model, self->kind, self->drag_channel, pan);
		send_pan(self, self->drag_channel,
		         row.channels[self->drag_channel].volume, pan, row.selected_output);
	} else if (self->drag_mode == DRAG_VOLUME) {
		volume = CLAMP(self->drag_volume - delta_y * .35f, -65.f, 6.f);
		self->drag_volume = volume;
		mix_model_set_volume(self->model, self->kind, self->drag_channel, volume);
		send_volume(self, self->drag_channel, volume,
		            row.channels[self->drag_channel].pan, row.selected_output);
	}
}

static void
on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y,
	gpointer data)
{
	OscmixMixerRow *self = data;

	(void)gesture;
	(void)offset_x;
	(void)offset_y;
	self->drag_channel = -1;
	self->drag_mode = DRAG_NONE;
}

static void
oscmix_mixer_row_dispose(GObject *object)
{
	OscmixMixerRow *self = OSCMIX_MIXER_ROW(object);
	int i;

	if (self->editor) {
		gtk_widget_unparent(self->editor);
		self->editor = NULL;
	}
	for (i = 0; i < OSCMIX_UFXP_OUTPUTS; ++i) {
		g_clear_object(&self->name_layouts[i]);
		g_clear_object(&self->db_layouts[i]);
		g_clear_object(&self->pan_layouts[i]);
	}
	g_clear_object(&self->mute_layout);
	g_clear_object(&self->solo_layout);
	g_clear_object(&self->eq_layout);
	g_clear_object(&self->dynamics_layout);
	g_clear_object(&self->stereo_layout);
	g_clear_object(&self->title_layout);
	for (i = 0; i < METER_SCALE_COUNT; ++i)
		g_clear_object(&self->meter_scale_layouts[i]);
	g_clear_object(&self->adjustment);
	G_OBJECT_CLASS(oscmix_mixer_row_parent_class)->dispose(object);
}

static void
oscmix_mixer_row_class_init(OscmixMixerRowClass *class)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);

	G_OBJECT_CLASS(class)->dispose = oscmix_mixer_row_dispose;
	widget_class->snapshot = oscmix_mixer_row_snapshot;
	widget_class->measure = oscmix_mixer_row_measure;
	widget_class->size_allocate = oscmix_mixer_row_size_allocate;
	gtk_widget_class_set_css_name(widget_class, "oscmix-mixer-row");
	signals[EQ_REQUESTED] =
		g_signal_new("eq-requested", G_TYPE_FROM_CLASS(class),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
	signals[DYNAMICS_REQUESTED] =
		g_signal_new("dynamics-requested", G_TYPE_FROM_CLASS(class),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
}

static void
oscmix_mixer_row_init(OscmixMixerRow *self)
{
	GtkEventController *scroll;
	GtkGesture *click, *drag;
	PangoFontDescription *meter_font, *stereo_font;
	int i;

	self->drag_channel = -1;
	self->editor_channel = -1;
	self->last_visible_count = -1;
	gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
	for (i = 0; i < OSCMIX_UFXP_OUTPUTS; ++i) {
		self->cached_db[i] = G_MININT;
		self->cached_pan[i] = G_MININT;
		self->display_levels[i] = -65.f;
		self->display_gain_reduction[i] = 0.f;
	}
	self->adjustment = gtk_adjustment_new(0, 0, 1, STRIP_WIDTH,
	                                      STRIP_WIDTH * 8, 1);
	g_object_ref_sink(self->adjustment);
	self->mute_layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), "M");
	self->solo_layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), "S");
	self->eq_layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), "EQ");
	self->dynamics_layout =
		gtk_widget_create_pango_layout(GTK_WIDGET(self), "Dyn");
	self->stereo_layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), "⚭");
	meter_font = pango_font_description_new();
	pango_font_description_set_absolute_size(meter_font, 8 * PANGO_SCALE);
	pango_font_description_set_weight(meter_font, PANGO_WEIGHT_SEMIBOLD);
	for (i = 0; i < METER_SCALE_COUNT; ++i) {
		self->meter_scale_layouts[i] =
			gtk_widget_create_pango_layout(GTK_WIDGET(self), meter_scale_labels[i]);
		pango_layout_set_font_description(self->meter_scale_layouts[i], meter_font);
	}
	pango_font_description_free(meter_font);
	stereo_font = pango_font_description_new();
	pango_font_description_set_absolute_size(stereo_font, 24 * PANGO_SCALE);
	pango_font_description_set_weight(stereo_font, PANGO_WEIGHT_SEMIBOLD);
	pango_layout_set_font_description(self->stereo_layout, stereo_font);
	pango_font_description_free(stereo_font);
	g_signal_connect(self->adjustment, "value-changed",
	                 G_CALLBACK(on_adjustment_changed), self);
	scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), self);
	gtk_widget_add_controller(GTK_WIDGET(self), scroll);
	click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
	g_signal_connect(click, "pressed", G_CALLBACK(on_click), self);
	g_signal_connect(click, "released", G_CALLBACK(on_click_released), self);
	gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
	drag = gtk_gesture_drag_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), self);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
	g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), self);
	gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag));
	gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick, NULL, NULL);
}

GtkWidget *
oscmix_mixer_row_new(MixModel *model, OscTransport *transport,
	MixChannelKind kind)
{
	OscmixMixerRow *self;

	self = g_object_new(OSCMIX_TYPE_MIXER_ROW, NULL);
	self->model = model;
	self->transport = transport;
	self->kind = kind;
	return GTK_WIDGET(self);
}

GtkAdjustment *
oscmix_mixer_row_get_adjustment(OscmixMixerRow *self)
{
	return self->adjustment;
}

void
oscmix_mixer_row_set_editor(OscmixMixerRow *self, GtkWidget *editor,
	int channel)
{
	g_return_if_fail(OSCMIX_IS_MIXER_ROW(self));
	g_return_if_fail(GTK_IS_WIDGET(editor));
	if (self->editor && self->editor != editor)
		gtk_widget_unparent(self->editor);
	self->editor = editor;
	self->editor_channel = channel;
	self->reveal_editor = TRUE;
	if (gtk_widget_get_parent(editor) != GTK_WIDGET(self)) {
		g_return_if_fail(gtk_widget_get_parent(editor) == NULL);
		gtk_widget_set_parent(editor, GTK_WIDGET(self));
	}
	gtk_widget_queue_resize(GTK_WIDGET(self));
	gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
oscmix_mixer_row_release_editor(OscmixMixerRow *self)
{
	g_return_if_fail(OSCMIX_IS_MIXER_ROW(self));
	if (!self->editor)
		return;
	gtk_widget_unparent(self->editor);
	self->editor = NULL;
	self->editor_channel = -1;
	gtk_widget_queue_resize(GTK_WIDGET(self));
	gtk_widget_queue_draw(GTK_WIDGET(self));
}
