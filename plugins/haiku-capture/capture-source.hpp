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
 * What the Screen Capture and Window Capture sources share: a capture thread around one Renga capture session
 * (BPrivate::BScreenCapture, roadmap F-31 phase 3; 3rdparty/ce/PORTING.md, "Screen capture and capture
 * sessions"), the user's permission, and the state shown in the source's properties.
 *
 * How a frame travels:
 *
 *   capture thread --NextFrame(bitmap)--> app_server copies what changed since this bitmap was last filled
 *                                         (ScreenCaptureManager::_Frame) into the bitmap, which is an area
 *                                         shared with us, and replies with changed/frame/time
 *   capture thread --obs_source_output_video()--> libobs copies the whole frame into its own cache (only
 *                                         when `changed`); the graphics thread uploads it as a texture
 *
 * One bitmap is enough: libobs has its own copy by the time obs_source_output_video() returns, and with a
 * second bitmap the app_server would copy every change twice (it tracks what each bitmap lacks separately).
 * The damage region isn't used: libobs has no partial update for asynchronous sources.
 *
 * Main screen only: the capture API has no screen parameter until multi-monitor support (F-38) exists.
 */

#pragma once

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <thread>

#include <Bitmap.h>
#include <OS.h>

#include <ScreenCapture.h> /* Renga private header (headers/private/interface) */

#define T_(text) obs_module_text(text)

/*
 * libobs calls show/hide (and so Hide(), which joins the capture thread) on the graphics thread. Every wait in
 * a capture thread goes through this so that Hide() wakes it at once instead of rendering stalling until a
 * sleep runs out.
 */
class StopSignal {
public:
	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopped = false;
	}

	void Stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopped = true;
		}
		cond.notify_all();
	}

	bool Stopped() const { return stopped; }

	/* Waits until os_gettime_ns() reaches `deadline`. Returns false if Stop() was called. */
	bool WaitUntil(uint64_t deadline)
	{
		std::unique_lock<std::mutex> lock(mutex);
		while (!stopped) {
			const uint64_t now = os_gettime_ns();
			if (now >= deadline) {
				return true;
			}
			cond.wait_for(lock, std::chrono::nanoseconds((int64_t)(deadline - now)));
		}
		return false;
	}

	bool WaitFor(uint64_t ns) { return WaitUntil(os_gettime_ns() + ns); }

private:
	std::mutex mutex;
	std::condition_variable cond;
	std::atomic_bool stopped = false;
};

/*
 * The user's permission to record the screen, for the whole process.
 *
 * The app_server asks the user the first time an application (by signature) starts a capture, and
 * BScreenCapture::Start...() waits for the answer. That wait can't be interrupted, and two sources waiting at
 * the same time would get two prompts when the answer is "Don't allow" without "Remember my decision" (the
 * first Start consumes the answer, the second one asks again: ScreenCaptureManager::_Start()). So the question
 * is asked once, by a helper thread of its own, and the sources' capture threads only start sessions once it
 * is answered, with a zero timeout: they never block on the prompt and can always be stopped at once.
 */
enum class CapturePermission {
	Unknown,
	Asking,  /* the prompt is (probably) on screen */
	Granted, /* as far as we know: it can be revoked at any time */
	Denied,  /* "Don't allow", no answer within the timeout, or the prompt could not be shown */
};

/* The current state. Starts asking if it is unknown; never blocks. */
CapturePermission capture_permission_poll();
/* A session start was refused although permission had been granted (revoked since): ask again. */
void capture_permission_invalidate();
/* The user wants to be asked again after a denial. */
void capture_permission_retry();

class CaptureSource {
public:
	enum class State {
		Idle, /* not shown: no thread, no session */
		WaitingForPermission,
		Capturing,
		Denied,
		StoppedByUser, /* from the Screen recording indicator or preferences, or the permission was revoked */
		SourceMissing, /* the window isn't open, the region is outside the screen */
		Failed,
	};

	CaptureSource(obs_source_t *source, const char *what);
	virtual ~CaptureSource();

	/* Start and stop the capture thread. Hide() must have returned before the object is destroyed. */
	void Show();
	void Hide();

	/* The "Try again"/"Start capture" button. */
	void Restart();

	/* Reads the settings both sources have (frame rate, cursor). */
	void UpdateCommon(obs_data_t *settings);
	static void CommonDefaults(obs_data_t *settings);
	/* The state text and the restart button, when the state calls for them. `self` may be null. */
	static void AddStatusProperties(obs_properties_t *props, CaptureSource *self);
	static void AddCommonProperties(obs_properties_t *props);

protected:
	enum class Poll { Capture, Skip, Restart };

	/* All of these run on the capture thread. */

	/* Starts the session, with a permission timeout of 0. B_ENTRY_NOT_FOUND: the source isn't there now (tried
	 * again once a second); B_NOT_ALLOWED: permission was revoked. */
	virtual status_t StartSession(BScreenCapture &capture) = 0;
	/* Before each frame: skip it (source paused) or restart the session (another window). */
	virtual Poll BeforeFrame(uint64_t now)
	{
		(void)now;
		return Poll::Capture;
	}
	/* Whether a frame that was just read may be shown. */
	virtual bool AcceptFrame(uint64_t now)
	{
		(void)now;
		return true;
	}
	/* NextFrame() reported that the captured window is gone. */
	virtual void SessionLost() {}
	virtual const char *MissingText() const = 0;

	/* SCREEN_CAPTURE_INCLUDE_CURSOR if the cursor is to be captured. */
	uint32 CommonFlags() const;
	/* Session parameters changed: start a new session (the flags and the source are fixed when it starts). */
	void RequestRestart() { session_generation++; }
	/* Stop showing the last frame. */
	void ClearOutput();

	obs_source_t *const source;
	const char *const what; /* for the log */

private:
	void Run();
	bool SetState(State state); /* whether it changed */

	std::mutex lifecycle_mutex;
	std::thread thread;
	StopSignal stop;

	std::atomic<State> state = State::Idle;
	std::atomic_bool user_stopped = false;
	std::atomic_bool capture_cursor = true;
	std::atomic_int fps = 30;
	std::atomic_uint32_t session_generation = 0;
};
