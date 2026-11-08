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
 *
 * Shared fields that change while readers are attached are accessed with __atomic_load_n(...,
 * __ATOMIC_ACQUIRE) / __atomic_store_n(..., __ATOMIC_RELEASE) on both sides. The format fields are a
 * seqlock: the writer makes format_serial odd, updates them, then makes it even again; a reader copies them
 * between two reads of the same even serial. Readers map the area read-write (for the heartbeat), so the
 * helper never trusts anything it reads back from it: the ring size is the compile-time constant and the
 * write position lives in the node, the shared copy is only published.
 */

#pragma once

#include <stdint.h>

#define OBS_TAP_AREA_NAME "obs-studio desktop audio tap"
#define OBS_TAP_MAGIC 0x4f425354u /* 'OBST' */
/* 2: channel_mask, seqlock format_serial, 4 MiB ring */
#define OBS_TAP_VERSION 2u

/* 4 MiB: ~340 ms at 192 kHz / 8 ch / 32-bit, ~22 s of 48 kHz stereo float. Readers skip ahead when they fall
 * more than half a ring behind, so this must stay well above one poll interval (10 ms) plus scheduling
 * hiccups at the largest format the mixer can use. */
#define OBS_TAP_RING_SIZE (4u << 20)

/* Most bytes the helper copies into the ring before publishing them in write_pos (larger buffers are cut to
 * their newest whole frames). Readers treat everything within this distance past write_pos as in flux. */
#define OBS_TAP_MAX_WRITE (OBS_TAP_RING_SIZE / 2)

/* The helper quits when no reader has sent a heartbeat for this long (µs) */
#define OBS_TAP_CLIENT_TIMEOUT 10000000LL

struct obs_tap_shm {
	uint32_t magic;
	uint32_t version;

	/* media_multi_audio_format of the mixer output. format_serial is odd while the writer updates the
	 * fields below and is bumped to the next even value when they are consistent again. */
	int32_t format_serial;
	float frame_rate;
	uint32_t channels;
	uint32_t format; /* media_raw_audio_format::B_AUDIO_* */
	uint32_t byte_order;
	uint32_t channel_mask; /* B_CHANNEL_* bits, channels interleaved in bit order; 0 = unknown */

	uint32_t ring_size;        /* always OBS_TAP_RING_SIZE; informational, readers check it */
	uint32_t reserved;         /* keeps the 64-bit fields 8-byte aligned on 32-bit x86 too */
	int64_t write_pos;         /* total bytes written; ring offset is write_pos % ring_size */
	int64_t client_heartbeat;  /* system_time() of the last reader activity */
	int32_t helper_alive;      /* 1 while the tap is inserted */
	int32_t helper_team;       /* team_id of the helper, so the last reader can stop it early */

	uint8_t ring[];
};

static inline uint32_t obs_tap_area_size(void)
{
	/* areas are page granular */
	const uint32_t total = (uint32_t)sizeof(struct obs_tap_shm) + OBS_TAP_RING_SIZE;
	return (total + 4095u) & ~4095u;
}
