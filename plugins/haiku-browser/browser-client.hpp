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
 * OBS-process side of one HaikuWebKit page: owns the shared frame area, starts (and restarts) the
 * obs-haiku-browser helper, sends it configuration and input, and reads frames. Used by both the browser
 * source and the browser docks. Thread-safe: input usually comes from the UI thread while another thread
 * polls frames.
 */

#pragma once

#include "browser-shm.h"

#include <Message.h>
#include <Messenger.h>
#include <OS.h>

#include <mutex>
#include <string>
#include <vector>

#include <sys/types.h>

struct HaikuBrowserConfig {
	std::string url;
	int width = 800;
	int height = 600;
	int fps = 30;
	std::string css;
	bool transparent = true;
};

struct HaikuBrowserFrame {
	std::vector<uint8_t> pixels; /* BGRA, `stride` bytes per row */
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t stride = 0;
	bool has_alpha = false;
};

class HaikuBrowserClient {
public:
	/* storage_path: WebKit persistent storage (local storage, databases) for this page; empty = shared default */
	explicit HaikuBrowserClient(const char *log_name, const std::string &storage_path = std::string());
	~HaikuBrowserClient();

	HaikuBrowserClient(const HaikuBrowserClient &) = delete;
	HaikuBrowserClient &operator=(const HaikuBrowserClient &) = delete;

	/* Applies a configuration; starts the helper on first use. Resizing reallocates the area if needed. */
	void Configure(const HaikuBrowserConfig &config);

	/* Stops the helper (e.g. "shutdown when not visible"); the next Configure()/Poll() after Start() brings
	 * it back with the last configuration. */
	void Stop();
	void Start();
	bool Running() const;

	/* Heartbeat, crash detection/restart and pending configuration. Call regularly (every frame or so). */
	void Poll();

	/* Copies the newest complete frame if it is newer than the last one returned. */
	bool ReadFrame(HaikuBrowserFrame &frame);

	/* Page information (for docks); returns true when it changed since the last call. */
	bool ReadInfo(std::string &title, std::string &url);

	void Reload(bool nocache);
	void ExecuteJavaScript(const std::string &script);
	void SetStartupScript(const std::string &script);
	void SetVisibility(bool visible);
	void SetActive(bool active);
	void Zoom(int direction);

	/* x/y in page pixels; buttons are B_PRIMARY_MOUSE_BUTTON etc.; modifiers are Haiku modifier bits */
	void MouseDown(int x, int y, int32 buttons, int32 clicks, int32 modifiers);
	void MouseUp(int x, int y, int32 buttons, int32 clicks, int32 modifiers);
	void MouseMove(int x, int y, int32 modifiers, bool leave);
	void MouseWheel(float dx, float dy, int32 modifiers);
	/* bytes: UTF-8 text of the key or a Haiku control byte (B_LEFT_ARROW...); key: Haiku raw key code */
	void Key(bool down, const std::string &bytes, int32 key, int32 modifiers);
	void Focus(bool focus);

private:
	bool CreateArea(uint32_t width, uint32_t height);
	void DestroyArea();
	void RetireArea();
	bool TrySendAttach();
	obs_browser_shm *HelperShm() const;
	bool SpawnHelper();
	void StopHelper();
	bool HelperAlive() const;
	bool SendLocked(BMessage &message);
	void SendConfigLocked();

	mutable std::mutex mutex;
	std::string name;
	std::string storage;

	HaikuBrowserConfig config;
	bool config_dirty = false;
	bool stopped = false;

	area_id area = -1;
	obs_browser_shm *shm = nullptr;
	uint32_t area_width = 0, area_height = 0;

	/* While a switch to a bigger area hasn't reached the helper, it keeps using the previous one: keep it
	 * mapped, read the helper's state from it and keep its heartbeat going. */
	area_id retired_area = -1;
	obs_browser_shm *retired_shm = nullptr;
	bool attach_pending = false;

	BMessenger messenger;

	pid_t helper = -1;
	bigtime_t helper_started = 0;
	bigtime_t next_restart = 0;
	int restarts = 0;
	bool ready = false;

	std::string startup_script;
	int64_t last_frame = -1;
	int32_t last_info = -1;
	int32_t pending_buttons = 0;
};
