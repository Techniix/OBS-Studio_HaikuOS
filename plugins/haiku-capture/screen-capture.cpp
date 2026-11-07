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
 * Screen capture through the app_server (BScreen::ReadBitmap).
 *
 * ReadBitmap copies the framebuffer into a client bitmap, which is a full-screen copy per frame, so it runs on
 * its own thread and feeds libobs as an asynchronous BGRX source instead of blocking the graphics thread.
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <Bitmap.h>
#include <OS.h>
#include <Screen.h>

#include "capture-throttle.hpp"

#define S_CAPTURE_CURSOR "capture_cursor"
#define S_FPS "fps"

#define T_(text) obs_module_text(text)

namespace {

struct ScreenCapture {
	obs_source_t *source = nullptr;

	std::mutex settings_mutex;
	bool capture_cursor = true;
	int fps = 30;

	std::atomic_bool active = false;
	std::thread thread;

	void Start();
	void Stop();
	void Run();
};

void ScreenCapture::Start()
{
	if (active.exchange(true))
		return;

	thread = std::thread(&ScreenCapture::Run, this);
}

void ScreenCapture::Stop()
{
	if (!active.exchange(false))
		return;

	if (thread.joinable())
		thread.join();

	/* Clear the last frame so a hidden source doesn't keep showing stale content */
	obs_source_output_video(source, nullptr);
}

void ScreenCapture::Run()
{
	os_set_thread_name("haiku-screen-capture");

	BScreen screen(B_MAIN_SCREEN_ID);
	if (!screen.IsValid()) {
		blog(LOG_ERROR, "[haiku-capture] Unable to connect to the main screen");
		return;
	}

	/* below the app_server and normal applications; the copy itself is throttled by CaptureThrottle */
	set_thread_priority(find_thread(nullptr), B_LOW_PRIORITY);

	CaptureThrottle throttle("Screen capture");
	BandedScreenReader reader;
	std::vector<uint8_t> pixels;
	uint32_t stride = 0;

	while (active) {
		bool cursor;
		int rate;
		{
			std::lock_guard<std::mutex> lock(settings_mutex);
			cursor = capture_cursor;
			rate = fps;
		}

		const BRect frame = screen.Frame();
		const uint64_t timestamp = os_gettime_ns();

		const uint64_t took = reader.Read(screen, frame, cursor, pixels, stride);
		if (took) {
			const uint32_t width = (uint32_t)frame.IntegerWidth() + 1;
			const uint32_t height = (uint32_t)frame.IntegerHeight() + 1;
			throttle.Record(took, (uint64_t)width * height);

			struct obs_source_frame out = {};
			out.data[0] = pixels.data();
			out.linesize[0] = stride;
			out.width = width;
			out.height = height;
			out.format = VIDEO_FORMAT_BGRX;
			out.full_range = true;
			out.timestamp = timestamp;
			obs_source_output_video(source, &out);
		} else {
			os_sleep_ms(100);
		}

		throttle.Wait(rate);
	}
}

const char *screen_capture_get_name(void *)
{
	return T_("ScreenCapture");
}

void screen_capture_update(void *data, obs_data_t *settings)
{
	ScreenCapture *sc = static_cast<ScreenCapture *>(data);
	std::lock_guard<std::mutex> lock(sc->settings_mutex);

	sc->capture_cursor = obs_data_get_bool(settings, S_CAPTURE_CURSOR);
	sc->fps = (int)obs_data_get_int(settings, S_FPS);
}

void *screen_capture_create(obs_data_t *settings, obs_source_t *source)
{
	ScreenCapture *sc = new ScreenCapture;
	sc->source = source;
	screen_capture_update(sc, settings);
	return sc;
}

void screen_capture_destroy(void *data)
{
	ScreenCapture *sc = static_cast<ScreenCapture *>(data);
	sc->Stop();
	delete sc;
}

void screen_capture_show(void *data)
{
	static_cast<ScreenCapture *>(data)->Start();
}

void screen_capture_hide(void *data)
{
	static_cast<ScreenCapture *>(data)->Stop();
}

void screen_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_CAPTURE_CURSOR, true);
	obs_data_set_default_int(settings, S_FPS, 30);
}

obs_properties_t *screen_capture_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_bool(props, S_CAPTURE_CURSOR, T_("CaptureCursor"));
	obs_property_t *fps = obs_properties_add_int_slider(props, S_FPS, T_("FrameRate"), 1, 60, 1);
	obs_property_int_set_suffix(fps, " FPS");

	return props;
}

} // namespace

void haiku_register_screen_capture(void)
{
	struct obs_source_info info = {};
	info.id = "haiku_screen_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = screen_capture_get_name;
	info.create = screen_capture_create;
	info.destroy = screen_capture_destroy;
	info.update = screen_capture_update;
	info.get_defaults = screen_capture_defaults;
	info.get_properties = screen_capture_properties;
	info.show = screen_capture_show;
	info.hide = screen_capture_hide;
	info.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE;

	obs_register_source(&info);
}
