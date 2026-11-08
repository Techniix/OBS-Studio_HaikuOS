/******************************************************************************
    Copyright (C) 2026 by the OBS Studio Haiku port contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/*
 * Audio monitoring ("Monitor and Output") on Haiku.
 *
 * Each monitored source gets a player. libobs pushes the source's audio (planar float) into a FIFO from its
 * audio thread, resampled to interleaved float at the player's rate and channel count; the player's
 * realtime thread pulls frames out of it. The FIFO is kept short so monitoring latency stays bounded; on
 * underrun the player outputs silence and re-buffers a little before resuming.
 *
 * Devices: "default" (added by the frontend) is the system mixer, which mixes, resamples and follows the
 * output chosen in Media preferences. The other entries are physical outputs (e.g. a USB headset) played to
 * directly, bypassing the mixer: those dictate rate and channel count (resampled here) and sample format
 * (converted by the shared device player, which also mixes the monitored sources). A saved device that is
 * missing or busy falls back to the mixer (see haiku-sound-player.cpp). A device's clock isn't synchronised
 * to OBS's: drift is absorbed by the FIFO (dropping on overflow, re-buffering on underrun).
 */

#include "../../media-io/audio-resampler.h"
#include "../../util/deque.h"
#include "../../util/threading.h"
#include "../../util/platform.h"
#include "../../obs-internal.h"
#include "../../util/darray.h"

#include "haiku-sound-player.h"

#include <string.h>

/* Player buffer length; the mixer schedules buffers of this size. */
#define BUFFER_MS 10
/* Amount of audio to collect before (re)starting output after an underrun. */
#define PREBUFFER_MS 40
/* Drop the oldest audio once the FIFO holds more than this. */
#define MAX_LATENCY_MS 200

struct audio_monitor {
	obs_source_t *source;
	struct obs_haiku_sound_player *player;

	pthread_mutex_t mutex;
	bool mutex_initialized;
	struct deque data;
	audio_resampler_t *resampler;
	uint32_t channels;
	size_t bytes_per_frame;
	size_t prebuffer_bytes;
	size_t max_bytes;
	bool buffering;

	bool ignore;
};

bool obs_audio_monitoring_available(void)
{
	return true;
}

void obs_enum_audio_monitoring_devices(obs_enum_audio_device_cb cb, void *data)
{
	/* The system mixer ("default") is added by the caller; list the physical outputs. */
	obs_haiku_enum_output_devices(cb, data);
}

bool devices_match(const char *id1, const char *id2)
{
	if (!id1 || !id2)
		return false;
	return strcmp(id1, id2) == 0;
}

static void fill_buffer(void *param, float *buffer, size_t frames)
{
	struct audio_monitor *monitor = param;
	const size_t want = frames * monitor->bytes_per_frame;
	size_t have = 0;

	pthread_mutex_lock(&monitor->mutex);

	if (monitor->buffering && monitor->data.size >= monitor->prebuffer_bytes)
		monitor->buffering = false;

	if (!monitor->buffering) {
		have = monitor->data.size < want ? monitor->data.size : want;
		/* keep whole frames */
		have -= have % monitor->bytes_per_frame;
		deque_pop_front(&monitor->data, buffer, have);

		if (have < want)
			monitor->buffering = true;
	}

	pthread_mutex_unlock(&monitor->mutex);

	if (have < want)
		memset((uint8_t *)buffer + have, 0, want - have);
}

static void on_audio_pause(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	struct audio_monitor *monitor = data;

	pthread_mutex_lock(&monitor->mutex);
	deque_free(&monitor->data);
	monitor->buffering = true;
	pthread_mutex_unlock(&monitor->mutex);
}

static void on_audio_playback(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(muted);

	struct audio_monitor *monitor = param;
	float vol = source->user_volume;

	uint8_t *resample_data[MAX_AV_PLANES];
	uint32_t resample_frames;
	uint64_t ts_offset;

	if (os_atomic_load_long(&source->activate_refs) == 0)
		return;

	if (!audio_resampler_resample(monitor->resampler, resample_data, &resample_frames, &ts_offset,
				      (const uint8_t *const *)audio_data->data, (uint32_t)audio_data->frames))
		return;

	const size_t bytes = monitor->bytes_per_frame * resample_frames;

	if (!close_float(vol, 1.0f, EPSILON)) {
		float *cur = (float *)resample_data[0];
		float *end = cur + (size_t)resample_frames * monitor->channels;

		while (cur < end)
			*(cur++) *= vol;
	}

	pthread_mutex_lock(&monitor->mutex);

	deque_push_back(&monitor->data, resample_data[0], bytes);

	if (monitor->data.size > monitor->max_bytes) {
		size_t excess = monitor->data.size - monitor->max_bytes;
		excess -= excess % monitor->bytes_per_frame;
		deque_pop_front(&monitor->data, NULL, excess);
	}

	pthread_mutex_unlock(&monitor->mutex);
}

static bool audio_monitor_init(struct audio_monitor *monitor, obs_source_t *source)
{
	const struct audio_output_info *info = audio_output_get_info(obs->audio.audio);
	const uint32_t channels = get_audio_channels(info->speakers);
	const uint32_t rate = info->samples_per_sec;

	monitor->source = source;
	monitor->buffering = true;

	if (pthread_mutex_init(&monitor->mutex, NULL) != 0) {
		blog(LOG_WARNING, "%s: Failed to init mutex", __FUNCTION__);
		return false;
	}
	monitor->mutex_initialized = true;

	const char *id = obs->audio.monitoring_device_id;
	if (!id || !*id)
		return false;

	if (source->info.output_flags & OBS_SOURCE_DO_NOT_SELF_MONITOR) {
		obs_data_t *s = obs_source_get_settings(source);
		const char *s_dev_id = obs_data_get_string(s, "device_id");
		bool match = devices_match(s_dev_id, id);
		obs_data_release(s);

		if (match) {
			monitor->ignore = true;
			return true;
		}
	}

	char name[128];
	snprintf(name, sizeof(name), "OBS Monitor: %s", obs_source_get_name(source));

	/* Not started yet, so fill_buffer can't run before the fields below are set. */
	struct obs_haiku_output_format out;
	monitor->player = obs_haiku_sound_player_create(id, name, rate, channels, (size_t)rate * BUFFER_MS / 1000,
							fill_buffer, monitor, &out);
	if (!monitor->player) {
		blog(LOG_WARNING, "%s: Failed to create BSoundPlayer (is the media server running?)", __FUNCTION__);
		return false;
	}

	monitor->channels = out.channels;
	monitor->bytes_per_frame = sizeof(float) * out.channels;
	monitor->prebuffer_bytes = monitor->bytes_per_frame * out.sample_rate * PREBUFFER_MS / 1000;
	monitor->max_bytes = monitor->bytes_per_frame * out.sample_rate * MAX_LATENCY_MS / 1000;

	/* 1-6 and 8 channels map directly onto speaker_layout values (checked by the player) */
	struct resample_info from = {.samples_per_sec = rate, .speakers = info->speakers, .format = AUDIO_FORMAT_FLOAT_PLANAR};
	struct resample_info to = {.samples_per_sec = out.sample_rate,
				   .speakers = (enum speaker_layout)out.channels,
				   .format = AUDIO_FORMAT_FLOAT};

	monitor->resampler = audio_resampler_create(&to, &from);
	if (!monitor->resampler) {
		blog(LOG_WARNING, "%s: Failed to create resampler", __FUNCTION__);
		return false;
	}

	if (!obs_haiku_sound_player_start(monitor->player)) {
		blog(LOG_WARNING, "%s: Failed to start BSoundPlayer", __FUNCTION__);
		return false;
	}

	return true;
}

static void audio_monitor_free(struct audio_monitor *monitor)
{
	if (monitor->source && !monitor->ignore) {
		obs_source_remove_audio_capture_callback(monitor->source, on_audio_playback, monitor);
		obs_source_remove_audio_pause_callback(monitor->source, on_audio_pause, monitor);
	}

	/* Stops the player thread before the FIFO and mutex go away */
	obs_haiku_sound_player_destroy(monitor->player);
	monitor->player = NULL;

	audio_resampler_destroy(monitor->resampler);
	deque_free(&monitor->data);
	/* audio_monitor_init can fail before the mutex exists (also on the audio_monitor_reset path) */
	if (monitor->mutex_initialized)
		pthread_mutex_destroy(&monitor->mutex);
	monitor->mutex_initialized = false;
}

static void audio_monitor_init_final(struct audio_monitor *monitor)
{
	if (monitor->ignore)
		return;

	obs_source_add_audio_capture_callback(monitor->source, on_audio_playback, monitor);
	obs_source_add_audio_pause_callback(monitor->source, on_audio_pause, monitor);
}

struct audio_monitor *audio_monitor_create(obs_source_t *source)
{
	struct audio_monitor *monitor = bzalloc(sizeof(*monitor));

	if (!audio_monitor_init(monitor, source))
		goto fail;

	pthread_mutex_lock(&obs->audio.monitoring_mutex);
	da_push_back(obs->audio.monitors, &monitor);
	pthread_mutex_unlock(&obs->audio.monitoring_mutex);

	audio_monitor_init_final(monitor);
	return monitor;

fail:
	monitor->ignore = true; /* callbacks were never added */
	audio_monitor_free(monitor);
	bfree(monitor);
	return NULL;
}

void audio_monitor_reset(struct audio_monitor *monitor)
{
	obs_source_t *source = monitor->source;

	audio_monitor_free(monitor);
	memset(monitor, 0, sizeof(*monitor));

	if (audio_monitor_init(monitor, source))
		audio_monitor_init_final(monitor);
}

void audio_monitor_destroy(struct audio_monitor *monitor)
{
	if (!monitor)
		return;

	audio_monitor_free(monitor);

	pthread_mutex_lock(&obs->audio.monitoring_mutex);
	da_erase_item(obs->audio.monitors, &monitor);
	pthread_mutex_unlock(&obs->audio.monitoring_mutex);

	bfree(monitor);
}
