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

/* Called on the sound player's realtime thread: fill `frames` interleaved float frames. */
typedef void (*obs_haiku_fill_cb)(void *param, float *buffer, size_t frames, uint32_t channels);

struct obs_haiku_sound_player;

/* Creates and starts a player connected to the system mixer. Returns NULL on failure. */
struct obs_haiku_sound_player *obs_haiku_sound_player_create(const char *name, uint32_t sample_rate,
							     uint32_t channels, size_t buffer_frames,
							     obs_haiku_fill_cb cb, void *param);

/* Stops playback (waiting for the player thread to finish its current buffer) and frees the player. */
void obs_haiku_sound_player_destroy(struct obs_haiku_sound_player *player);

#ifdef __cplusplus
}
#endif
