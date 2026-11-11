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
 * Media Kit raw audio buffer -> obs_source_audio, shared by the sound card input source (audio-input.cpp) and
 * the loopback sources (loopback-audio.cpp).
 *
 * The sample data is passed through untouched where OBS has the same representation (float, 32-bit, 16-bit,
 * unsigned 8-bit in an OBS channel order); only signed 8-bit samples and channel layouts OBS orders
 * differently (audio-channels.hpp) are rewritten, into a buffer owned by the converter.
 */

#pragma once

#include "audio-channels.hpp"

#include <obs-module.h>

#include <atomic>
#include <vector>

#include <MediaDefs.h>

/* One per connection; used from the Media Kit recorder thread that delivers the buffers only. */
class HaikuAudioConverter {
public:
	explicit HaikuAudioConverter(obs_source_t *source_) : source(source_) {}

	/* A new connection: its format may differ, and an unsupported one is worth one more warning. Any thread
	 * (the capture's supervisor calls it while buffers may already arrive), hence only a flag. */
	void Reset() { reset_pending = true; }

	/* Fills everything in `audio` except the timestamp. Returns false (having logged once per
	 * connection) if OBS can't take the format, or if the buffer holds no whole frame. `audio.data[0]`
	 * points into `data` or into this object and is valid until the next call. */
	bool Convert(const void *data, size_t size, const media_format &format, struct obs_source_audio &audio)
	{
		if (reset_pending.exchange(false)) {
			warned_format = false;
			have_map = false;
		}

		const media_multi_audio_format &raw = format.u.raw_audio;
		const uint32_t channels = raw.channel_count;
		if (!have_map || channels != map_channels || raw.channel_mask != map_mask) {
			/* Media Kit channel order is channel_mask bit order; the map to OBS's layout is built once
			 * per format */
			have_map = true;
			map_channels = channels;
			map_mask = raw.channel_mask;
			channel_map = haiku_audio_channel_map(channels, raw.channel_mask);
			if (channel_map.Valid() && !channel_map.identity)
				blog(LOG_INFO,
				     "[haiku-media] '%s': %u channels (mask 0x%x) are remapped to an OBS %u-channel "
				     "layout",
				     obs_source_get_name(source), channels, raw.channel_mask, channel_map.out_channels);
			if (channel_map.dropped_mask)
				blog(LOG_WARNING,
				     "[haiku-media] '%s': channel mask 0x%x has channels OBS has no slot for (0x%x); "
				     "they are dropped",
				     obs_source_get_name(source), raw.channel_mask, channel_map.dropped_mask);
		}

		const size_t sample_size = raw.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;

		audio_format obs_format = AUDIO_FORMAT_UNKNOWN;
		switch (raw.format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
			obs_format = AUDIO_FORMAT_FLOAT;
			break;
		case media_raw_audio_format::B_AUDIO_INT:
			obs_format = AUDIO_FORMAT_32BIT;
			break;
		case media_raw_audio_format::B_AUDIO_SHORT:
			obs_format = AUDIO_FORMAT_16BIT;
			break;
		case media_raw_audio_format::B_AUDIO_UCHAR:
		case media_raw_audio_format::B_AUDIO_CHAR:
			obs_format = AUDIO_FORMAT_U8BIT;
			break;
		default:
			break;
		}

		/* the rate must survive the conversion to OBS's integer rate: NaN and absurd values fail here */
		if (obs_format == AUDIO_FORMAT_UNKNOWN || !channel_map.Valid() || !(raw.frame_rate >= 1.0f) ||
		    !(raw.frame_rate <= 1000000.0f) || sample_size == 0 || raw.byte_order != B_MEDIA_HOST_ENDIAN) {
			if (!warned_format) {
				warned_format = true;
				blog(LOG_WARNING,
				     "[haiku-media] '%s': unsupported audio format (format 0x%x, %u channels, %.0f Hz, "
				     "byte order %u)",
				     obs_source_get_name(source), raw.format, channels, raw.frame_rate, raw.byte_order);
			}
			return false;
		}

		const size_t frame_size = sample_size * channels;
		const uint32_t frames = (uint32_t)(size / frame_size);
		if (!frames)
			return false;

		const uint8_t *samples = static_cast<const uint8_t *>(data);
		const bool signed_8bit = raw.format == media_raw_audio_format::B_AUDIO_CHAR;

		if (!channel_map.identity) {
			/* Haiku channel order -> OBS layout, converting signed 8-bit on the way */
			buffer.resize((size_t)frames * channel_map.out_channels * sample_size);
			haiku_repack_channels(samples, buffer.data(), frames, sample_size, channel_map, signed_8bit);
			samples = buffer.data();
		} else if (signed_8bit) {
			/* signed 8-bit -> unsigned 8-bit */
			const size_t bytes = (size_t)frames * frame_size;
			buffer.resize(bytes);
			for (size_t i = 0; i < bytes; i++)
				buffer[i] = samples[i] ^ 0x80;
			samples = buffer.data();
		}

		audio.data[0] = samples;
		audio.frames = frames;
		audio.speakers = channel_map.layout;
		audio.format = obs_format;
		audio.samples_per_sec = (uint32_t)(raw.frame_rate + 0.5f);
		return true;
	}

private:
	obs_source_t *source;
	std::vector<uint8_t> buffer;
	std::atomic_bool reset_pending{false};
	bool warned_format = false;

	/* channel map for the last (channel count, mask) seen */
	bool have_map = false;
	uint32_t map_channels = 0;
	uint32_t map_mask = 0;
	AudioChannelMap channel_map;
};
