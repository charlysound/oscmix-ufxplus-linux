#ifndef OSCMIX_GTK4_MODEL_H
#define OSCMIX_GTK4_MODEL_H

#include <glib.h>
#include "../device.h"

#define OSCMIX_UFXP_INPUTS 94
#define OSCMIX_UFXP_PLAYBACKS 94
#define OSCMIX_UFXP_OUTPUTS 94
#define OSCMIX_OSC_MAX_ARGS 8

typedef enum {
	MIX_CHANNEL_INPUT,
	MIX_CHANNEL_PLAYBACK,
	MIX_CHANNEL_OUTPUT,
	MIX_CHANNEL_COUNT
} MixChannelKind;

typedef struct {
	char type;
	union {
		gint32 i;
		float f;
		const char *s;
	} value;
} OscArg;

typedef struct {
	const char *address;
	guint n_args;
	/* Monotonic receive time, in microseconds. Zero lets local callers use now. */
	gint64 received_at;
	OscArg args[OSCMIX_OSC_MAX_ARGS];
} OscEvent;

typedef struct {
	char type;
	gint32 i;
	float f;
	char s[96];
} MixParamValue;

typedef struct {
	char name[32];
	float volume;
	float level;
	/* Normalized 0..1 dynamics gain reduction. */
	float gain_reduction;
	float gain;
	int pan;
	int gain_min;
	int gain_max;
	int gain_gap_end;
	unsigned flags;
	gboolean mute;
	gboolean solo;
	gboolean stereo;
	gboolean phantom;
	gboolean hiz;
	gboolean eq;
	gboolean dynamics;
	gboolean gain_reduction_native;
} MixChannelState;

typedef struct {
	int count;
	int selected_output;
	int selected_kind;
	int selected_channel;
	gboolean connected;
	gboolean any_solo;
	guint64 generation;
	MixChannelState channels[OSCMIX_UFXP_OUTPUTS];
} MixRowSnapshot;

typedef struct MixModel MixModel;

MixModel *mix_model_new(void);
void mix_model_free(MixModel *self);
gboolean mix_model_load_routes(MixModel *self, const char *path, GError **error);
gboolean mix_model_save_routes(MixModel *self, const char *path, GError **error);

void mix_model_apply_event(MixModel *self, const OscEvent *event);
void mix_model_mark_disconnected(MixModel *self);
void mix_model_snapshot_row(MixModel *self, MixChannelKind kind, MixRowSnapshot *snapshot);
void mix_model_snapshot_selected(MixModel *self, MixChannelKind *kind, int *index,
	MixChannelState *channel, int *selected_output, gboolean *connected,
	char *device_id, gsize device_id_size);

int mix_model_channel_count(MixModel *self, MixChannelKind kind);
void mix_model_select_channel(MixModel *self, MixChannelKind kind, int index);
void mix_model_select_output(MixModel *self, int index);
void mix_model_set_volume(MixModel *self, MixChannelKind kind, int index, float volume);
void mix_model_set_pan(MixModel *self, MixChannelKind kind, int index, int pan);
void mix_model_set_mute(MixModel *self, MixChannelKind kind, int index, gboolean mute);
void mix_model_set_solo(MixModel *self, MixChannelKind kind, int index, gboolean solo);
void mix_model_set_stereo(MixModel *self, MixChannelKind kind, int index, gboolean stereo);
void mix_model_set_gain(MixModel *self, int index, float gain);
void mix_model_set_phantom(MixModel *self, int index, gboolean phantom);
void mix_model_set_hiz(MixModel *self, int index, gboolean hiz);
gboolean mix_model_get_parameter(MixModel *self, const char *address,
	MixParamValue *value);
guint64 mix_model_control_generation(MixModel *self);
guint64 mix_model_connection_generation(MixModel *self);
void mix_model_set_parameter_int(MixModel *self, const char *address, int value);
void mix_model_set_parameter_float(MixModel *self, const char *address, float value);

const char *mix_channel_kind_name(MixChannelKind kind);
const char *mix_channel_kind_singular_name(MixChannelKind kind);
const char *mix_channel_kind_osc_name(MixChannelKind kind);
gboolean mix_row_format_channel_name(const MixRowSnapshot *row, int index,
	char *buffer, gsize buffer_size);

#endif
