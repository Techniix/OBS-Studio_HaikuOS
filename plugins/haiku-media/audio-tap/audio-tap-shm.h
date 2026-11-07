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
 * Shared-memory protocol between the obs-haiku-audio-tap helper (writer) and the Desktop Audio source in
 * the haiku-media plugin (readers).
 *
 * The helper creates a cloneable area named OBS_TAP_AREA_NAME holding this header followed by a byte ring
 * buffer. It copies every buffer that the system mixer sends to the sound card into the ring and then
 * publishes the new total byte count in write_pos. Readers keep their own read position, and touch
 * client_heartbeat so the helper knows someone is still listening; without readers it restores the
 * original mixer -> sound card connection and exits.
 */

#pragma once

#include <stdint.h>

#define OBS_TAP_AREA_NAME "obs-studio desktop audio tap"
#define OBS_TAP_MAGIC 0x4f425354u /* 'OBST' */
#define OBS_TAP_VERSION 1u

/* 1 MiB: ~2.7 s of 48 kHz stereo float */
#define OBS_TAP_RING_SIZE (1u << 20)

/* The helper quits when no reader has sent a heartbeat for this long (µs) */
#define OBS_TAP_CLIENT_TIMEOUT 10000000LL

struct obs_tap_shm {
	uint32_t magic;
	uint32_t version;

	/* media_raw_audio_format of the mixer output; format_serial changes whenever these do */
	volatile int32_t format_serial;
	float frame_rate;
	uint32_t channels;
	uint32_t format; /* media_raw_audio_format::B_AUDIO_* */
	uint32_t byte_order;

	uint32_t ring_size;
	volatile int64_t write_pos;        /* total bytes written; ring offset is write_pos % ring_size */
	volatile int64_t client_heartbeat; /* system_time() of the last reader activity */
	volatile int32_t helper_alive;     /* 1 while the tap is inserted */
	int32_t helper_team;               /* team_id of the helper, so the last reader can stop it early */

	uint8_t ring[];
};

static inline uint32_t obs_tap_area_size(void)
{
	/* areas are page granular */
	const uint32_t total = (uint32_t)sizeof(struct obs_tap_shm) + OBS_TAP_RING_SIZE;
	return (total + 4095u) & ~4095u;
}
