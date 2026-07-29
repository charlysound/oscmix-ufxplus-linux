#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model.h"

extern const struct device ffufxp;

#define ROUTE_STATE_VERSION 1
#define CC_AUDIO_CHANNELS 24

struct MixModel {
	GMutex mutex;
	const struct device *device;
	MixChannelState channels[MIX_CHANNEL_COUNT][OSCMIX_UFXP_OUTPUTS];
	float *route_volume;
	int16_t *route_pan;
	gboolean *route_solo;
	int selected_output;
	MixChannelKind selected_kind;
	int selected_channel;
	gboolean connected;
	char device_id[32];
	gint32 backend_session;
	guint64 row_generation[MIX_CHANNEL_COUNT];
	guint64 control_generation;
	guint64 connection_generation;
	GHashTable *parameters;
	gint64 gr_updated_at[MIX_CHANNEL_COUNT][OSCMIX_UFXP_OUTPUTS];
};

static void
bump_all_rows_locked(MixModel *self)
{
	int kind;

	for (kind = 0; kind < MIX_CHANNEL_COUNT; ++kind)
		++self->row_generation[kind];
}

static void
store_parameter_locked(MixModel *self, const OscEvent *event)
{
	MixParamValue *value;

	if (event->n_args == 0)
		return;
	value = g_hash_table_lookup(self->parameters, event->address);
	if (!value) {
		value = g_new0(MixParamValue, 1);
		g_hash_table_insert(self->parameters, g_strdup(event->address), value);
	}
	value->type = event->args[0].type;
	if (value->type == 'i')
		value->i = event->args[0].value.i;
	else if (value->type == 'f')
		value->f = event->args[0].value.f;
	else if (value->type == 's')
		g_strlcpy(value->s, event->args[0].value.s, sizeof value->s);
}

static float
channel_parameter_locked(MixModel *self, MixChannelKind kind, int index,
	const char *leaf, float fallback)
{
	MixParamValue *value;
	char address[96];

	g_snprintf(address, sizeof address, "/%s/%d/%s",
	           mix_channel_kind_osc_name(kind), index + 1, leaf);
	value = g_hash_table_lookup(self->parameters, address);
	if (!value)
		return fallback;
	if (value->type == 'f')
		return value->f;
	if (value->type == 'i')
		return value->i;
	return fallback;
}

static float
follow_gain_reduction(float current_db, float target_db, gint64 elapsed_us,
	float attack_ms, float release_ms)
{
	float time_ms, coefficient;

	if (elapsed_us <= 0)
		return current_db;
	time_ms = target_db > current_db ? attack_ms : release_ms;
	if (time_ms <= 0.f)
		return target_db;
	coefficient = -expm1f(-(float)elapsed_us / (time_ms * 1000.f));
	return current_db + (target_db - current_db) * CLAMP(coefficient, 0.f, 1.f);
}

static int
kind_count(MixChannelKind kind)
{
	return kind == MIX_CHANNEL_INPUT ? OSCMIX_UFXP_INPUTS : OSCMIX_UFXP_OUTPUTS;
}

static size_t
route_index(MixChannelKind kind, int channel, int output)
{
	size_t source;

	source = channel;
	if (kind == MIX_CHANNEL_PLAYBACK)
		source += OSCMIX_UFXP_INPUTS;
	return (size_t)output * (OSCMIX_UFXP_INPUTS + OSCMIX_UFXP_PLAYBACKS) + source;
}

static void
sync_solo_pair_locked(MixModel *self, MixChannelKind kind, int left)
{
	gboolean enabled;
	int i;

	left &= ~1;
	if (kind == MIX_CHANNEL_OUTPUT) {
		if (left + 1 >= OSCMIX_UFXP_OUTPUTS)
			return;
		for (i = 0; i < OSCMIX_UFXP_INPUTS + OSCMIX_UFXP_PLAYBACKS; ++i) {
			size_t route_left = (size_t)left *
			                    (OSCMIX_UFXP_INPUTS + OSCMIX_UFXP_PLAYBACKS) + i;
			size_t route_right = (size_t)(left + 1) *
			                     (OSCMIX_UFXP_INPUTS + OSCMIX_UFXP_PLAYBACKS) + i;
			enabled = self->route_solo[route_left] || self->route_solo[route_right];
			self->route_solo[route_left] = enabled;
			self->route_solo[route_right] = enabled;
		}
		return;
	}
	if (left + 1 >= kind_count(kind))
		return;
	for (i = 0; i < OSCMIX_UFXP_OUTPUTS; ++i) {
		size_t route_left = route_index(kind, left, i);
		size_t route_right = route_index(kind, left + 1, i);

		enabled = self->route_solo[route_left] || self->route_solo[route_right];
		self->route_solo[route_left] = enabled;
		self->route_solo[route_right] = enabled;
	}
}

static float
arg_float(const OscEvent *event, guint index, float fallback)
{
	if (index >= event->n_args)
		return fallback;
	if (event->args[index].type == 'f')
		return event->args[index].value.f;
	if (event->args[index].type == 'i')
		return event->args[index].value.i;
	return fallback;
}

static int
arg_int(const OscEvent *event, guint index, int fallback)
{
	if (index >= event->n_args)
		return fallback;
	if (event->args[index].type == 'i')
		return event->args[index].value.i;
	if (event->args[index].type == 'f')
		return lroundf(event->args[index].value.f);
	return fallback;
}

static gboolean
parse_channel_address(const char *address, MixChannelKind *kind, int *index,
	const char **leaf)
{
	const char *pos;
	char *end;
	long value;

	if (g_str_has_prefix(address, "/input/")) {
		*kind = MIX_CHANNEL_INPUT;
		pos = address + strlen("/input/");
	} else if (g_str_has_prefix(address, "/playback/")) {
		*kind = MIX_CHANNEL_PLAYBACK;
		pos = address + strlen("/playback/");
	} else if (g_str_has_prefix(address, "/output/")) {
		*kind = MIX_CHANNEL_OUTPUT;
		pos = address + strlen("/output/");
	} else {
		return FALSE;
	}
	value = strtol(pos, &end, 10);
	if (end == pos || value < 1 || value > kind_count(*kind) || *end != '/')
		return FALSE;
	*index = (int)value - 1;
	*leaf = end + 1;
	return **leaf != '\0';
}

static gboolean
parse_mix_address(const char *address, int *output, MixChannelKind *kind,
	int *channel)
{
	char type[16], tail;
	int out, source;

	if (sscanf(address, "/mix/%d/%15[^/]/%d%c", &out, type, &source, &tail) != 3)
		return FALSE;
	if (out < 1 || out > OSCMIX_UFXP_OUTPUTS || source < 1 ||
	    source > OSCMIX_UFXP_INPUTS)
		return FALSE;
	if (strcmp(type, "input") == 0)
		*kind = MIX_CHANNEL_INPUT;
	else if (strcmp(type, "playback") == 0)
		*kind = MIX_CHANNEL_PLAYBACK;
	else
		return FALSE;
	*output = out - 1;
	*channel = source - 1;
	return TRUE;
}

static gboolean
parse_solo_address(const char *address, int *output, MixChannelKind *kind,
	int *channel)
{
	char type[16], tail;
	int out, source;

	if (sscanf(address, "/solo/%d/%15[^/]/%d%c", &out, type, &source, &tail) != 3)
		return FALSE;
	if (out < 1 || out > OSCMIX_UFXP_OUTPUTS || source < 1 ||
	    source > OSCMIX_UFXP_INPUTS)
		return FALSE;
	if (strcmp(type, "input") == 0)
		*kind = MIX_CHANNEL_INPUT;
	else if (strcmp(type, "playback") == 0)
		*kind = MIX_CHANNEL_PLAYBACK;
	else
		return FALSE;
	*output = out - 1;
	*channel = source - 1;
	return TRUE;
}

MixModel *
mix_model_new(void)
{
	MixModel *self;
	MixChannelState *channel;
	size_t routes, i;
	int kind, index;

	self = g_new0(MixModel, 1);
	g_mutex_init(&self->mutex);
	self->device = &ffufxp;
	self->parameters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->selected_kind = MIX_CHANNEL_INPUT;
	self->control_generation = 1;
	bump_all_rows_locked(self);
	routes = (OSCMIX_UFXP_INPUTS + OSCMIX_UFXP_PLAYBACKS) * OSCMIX_UFXP_OUTPUTS;
	self->route_volume = g_new(float, routes);
	self->route_pan = g_new0(int16_t, routes);
	self->route_solo = g_new0(gboolean, routes);
	for (i = 0; i < routes; ++i)
		self->route_volume[i] = -65.f;
	/* The UFX+ CC register dump does not include its mixer matrix.  Use the
	 * standard direct playback routing only until a saved application state
	 * is loaded or a real /mix event supersedes it.  This changes the display
	 * model only; it never writes a route to the device at startup. */
	for (index = 0; index < CC_AUDIO_CHANNELS; ++index)
		self->route_volume[route_index(MIX_CHANNEL_PLAYBACK, index, index)] = 0.f;
	for (kind = 0; kind < MIX_CHANNEL_COUNT; ++kind) {
		for (index = 0; index < kind_count(kind); ++index) {
			channel = &self->channels[kind][index];
			channel->volume = -65.f;
			channel->level = -65.f;
			if (kind == MIX_CHANNEL_INPUT) {
				g_strlcpy(channel->name, self->device->inputs[index].name,
				            sizeof channel->name);
				channel->flags = self->device->inputs[index].flags;
				channel->gain_min = self->device->inputs[index].gain.min;
				channel->gain_max = self->device->inputs[index].gain.max;
				channel->gain_gap_end = self->device->inputs[index].gain.instrument_min;
			} else {
				g_strlcpy(channel->name, self->device->outputs[index].name,
				            sizeof channel->name);
				if (kind == MIX_CHANNEL_OUTPUT)
					channel->flags = self->device->outputs[index].flags;
			}
		}
	}
	return self;
}

void
mix_model_free(MixModel *self)
{
	if (!self)
		return;
	g_mutex_clear(&self->mutex);
	g_free(self->route_volume);
	g_free(self->route_pan);
	g_free(self->route_solo);
	g_hash_table_unref(self->parameters);
	g_free(self);
}

gboolean
mix_model_load_routes(MixModel *self, const char *path, GError **error)
{
	GKeyFile *key_file;
	gdouble *volumes;
	gint *pans;
	gsize volume_count, pan_count;
	size_t routes, i;
	int version;
	gboolean loaded;

	key_file = g_key_file_new();
	loaded = g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, error);
	if (!loaded) {
		g_key_file_unref(key_file);
		return FALSE;
	}
	version = g_key_file_get_integer(key_file, "matrix", "version", error);
	if (error && *error)
		goto invalid;
	routes = (OSCMIX_UFXP_INPUTS + OSCMIX_UFXP_PLAYBACKS) * OSCMIX_UFXP_OUTPUTS;
	volumes = g_key_file_get_double_list(key_file, "matrix", "volumes",
	                                     &volume_count, error);
	if (!volumes)
		goto invalid;
	pans = g_key_file_get_integer_list(key_file, "matrix", "pans",
	                                   &pan_count, error);
	if (!pans) {
		g_free(volumes);
		goto invalid;
	}
	if (version != ROUTE_STATE_VERSION || volume_count != routes ||
	    pan_count != routes) {
		g_set_error_literal(error, G_KEY_FILE_ERROR,
		                    G_KEY_FILE_ERROR_INVALID_VALUE,
		                    "incompatible mixer matrix state");
		g_free(volumes);
		g_free(pans);
		goto invalid;
	}
	g_mutex_lock(&self->mutex);
	for (i = 0; i < routes; ++i) {
		self->route_volume[i] = CLAMP(volumes[i], -65.f, 6.f);
		self->route_pan[i] = CLAMP(pans[i], -100, 100);
	}
	bump_all_rows_locked(self);
	++self->control_generation;
	g_mutex_unlock(&self->mutex);
	g_free(volumes);
	g_free(pans);
	g_key_file_unref(key_file);
	return TRUE;

invalid:
	g_key_file_unref(key_file);
	return FALSE;
}

gboolean
mix_model_save_routes(MixModel *self, const char *path, GError **error)
{
	GKeyFile *key_file;
	gdouble *volumes;
	gint *pans;
	char *data;
	gsize data_size;
	size_t routes, i;
	gboolean saved;

	routes = (OSCMIX_UFXP_INPUTS + OSCMIX_UFXP_PLAYBACKS) * OSCMIX_UFXP_OUTPUTS;
	volumes = g_new(gdouble, routes);
	pans = g_new(gint, routes);
	g_mutex_lock(&self->mutex);
	for (i = 0; i < routes; ++i) {
		volumes[i] = self->route_volume[i];
		pans[i] = self->route_pan[i];
	}
	g_mutex_unlock(&self->mutex);
	key_file = g_key_file_new();
	g_key_file_set_integer(key_file, "matrix", "version", ROUTE_STATE_VERSION);
	g_key_file_set_double_list(key_file, "matrix", "volumes", volumes, routes);
	g_key_file_set_integer_list(key_file, "matrix", "pans", pans, routes);
	data = g_key_file_to_data(key_file, &data_size, NULL);
	saved = g_file_set_contents(path, data, data_size, error);
	g_free(data);
	g_key_file_unref(key_file);
	g_free(volumes);
	g_free(pans);
	return saved;
}

void
mix_model_apply_event(MixModel *self, const OscEvent *event)
{
	MixChannelState *channel;
	MixChannelKind kind;
	const char *leaf;
	size_t route;
	int index, output;
	float value;

	g_mutex_lock(&self->mutex);
	if (strcmp(event->address, "/device/id") == 0 && event->n_args > 0 &&
	    event->args[0].type == 's') {
		gboolean connected = strcmp(event->args[0].value.s, "ffufxp") == 0;

		if (strcmp(self->device_id, event->args[0].value.s) != 0 ||
		    self->connected != connected) {
			g_strlcpy(self->device_id, event->args[0].value.s,
			           sizeof self->device_id);
			self->connected = connected;
			++self->control_generation;
		}
		g_mutex_unlock(&self->mutex);
		return;
	}
	if (strcmp(event->address, "/device/session") == 0 && event->n_args > 0) {
		gint32 session = arg_int(event, 0, 0);

		if (session != self->backend_session) {
			self->backend_session = session;
			++self->connection_generation;
		}
		g_mutex_unlock(&self->mutex);
		return;
	}
	if (parse_mix_address(event->address, &output, &kind, &index)) {
		route = route_index(kind, index, output);
		self->route_volume[route] = CLAMP(arg_float(event, 0, -65.f), -65.f, 6.f);
		if (event->n_args > 1)
			self->route_pan[route] = CLAMP(arg_int(event, 1, 0), -100, 100);
		++self->row_generation[kind];
		++self->control_generation;
		g_mutex_unlock(&self->mutex);
		return;
	}
	if (parse_solo_address(event->address, &output, &kind, &index)) {
		if ((output & 1) && self->channels[MIX_CHANNEL_OUTPUT][output].stereo)
			--output;
		if ((index & 1) && self->channels[kind][index].stereo)
			--index;
		route = route_index(kind, index, output);
		self->route_solo[route] = arg_int(event, 0, FALSE) != 0;
		if (self->channels[kind][index].stereo && index + 1 < kind_count(kind))
			self->route_solo[route_index(kind, index + 1, output)] =
				arg_int(event, 0, FALSE) != 0;
		++self->row_generation[kind];
		++self->control_generation;
		g_mutex_unlock(&self->mutex);
		return;
	}
	if (!parse_channel_address(event->address, &kind, &index, &leaf)) {
		store_parameter_locked(self, event);
		++self->control_generation;
		g_mutex_unlock(&self->mutex);
		return;
	}
	channel = &self->channels[kind][index];
	if (strcmp(leaf, "name") == 0 && event->n_args > 0 && event->args[0].type == 's')
		g_strlcpy(channel->name, event->args[0].value.s, sizeof channel->name);
	else if (strcmp(leaf, "level") == 0) {
		float attack_ms, current_db, post_dsp, pre_dsp, release_ms;
		float target_db;
		gint64 elapsed_us, received_at;

		value = arg_float(event, 0, -65.f);
		channel->level = isfinite(value) ? CLAMP(value, -65.f, 6.f) : -65.f;
		/* The UFX+ CC stream contains pre/post-DSP peak values but, unlike the
		 * UCX II, does not publish a dedicated dynamics meter. Use the measured
		 * attenuation as a fallback target and follow it with the channel's real
		 * Attack and Release settings. This preserves parameter-dependent meter
		 * ballistics when a falling input makes the raw pre/post difference
		 * disappear before the compressor envelope has released. */
		if (!channel->gain_reduction_native && channel->dynamics &&
		    event->n_args >= 4) {
			pre_dsp = arg_float(event, 0, -INFINITY);
			post_dsp = arg_float(event, 2, -INFINITY);
			target_db = isfinite(pre_dsp) && isfinite(post_dsp) ?
			            CLAMP(pre_dsp - post_dsp, 0.f, 60.f) : 0.f;
			received_at = event->received_at > 0 ? event->received_at :
			              g_get_monotonic_time();
			if (self->gr_updated_at[kind][index] == 0) {
				channel->gain_reduction = target_db / 60.f;
			} else {
				elapsed_us = received_at - self->gr_updated_at[kind][index];
				attack_ms = CLAMP(channel_parameter_locked(
					self, kind, index, "dynamics/attack", 0.f), 0.f, 200.f);
				release_ms = CLAMP(channel_parameter_locked(
					self, kind, index, "dynamics/release", 100.f), 100.f, 999.f);
				current_db = channel->gain_reduction * 60.f;
				channel->gain_reduction = CLAMP(follow_gain_reduction(
					current_db, target_db, elapsed_us, attack_ms, release_ms) /
					60.f, 0.f, 1.f);
			}
			self->gr_updated_at[kind][index] = received_at;
		} else if (!channel->gain_reduction_native && !channel->dynamics) {
			channel->gain_reduction = 0.f;
			self->gr_updated_at[kind][index] = 0;
		}
	} else if (strcmp(leaf, "dynamics/meter") == 0) {
		/* Native meters are unsigned bytes, one per channel. */
		channel->gain_reduction =
			CLAMP(arg_int(event, 0, 0), 0, 255) / 255.f;
		channel->gain_reduction_native = TRUE;
	} else if (strcmp(leaf, "gain") == 0)
		channel->gain = arg_float(event, 0, channel->gain);
	else if (strcmp(leaf, "gainrange") == 0) {
		channel->gain_min = arg_int(event, 0, channel->gain_min);
		channel->gain_max = arg_int(event, 1, channel->gain_max);
	} else if (strcmp(leaf, "flags") == 0)
		channel->flags = arg_int(event, 0, channel->flags);
	else if (strcmp(leaf, "48v") == 0)
		channel->phantom = arg_int(event, 0, channel->phantom);
	else if (strcmp(leaf, "hi-z") == 0)
		channel->hiz = arg_int(event, 0, channel->hiz);
	else if (strcmp(leaf, "mute") == 0)
		channel->mute = arg_int(event, 0, channel->mute);
	else if (strcmp(leaf, "stereo") == 0) {
		gboolean stereo = arg_int(event, 0, channel->stereo) != 0;
		int left = index & ~1;
		self->channels[kind][left].stereo = stereo;
		if (left + 1 < kind_count(kind))
			self->channels[kind][left + 1].stereo = stereo;
		if (stereo)
			sync_solo_pair_locked(self, kind, left);
		if (stereo && self->selected_kind == kind && self->selected_channel == left + 1)
			self->selected_channel = left;
		if (stereo && kind == MIX_CHANNEL_OUTPUT && self->selected_output == left + 1)
			self->selected_output = left;
		bump_all_rows_locked(self);
	}
	else if (strcmp(leaf, "pan") == 0)
		channel->pan = CLAMP(arg_int(event, 0, channel->pan), -100, 100);
	else if (strcmp(leaf, "volume") == 0)
		channel->volume = CLAMP(arg_float(event, 0, channel->volume), -65.f, 6.f);
	else if (strcmp(leaf, "eq") == 0) {
		channel->eq = arg_int(event, 0, channel->eq) != 0;
		store_parameter_locked(self, event);
	}
	else if (strcmp(leaf, "dynamics") == 0) {
		channel->dynamics = arg_int(event, 0, channel->dynamics) != 0;
		if (!channel->dynamics) {
			channel->gain_reduction = 0.f;
			self->gr_updated_at[kind][index] = 0;
		}
		store_parameter_locked(self, event);
	}
	else {
		store_parameter_locked(self, event);
		++self->control_generation;
		g_mutex_unlock(&self->mutex);
		return;
	}
	++self->row_generation[kind];
	if (strcmp(leaf, "level") != 0 && strcmp(leaf, "dynamics/meter") != 0)
		++self->control_generation;
	g_mutex_unlock(&self->mutex);
}

void
mix_model_mark_disconnected(MixModel *self)
{
	int index, kind;

	g_mutex_lock(&self->mutex);
	if (!self->connected && self->device_id[0] == '\0' &&
	    self->backend_session == 0) {
		g_mutex_unlock(&self->mutex);
		return;
	}
	self->connected = FALSE;
	self->device_id[0] = '\0';
	self->backend_session = 0;
	++self->connection_generation;
	for (kind = 0; kind < MIX_CHANNEL_COUNT; ++kind) {
		for (index = 0; index < kind_count(kind); ++index) {
			self->channels[kind][index].gain_reduction = 0.f;
			self->channels[kind][index].gain_reduction_native = FALSE;
			self->gr_updated_at[kind][index] = 0;
		}
		++self->row_generation[kind];
	}
	++self->control_generation;
	g_mutex_unlock(&self->mutex);
}

void
mix_model_snapshot_row(MixModel *self, MixChannelKind kind, MixRowSnapshot *snapshot)
{
	int i;

	g_mutex_lock(&self->mutex);
	memset(snapshot, 0, sizeof *snapshot);
	snapshot->count = kind_count(kind);
	snapshot->selected_output = self->selected_output;
	snapshot->selected_kind = self->selected_kind;
	snapshot->selected_channel = self->selected_channel;
	snapshot->connected = self->connected;
	snapshot->generation = self->row_generation[kind];
	memcpy(snapshot->channels, self->channels[kind],
	       sizeof snapshot->channels[0] * snapshot->count);
	if (kind != MIX_CHANNEL_OUTPUT) {
		for (i = 0; i < snapshot->count; ++i) {
			snapshot->channels[i].volume =
				self->route_volume[route_index(kind, i, self->selected_output)];
			snapshot->channels[i].pan =
				self->route_pan[route_index(kind, i, self->selected_output)];
			snapshot->channels[i].solo =
				self->route_solo[route_index(kind, i, self->selected_output)];
		}
		for (i = 0; i < OSCMIX_UFXP_INPUTS + OSCMIX_UFXP_PLAYBACKS; ++i) {
			if (self->route_solo[(size_t)self->selected_output *
			                     (OSCMIX_UFXP_INPUTS + OSCMIX_UFXP_PLAYBACKS) + i]) {
				snapshot->any_solo = TRUE;
				break;
			}
		}
	}
	g_mutex_unlock(&self->mutex);
}

void
mix_model_snapshot_selected(MixModel *self, MixChannelKind *kind, int *index,
	MixChannelState *channel, int *selected_output, gboolean *connected,
	char *device_id, gsize device_id_size)
{
	g_mutex_lock(&self->mutex);
	*kind = self->selected_kind;
	*index = self->selected_channel;
	*selected_output = self->selected_output;
	*connected = self->connected;
	*channel = self->channels[*kind][*index];
	if (*kind != MIX_CHANNEL_OUTPUT) {
		channel->volume = self->route_volume[route_index(*kind, *index, *selected_output)];
		channel->pan = self->route_pan[route_index(*kind, *index, *selected_output)];
		channel->solo = self->route_solo[route_index(*kind, *index, *selected_output)];
	}
	g_strlcpy(device_id, self->device_id, device_id_size);
	g_mutex_unlock(&self->mutex);
}

int
mix_model_channel_count(MixModel *self, MixChannelKind kind)
{
	(void)self;
	return kind_count(kind);
}

void
mix_model_select_channel(MixModel *self, MixChannelKind kind, int index)
{
	g_mutex_lock(&self->mutex);
	if (index >= 0 && index < kind_count(kind)) {
		if ((index & 1) && self->channels[kind][index].stereo)
			--index;
		self->selected_kind = kind;
		self->selected_channel = index;
		bump_all_rows_locked(self);
		++self->control_generation;
	}
	g_mutex_unlock(&self->mutex);
}

void
mix_model_select_output(MixModel *self, int index)
{
	g_mutex_lock(&self->mutex);
	if (index >= 0 && index < OSCMIX_UFXP_OUTPUTS) {
		if ((index & 1) && self->channels[MIX_CHANNEL_OUTPUT][index].stereo)
			--index;
		self->selected_output = index;
		bump_all_rows_locked(self);
		++self->control_generation;
	}
	g_mutex_unlock(&self->mutex);
}

void
mix_model_set_volume(MixModel *self, MixChannelKind kind, int index, float volume)
{
	g_mutex_lock(&self->mutex);
	volume = CLAMP(volume, -65.f, 6.f);
	if (kind == MIX_CHANNEL_OUTPUT)
		self->channels[kind][index].volume = volume;
	else
		self->route_volume[route_index(kind, index, self->selected_output)] = volume;
	++self->row_generation[kind];
	++self->control_generation;
	g_mutex_unlock(&self->mutex);
}

void
mix_model_set_pan(MixModel *self, MixChannelKind kind, int index, int pan)
{
	g_mutex_lock(&self->mutex);
	pan = CLAMP(pan, -100, 100);
	if (kind == MIX_CHANNEL_OUTPUT)
		self->channels[kind][index].pan = pan;
	else
		self->route_pan[route_index(kind, index, self->selected_output)] = pan;
	++self->row_generation[kind];
	++self->control_generation;
	g_mutex_unlock(&self->mutex);
}

void
mix_model_set_mute(MixModel *self, MixChannelKind kind, int index, gboolean mute)
{
	g_mutex_lock(&self->mutex);
	if ((index & 1) && self->channels[kind][index].stereo)
		--index;
	self->channels[kind][index].mute = !!mute;
	if (self->channels[kind][index].stereo && index + 1 < kind_count(kind))
		self->channels[kind][index + 1].mute = !!mute;
	++self->row_generation[kind];
	++self->control_generation;
	g_mutex_unlock(&self->mutex);
}

void
mix_model_set_solo(MixModel *self, MixChannelKind kind, int index, gboolean solo)
{
	size_t route;

	if (kind == MIX_CHANNEL_OUTPUT)
		return;
	g_mutex_lock(&self->mutex);
	if ((index & 1) && self->channels[kind][index].stereo)
		--index;
	route = route_index(kind, index, self->selected_output);
	self->route_solo[route] = !!solo;
	if (self->channels[kind][index].stereo && index + 1 < kind_count(kind))
		self->route_solo[route_index(kind, index + 1, self->selected_output)] = !!solo;
	++self->row_generation[kind];
	++self->control_generation;
	g_mutex_unlock(&self->mutex);
}

void
mix_model_set_stereo(MixModel *self, MixChannelKind kind, int index, gboolean stereo)
{
	g_mutex_lock(&self->mutex);
	index &= ~1;
	self->channels[kind][index].stereo = !!stereo;
	if (index + 1 < kind_count(kind))
		self->channels[kind][index + 1].stereo = !!stereo;
	if (stereo)
		sync_solo_pair_locked(self, kind, index);
	if (stereo && self->selected_kind == kind && self->selected_channel == index + 1)
		self->selected_channel = index;
	if (stereo && kind == MIX_CHANNEL_OUTPUT && self->selected_output == index + 1)
		self->selected_output = index;
	bump_all_rows_locked(self);
	++self->control_generation;
	g_mutex_unlock(&self->mutex);
}

void
mix_model_set_gain(MixModel *self, int index, float gain)
{
	g_mutex_lock(&self->mutex);
	self->channels[MIX_CHANNEL_INPUT][index].gain = gain;
	++self->control_generation;
	g_mutex_unlock(&self->mutex);
}

void
mix_model_set_phantom(MixModel *self, int index, gboolean phantom)
{
	g_mutex_lock(&self->mutex);
	self->channels[MIX_CHANNEL_INPUT][index].phantom = !!phantom;
	++self->control_generation;
	g_mutex_unlock(&self->mutex);
}

void
mix_model_set_hiz(MixModel *self, int index, gboolean hiz)
{
	g_mutex_lock(&self->mutex);
	self->channels[MIX_CHANNEL_INPUT][index].hiz = !!hiz;
	++self->control_generation;
	g_mutex_unlock(&self->mutex);
}

gboolean
mix_model_get_parameter(MixModel *self, const char *address, MixParamValue *value)
{
	MixParamValue *stored;
	gboolean found;

	g_mutex_lock(&self->mutex);
	stored = g_hash_table_lookup(self->parameters, address);
	found = stored != NULL;
	if (found)
		*value = *stored;
	g_mutex_unlock(&self->mutex);
	return found;
}

guint64
mix_model_control_generation(MixModel *self)
{
	guint64 generation;

	g_mutex_lock(&self->mutex);
	generation = self->control_generation;
	g_mutex_unlock(&self->mutex);
	return generation;
}

guint64
mix_model_connection_generation(MixModel *self)
{
	guint64 generation;

	g_mutex_lock(&self->mutex);
	generation = self->connection_generation;
	g_mutex_unlock(&self->mutex);
	return generation;
}

void
mix_model_set_parameter_int(MixModel *self, const char *address, int integer)
{
	OscEvent event = {.address = address, .n_args = 1};

	event.args[0].type = 'i';
	event.args[0].value.i = integer;
	mix_model_apply_event(self, &event);
}

void
mix_model_set_parameter_float(MixModel *self, const char *address, float number)
{
	OscEvent event = {.address = address, .n_args = 1};

	event.args[0].type = 'f';
	event.args[0].value.f = number;
	mix_model_apply_event(self, &event);
}

const char *
mix_channel_kind_name(MixChannelKind kind)
{
	static const char *const names[] = {"Inputs", "Playback", "Outputs"};
	return kind >= 0 && kind < MIX_CHANNEL_COUNT ? names[kind] : "";
}

const char *
mix_channel_kind_singular_name(MixChannelKind kind)
{
	static const char *const names[] = {"Input", "Playback", "Output"};
	return kind >= 0 && kind < MIX_CHANNEL_COUNT ? names[kind] : "";
}

const char *
mix_channel_kind_osc_name(MixChannelKind kind)
{
	static const char *const names[] = {"input", "playback", "output"};
	return kind >= 0 && kind < MIX_CHANNEL_COUNT ? names[kind] : "";
}

gboolean
mix_row_format_channel_name(const MixRowSnapshot *row, int index,
	char *buffer, gsize buffer_size)
{
	const char *left_name, *right_name, *left_suffix, *right_suffix;
	gsize left_prefix, right_prefix;
	int left;
	gboolean paired;

	g_return_val_if_fail(row != NULL, FALSE);
	g_return_val_if_fail(buffer != NULL, FALSE);
	if (buffer_size == 0)
		return FALSE;
	buffer[0] = '\0';
	if (index < 0 || index >= row->count)
		return FALSE;

	left = index & ~1;
	paired = left + 1 < row->count &&
	         row->channels[left].stereo && row->channels[left + 1].stereo;
	if (!paired) {
		g_strlcpy(buffer, row->channels[index].name, buffer_size);
		return FALSE;
	}

	left_name = row->channels[left].name;
	right_name = row->channels[left + 1].name;
	if (strcmp(left_name, right_name) == 0) {
		g_strlcpy(buffer, left_name, buffer_size);
		return TRUE;
	}
	left_suffix = strrchr(left_name, ' ');
	right_suffix = strrchr(right_name, ' ');
	left_prefix = left_suffix ? (gsize)(left_suffix - left_name) : 0;
	right_prefix = right_suffix ? (gsize)(right_suffix - right_name) : 0;
	if (left_suffix && right_suffix && left_prefix == right_prefix &&
	    strncmp(left_name, right_name, left_prefix) == 0) {
		g_snprintf(buffer, buffer_size, "%.*s %s/%s", (int)left_prefix,
		           left_name, left_suffix + 1, right_suffix + 1);
	} else {
		g_snprintf(buffer, buffer_size, "%s/%s", left_name, right_name);
	}
	return TRUE;
}
