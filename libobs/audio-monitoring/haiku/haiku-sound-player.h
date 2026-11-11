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

/* Minimal C interface to the Media Kit for haiku-output.c: BSoundPlayer streams into Renga's system mixer and
 * the mixer's per-stream output routing (3rdparty/ce/PORTING.md, "Per-application audio, routing and
 * loopback"). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monitoring device id meaning "the system's default output" (what the frontend stores for its Default
 * entry); the mixer's routing index 0. */
#define OBS_HAIKU_MONITOR_DEFAULT_ID "default"

/* Called on a sound player thread: fill `frames` interleaved float frames (the channel count and rate the
 * player was created with). */
typedef void (*obs_haiku_fill_cb)(void *param, float *buffer, size_t frames);

/* Called once per output device the mixer can route a stream to. name and id are both the device's flavor
 * name, which is what the mixer remembers routes by. Return false to stop. */
typedef bool (*obs_haiku_device_cb)(void *param, const char *name, const char *id);

struct obs_haiku_sound_player;

/* Lists the output devices, in the mixer's order. Does nothing if the media server isn't running. */
void obs_haiku_enum_output_devices(obs_haiku_device_cb cb, void *param);

/* Creates (but does not start) a BSoundPlayer playing float audio of the given rate and channel count
 * (1-8) into the system mixer. Returns NULL on failure (e.g. no media server). */
struct obs_haiku_sound_player *obs_haiku_sound_player_create(const char *name, uint32_t sample_rate,
							     uint32_t channels, size_t buffer_frames,
							     obs_haiku_fill_cb cb, void *param);

/* Starts calling the fill callback. */
bool obs_haiku_sound_player_start(struct obs_haiku_sound_player *player);

/* Stops playback (the fill callback is not running and won't run again once this returns) and frees
 * the player. Accepts NULL. */
void obs_haiku_sound_player_destroy(struct obs_haiku_sound_player *player);

/* Routes every stream of this program in the system mixer to `device_id` (OBS_HAIKU_MONITOR_DEFAULT_ID, NULL
 * or "" for the default output). Returns false if the mixer has no such device now or no stream of OBS; the
 * failure is logged once per device. */
bool obs_haiku_route_streams(const char *device_id);

#ifdef __cplusplus
}
#endif
