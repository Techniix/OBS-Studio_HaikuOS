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
 * Window capture, screen-region implementation.
 *
 * The app_server keeps no per-window backing store that other teams can read (it composes straight into
 * the frame buffer), so a window's own pixels can't be captured in isolation. This source instead tracks
 * a window through the window-list API that Deskbar and the Switcher use (get_token_list/get_window_info),
 * and reads exactly that window's screen area every frame with BScreen::ReadBitmap(). It follows moves and
 * resizes, pauses while the window is minimized, hidden or on another workspace, and reports when other
 * windows cover it (their pixels are then captured too). This is a fallback, not isolated window capture.
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <Bitmap.h>
#include <InterfaceDefs.h>
#include <OS.h>
#include <Roster.h>
#include <Screen.h>

#include "capture-throttle.hpp"
#include "haiku-window-info.h"

#define S_WINDOW "window"
#define S_DECORATIONS "include_decorations"
#define S_CAPTURE_CURSOR "capture_cursor"
#define S_FPS "fps"

#define T_(text) obs_module_text(text)

/* Window ids are "<application name>\x1f<window title>"; the unit separator never appears in either. */
#define ID_SEPARATOR '\x1f'

namespace {

struct WindowEntry {
	int32 token = -1;
	team_id team = -1;
	std::string app;
	std::string title;
	BRect frame;     /* content area */
	BRect decorated; /* including tab and border */
	bool on_screen = false;
};

std::string app_name_for_team(team_id team)
{
	app_info info;
	if (be_roster->GetRunningAppInfo(team, &info) == B_OK)
		return info.ref.name;
	return "?";
}

bool read_window(int32 token, WindowEntry &entry)
{
	client_window_info *info = get_window_info(token);
	if (!info)
		return false;

	entry.token = token;
	entry.title = info->name;
	entry.frame = BRect(info->window_left, info->window_top, info->window_right, info->window_bottom);

	const float border = info->border_size;
	entry.decorated = entry.frame;
	entry.decorated.InsetBy(-border, -border);
	entry.decorated.top -= info->tab_height;

	const bool on_workspace = (info->workspaces & (1u << current_workspace())) != 0;
	entry.on_screen = info->layer >= 3 && !info->is_mini && info->show_hide_level <= 0 && on_workspace;

	const team_id team = info->team;
	entry.team = team;
	const bool normal = info->layer >= 3 || info->layer == 1 || info->is_mini;
	free(info);

	if (!normal)
		return false; /* hidden or desktop windows */

	entry.app = app_name_for_team(team);
	return true;
}

std::vector<WindowEntry> list_windows()
{
	std::vector<WindowEntry> windows;

	int32 count = 0;
	int32 *tokens = get_token_list(-1, &count);
	if (!tokens)
		return windows;

	/* On Haiku a process id is its team id */
	const team_id self = (team_id)getpid();

	for (int32 i = 0; i < count; i++) {
		WindowEntry entry;
		if (!read_window(tokens[i], entry) || entry.title.empty())
			continue;

		/* Skip OBS's own windows (capturing them would recurse) */
		if (entry.team == self)
			continue;

		windows.push_back(std::move(entry));
	}

	free(tokens);
	return windows;
}

std::string make_id(const WindowEntry &w)
{
	return w.app + ID_SEPARATOR + w.title;
}

/* Exact app+title match first, then the first window of the same application (titles change, e.g. in
 * a web browser). */
bool find_window(const std::string &id, WindowEntry &out)
{
	const size_t sep = id.find(ID_SEPARATOR);
	const std::string app = sep == std::string::npos ? id : id.substr(0, sep);

	std::vector<WindowEntry> windows = list_windows();

	for (const WindowEntry &w : windows) {
		if (make_id(w) == id) {
			out = w;
			return true;
		}
	}
	for (const WindowEntry &w : windows) {
		if (w.app == app) {
			out = w;
			return true;
		}
	}
	return false;
}

/* Whether another window in front of `token` overlaps `rect` on the current workspace. */
bool is_covered(int32 token, const BRect &rect)
{
	int32 *tokens = nullptr;
	int32 count = 0;
	if (BPrivate::get_window_order(current_workspace(), &tokens, &count) != B_OK || !tokens)
		return false;

	bool covered = false;
	for (int32 i = 0; i < count && tokens[i] != token; i++) {
		client_window_info *info = get_window_info(tokens[i]);
		if (!info)
			continue;

		BRect other(info->window_left, info->window_top, info->window_right, info->window_bottom);
		other.InsetBy(-info->border_size, -info->border_size);
		other.top -= info->tab_height;
		const bool visible = info->layer >= 3 && !info->is_mini && info->show_hide_level <= 0;
		free(info);

		if (visible && other.Intersects(rect)) {
			covered = true;
			break;
		}
	}

	free(tokens);
	return covered;
}

struct WindowCapture {
	obs_source_t *source = nullptr;

	std::mutex settings_mutex;
	std::string window_id;
	bool decorations = false;
	bool capture_cursor = true;
	int fps = 30;
	bool settings_changed = true;

	std::atomic_bool active = false;
	std::thread thread;

	void Start();
	void Stop();
	void Run();
};

void WindowCapture::Start()
{
	if (active.exchange(true))
		return;
	thread = std::thread(&WindowCapture::Run, this);
}

void WindowCapture::Stop()
{
	if (!active.exchange(false))
		return;
	if (thread.joinable())
		thread.join();
	obs_source_output_video(source, nullptr);
}

void WindowCapture::Run()
{
	os_set_thread_name("haiku-window-capture");
	set_thread_priority(find_thread(nullptr), B_LOW_PRIORITY);

	BScreen screen(B_MAIN_SCREEN_ID);
	if (!screen.IsValid()) {
		blog(LOG_ERROR, "[haiku-capture] Unable to connect to the main screen");
		return;
	}

	BandedScreenReader reader;
	std::vector<uint8_t> pixels;
	uint32_t stride = 0;
	WindowEntry window;
	bool have_window = false;
	bool was_covered = false;
	bool warned_missing = false;
	CaptureThrottle throttle("Window capture");
	uint64_t next_rescan = 0;
	uint64_t next_cover_check = 0;

	while (active) {
		std::string id;
		bool decor, cursor;
		int rate;
		{
			std::lock_guard<std::mutex> lock(settings_mutex);
			id = window_id;
			decor = decorations;
			cursor = capture_cursor;
			rate = fps > 0 ? fps : 30;
			if (settings_changed) {
				have_window = false;
				next_rescan = 0;
				settings_changed = false;
			}
		}
		const uint64_t now = os_gettime_ns();

		/* Refresh the window's geometry each frame; look it up again if it disappeared */
		if (have_window && !read_window(window.token, window))
			have_window = false;

		if (!have_window && !id.empty() && now >= next_rescan) {
			have_window = find_window(id, window);
			next_rescan = now + 1000000000ULL; /* retry once a second */
			if (have_window) {
				warned_missing = false;
				blog(LOG_INFO, "[haiku-capture] Capturing window '%s' of '%s'", window.title.c_str(),
				     window.app.c_str());
			} else if (!warned_missing) {
				warned_missing = true;
				blog(LOG_INFO, "[haiku-capture] Window not found, waiting for it to appear");
			}
		}

		if (have_window && window.on_screen) {
			BRect rect = decor ? window.decorated : window.frame;
			rect = rect & screen.Frame();
			rect.left = floorf(rect.left);
			rect.top = floorf(rect.top);
			rect.right = floorf(rect.right);
			rect.bottom = floorf(rect.bottom);

			if (rect.IsValid() && rect.Width() >= 1 && rect.Height() >= 1) {
				if (now >= next_cover_check) {
					next_cover_check = now + 500000000ULL;
					bool covered = is_covered(window.token, rect);
					if (covered != was_covered)
						blog(LOG_INFO,
						     covered ? "[haiku-capture] Window '%s' is partly covered; "
							       "the covering windows are captured too"
							     : "[haiku-capture] Window '%s' is no longer covered",
						     window.title.c_str());
					was_covered = covered;
				}

				const uint64_t took = reader.Read(screen, rect, cursor, pixels, stride);
				if (took) {
					const uint32_t width = (uint32_t)rect.IntegerWidth() + 1;
					const uint32_t height = (uint32_t)rect.IntegerHeight() + 1;
					throttle.Record(took, (uint64_t)width * height);

					struct obs_source_frame frame = {};
					frame.data[0] = pixels.data();
					frame.linesize[0] = stride;
					frame.width = width;
					frame.height = height;
					frame.format = VIDEO_FORMAT_BGRX;
					frame.full_range = true;
					frame.timestamp = now;
					obs_source_output_video(source, &frame);
				}
			}
		}
		/* Minimized/hidden/other workspace: keep showing the last frame */

		throttle.Wait(rate);
	}
}

const char *window_capture_get_name(void *)
{
	return T_("WindowCapture");
}

void window_capture_update(void *data, obs_data_t *settings)
{
	WindowCapture *wc = static_cast<WindowCapture *>(data);
	std::lock_guard<std::mutex> lock(wc->settings_mutex);

	wc->window_id = obs_data_get_string(settings, S_WINDOW);
	wc->decorations = obs_data_get_bool(settings, S_DECORATIONS);
	wc->capture_cursor = obs_data_get_bool(settings, S_CAPTURE_CURSOR);
	wc->fps = (int)obs_data_get_int(settings, S_FPS);
	wc->settings_changed = true;
}

void *window_capture_create(obs_data_t *settings, obs_source_t *source)
{
	WindowCapture *wc = new WindowCapture;
	wc->source = source;
	window_capture_update(wc, settings);
	return wc;
}

void window_capture_destroy(void *data)
{
	WindowCapture *wc = static_cast<WindowCapture *>(data);
	wc->Stop();
	delete wc;
}

void window_capture_show(void *data)
{
	static_cast<WindowCapture *>(data)->Start();
}

void window_capture_hide(void *data)
{
	static_cast<WindowCapture *>(data)->Stop();
}

void window_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_DECORATIONS, false);
	obs_data_set_default_bool(settings, S_CAPTURE_CURSOR, true);
	obs_data_set_default_int(settings, S_FPS, 30);
}

void fill_window_list(obs_property_t *list, const char *current)
{
	obs_property_list_clear(list);

	bool have_current = !current || !*current;
	for (const WindowEntry &w : list_windows()) {
		const std::string id = make_id(w);
		const std::string label = "[" + w.app + "] " + w.title;
		obs_property_list_add_string(list, label.c_str(), id.c_str());
		if (id == current)
			have_current = true;
	}

	/* Keep a saved window selectable while it isn't open */
	if (!have_current) {
		std::string id = current;
		std::replace(id.begin(), id.end(), ID_SEPARATOR, ' ');
		const std::string label = std::string(T_("NotRunning")) + " " + id;
		obs_property_list_insert_string(list, 0, label.c_str(), current);
	}
}

bool refresh_windows(obs_properties_t *props, obs_property_t *, void *data)
{
	WindowCapture *wc = static_cast<WindowCapture *>(data);
	std::string current;
	if (wc) {
		std::lock_guard<std::mutex> lock(wc->settings_mutex);
		current = wc->window_id;
	}
	fill_window_list(obs_properties_get(props, S_WINDOW), current.c_str());
	return true;
}

obs_properties_t *window_capture_properties(void *data)
{
	WindowCapture *wc = static_cast<WindowCapture *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_WINDOW, T_("Window"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	std::string current;
	if (wc) {
		std::lock_guard<std::mutex> lock(wc->settings_mutex);
		current = wc->window_id;
	}
	fill_window_list(list, current.c_str());

	obs_properties_add_button2(props, "refresh", T_("RefreshWindows"), refresh_windows, wc);
	obs_properties_add_bool(props, S_DECORATIONS, T_("IncludeDecorations"));
	obs_properties_add_bool(props, S_CAPTURE_CURSOR, T_("CaptureCursor"));
	obs_property_t *fps = obs_properties_add_int_slider(props, S_FPS, T_("FrameRate"), 1, 60, 1);
	obs_property_int_set_suffix(fps, " FPS");
	obs_properties_add_text(props, "note", T_("WindowCaptureNote"), OBS_TEXT_INFO);

	return props;
}

} // namespace

void haiku_register_window_capture(void)
{
	struct obs_source_info info = {};
	info.id = "haiku_window_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_name = window_capture_get_name;
	info.create = window_capture_create;
	info.destroy = window_capture_destroy;
	info.update = window_capture_update;
	info.get_defaults = window_capture_defaults;
	info.get_properties = window_capture_properties;
	info.show = window_capture_show;
	info.hide = window_capture_hide;
	info.icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE;

	obs_register_source(&info);
}
