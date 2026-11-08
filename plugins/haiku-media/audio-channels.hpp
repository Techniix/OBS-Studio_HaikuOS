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
 * Media Kit channel order -> OBS speaker layout.
 *
 * Haiku interleaves channels in channel_mask bit order (B_CHANNEL_LEFT, RIGHT, CENTER, SUB, REARLEFT,
 * REARRIGHT, ..., SIDE_LEFT, SIDE_RIGHT). OBS assigns fixed meanings per channel count, which differ for 3, 4,
 * 5 and 7 channels (OBS 3 = FL FR LFE, 4 = FL FR FC RC, 5 = FL FR FC LFE RC; the mixer's defaults are
 * L R C, L R RL RR and L R C RL RR). AudioChannelMap picks the smallest OBS layout that has a slot for every
 * channel present and gives, per OBS channel, the source channel to take (or silence). For the usual 1, 2,
 * 6 and 8 channel masks the map is the identity and no repacking happens.
 */

#pragma once

#include <media-io/audio-io.h>

#include <MediaDefs.h>

#include <stdint.h>
#include <string.h>

struct AudioChannelMap {
	speaker_layout layout = SPEAKERS_UNKNOWN;
	uint32_t in_channels = 0;
	uint32_t out_channels = 0;
	bool identity = true;
	int8_t source[MAX_AUDIO_CHANNELS]; /* per OBS channel: input channel index, or -1 for silence */
	uint32_t dropped_mask = 0;         /* channels of the input that no OBS layout can hold */

	bool Valid() const { return layout != SPEAKERS_UNKNOWN; }
};

/* The mask the system mixer uses for `channels` when the format doesn't carry one (MixerUtils.cpp) */
static inline uint32_t haiku_default_channel_mask(uint32_t channels)
{
	const uint32_t L = B_CHANNEL_LEFT, R = B_CHANNEL_RIGHT, C = B_CHANNEL_CENTER, S = B_CHANNEL_SUB,
		       RL = B_CHANNEL_REARLEFT, RR = B_CHANNEL_REARRIGHT;
	switch (channels) {
	case 1:
		return L;
	case 2:
		return L | R;
	case 3:
		return L | R | C;
	case 4:
		return L | R | RL | RR;
	case 5:
		return L | R | C | RL | RR;
	case 6:
		return L | R | C | S | RL | RR;
	case 7:
		return L | R | C | S | RL | RR | B_CHANNEL_BACK_CENTER;
	case 8:
		return L | R | C | S | RL | RR | B_CHANNEL_SIDE_LEFT | B_CHANNEL_SIDE_RIGHT;
	default:
		return 0;
	}
}

static inline AudioChannelMap haiku_audio_channel_map(uint32_t channels, uint32_t mask)
{
	AudioChannelMap map;
	map.in_channels = channels;
	memset(map.source, -1, sizeof(map.source));
	if (channels < 1 || channels > 8)
		return map;

	if (channels == 1) {
		/* whatever the single channel is called, OBS mono is the only sensible reading */
		map.layout = SPEAKERS_MONO;
		map.out_channels = 1;
		map.source[0] = 0;
		return map;
	}

	if ((uint32_t)__builtin_popcount(mask) != channels)
		mask = haiku_default_channel_mask(channels); /* 0, 0xffffffff or inconsistent */

	/* OBS layouts, smallest first, as B_CHANNEL_* per OBS channel. "RC" of 4.0/4.1 is the back center. */
	struct Layout {
		speaker_layout layout;
		uint32_t count;
		uint32_t bits[8];
	};
	static const Layout layouts[] = {
		{SPEAKERS_STEREO, 2, {B_CHANNEL_LEFT, B_CHANNEL_RIGHT}},
		{SPEAKERS_2POINT1, 3, {B_CHANNEL_LEFT, B_CHANNEL_RIGHT, B_CHANNEL_SUB}},
		{SPEAKERS_4POINT0, 4, {B_CHANNEL_LEFT, B_CHANNEL_RIGHT, B_CHANNEL_CENTER, B_CHANNEL_BACK_CENTER}},
		{SPEAKERS_4POINT1,
		 5,
		 {B_CHANNEL_LEFT, B_CHANNEL_RIGHT, B_CHANNEL_CENTER, B_CHANNEL_SUB, B_CHANNEL_BACK_CENTER}},
		{SPEAKERS_5POINT1,
		 6,
		 {B_CHANNEL_LEFT, B_CHANNEL_RIGHT, B_CHANNEL_CENTER, B_CHANNEL_SUB, B_CHANNEL_REARLEFT,
		  B_CHANNEL_REARRIGHT}},
		{SPEAKERS_7POINT1,
		 8,
		 {B_CHANNEL_LEFT, B_CHANNEL_RIGHT, B_CHANNEL_CENTER, B_CHANNEL_SUB, B_CHANNEL_REARLEFT,
		  B_CHANNEL_REARRIGHT, B_CHANNEL_SIDE_LEFT, B_CHANNEL_SIDE_RIGHT}},
	};

	const Layout *chosen = &layouts[sizeof(layouts) / sizeof(layouts[0]) - 1];
	for (const Layout &l : layouts) {
		uint32_t all = 0;
		for (uint32_t i = 0; i < l.count; i++)
			all |= l.bits[i];
		if ((mask & ~all) == 0) {
			chosen = &l;
			break;
		}
	}

	uint32_t covered = 0;
	map.layout = chosen->layout;
	map.out_channels = chosen->count;
	for (uint32_t o = 0; o < chosen->count; o++) {
		const uint32_t bit = chosen->bits[o];
		covered |= bit;
		if (!(mask & bit))
			continue;
		/* input index = number of mask bits below this one */
		map.source[o] = (int8_t)__builtin_popcount(mask & (bit - 1));
	}
	map.dropped_mask = mask & ~covered;

	map.identity = map.out_channels == channels;
	for (uint32_t o = 0; map.identity && o < map.out_channels; o++)
		map.identity = map.source[o] == (int8_t)o;
	return map;
}

template<typename T>
static inline void haiku_repack_channels_t(const T *in, T *out, uint32_t frames, const AudioChannelMap &map,
					   T silence, T flip)
{
	const uint32_t ic = map.in_channels, oc = map.out_channels;
	for (uint32_t f = 0; f < frames; f++, in += ic, out += oc)
		for (uint32_t o = 0; o < oc; o++)
			out[o] = map.source[o] >= 0 ? (T)(in[map.source[o]] ^ flip) : silence;
}

/* Converts `frames` interleaved frames of `sample_size`-byte samples into map.out_channels channels.
 * signed_8bit: the input is B_AUDIO_CHAR and is turned into OBS's unsigned 8-bit on the way. */
static inline void haiku_repack_channels(const void *in, void *out, uint32_t frames, size_t sample_size,
					 const AudioChannelMap &map, bool signed_8bit)
{
	switch (sample_size) {
	case 1:
		haiku_repack_channels_t<uint8_t>((const uint8_t *)in, (uint8_t *)out, frames, map, 0x80,
						 signed_8bit ? 0x80 : 0);
		break;
	case 2:
		haiku_repack_channels_t<uint16_t>((const uint16_t *)in, (uint16_t *)out, frames, map, 0, 0);
		break;
	case 4:
		/* float and int32: bit copies, all-zero bits are silence for both */
		haiku_repack_channels_t<uint32_t>((const uint32_t *)in, (uint32_t *)out, frames, map, 0, 0);
		break;
	default:
		break;
	}
}
