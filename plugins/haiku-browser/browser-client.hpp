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
 * OBS-process side of one HaikuWebKit page: starts (and restarts) the obs-haiku-browser helper, sends it
 * configuration and input, and receives its frames and page information. Used by both the browser source and
 * the browser docks. See browser-ipc.hpp for the protocol.
 *
 * Threads: every public method may be called from any thread and never waits for the helper; it queues a
 * message and returns. One I/O thread per client owns the helper process, its socket and the mapped frame
 * buffer, and sleeps in epoll_wait() on {socket, frame eventfd, wake-up eventfd}. The callbacks run on that
 * thread.
 */

#pragma once

#include "browser-ipc.hpp"

#include <Message.h>
#include <OS.h>

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <time.h>

struct HaikuBrowserConfig {
	std::string url;
	int width = 800;
	int height = 600;
	int fps = 30;
	std::string css;
	bool transparent = true;
};

/* Only valid during the frame callback */
struct HaikuBrowserFrame {
	const uint8_t *pixels = nullptr; /* BGRA, `stride` bytes per row */
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t stride = 0;
	bool has_alpha = false;
};

class HaikuBrowserClient {
public:
	/* Called on the client's I/O thread, never after Stop() or the destructor returned. They must not call
	 * back into the client's Stop() or destroy it. */
	struct Callbacks {
		/* a new complete frame */
		std::function<void(const HaikuBrowserFrame &)> frame;
		/* the page's title or URL changed (may be empty) */
		std::function<void(const std::string &title, const std::string &url)> info;
	};

	/* storage_path: WebKit persistent storage (cookies, local storage) for this page; empty = shared default */
	HaikuBrowserClient(const char *log_name, Callbacks callbacks, const std::string &storage_path = std::string());
	~HaikuBrowserClient();

	HaikuBrowserClient(const HaikuBrowserClient &) = delete;
	HaikuBrowserClient &operator=(const HaikuBrowserClient &) = delete;

	/* Applies a configuration and remembers it for helpers started later. The first call starts the helper
	 * (unless the client is stopped). */
	void Configure(const HaikuBrowserConfig &config);

	/* Stop() ends the helper (e.g. "shutdown when not visible") and keeps it down; when it returns no
	 * callback is running and none follows. Start() brings the helper back with the last configuration. */
	void Stop();
	void Start();

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
	/* any thread */
	void Post(const BMessage &message, bool mouse_move = false);
	void Wake();

	/* I/O thread */
	void Run();
	bool Spawn(bigtime_t now);
	void ScheduleRestart(bigtime_t now);
	void CloseHelper();
	void HelperDied(bigtime_t now);
	void KillHelper(bigtime_t now, const char *why, const std::string &crash_report = std::string());
	void Housekeeping(bigtime_t now);
	void ReadSocket(bigtime_t now);
	bool HandleEvent(const uint8_t *payload, size_t size, bigtime_t now);
	bool MapBuffer(int fd);
	void UnmapBuffer();
	void ReadFrame();
	void Flush(bigtime_t now);
	void WatchSocket(bool writable);
	void HelperExited(bigtime_t now, pid_t pid, int status);
	template<typename Call> void Deliver(Call &&call);
	bool EnqueueLocked(std::vector<uint8_t> &&bytes, bool mouse_move);
	void QueueConfigLocked();

	const std::string name;
	const std::string storage;
	const Callbacks callbacks;

	/* --- shared, under `mutex` --- */
	std::mutex mutex;
	HaikuBrowserConfig config;
	bool have_config = false;
	std::string startup_script;
	bool stopped = false;
	bool quit = false;
	bool reset_backoff = false;
	bool connected = false; /* a helper is running: messages are queued for it */
	std::deque<std::vector<uint8_t>> queue;
	size_t queue_bytes = 0;
	bool queue_tail_is_move = false;
	bool queue_overflow_logged = false;

	/* Held by the I/O thread around a callback; Stop() passes through it so that it returns only once a
	 * callback that was already running is done. Never held together with `mutex` by the same thread while
	 * waiting for the other. */
	std::mutex deliver_mutex;

	/* --- I/O thread only (after construction) --- */
	std::thread thread;
	int epoll_fd = -1;
	int wake_fd = -1;  /* eventfd: queue or state changed */
	int frame_fd = -1; /* eventfd: the helper finished a frame; one per helper */
	int sock = -1;
	bool sock_writable_watched = false;
	pid_t helper = -1;

	bigtime_t helper_started = 0;
	time_t helper_started_wall = 0;
	bigtime_t last_alive = 0; /* start, hello or the last answered ping */
	bigtime_t last_ping = 0;
	bigtime_t last_crash_check = 0;
	int32 ping_serial = 0;
	bool ping_outstanding = false;
	bool hello = false;
	bigtime_t next_restart = 0;
	int restarts = 0;
	bool spawn_failing = false; /* logged once per streak of failed starts */

	std::vector<uint8_t> rx;
	std::deque<int> rx_fds; /* descriptors received, in order, for the buffer events being parsed */
	std::vector<uint8_t> sending;
	size_t sending_offset = 0;

	const uint8_t *map = nullptr; /* read-only mapping of the helper's current frame memfd */
	size_t map_size = 0;
	uint64_t last_frame = 0;
	bool have_last_frame = false;
	std::vector<uint8_t> copy;
};

/* Waits for helpers that are still shutting down; call from obs_module_unload(). */
void haiku_browser_shutdown(void);
