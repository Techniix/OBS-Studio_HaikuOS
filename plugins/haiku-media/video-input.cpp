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
#include <string>

#include <GraphicsDefs.h>

#define S_DEVICE_ID "device_id"
#define S_ACTIVE_ONLY_WHEN_SHOWN "deactivate_when_not_showing"

#define T_(text) obs_module_text(text)

namespace {

struct VideoInput {
	obs_source_t *source = nullptr;
	std::string device_id;
	bool active_only_when_shown = false;
	bool shown = false;
	std::unique_ptr<HaikuRecorder> recorder;
	std::atomic_bool warned_format = false;

	void Open();
	void Close();
	void Refresh();
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

void VideoInput::Open()
{
	if (!recorder) {
		recorder = std::make_unique<HaikuRecorder>(
			"OBS Video Input", B_MEDIA_RAW_VIDEO,
			[this](const void *data, size_t size, const media_format &format) {
				OnBuffer(data, size, format);
			});
	}

	if (recorder->Open(device_id, B_MEDIA_RAW_VIDEO) == B_OK) {
		const media_raw_video_format &raw = recorder->ConnectedFormat().u.raw_video;
		blog(LOG_INFO, "[haiku-media] '%s': capturing from '%s' (%ux%u, color space 0x%x, %.2f fps)",
		     obs_source_get_name(source), device_id.c_str(), raw.display.line_width, raw.display.line_count,
		     (unsigned)raw.display.format, raw.field_rate);
	}
}

void VideoInput::Close()
{
	if (recorder)
		recorder->Close();

	obs_source_output_video(source, nullptr);
}

void VideoInput::Refresh()
{
	const bool want_open = !active_only_when_shown || shown;
	const bool is_open = recorder && recorder->IsOpen();

	if (want_open && !is_open)
		Open();
	else if (!want_open && is_open)
		Close();
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
	std::string device_id = obs_data_get_string(settings, S_DEVICE_ID);
	vi->active_only_when_shown = obs_data_get_bool(settings, S_ACTIVE_ONLY_WHEN_SHOWN);

	if (device_id != vi->device_id) {
		vi->device_id = device_id;
		vi->warned_format = false;
		if (vi->recorder && vi->recorder->IsOpen())
			vi->Close();
	}

	vi->Refresh();
}

void *video_input_create(obs_data_t *settings, obs_source_t *source)
{
	VideoInput *vi = new VideoInput;
	vi->source = source;
	vi->device_id = obs_data_get_string(settings, S_DEVICE_ID);
	video_input_update(vi, settings);
	return vi;
}

void video_input_destroy(void *data)
{
	VideoInput *vi = static_cast<VideoInput *>(data);
	vi->recorder.reset();
	delete vi;
}

void video_input_show(void *data)
{
	VideoInput *vi = static_cast<VideoInput *>(data);
	vi->shown = true;
	vi->Refresh();
}

void video_input_hide(void *data)
{
	VideoInput *vi = static_cast<VideoInput *>(data);
	vi->shown = false;
	vi->Refresh();
}

void video_input_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_DEVICE_ID, HAIKU_MEDIA_DEFAULT_DEVICE);
	obs_data_set_default_bool(settings, S_ACTIVE_ONLY_WHEN_SHOWN, false);
}

obs_properties_t *video_input_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_DEVICE_ID, T_("Device"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	HaikuFillDeviceList(list, B_MEDIA_RAW_VIDEO);

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
