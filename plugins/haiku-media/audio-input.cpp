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

#include "audio-convert.hpp"
#include "media-device.hpp"

#include <util/platform.h>
#include <util/util_uint64.h>

#include <memory>

#define S_DEVICE_ID "device_id"

#define T_(text) obs_module_text(text)

namespace {

struct AudioInput {
	obs_source_t *source = nullptr;
	HaikuAudioConverter converter;
	/* opens the device and keeps it open, retrying while it is busy or gone (media-device.hpp). Declared
	 * last: it is destroyed first, which stops the threads that call into this object. */
	std::unique_ptr<HaikuCapture> capture;

	explicit AudioInput(obs_source_t *source_) : source(source_), converter(source_) {}

	void OnOpened(const media_format &format);
	void OnBuffer(const void *data, size_t size, const media_format &format);
};

/* supervisor thread */
void AudioInput::OnOpened(const media_format &format)
{
	const media_raw_audio_format &raw = format.u.raw_audio;
	converter.Reset();
	blog(LOG_INFO, "[haiku-media] '%s': capturing (%u ch, %.0f Hz, format 0x%x)", obs_source_get_name(source),
	     raw.channel_count, raw.frame_rate, raw.format);
}

/* recorder thread */
void AudioInput::OnBuffer(const void *data, size_t size, const media_format &format)
{
	struct obs_source_audio audio = {};
	if (!converter.Convert(data, size, format, audio))
		return;

	/* Stamped on arrival, and libobs smooths the jitter. A sound card's buffers do carry a start time
	 * (multi_audio: the performance time of the driver's recorded_real_time), but whether that marks the
	 * first or the last frame of the buffer is up to each driver, so it is no better than this. The
	 * loopback sources, whose time stamps are exact, use them (loopback-audio.cpp). */
	audio.timestamp = os_gettime_ns() - util_mul_div64(audio.frames, 1000000000ULL, audio.samples_per_sec);

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
	AudioInput *ai = new AudioInput(source);
	ai->capture = std::make_unique<HaikuCapture>(
		source, "OBS Audio Input", B_MEDIA_RAW_AUDIO, HaikuCaptureKind::Device,
		[ai](const void *data, size_t size, const media_format &format, const media_header &) {
			ai->OnBuffer(data, size, format);
		},
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

/* Called again whenever a device is plugged in or removed (HaikuCapture::DevicesChanged()). A saved device
 * that is not plugged in stays listed, so that opening the properties does not silently select another. */
obs_properties_t *audio_input_properties(void *data)
{
	AudioInput *ai = static_cast<AudioInput *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_DEVICE_ID, T_("Device"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	std::string current;
	if (ai) {
		obs_data_t *settings = obs_source_get_settings(ai->source);
		current = obs_data_get_string(settings, S_DEVICE_ID);
		obs_data_release(settings);
	}
	HaikuFillDeviceList(list, B_MEDIA_RAW_AUDIO, current.c_str());

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
