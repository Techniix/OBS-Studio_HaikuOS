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

#pragma once

/* Minimal C interface to the Media Kit's BSoundPlayer, used by haiku-output.c */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monitoring device id meaning "the system mixer" (what the frontend stores for its Default entry). */
#define OBS_HAIKU_MONITOR_DEFAULT_ID "default"

/* Called on a sound player thread: fill `frames` interleaved float frames (channel count and rate as
 * returned in obs_haiku_output_format). */
typedef void (*obs_haiku_fill_cb)(void *param, float *buffer, size_t frames);

/* Called once per physical audio output that can be used for monitoring. Return false to stop. */
typedef bool (*obs_haiku_device_cb)(void *param, const char *name, const char *id);

/* What the fill callback must produce. */
struct obs_haiku_output_format {
	uint32_t sample_rate;
	uint32_t channels; /* 1-6 or 8, i.e. a valid enum speaker_layout value */
};

struct obs_haiku_sound_player;

/* Lists physical audio outputs (the ones Haiku's Media preferences lists) that monitoring can play to
 * directly: the system's default output (which the mixer owns) and running outputs without a free input
 * are left out, unless OBS itself is already playing to them. Ids are stable across restarts: they are
 * derived from the node name, with "#2", "#3", ... appended to repeated names. Does nothing if the media
 * server isn't running. */
void obs_haiku_enum_output_devices(obs_haiku_device_cb cb, void *param);

/* Creates (but does not start) a player. device_id is OBS_HAIKU_MONITOR_DEFAULT_ID (or NULL/empty) for the
 * system mixer, or an id from obs_haiku_enum_output_devices(). All players on one hardware output share a
 * single connection and are mixed. If the device is gone or can't be connected to, the player falls back
 * to the system mixer (logging once per device). The mixer is fed sample_rate/channels as given; a hardware
 * output dictates its own rate and channel count, returned in *out. Returns NULL on failure (e.g. no media
 * server). */
struct obs_haiku_sound_player *obs_haiku_sound_player_create(const char *device_id, const char *name,
							     uint32_t sample_rate, uint32_t channels,
							     size_t buffer_frames, obs_haiku_fill_cb cb, void *param,
							     struct obs_haiku_output_format *out);

/* Starts calling the fill callback. */
bool obs_haiku_sound_player_start(struct obs_haiku_sound_player *player);

/* Stops playback (the fill callback is not running and won't run again once this returns) and frees
 * the player. Accepts NULL. */
void obs_haiku_sound_player_destroy(struct obs_haiku_sound_player *player);

#ifdef __cplusplus
}
#endif
