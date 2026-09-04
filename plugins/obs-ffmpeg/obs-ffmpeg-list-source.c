/*
 * Copyright (c) 2015 John R. Bradley <jrb@turrettech.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <obs-module.h>

#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>

#include "obs-ffmpeg-compat.h"
#include "obs-ffmpeg-formats.h"

#include <media-playback/media-playback.h>

#define FF_LOG_S(source, level, format, ...)        \
	blog(level, "[Media List Source '%s']: " format, \
	     obs_source_get_name(source), ##__VA_ARGS__)
#define FF_BLOG(level, format, ...) \
	FF_LOG_S(s->source, level, format, ##__VA_ARGS__)

#define T_(text) obs_module_text("MediaList." text)
#define T_FILES T_("Files")
#define T_CURRENT_FILE_NAME T_("CurrentFileName")
#define S_PLAYLIST "playlist"
#define S_CURRENT_FILE_NAME "current_file_name"


struct media_file_data {
	char *path;
	obs_source_t *source;
};

typedef DARRAY(struct media_file_data) media_file_array_t;

enum behavior {
	BEHAVIOR_STOP_RESTART,
	BEHAVIOR_PAUSE_UNPAUSE,
	BEHAVIOR_ALWAYS_PLAY,
};

struct ffmpeg_list_source {
	media_playback_t *media;
	media_playback_t *tmp_media;
	bool destroy_media;

	enum video_range_type range;
	bool is_linear_alpha;
	obs_source_t *source;
	obs_hotkey_id hotkey;

	pthread_mutex_t mutex;
	media_file_array_t files;
	enum behavior behavior;
	size_t cur_item;
	bool is_stop;

	char *input;
	char *input_format;
	char *ffmpeg_options;
	int buffering_mb;
	int speed_percent;
	bool is_looping;
	bool is_hw_decoding;
	bool full_decode;
	bool is_clear_on_media_end;
	bool restart_on_activate;
	bool close_when_inactive;
	bool is_stinger;
	bool is_track_matte;
	bool log_changes;

	enum obs_media_state state;
	obs_hotkey_pair_id play_pause_hotkey;
	obs_hotkey_id stop_hotkey;

	bool is_get_first_frame;
	uint32_t frame_width;
	uint32_t frame_height;
};

static void set_media_state(void *data, enum obs_media_state state)
{
	struct ffmpeg_list_source *s = data;
	s->state = state;
}

static void ffmpeg_list_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "is_local_file", true);
	obs_data_set_default_bool(settings, "looping", false);
	obs_data_set_default_bool(settings, "clear_on_media_end", true);
	obs_data_set_default_bool(settings, "restart_on_activate", true);
	obs_data_set_default_bool(settings, "linear_alpha", false);
	obs_data_set_default_int(settings, "buffering_mb", 2);
	obs_data_set_default_int(settings, "speed_percent", 100);
	obs_data_set_default_bool(settings, "log_changes", true);
}

static const char *video_filter =
	"Video files (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm)";

static obs_source_t *get_source(media_file_array_t *files, const char *path)
{
	obs_source_t *source = NULL;

	for (size_t i = 0; i < files->num; i++) {
		const char *cur_path = files->array[i].path;

		if (strcmp(path, cur_path) == 0) {
			source = obs_source_get_ref(files->array[i].source);
			break;
		}
	}

	return source;
}

static obs_source_t *create_source_from_file(const char *file)
{
    UNUSED_PARAMETER(file);
    
	obs_data_t *settings = obs_data_create();
	obs_source_t *source;

	ffmpeg_list_source_defaults(settings);
	source = obs_source_create_private("ffmpeg_list_source", NULL, settings);

	obs_data_release(settings);
	return source;
}

static void add_file(struct ffmpeg_list_source *s,
		     media_file_array_t *new_files,
		     const char *path)
{
	struct media_file_data data;
	obs_source_t *new_source;

	pthread_mutex_lock(&s->mutex);
	new_source = get_source(&s->files, path);
	pthread_mutex_unlock(&s->mutex);

	if (!new_source)
		new_source = get_source(new_files, path);
	if (!new_source)
		new_source = create_source_from_file(path);

	if (new_source) {
		data.path = bstrdup(path);
		data.source = new_source;
		da_push_back(*new_files, &data);
	}
}

static obs_properties_t *ffmpeg_list_source_getproperties(void *data)
{
	struct ffmpeg_list_source *s = data;
	struct dstr path = {0};

	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	/* ----------------- */
	if (s) {
		pthread_mutex_lock(&s->mutex);
		if (s->files.num) {
			struct media_file_data *last = da_end(s->files);
			const char *slash;

			dstr_copy(&path, last->path);
			dstr_replace(&path, "\\", "/");
			slash = strrchr(path.array, '/');
			if (slash)
				dstr_resize(&path, slash - path.array + 1);
		}
		pthread_mutex_unlock(&s->mutex);
	}

	obs_properties_add_editable_list(props, S_PLAYLIST, T_FILES,
					 OBS_EDITABLE_LIST_TYPE_FILES,
					 video_filter, path.array);
	dstr_free(&path);

	obs_property_t *prop;
	prop = obs_properties_add_text(props, S_CURRENT_FILE_NAME,
				    T_CURRENT_FILE_NAME, OBS_TEXT_INFO);
	obs_properties_add_bool(props, "looping", obs_module_text("Looping"));

	obs_properties_add_bool(props, "restart_on_activate",
				obs_module_text("RestartWhenActivated"));

	obs_properties_add_bool(props, "hw_decode",
				obs_module_text("HardwareDecode"));

	obs_properties_add_bool(props, "clear_on_media_end",
				obs_module_text("ClearOnMediaEnd"));

	
	prop = obs_properties_add_bool(
		props, "close_when_inactive",
		obs_module_text("CloseFileWhenInactive"));

	obs_property_set_long_description(
		prop, obs_module_text("CloseFileWhenInactive.ToolTip"));

	prop = obs_properties_add_int_slider(props, "speed_percent",
		obs_module_text("SpeedPercentage"),
		1, 200, 1);
	obs_property_int_set_suffix(prop, "%");

	prop = obs_properties_add_list(props, "color_range",
		obs_module_text("ColorRange"),
		OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Auto"),
		VIDEO_RANGE_DEFAULT);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Partial"),
		VIDEO_RANGE_PARTIAL);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Full"),
		VIDEO_RANGE_FULL);

	obs_properties_add_bool(props, "linear_alpha",
		obs_module_text("LinearAlpha"));

	prop = obs_properties_add_text(props, "ffmpeg_options",
		obs_module_text("FFmpegOpts"),
		OBS_TEXT_DEFAULT);
	obs_property_set_long_description(
		prop, obs_module_text("FFmpegOpts.ToolTip.Source"));

	return props;
}

static void dump_source_info(struct ffmpeg_list_source* s, const char* input,
	const char* input_format)
{
	if (!s->log_changes)
		return;
	FF_BLOG(LOG_INFO,
		"settings:\n"
		"\tinput:                   %s\n"
		"\tinput_format:            %s\n"
		"\tspeed:                   %d\n"
		"\tis_looping:              %s\n"
		"\tis_linear_alpha:         %s\n"
		"\tis_hw_decoding:          %s\n"
		"\tis_clear_on_media_end:   %s\n"
		"\trestart_on_activate:     %s\n"
		"\tclose_when_inactive:     %s\n"
		"\tfull_decode:             %s\n"
		"\tffmpeg_options:          %s",
		input ? input : "(null)",
		input_format ? input_format : "(null)", s->speed_percent,
		/* s->is_looping ? "yes" :*/ "no",
		s->is_linear_alpha ? "yes" : "no",
		s->is_hw_decoding ? "yes" : "no",
		s->is_clear_on_media_end ? "yes" : "no",
		s->restart_on_activate ? "yes" : "no",
		s->close_when_inactive ? "yes" : "no",
		s->full_decode ? "yes" : "no", s->ffmpeg_options);
}

static void get_frame(void* opaque, struct obs_source_frame* f)
{
	struct ffmpeg_list_source* s = opaque;
	obs_source_output_video(s->source, f);
}

static void preload_frame(void* opaque, struct obs_source_frame* f)
{
	struct ffmpeg_list_source* s = opaque;
	if (s->close_when_inactive)
		return;

	if (s->is_clear_on_media_end)
		obs_source_preload_video(s->source, f);
}

static void seek_frame(void* opaque, struct obs_source_frame* f)
{
	struct ffmpeg_list_source* s = opaque;
	obs_source_set_video_frame(s->source, f);
}

static void get_audio(void* opaque, struct obs_source_audio* a)
{
	struct ffmpeg_list_source* s = opaque;
	obs_source_output_audio(s->source, a);
}

static void media_stopped(void *opaque);

static void ffmpeg_list_source_open(struct ffmpeg_list_source *s)
{
	if (s->input && *s->input) {
		struct mp_media_info info = {
			.opaque = s,
			.v_cb = get_frame,
			.v_preload_cb = preload_frame,
			.v_seek_cb = seek_frame,
			.a_cb = get_audio,
			.stop_cb = media_stopped,
			.path = s->input,
			.format = s->input_format,
			.buffering = s->buffering_mb * 1024 * 1024,
			.speed = s->speed_percent,
			.force_range = s->range,
			.is_linear_alpha = s->is_linear_alpha,
			.hardware_decoding = s->is_hw_decoding,
			.ffmpeg_options = s->ffmpeg_options,
			.is_local_file = true,
			.request_preload = s->is_stinger,
			.full_decode = s->full_decode,
		};

		s->media = media_playback_create(&info);
	}
}

static void update_current_filename_setting(struct ffmpeg_list_source *s,
					    char *szName)
{
	obs_data_t *settings = obs_source_get_settings(s->source);
	obs_data_set_string(settings, S_CURRENT_FILE_NAME, szName);
	obs_source_update_properties(s->source);
}

static void ffmpeg_list_source_start(struct ffmpeg_list_source *s)
{
	if (!s->media)
		ffmpeg_list_source_open(s);

	if (!s->media)
		return;

	media_playback_play(s->media, false, false);
	if (media_playback_has_video(s->media) &&
	    (s->is_clear_on_media_end))
		obs_source_show_preloaded_video(s->source);
	set_media_state(s, OBS_MEDIA_STATE_PLAYING);
	update_current_filename_setting(s, s->input);
	s->is_stop = false;
	obs_source_media_started(s->source);
}

static void ffmpeg_list_source_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct ffmpeg_list_source *s = data;
	if (s->destroy_media) {
		if (s->media) {
			media_playback_destroy(s->media);
			s->media = NULL;
		}

		s->destroy_media = false;
	}
}

#define SRT_PROTO "srt"
#define RIST_PROTO "rist"

static bool requires_mpegts(const char *path)
{
	return !astrcmpi_n(path, SRT_PROTO, sizeof(SRT_PROTO) - 1) ||
	       !astrcmpi_n(path, RIST_PROTO, sizeof(RIST_PROTO) - 1);
}

static bool valid_extension(const char *ext)
{
	if (!ext)
		return false;
	return astrcmpi(ext, ".mp4") == 0 || astrcmpi(ext, ".m4v") == 0 ||
	       astrcmpi(ext, ".ts") == 0 || astrcmpi(ext, ".mov") == 0 ||
	       astrcmpi(ext, ".mxf") == 0 || astrcmpi(ext, ".flv") == 0 ||
	       astrcmpi(ext, ".mkv") == 0 || astrcmpi(ext, ".avi") == 0 ||
	       astrcmpi(ext, ".gif") == 0 || astrcmpi(ext, ".webm") == 0;
}

static void ffmpeg_list_source_update(void *data, obs_data_t *settings)
{
	media_file_array_t new_files;
	media_file_array_t old_files;

	obs_data_array_t *array;
	size_t count;

	da_init(new_files);
	da_init(old_files);

	array = obs_data_get_array(settings, S_PLAYLIST);
	count = obs_data_array_count(array);


	struct ffmpeg_list_source *s = data;

	bool active = obs_source_active(s->source);
	bool is_stinger = obs_data_get_bool(settings, "is_stinger");
	bool is_track_matte = obs_data_get_bool(settings, "is_track_matte");
	bool should_restart_media = (is_stinger != s->is_stinger);

	char *input = NULL;
	const char *input_format;
	const char *ffmpeg_options;

	bool is_hw_decoding;
	enum video_range_type range;
	bool is_linear_alpha;
	int speed_percent;
	bool is_looping;

	bfree(s->input_format);

	/* ------------------------------------- */
	/* create new list of sources */

	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");
		if (!path || !*path) {
			obs_data_release(item);
			continue;
		}
		os_dir_t *dir = os_opendir(path);

		if (dir) {
			struct dstr dir_path = {0};
			struct os_dirent *ent;

			for (;;) {
				const char *ext;

				ent = os_readdir(dir);
				if (!ent)
					break;
				if (ent->directory)
					continue;

				ext = os_get_path_extension(ent->d_name);
				if (!valid_extension(ext))
					continue;

				dstr_copy(&dir_path, path);
				dstr_cat_ch(&dir_path, '/');
				dstr_cat(&dir_path, ent->d_name);
				add_file(s, &new_files, dir_path.array);
			}

			dstr_free(&dir_path);
			os_closedir(dir);
		} else {
			add_file(s, &new_files, path);
		}

		obs_data_release(item);
	}

	pthread_mutex_lock(&s->mutex);
	old_files = s->files;
	s->files = new_files;
	pthread_mutex_unlock(&s->mutex);

	if (!s->input || strcmp(s->input, "") == 0) {
		s->cur_item = 0;
		input = (s->files.num) ? bstrdup(s->files.array[s->cur_item].path)
				: bstrdup("");
	} else {
		input = bstrdup(s->input);
	}

	input_format = NULL;
	is_looping = obs_data_get_bool(settings, "looping");

	if (s->input && !should_restart_media)
		should_restart_media |= strcmp(s->input, input) != 0;	

	is_hw_decoding = obs_data_get_bool(settings, "hw_decode");
	range = obs_data_get_int(settings, "color_range");
	speed_percent = (int)obs_data_get_int(settings, "speed_percent");
	if (speed_percent < 1 || speed_percent > 200)
		speed_percent = 100;
	ffmpeg_options = obs_data_get_string(settings, "ffmpeg_options");

	/* Restart media source if these properties are changed */
	if (s->is_hw_decoding != is_hw_decoding || s->range != range ||
	    s->speed_percent != speed_percent ||
	    (s->ffmpeg_options &&
	     strcmp(s->ffmpeg_options, ffmpeg_options) != 0))
		should_restart_media = true;

	bfree(s->input);
	bfree(s->ffmpeg_options);

	s->is_looping = is_looping;
	s->close_when_inactive =
		obs_data_get_bool(settings, "close_when_inactive");
	s->input = input ? bstrdup(input) : NULL;
	s->input_format = input_format ? bstrdup(input_format) : NULL;
	s->is_hw_decoding = is_hw_decoding;
	s->full_decode = obs_data_get_bool(settings, "full_decode");
	s->is_clear_on_media_end =
		obs_data_get_bool(settings, "clear_on_media_end");
	s->restart_on_activate =
		!astrcmpi_n(input, RIST_PROTO, sizeof(RIST_PROTO) - 1)
			? false
			: obs_data_get_bool(settings, "restart_on_activate");
	s->range = range;
	is_linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	s->is_linear_alpha = is_linear_alpha;
	s->buffering_mb = (int)obs_data_get_int(settings, "buffering_mb");
	s->speed_percent = speed_percent;
	s->ffmpeg_options = ffmpeg_options ? bstrdup(ffmpeg_options) : NULL;
	s->is_stinger = is_stinger;
	s->is_track_matte = is_track_matte;
	s->log_changes = obs_data_get_bool(settings, "log_changes");

	if (s->speed_percent < 1 || s->speed_percent > 200)
		s->speed_percent = 100;

	if (s->media && should_restart_media) {
		media_playback_destroy(s->media);
		s->media = NULL;
	}

	/* directly set options if media is playing */
	if (s->media) {
		media_playback_set_looping(s->media, false);
		media_playback_set_is_linear_alpha(s->media, is_linear_alpha);
	}
	if ((!s->close_when_inactive || active) && should_restart_media)
		ffmpeg_list_source_open(s);

	dump_source_info(s, input, input_format);
	if ((!s->restart_on_activate || active) && should_restart_media)
		ffmpeg_list_source_start(s);

	if (!s->media) {
		media_playback_stop(s->media);
		obs_source_output_video(s->source, NULL);
		set_media_state(s, OBS_MEDIA_STATE_STOPPED);
	}
	bfree(input);	
}

static const char *ffmpeg_list_source_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FFmpegListSource");
}

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey,
			   bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct ffmpeg_list_source *s = data;
	if (obs_source_showing(s->source))
		obs_source_media_restart(s->source);
}

static void restart_proc(void *data, calldata_t *cd)
{
	restart_hotkey(data, 0, NULL, true);
	UNUSED_PARAMETER(cd);
}

static void preload_first_frame_proc(void *data, calldata_t *cd)
{
	struct ffmpeg_list_source *s = data;
	if (s->is_track_matte)
		obs_source_output_video(s->source, NULL);
	media_playback_preload_frame(s->media);
	UNUSED_PARAMETER(cd);
}

static void get_duration(void *data, calldata_t *cd)
{
	struct ffmpeg_list_source *s = data;
	int64_t dur = 0;
	if (s->media)
		dur = media_playback_get_duration(s->media);

	calldata_set_int(cd, "duration", dur * 1000);
}

static void get_nb_frames(void *data, calldata_t *cd)
{
	struct ffmpeg_list_source *s = data;
	int64_t frames = media_playback_get_frames(s->media);
	calldata_set_int(cd, "num_frames", frames);
}

static bool ffmpeg_list_source_play_hotkey(void *data, obs_hotkey_pair_id id,
				      obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return false;

	struct ffmpeg_list_source *s = data;

	if (s->state == OBS_MEDIA_STATE_PLAYING ||
	    !obs_source_showing(s->source))
		return false;

	obs_source_media_play_pause(s->source, false);
	return true;
}

static bool ffmpeg_list_source_pause_hotkey(void *data, obs_hotkey_pair_id id,
				       obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return false;

	struct ffmpeg_list_source *s = data;

	if (s->state != OBS_MEDIA_STATE_PLAYING ||
	    !obs_source_showing(s->source))
		return false;

	obs_source_media_play_pause(s->source, true);
	return true;
}

static void ffmpeg_list_source_stop_hotkey(void *data, obs_hotkey_id id,
				      obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct ffmpeg_list_source *s = data;

	if (obs_source_showing(s->source))
		obs_source_media_stop(s->source);
}

static void free_files(media_file_array_t *files)
{
	for (size_t i = 0; i < files->num; i++) {
		bfree(files->array[i].path);
		obs_source_release(files->array[i].source);
	}

	da_free(*files);
}

static void ffmpeg_list_source_destroy(void *data)
{
	struct ffmpeg_list_source *s = data;
	s->is_stop = true;
	if (s->hotkey)
		obs_hotkey_unregister(s->hotkey);
	if (s->media)
		media_playback_destroy(s->media);
	if (s->tmp_media) {
		media_playback_destroy(s->tmp_media);
	}
	bfree(s->input);
	bfree(s->input_format);
	bfree(s->ffmpeg_options);

	free_files(&s->files);
	pthread_mutex_destroy(&s->mutex);

	bfree(s);
}

static void *ffmpeg_list_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct ffmpeg_list_source *s = bzalloc(sizeof(struct ffmpeg_list_source));
	s->source = source;
#pragma region _SOOP_SOURCE_MONITORING_TYPE
	soop_source_set_monitoring_type(source);
#pragma endregion
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void restart()", restart_proc, s);
	proc_handler_add(ph, "void preload_first_frame()",
			 preload_first_frame_proc, s);
	proc_handler_add(ph, "void get_duration(out int duration)",
			 get_duration, s);
	proc_handler_add(ph, "void get_nb_frames(out int num_frames)",
			 get_nb_frames, s);

	pthread_mutex_init_value(&s->mutex);
	if (pthread_mutex_init(&s->mutex, NULL) != 0)
		goto error;

	ffmpeg_list_source_update(s, settings);
	return s;
error:
	ffmpeg_list_source_destroy(s);
	return NULL;
}

static void ffmpeg_list_source_activate(void *data)
{
	struct ffmpeg_list_source *s = data;

	if (s->restart_on_activate)
		obs_source_media_restart(s->source);
}

static void ffmpeg_list_source_deactivate(void *data)
{
	struct ffmpeg_list_source *s = data;

	if (s->restart_on_activate) {
		if (s->media) {
			media_playback_stop(s->media);

			if (s->is_clear_on_media_end)
				obs_source_output_video(s->source, NULL);
		}
	}
}

static void ffmpeg_list_source_play_pause(void *data, bool pause)
{
	struct ffmpeg_list_source *s = data;

	if (!s->media)
		ffmpeg_list_source_open(s);

	if (!s->media)
		return;

	media_playback_play_pause(s->media, pause);

	if (pause) {

		set_media_state(s, OBS_MEDIA_STATE_PAUSED);
	} else {

		set_media_state(s, OBS_MEDIA_STATE_PLAYING);
		update_current_filename_setting(s, s->input);
		s->is_stop = false;
		obs_source_media_started(s->source);
	}
}

static void ffmpeg_list_source_stop(void *data)
{
	struct ffmpeg_list_source *s = data;

	if (s->media) {
		update_current_filename_setting(s, " ");
		s->is_stop = true;
		media_playback_stop(s->media);
		obs_source_output_video(s->source, NULL);
		set_media_state(s, OBS_MEDIA_STATE_STOPPED);		
	}
}

static void ffmpeg_playlist_next(void *data)
{
	struct ffmpeg_list_source *s = data;

	do {
		if (!s->files.num)
			break;
		if (++s->cur_item >= s->files.num)
			s->cur_item = 0;
		const char *input;
		input = s->files.array[s->cur_item].path;
		bfree(s->input);
		s->input = input ? bstrdup(input) : NULL;

		media_playback_stop(s->media);
		media_playback_destroy(s->media);
		s->media = NULL;
		ffmpeg_list_source_start(s);
		return;
	} while (false);
	media_playback_stop(s->media);
	obs_source_output_video(s->source, NULL);
	set_media_state(s, OBS_MEDIA_STATE_STOPPED);
	
}

static void ffmpeg_playlist_prev(void *data)
{
	struct ffmpeg_list_source *s = data;

	do {
		if (!s->files.num)
			break;
		if (s->cur_item == 0)
			s->cur_item = s->files.num - 1;
		else
			--s->cur_item;
		const char *input;
		input = s->files.array[s->cur_item].path;
		bfree(s->input);
		s->input = input ? bstrdup(input) : NULL;
		media_playback_stop(s->media);
		media_playback_destroy(s->media);
		s->media = NULL;
		ffmpeg_list_source_start(s);
		return;
	} while (false);
	media_playback_stop(s->media);
	obs_source_output_video(s->source, NULL);
	set_media_state(s, OBS_MEDIA_STATE_STOPPED);
}

static void ffmpeg_list_source_restart(void *data)
{
	struct ffmpeg_list_source *s = data;

	if (obs_source_showing(s->source))
		ffmpeg_list_source_start(s);

	set_media_state(s, OBS_MEDIA_STATE_PLAYING);
	update_current_filename_setting(s, s->input);
	s->is_stop = false;
}

static void media_stopped(void *opaque)
{
	// 영상 재생 완료
	struct ffmpeg_list_source *s = opaque;
	if (s->is_stop)
		return;

	if (!s->input && 0 == s->files.num) {
		if (s->is_clear_on_media_end && !s->is_track_matte) {
			obs_source_output_video(s->source, NULL);
		}
		if ((s->close_when_inactive) && s->media)
			s->destroy_media = true;

		if (s->state != OBS_MEDIA_STATE_STOPPED) {
			set_media_state(s, OBS_MEDIA_STATE_ENDED);
			obs_source_media_ended(s->source);
			update_current_filename_setting(s, " ");
		}
		return;
	}

	// is last media
	if (s->cur_item + 1 >= s->files.num) {
		if (!s->is_looping) {
			if (s->is_clear_on_media_end && !s->is_track_matte) {
				obs_source_output_video(s->source, NULL);
			}
			if ((s->close_when_inactive) && s->media)
				s->destroy_media = true;

			if (s->state != OBS_MEDIA_STATE_STOPPED) {
				set_media_state(s, OBS_MEDIA_STATE_ENDED);
				obs_source_media_ended(s->source);
				update_current_filename_setting(s, " ");
			}
			return;
		}		
	}

	if (!os_file_exists(s->input))
		return;

	if (s->tmp_media) {
		media_playback_stop(s->tmp_media);
		media_playback_destroy(s->tmp_media);
		s->tmp_media = NULL;
	}

	media_playback_t *prevmedia = s->media;
	s->media = s->tmp_media;
	s->tmp_media = prevmedia;
	ffmpeg_playlist_next(s);
}

static int64_t ffmpeg_list_source_get_duration(void *data)
{
	struct ffmpeg_list_source *s = data;
	int64_t dur = 0;

	if (s->media)
		dur = media_playback_get_duration(s->media) / INT64_C(1000);

	return dur;
}

static int64_t ffmpeg_list_source_get_time(void *data)
{
	struct ffmpeg_list_source *s = data;

	return media_playback_get_current_time(s->media);
}

static void ffmpeg_list_source_set_time(void *data, int64_t ms)
{
	struct ffmpeg_list_source *s = data;

	if (!s->media)
		return;

	media_playback_seek(s->media, ms);
}

static enum obs_media_state ffmpeg_list_source_get_state(void *data)
{
	struct ffmpeg_list_source *s = data;

	return s->state;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct ffmpeg_list_source* s = src;
	const char* orig_path = data;

	obs_source_t* source = s->source;
	obs_data_t* settings = obs_source_get_settings(source);
	obs_data_array_t* files = obs_data_get_array(settings, S_PLAYLIST);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t* file = obs_data_array_item(files, i);
		const char* path = obs_data_get_string(file, "value");

		if (strcmp(path, orig_path) == 0) {
			if (new_path && *new_path)
				obs_data_set_string(file, "value", new_path);
			else
				obs_data_array_erase(files, i);

			obs_data_release(file);
			break;
		}

		obs_data_release(file);
	}

	obs_source_update(source, settings);

	obs_data_array_release(files);
	obs_data_release(settings);
}

static obs_missing_files_t *ffmpeg_list_source_missingfiles(void *data)
{
	struct ffmpeg_list_source* s = data;
	obs_missing_files_t* missing_files = obs_missing_files_create();

	obs_source_t* source = s->source;
	obs_data_t* settings = obs_source_get_settings(source);
	obs_data_array_t* files = obs_data_get_array(settings, S_PLAYLIST);

	size_t l = obs_data_array_count(files);
	for (size_t i = 0; i < l; i++) {
		obs_data_t* item = obs_data_array_item(files, i);
		const char* path = obs_data_get_string(item, "value");

		if (strcmp(path, "") != 0) {
			if (!os_file_exists(path)) {
				obs_missing_file_t* file =
					obs_missing_file_create(
						path, missing_file_callback,
						OBS_MISSING_FILE_SOURCE, source,
						(void*)path);

				obs_missing_files_add_file(missing_files, file);
			}
		}

		obs_data_release(item);
	}

	obs_data_array_release(files);
	obs_data_release(settings);

	return missing_files;
}

struct obs_source_info ffmpeg_list_source = {
	.id = "ffmpeg_list_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = ffmpeg_list_source_getname,
	.create = ffmpeg_list_source_create,
	.destroy = ffmpeg_list_source_destroy,
	.get_defaults = ffmpeg_list_source_defaults,
	.get_properties = ffmpeg_list_source_getproperties,
	.activate = ffmpeg_list_source_activate,
	.deactivate = ffmpeg_list_source_deactivate,
	.video_tick = ffmpeg_list_source_tick,
	.missing_files = ffmpeg_list_source_missingfiles,
	.update = ffmpeg_list_source_update,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = ffmpeg_list_source_play_pause,
	.media_restart = ffmpeg_list_source_restart,
	.media_stop = ffmpeg_list_source_stop,
	.media_next = ffmpeg_playlist_next,
	.media_previous = ffmpeg_playlist_prev,
	.media_get_duration = ffmpeg_list_source_get_duration,
	.media_get_time = ffmpeg_list_source_get_time,
	.media_set_time = ffmpeg_list_source_set_time,
	.media_get_state = ffmpeg_list_source_get_state,
};

