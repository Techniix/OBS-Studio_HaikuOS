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
 * Window Capture through Renga's capture API (BScreenCapture::StartWindow(), F-31 phase 3).
 *
 * What the app_server does for a window session (src/servers/app/ScreenCaptureManager.cpp, _SourceFrame() and
 * _Frame()), and what this source does about it:
 *
 *  - It copies the window's frame from the screen (with tab and border under SCREEN_CAPTURE_WINDOW_BORDER),
 *    clipped to the screen. There is no compositor (roadmap F-33), so this is not an isolated capture: a
 *    window on top of the captured one is in the picture. The properties say so.
 *  - It follows the window: Frame() changes when the window moves or changes size, the bitmap is filled from
 *    its top left. CaptureSource keeps the bitmap large enough.
 *  - It does not look at whether the window is visible: for a minimized or hidden window, or one on another
 *    workspace, it would copy whatever is on screen at the window's position. So when a frame has changed,
 *    this source asks for the window's state (get_window_info(), as Deskbar and the Switcher do) before it
 *    shows the frame, and pauses, keeping the last picture, until the window is back.
 *  - B_ENTRY_NOT_FOUND means the window is closed, or entirely outside the screen. The picture is cleared and
 *    the window looked for again once a second, by application and title, so it is found again when it
 *    reopens with a new token.
 *
 * The window list comes from the private window-list API (WindowInfo.h); the same tokens are what
 * StartWindow() takes.
 */

#include "capture-source.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

#include <InterfaceDefs.h>
#include <Rect.h>
#include <Roster.h>

#include <WindowInfo.h> /* Renga/Haiku private header (headers/private/interface) */

#define S_WINDOW "window"
#define S_DECORATIONS "include_decorations"

/* Window ids are "<application name>\x1f<window title>"; the unit separator never appears in either. */
#define ID_SEPARATOR '\x1f'

namespace {

const uint64_t kSecond = 1000000000ULL;
const uint64_t kVisibilityPoll = 250000000ULL;

struct WindowEntry {
	int32 token = -1;
	team_id team = -1;
	std::string app;
	std::string title;
};

std::string app_name_for_team(team_id team)
{
	app_info info;
	if (be_roster->GetRunningAppInfo(team, &info) == B_OK) {
		return info.ref.name;
	}
	return "?";
}

/* Reads a window's title and team; false for windows that aren't offered (hidden ones, the desktop). The
 * application name is looked up only with `lookup_app` (a roster round trip). */
bool read_window(int32 token, WindowEntry &entry, bool lookup_app)
{
	client_window_info *info = get_window_info(token);
	if (!info) {
		return false;
	}

	entry.token = token;
	entry.title = info->name;
	entry.team = info->team;
	/* layer (ServerWindow::GetInfo()): 0 hidden, 1 not visible now (another workspace), 2 desktop, 3 normal,
	 * 4 floating or modal */
	const bool normal = info->layer >= 3 || info->layer == 1 || info->is_mini;
	free(info);

	if (!normal) {
		return false;
	}

	if (lookup_app) {
		entry.app = app_name_for_team(entry.team);
	}
	return true;
}

enum class Visibility { Gone, Hidden, OnScreen };

/* Whether the window's area of the screen shows the window now (other windows may still cover it). */
Visibility window_visibility(int32 token)
{
	client_window_info *info = get_window_info(token);
	if (!info) {
		return Visibility::Gone;
	}

	const bool on_workspace = (info->workspaces & (1u << current_workspace())) != 0;
	const bool on_screen = info->layer >= 3 && !info->is_mini && info->show_hide_level <= 0 && on_workspace;
	free(info);
	return on_screen ? Visibility::OnScreen : Visibility::Hidden;
}

std::vector<WindowEntry> list_windows()
{
	std::vector<WindowEntry> windows;

	int32 count = 0;
	int32 *tokens = get_token_list(-1, &count);
	if (!tokens) {
		return windows;
	}

	/* On Haiku a process id is its team id */
	const team_id self = (team_id)getpid();

	for (int32 i = 0; i < count; i++) {
		WindowEntry entry;
		if (!read_window(tokens[i], entry, false) || entry.title.empty()) {
			continue;
		}

		/* Skip OBS's own windows (capturing them would recurse) */
		if (entry.team == self) {
			continue;
		}

		entry.app = app_name_for_team(entry.team);
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

/* Exact app+title match only. The application is looked up only for windows that have the right title. */
bool find_exact_window(const std::string &id, WindowEntry &out)
{
	const size_t sep = id.find(ID_SEPARATOR);
	if (sep == std::string::npos || sep + 1 == id.size()) {
		return false;
	}
	const std::string app = id.substr(0, sep);
	const std::string title = id.substr(sep + 1);

	int32 count = 0;
	int32 *tokens = get_token_list(-1, &count);
	if (!tokens) {
		return false;
	}

	const team_id self = (team_id)getpid();
	bool found = false;
	for (int32 i = 0; i < count && !found; i++) {
		WindowEntry entry;
		if (!read_window(tokens[i], entry, false) || entry.team == self || entry.title != title) {
			continue;
		}
		entry.app = app_name_for_team(entry.team);
		if (entry.app == app) {
			out = std::move(entry);
			found = true;
		}
	}

	free(tokens);
	return found;
}

class WindowCapture : public CaptureSource {
public:
	explicit WindowCapture(obs_source_t *source) : CaptureSource(source, "Window capture") {}

	void Update(obs_data_t *settings)
	{
		const std::string wanted = obs_data_get_string(settings, S_WINDOW);
		const bool border = obs_data_get_bool(settings, S_DECORATIONS);
		{
			std::lock_guard<std::mutex> lock(settings_mutex);
			/* The window and the border flag are fixed when a session starts */
			if (wanted != window_id || border != decorations) {
				RequestRestart();
			}
			window_id = wanted;
			decorations = border;
		}
		UpdateCommon(settings);
	}

	std::string Selection()
	{
		std::lock_guard<std::mutex> lock(settings_mutex);
		return window_id;
	}

protected:
	status_t StartSession(BScreenCapture &capture) override
	{
		std::string wanted;
		bool border;
		{
			std::lock_guard<std::mutex> lock(settings_mutex);
			wanted = window_id;
			border = decorations;
		}

		/* Only another selection drops the tracked window. After any other change (cursor, border) the
		 * session starts again for the same token: looking the window up again would miss it once its
		 * title has changed, and pick another window of the application. */
		if (wanted != id) {
			id = wanted;
			have_window = false;
			ClearOutput();
		}
		if (id.empty()) {
			return B_ENTRY_NOT_FOUND;
		}

		const uint32 flags = CommonFlags() | (border ? BPrivate::SCREEN_CAPTURE_WINDOW_BORDER : 0);
		status_t status = B_ENTRY_NOT_FOUND;
		for (int attempt = 0; attempt < 2; attempt++) {
			bool looked_up = false;
			if (!have_window) {
				if (!find_window(id, window)) {
					return B_ENTRY_NOT_FOUND;
				}
				have_window = true;
				exact = make_id(window) == id;
				looked_up = true;
			}

			status = capture.StartWindow(window.token, flags, 0);
			if (status != B_ENTRY_NOT_FOUND) {
				break;
			}
			/* The remembered token is stale: look once more. A window that was just found and still
			 * isn't there is entirely outside the screen. */
			have_window = false;
			if (looked_up) {
				break;
			}
		}
		if (status != B_OK) {
			return status;
		}

		paused = false;
		next_exact_check = os_gettime_ns() + kSecond;
		blog(LOG_INFO,
		     exact ? "[haiku-capture] Capturing window '%s' of '%s'"
			   : "[haiku-capture] Selected window not open; capturing window '%s' of '%s' until it "
			     "appears",
		     window.title.c_str(), window.app.c_str());
		return B_OK;
	}

	Poll BeforeFrame(uint64_t now) override
	{
		/* Capturing a stand-in of the same application: switch to the selected window when it opens */
		if (!exact && now >= next_exact_check) {
			next_exact_check = now + kSecond;
			WindowEntry found;
			if (find_exact_window(id, found)) {
				window = std::move(found);
				exact = true;
				return Poll::Restart;
			}
		}

		if (!paused) {
			return Poll::Capture;
		}
		if (now < next_visibility_check) {
			return Poll::Skip;
		}
		next_visibility_check = now + kVisibilityPoll;

		switch (window_visibility(window.token)) {
		case Visibility::Hidden:
			return Poll::Skip;
		case Visibility::OnScreen:
			blog(LOG_INFO, "[haiku-capture] Window '%s' is back on screen", window.title.c_str());
			break;
		case Visibility::Gone:
			/* The next frame reports it (B_ENTRY_NOT_FOUND) */
			break;
		}
		paused = false;
		return Poll::Capture;
	}

	bool AcceptFrame(uint64_t now) override
	{
		/* Asked after the frame was read, and only for frames that changed: hiding or minimizing a window
		 * changes its area of the screen, so the frame that would show what is behind it is one of them. */
		switch (window_visibility(window.token)) {
		case Visibility::OnScreen:
			return true;
		case Visibility::Hidden:
			paused = true;
			next_visibility_check = now + kVisibilityPoll;
			blog(LOG_INFO,
			     "[haiku-capture] Window '%s' is minimized, hidden or on another workspace; paused",
			     window.title.c_str());
			return false;
		case Visibility::Gone:
			break;
		}
		return false;
	}

	void SessionLost() override
	{
		blog(LOG_INFO, "[haiku-capture] Window '%s' is gone, waiting for it to appear", window.title.c_str());
		have_window = false;
	}

	const char *MissingText() const override { return T_("Status.WindowNotOpen"); }

private:
	std::mutex settings_mutex;
	std::string window_id;
	bool decorations = false;

	/* Capture thread only */
	std::string id; /* the selection `window` was found for */
	WindowEntry window;
	bool have_window = false;
	bool exact = false; /* `window` is the selected window, not another one of the same application */
	bool paused = false;
	uint64_t next_exact_check = 0;
	uint64_t next_visibility_check = 0;
};

const char *window_capture_get_name(void *)
{
	return T_("WindowCapture");
}

void window_capture_update(void *data, obs_data_t *settings)
{
	static_cast<WindowCapture *>(data)->Update(settings);
}

void *window_capture_create(obs_data_t *settings, obs_source_t *source)
{
	WindowCapture *wc = new WindowCapture(source);
	wc->Update(settings);
	return wc;
}

void window_capture_destroy(void *data)
{
	WindowCapture *wc = static_cast<WindowCapture *>(data);
	wc->Hide();
	delete wc;
}

void window_capture_show(void *data)
{
	static_cast<WindowCapture *>(data)->Show();
}

void window_capture_hide(void *data)
{
	static_cast<WindowCapture *>(data)->Hide();
}

void window_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_DECORATIONS, false);
	CaptureSource::CommonDefaults(settings);
}

void fill_window_list(obs_property_t *list, const std::string &current)
{
	obs_property_list_clear(list);

	bool have_current = current.empty();
	for (const WindowEntry &w : list_windows()) {
		const std::string id = make_id(w);
		const std::string label = "[" + w.app + "] " + w.title;
		obs_property_list_add_string(list, label.c_str(), id.c_str());
		if (id == current) {
			have_current = true;
		}
	}

	/* Keep a saved window selectable while it isn't open */
	if (!have_current) {
		std::string id = current;
		std::replace(id.begin(), id.end(), ID_SEPARATOR, ' ');
		const std::string label = std::string(T_("NotRunning")) + " " + id;
		obs_property_list_insert_string(list, 0, label.c_str(), current.c_str());
	}
}

bool refresh_windows(obs_properties_t *props, obs_property_t *, void *data)
{
	WindowCapture *wc = static_cast<WindowCapture *>(data);
	fill_window_list(obs_properties_get(props, S_WINDOW), wc ? wc->Selection() : std::string());
	return true;
}

obs_properties_t *window_capture_properties(void *data)
{
	WindowCapture *wc = static_cast<WindowCapture *>(data);
	obs_properties_t *props = obs_properties_create();

	CaptureSource::AddStatusProperties(props, wc);

	obs_property_t *list =
		obs_properties_add_list(props, S_WINDOW, T_("Window"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_window_list(list, wc ? wc->Selection() : std::string());

	obs_properties_add_button2(props, "refresh", T_("RefreshWindows"), refresh_windows, wc);
	obs_properties_add_bool(props, S_DECORATIONS, T_("IncludeDecorations"));
	CaptureSource::AddCommonProperties(props);
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
