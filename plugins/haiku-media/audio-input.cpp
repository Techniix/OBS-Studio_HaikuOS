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

/* Audio input capture (microphones, line in) through the Media Kit. */

#include "audio-channels.hpp"
#include "media-device.hpp"

#include <util/platform.h>
#include <util/util_uint64.h>

#include <atomic>
#include <memory>
#include <vector>

#define S_DEVICE_ID "device_id"

#define T_(text) obs_module_text(text)

namespace {

struct AudioInput {
	obs_source_t *source = nullptr;
	/* opens the device and keeps it open, retrying while it is busy or gone (media-device.hpp) */
	std::unique_ptr<HaikuCapture> capture;
	std::vector<uint8_t> convert_buffer;
	std::atomic_bool warned_format = false;

	/* channel map for the last (channel count, mask) seen; only touched by the recorder thread */
	uint32_t map_channels = 0;
	uint32_t map_mask = 0;
	AudioChannelMap channel_map;

	void OnOpened(const media_format &format);
	void OnBuffer(const void *data, size_t size, const media_format &format);
};

/* supervisor thread */
void AudioInput::OnOpened(const media_format &format)
{
	const media_raw_audio_format &raw = format.u.raw_audio;
	warned_format = false;
	blog(LOG_INFO, "[haiku-media] '%s': capturing (%u ch, %.0f Hz, format 0x%x)", obs_source_get_name(source),
	     raw.channel_count, raw.frame_rate, raw.format);
}

void AudioInput::OnBuffer(const void *data, size_t size, const media_format &format)
{
	const media_multi_audio_format &raw = format.u.raw_audio;
	const uint32_t channels = raw.channel_count;
	if (channels != map_channels || raw.channel_mask != map_mask || !channel_map.in_channels) {
		/* Media Kit channel order is channel_mask bit order; build the map to OBS's layout once per format */
		map_channels = channels;
		map_mask = raw.channel_mask;
		channel_map = channels >= 1 && channels <= 8 ? haiku_audio_channel_map(channels, raw.channel_mask)
							     : AudioChannelMap();
		channel_map.in_channels = channels;
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
	const speaker_layout speakers = channel_map.layout;
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

	if (obs_format == AUDIO_FORMAT_UNKNOWN || speakers == SPEAKERS_UNKNOWN || raw.frame_rate <= 0.0f ||
	    sample_size == 0 || raw.byte_order != B_MEDIA_HOST_ENDIAN) {
		if (!warned_format.exchange(true))
			blog(LOG_WARNING,
			     "[haiku-media] '%s': unsupported audio format (format 0x%x, %u channels, %.0f Hz, "
			     "byte order %u)",
			     obs_source_get_name(source), raw.format, channels, raw.frame_rate, raw.byte_order);
		return;
	}

	const size_t frame_size = sample_size * channels;
	const uint32_t frames = (uint32_t)(size / frame_size);
	if (!frames)
		return;

	const uint8_t *samples = static_cast<const uint8_t *>(data);
	const bool signed_8bit = raw.format == media_raw_audio_format::B_AUDIO_CHAR;

	if (!channel_map.identity) {
		/* Haiku channel order -> OBS layout, converting signed 8-bit on the way */
		convert_buffer.resize((size_t)frames * channel_map.out_channels * sample_size);
		haiku_repack_channels(samples, convert_buffer.data(), frames, sample_size, channel_map, signed_8bit);
		samples = convert_buffer.data();
	} else if (signed_8bit) {
		/* signed 8-bit -> unsigned 8-bit */
		const size_t bytes = (size_t)frames * frame_size;
		convert_buffer.resize(bytes);
		for (size_t i = 0; i < bytes; i++)
			convert_buffer[i] = samples[i] ^ 0x80;
		samples = convert_buffer.data();
	}

	struct obs_source_audio audio = {};
	audio.data[0] = samples;
	audio.frames = frames;
	audio.speakers = speakers;
	audio.format = obs_format;
	audio.samples_per_sec = (uint32_t)raw.frame_rate;
	/* Buffer timestamps are in the producer's performance time; stamp on arrival instead and let libobs
	 * smooth out the jitter. */
	audio.timestamp = os_gettime_ns() - util_mul_div64(frames, 1000000000ULL, audio.samples_per_sec);

	obs_source_output_audio(source, &audio);
}

const char *audio_input_get_name(void *)
{
	return T_("AudioInput");
}

void audio_input_update(void *data, obs_data_t *settings)
{
	AudioInput *ai = static_cast<AudioInput *>(data);
	/* no Media Kit work here: the capture's thread (re)opens the device, also when it is busy now */
	ai->capture->Configure(obs_data_get_string(settings, S_DEVICE_ID), HaikuVideoSize(), true);
}

void *audio_input_create(obs_data_t *settings, obs_source_t *source)
{
	AudioInput *ai = new AudioInput;
	ai->source = source;
	ai->capture = std::make_unique<HaikuCapture>(
		source, "OBS Audio Input", B_MEDIA_RAW_AUDIO,
		[ai](const void *data, size_t size, const media_format &format) { ai->OnBuffer(data, size, format); },
		[ai](const media_format &format, const HaikuOpenResult &) { ai->OnOpened(format); }, nullptr);
	audio_input_update(ai, settings);
	return ai;
}

void audio_input_destroy(void *data)
{
	AudioInput *ai = static_cast<AudioInput *>(data);
	ai->capture.reset(); /* stops the supervisor and recorder threads before AudioInput goes away */
	delete ai;
}

void audio_input_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_DEVICE_ID, HAIKU_MEDIA_DEFAULT_DEVICE);
}

obs_properties_t *audio_input_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_DEVICE_ID, T_("Device"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	HaikuFillDeviceList(list, B_MEDIA_RAW_AUDIO);

	return props;
}

} // namespace

void haiku_register_audio_input(void)
{
	struct obs_source_info info = {};
	info.id = "haiku_audio_input_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = audio_input_get_name;
	info.create = audio_input_create;
	info.destroy = audio_input_destroy;
	info.update = audio_input_update;
	info.get_defaults = audio_input_defaults;
	info.get_properties = audio_input_properties;
	info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;

	obs_register_source(&info);
}
