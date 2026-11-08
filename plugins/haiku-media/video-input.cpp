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

/* Video capture devices (webcams via the usb_webcam add-on, TV tuners, ...) through the Media Kit. */

#include "media-device.hpp"

#include <util/platform.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <GraphicsDefs.h>

#define S_DEVICE_ID "device_id"
#define S_RESOLUTION "resolution"
#define S_ACTIVE_ONLY_WHEN_SHOWN "deactivate_when_not_showing"

#define T_(text) obs_module_text(text)

namespace {

struct VideoInput {
	obs_source_t *source = nullptr;
	/* opens the device and keeps it open, retrying while it is busy or gone (media-device.hpp) */
	std::unique_ptr<HaikuCapture> capture;
	std::atomic_bool warned_format = false;

	/* update() runs on the UI thread, show()/hide() wherever libobs calls them; properties() reads too */
	std::mutex settings_mutex;
	std::string device_id;
	std::string resolution;
	bool active_only_when_shown = false;
	bool shown = false;

	void Refresh(); /* settings_mutex held */
	void OnOpened(const media_format &format, const HaikuOpenResult &result);
	void OnBuffer(const void *data, size_t size, const media_format &format);
};

bool map_color_space(color_space cs, video_format &format, uint32_t &bytes_per_pixel)
{
	switch (cs) {
	case B_RGB32: /* B,G,R,X in memory */
		format = VIDEO_FORMAT_BGRX;
		bytes_per_pixel = 4;
		return true;
	case B_RGBA32: /* B,G,R,A */
		format = VIDEO_FORMAT_BGRA;
		bytes_per_pixel = 4;
		return true;
	case B_RGB24: /* B,G,R */
		format = VIDEO_FORMAT_BGR3;
		bytes_per_pixel = 3;
		return true;
	case B_YCbCr422: /* Y0,Cb,Y1,Cr */
		format = VIDEO_FORMAT_YUY2;
		bytes_per_pixel = 2;
		return true;
	default:
		return false;
	}
}

/* supervisor thread */
void VideoInput::OnOpened(const media_format &format, const HaikuOpenResult &result)
{
	const media_raw_video_format &raw = format.u.raw_video;
	warned_format = false;
	if (result.size_refused)
		blog(LOG_WARNING, "[haiku-media] '%s': the device refused the requested frame size; using its default",
		     obs_source_get_name(source));
	blog(LOG_INFO, "[haiku-media] '%s': capturing (%ux%u, color space 0x%x, %.2f fps)",
	     obs_source_get_name(source), raw.display.line_width, raw.display.line_count,
	     (unsigned)raw.display.format, raw.field_rate);
}

void VideoInput::Refresh()
{
	capture->Configure(device_id, HaikuParseVideoSize(resolution.c_str()), !active_only_when_shown || shown);
}

void VideoInput::OnBuffer(const void *data, size_t size, const media_format &format)
{
	const media_raw_video_format &raw = format.u.raw_video;
	const media_video_display_info &display = raw.display;

	video_format obs_format;
	uint32_t bpp;
	if (!map_color_space(display.format, obs_format, bpp) || !display.line_width || !display.line_count) {
		if (!warned_format.exchange(true))
			blog(LOG_WARNING, "[haiku-media] '%s': unsupported video format (color space 0x%x, %ux%u)",
			     obs_source_get_name(source), (unsigned)display.format, display.line_width,
			     display.line_count);
		return;
	}

	const uint32_t linesize = display.bytes_per_row ? display.bytes_per_row : display.line_width * bpp;
	const size_t offset = (size_t)display.line_offset * linesize + (size_t)display.pixel_offset * bpp;
	if (offset + (size_t)linesize * display.line_count > size)
		return; /* truncated buffer */

	struct obs_source_frame2 frame = {};
	frame.data[0] = (uint8_t *)data + offset;
	frame.linesize[0] = linesize;
	frame.width = display.line_width;
	frame.height = display.line_count;
	frame.format = obs_format;
	frame.timestamp = os_gettime_ns();
	frame.flip = raw.orientation == B_VIDEO_BOTTOM_LEFT_RIGHT;

	if (obs_format == VIDEO_FORMAT_YUY2) {
		frame.range = VIDEO_RANGE_PARTIAL;
		video_format_get_parameters_for_format(VIDEO_CS_DEFAULT, frame.range, frame.format, frame.color_matrix,
						       frame.color_range_min, frame.color_range_max);
	} else {
		frame.range = VIDEO_RANGE_FULL;
	}

	obs_source_output_video2(source, &frame);
}

const char *video_input_get_name(void *)
{
	return T_("VideoInput");
}

void video_input_update(void *data, obs_data_t *settings)
{
	VideoInput *vi = static_cast<VideoInput *>(data);
	std::lock_guard<std::mutex> lock(vi->settings_mutex);
	vi->device_id = obs_data_get_string(settings, S_DEVICE_ID);
	vi->resolution = obs_data_get_string(settings, S_RESOLUTION);
	vi->active_only_when_shown = obs_data_get_bool(settings, S_ACTIVE_ONLY_WHEN_SHOWN);
	vi->Refresh();
}

void *video_input_create(obs_data_t *settings, obs_source_t *source)
{
	VideoInput *vi = new VideoInput;
	vi->source = source;
	vi->capture = std::make_unique<HaikuCapture>(
		source, "OBS Video Input", B_MEDIA_RAW_VIDEO,
		[vi](const void *data, size_t size, const media_format &format) { vi->OnBuffer(data, size, format); },
		[vi](const media_format &format, const HaikuOpenResult &result) { vi->OnOpened(format, result); },
		[vi]() { obs_source_output_video(vi->source, nullptr); });
	video_input_update(vi, settings);
	return vi;
}

void video_input_destroy(void *data)
{
	VideoInput *vi = static_cast<VideoInput *>(data);
	vi->capture.reset(); /* stops the supervisor and recorder threads before VideoInput goes away */
	delete vi;
}

void video_input_show(void *data)
{
	VideoInput *vi = static_cast<VideoInput *>(data);
	std::lock_guard<std::mutex> lock(vi->settings_mutex);
	vi->shown = true;
	vi->Refresh();
}

void video_input_hide(void *data)
{
	VideoInput *vi = static_cast<VideoInput *>(data);
	std::lock_guard<std::mutex> lock(vi->settings_mutex);
	vi->shown = false;
	vi->Refresh();
}

void video_input_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_DEVICE_ID, HAIKU_MEDIA_DEFAULT_DEVICE);
	obs_data_set_default_string(settings, S_RESOLUTION, HAIKU_MEDIA_DEFAULT_SIZE);
	obs_data_set_default_bool(settings, S_ACTIVE_ONLY_WHEN_SHOWN, false);
}

/* the frame sizes depend on the device */
bool video_input_device_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *sizes = obs_properties_get(props, S_RESOLUTION);
	if (sizes)
		HaikuFillVideoSizeList(sizes, obs_data_get_string(settings, S_DEVICE_ID),
				       obs_data_get_string(settings, S_RESOLUTION));
	return true;
}

obs_properties_t *video_input_properties(void *data)
{
	VideoInput *vi = static_cast<VideoInput *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_DEVICE_ID, T_("Device"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	HaikuFillDeviceList(list, B_MEDIA_RAW_VIDEO);
	obs_property_set_modified_callback(list, video_input_device_modified);

	obs_property_t *sizes = obs_properties_add_list(props, S_RESOLUTION, T_("Resolution"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	std::string device_id = HAIKU_MEDIA_DEFAULT_DEVICE, resolution = HAIKU_MEDIA_DEFAULT_SIZE;
	if (vi) {
		std::lock_guard<std::mutex> lock(vi->settings_mutex);
		device_id = vi->device_id;
		resolution = vi->resolution;
	}
	HaikuFillVideoSizeList(sizes, device_id, resolution.c_str());
	obs_property_set_long_description(sizes, T_("Resolution.Description"));

	obs_properties_add_bool(props, S_ACTIVE_ONLY_WHEN_SHOWN, T_("DeactivateWhenNotShowing"));

	return props;
}

} // namespace

void haiku_register_video_input(void)
{
	struct obs_source_info info = {};
	info.id = "haiku_video_input_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = video_input_get_name;
	info.create = video_input_create;
	info.destroy = video_input_destroy;
	info.update = video_input_update;
	info.get_defaults = video_input_defaults;
	info.get_properties = video_input_properties;
	info.show = video_input_show;
	info.hide = video_input_hide;
	info.icon_type = OBS_ICON_TYPE_CAMERA;

	obs_register_source(&info);
}
