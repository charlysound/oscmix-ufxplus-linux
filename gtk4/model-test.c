#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include "model.h"

static OscEvent
event_float(const char *address, float value)
{
	OscEvent event = {.address = address, .n_args = 1};

	event.args[0].type = 'f';
	event.args[0].value.f = value;
	return event;
}

static OscEvent
event_int(const char *address, int value)
{
	OscEvent event = {.address = address, .n_args = 1};

	event.args[0].type = 'i';
	event.args[0].value.i = value;
	return event;
}

static void
test_ufxp_model(void)
{
	MixModel *model;
	MixRowSnapshot row;
	OscEvent event;
	MixParamValue parameter;
	guint64 generation;

	model = mix_model_new();
	g_assert_cmpint(mix_model_channel_count(model, MIX_CHANNEL_INPUT), ==, 94);
	g_assert_cmpint(mix_model_channel_count(model, MIX_CHANNEL_PLAYBACK), ==, 94);
	g_assert_cmpint(mix_model_channel_count(model, MIX_CHANNEL_OUTPUT), ==, 94);
	mix_model_snapshot_row(model, MIX_CHANNEL_PLAYBACK, &row);
	g_assert_cmpfloat(row.channels[0].volume, ==, 0.f);
	g_assert_cmpfloat(row.channels[1].volume, ==, -65.f);

	event = event_float("/input/12/gain", 75.f);
	mix_model_apply_event(model, &event);
	event = event_int("/input/12/48v", 1);
	mix_model_apply_event(model, &event);
	event = event_int("/input/12/hi-z", 1);
	mix_model_apply_event(model, &event);
	mix_model_snapshot_row(model, MIX_CHANNEL_INPUT, &row);
	g_assert_cmpfloat(row.channels[11].gain, ==, 75.f);
	g_assert_true(row.channels[11].phantom);
	g_assert_true(row.channels[11].hiz);
	g_assert_cmpint(row.channels[11].gain_max, ==, 75);
	g_assert_cmpint(row.channels[11].gain_gap_end, ==, 8);

	event = event_int("/input/12/dynamics", 1);
	mix_model_apply_event(model, &event);
	event = event_int("/input/12/dynamics/attack", 0);
	mix_model_apply_event(model, &event);
	event = event_int("/input/12/dynamics/release", 100);
	mix_model_apply_event(model, &event);
	generation = mix_model_control_generation(model);
	memset(&event, 0, sizeof event);
	event.address = "/input/12/level";
	event.received_at = 1000000;
	event.n_args = 4;
	event.args[0].type = 'f';
	event.args[0].value.f = -18.f;
	event.args[1].type = 'f';
	event.args[1].value.f = -18.f;
	event.args[2].type = 'f';
	event.args[2].value.f = -27.f;
	event.args[3].type = 'f';
	event.args[3].value.f = -27.f;
	mix_model_apply_event(model, &event);
	g_assert_cmpuint(mix_model_control_generation(model), ==, generation);
	g_assert_false(mix_model_get_parameter(model, "/input/12/level", &parameter));
	mix_model_snapshot_row(model, MIX_CHANNEL_INPUT, &row);
	g_assert_cmpfloat_with_epsilon(row.channels[11].gain_reduction, .15f, .0001f);

	/* The UFX+ fallback must release with the configured channel time rather
	 * than an unrelated fixed UI rate. After one 100 ms time constant, 9 dB
	 * has decayed to 9/e dB. */
	event.args[2].value.f = -18.f;
	event.received_at = 1100000;
	mix_model_apply_event(model, &event);
	mix_model_snapshot_row(model, MIX_CHANNEL_INPUT, &row);
	g_assert_cmpfloat_with_epsilon(row.channels[11].gain_reduction,
	                               (9.f / expf(1.f)) / 60.f, .0001f);

	/* A 999 ms Release holds substantially more GR over the same interval. */
	event = event_int("/input/12/dynamics/release", 999);
	mix_model_apply_event(model, &event);
	event.address = "/input/12/level";
	event.n_args = 4;
	event.received_at = 2000000;
	event.args[0].type = event.args[1].type = 'f';
	event.args[2].type = event.args[3].type = 'f';
	event.args[0].value.f = event.args[1].value.f = -18.f;
	event.args[2].value.f = event.args[3].value.f = -27.f;
	mix_model_apply_event(model, &event);
	event.received_at = 2100000;
	event.args[2].value.f = event.args[3].value.f = -18.f;
	mix_model_apply_event(model, &event);
	mix_model_snapshot_row(model, MIX_CHANNEL_INPUT, &row);
	g_assert_cmpfloat_with_epsilon(row.channels[11].gain_reduction,
	                               (9.f * expf(-100.f / 999.f)) / 60.f,
	                               .0001f);

	/* Attack uses the same per-channel envelope. */
	event = event_int("/input/13/dynamics", 1);
	mix_model_apply_event(model, &event);
	event = event_int("/input/13/dynamics/attack", 200);
	mix_model_apply_event(model, &event);
	memset(&event, 0, sizeof event);
	event.address = "/input/13/level";
	event.n_args = 4;
	event.received_at = 3000000;
	event.args[0].type = event.args[1].type = 'f';
	event.args[2].type = event.args[3].type = 'f';
	event.args[0].value.f = event.args[1].value.f = -18.f;
	event.args[2].value.f = event.args[3].value.f = -18.f;
	mix_model_apply_event(model, &event);
	event.received_at = 3100000;
	event.args[2].value.f = event.args[3].value.f = -30.f;
	mix_model_apply_event(model, &event);
	mix_model_snapshot_row(model, MIX_CHANNEL_INPUT, &row);
	g_assert_cmpfloat_with_epsilon(row.channels[12].gain_reduction,
	                               (12.f * (1.f - expf(-.5f))) / 60.f,
	                               .0001f);

	generation = mix_model_control_generation(model);
	event = event_int("/input/12/dynamics/meter", 128);
	mix_model_apply_event(model, &event);
	g_assert_cmpuint(mix_model_control_generation(model), ==, generation);
	mix_model_snapshot_row(model, MIX_CHANNEL_INPUT, &row);
	g_assert_cmpfloat_with_epsilon(row.channels[11].gain_reduction,
	                               128.f / 255.f, .0001f);
	g_assert_true(row.channels[11].gain_reduction_native);

	/* Hardware outputs carry the same pre/post-DSP peak pair. Keep their GR
	 * data available to both the output strips and the Dynamics editors. */
	event = event_int("/output/2/dynamics", 1);
	mix_model_apply_event(model, &event);
	memset(&event, 0, sizeof event);
	event.address = "/output/2/level";
	event.n_args = 4;
	event.args[0].type = 'f';
	event.args[0].value.f = -9.f;
	event.args[1].type = 'f';
	event.args[1].value.f = -12.f;
	event.args[2].type = 'f';
	event.args[2].value.f = -21.f;
	event.args[3].type = 'f';
	event.args[3].value.f = -24.f;
	mix_model_apply_event(model, &event);
	mix_model_snapshot_row(model, MIX_CHANNEL_OUTPUT, &row);
	g_assert_true(row.channels[1].dynamics);
	g_assert_cmpfloat_with_epsilon(row.channels[1].level, -9.f, .0001f);
	g_assert_cmpfloat_with_epsilon(row.channels[1].gain_reduction, .2f, .0001f);

	mix_model_select_output(model, 93);
	memset(&event, 0, sizeof event);
	event.address = "/mix/94/input/12";
	event.n_args = 2;
	event.args[0].type = 'f';
	event.args[0].value.f = -3.5f;
	event.args[1].type = 'i';
	event.args[1].value.i = 27;
	mix_model_apply_event(model, &event);
	mix_model_snapshot_row(model, MIX_CHANNEL_INPUT, &row);
	g_assert_cmpfloat(row.channels[11].volume, ==, -3.5f);
	g_assert_cmpint(row.channels[11].pan, ==, 27);
	g_assert_cmpint(row.selected_output, ==, 93);

	/* Playback submix echoes use their own namespace.  They must replace
	 * the startup -infinity default just like physical-input sends do. */
	mix_model_select_output(model, 0);
	memset(&event, 0, sizeof event);
	event.address = "/mix/1/playback/5";
	event.n_args = 2;
	event.args[0].type = 'f';
	event.args[0].value.f = -7.25f;
	event.args[1].type = 'i';
	event.args[1].value.i = -14;
	mix_model_apply_event(model, &event);
	mix_model_snapshot_row(model, MIX_CHANNEL_PLAYBACK, &row);
	g_assert_cmpfloat(row.channels[4].volume, ==, -7.25f);
	g_assert_cmpint(row.channels[4].pan, ==, -14);
	g_assert_cmpint(row.selected_output, ==, 0);
	mix_model_select_output(model, 93);

	/* Pairing preserves a Solo that was set on either member beforehand. */
	event = event_int("/solo/94/input/12", 1);
	mix_model_apply_event(model, &event);
	event = event_int("/input/12/stereo", 1);
	mix_model_apply_event(model, &event);
	mix_model_select_channel(model, MIX_CHANNEL_INPUT, 11);
	mix_model_snapshot_row(model, MIX_CHANNEL_INPUT, &row);
	g_assert_true(row.channels[10].stereo);
	g_assert_true(row.channels[11].stereo);
	g_assert_cmpint(row.selected_channel, ==, 10);
	g_assert_true(row.channels[10].solo);
	g_assert_true(row.channels[11].solo);
	g_assert_true(row.any_solo);
	mix_model_set_solo(model, MIX_CHANNEL_INPUT, 10, FALSE);
	mix_model_snapshot_row(model, MIX_CHANNEL_INPUT, &row);
	g_assert_false(row.channels[10].solo);
	g_assert_false(row.channels[11].solo);
	g_assert_false(row.any_solo);

	event = event_float("/input/12/eq/band1gain", 4.5f);
	mix_model_apply_event(model, &event);
	g_assert_true(mix_model_get_parameter(model, "/input/12/eq/band1gain", &parameter));
	g_assert_cmpint(parameter.type, ==, 'f');
	g_assert_cmpfloat(parameter.f, ==, 4.5f);
	event = event_int("/input/12/eq", 1);
	mix_model_apply_event(model, &event);
	event = event_int("/input/12/dynamics", 1);
	mix_model_apply_event(model, &event);
	mix_model_snapshot_row(model, MIX_CHANNEL_INPUT, &row);
	g_assert_true(row.channels[11].eq);
	g_assert_true(row.channels[11].dynamics);
	g_assert_true(mix_model_get_parameter(model, "/input/12/eq", &parameter));
	g_assert_true(mix_model_get_parameter(model, "/input/12/dynamics", &parameter));
	mix_model_set_parameter_int(model, "/clock/source", 3);
	g_assert_true(mix_model_get_parameter(model, "/clock/source", &parameter));
	g_assert_cmpint(parameter.type, ==, 'i');
	g_assert_cmpint(parameter.i, ==, 3);

	mix_model_free(model);
}

static void
test_channel_display_names(void)
{
	MixRowSnapshot row = {.count = 6};
	char name[72];

	g_strlcpy(row.channels[0].name, "Analog 1", sizeof row.channels[0].name);
	g_strlcpy(row.channels[1].name, "Analog 2", sizeof row.channels[1].name);
	row.channels[0].stereo = row.channels[1].stereo = TRUE;
	g_assert_true(mix_row_format_channel_name(&row, 0, name, sizeof name));
	g_assert_cmpstr(name, ==, "Analog 1/2");
	g_assert_true(mix_row_format_channel_name(&row, 1, name, sizeof name));
	g_assert_cmpstr(name, ==, "Analog 1/2");

	g_strlcpy(row.channels[2].name, "AES L", sizeof row.channels[2].name);
	g_strlcpy(row.channels[3].name, "AES R", sizeof row.channels[3].name);
	row.channels[2].stereo = row.channels[3].stereo = TRUE;
	g_assert_true(mix_row_format_channel_name(&row, 2, name, sizeof name));
	g_assert_cmpstr(name, ==, "AES L/R");

	g_strlcpy(row.channels[4].name, "Vocal", sizeof row.channels[4].name);
	g_strlcpy(row.channels[5].name, "Guitar", sizeof row.channels[5].name);
	row.channels[4].stereo = row.channels[5].stereo = TRUE;
	g_assert_true(mix_row_format_channel_name(&row, 4, name, sizeof name));
	g_assert_cmpstr(name, ==, "Vocal/Guitar");
	row.channels[4].stereo = row.channels[5].stereo = FALSE;
	g_assert_false(mix_row_format_channel_name(&row, 5, name, sizeof name));
	g_assert_cmpstr(name, ==, "Guitar");
}

static void
test_route_persistence(void)
{
	MixModel *saved, *restored;
	MixRowSnapshot row;
	GError *error = NULL;
	char *directory, *path;

	directory = g_dir_make_tmp("oscmix-model-XXXXXX", &error);
	g_assert_no_error(error);
	g_assert_nonnull(directory);
	path = g_build_filename(directory, "matrix.ini", NULL);
	saved = mix_model_new();
	mix_model_select_output(saved, 2);
	mix_model_set_volume(saved, MIX_CHANNEL_PLAYBACK, 4, -12.5f);
	mix_model_set_pan(saved, MIX_CHANNEL_PLAYBACK, 4, -33);
	g_assert_true(mix_model_save_routes(saved, path, &error));
	g_assert_no_error(error);
	mix_model_free(saved);

	restored = mix_model_new();
	g_assert_true(mix_model_load_routes(restored, path, &error));
	g_assert_no_error(error);
	mix_model_select_output(restored, 2);
	mix_model_snapshot_row(restored, MIX_CHANNEL_PLAYBACK, &row);
	g_assert_cmpfloat(row.channels[4].volume, ==, -12.5f);
	g_assert_cmpint(row.channels[4].pan, ==, -33);
	mix_model_free(restored);

	g_assert_cmpint(g_remove(path), ==, 0);
	g_assert_cmpint(g_rmdir(directory), ==, 0);
	g_free(path);
	g_free(directory);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/model/ufxp", test_ufxp_model);
	g_test_add_func("/model/channel-display-names", test_channel_display_names);
	g_test_add_func("/model/route-persistence", test_route_persistence);
	return g_test_run();
}
