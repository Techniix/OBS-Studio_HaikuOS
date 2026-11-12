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
 *   app_server --releases the source's semaphore--> capture thread (SetFrameNotification(), F-58 / O-8: once
 *                                         when the source's area changes after a frame was taken)
 *   capture thread --NextFrame(bitmap)--> app_server copies what changed since this bitmap was last filled
 *                                         (ScreenCaptureManager::_Frame) into the bitmap, which is an area
 *                                         shared with us, and replies with changed/frame/time/visibility flags;
 *                                         taking the frame re-arms the notification
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

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <thread>

#include <Bitmap.h>
#include <OS.h>

#include <ScreenCapture.h> /* Renga private header (headers/private/interface) */

#define T_(text) obs_module_text(text)

/*
 * The one thing a capture thread sleeps on: a semaphore that is released
 *  - by the app_server when there is a new frame (the capture session's frame notification, F-58 / O-8),
 *  - by Stop() (Hide() on the graphics thread, which then joins the thread: it must wake at once),
 *  - by Wake(): the permission answer arrived, or a "Start capture" button was pressed.
 * A single semaphore keeps the stop logic simple: whatever the thread waits for, Stop() ends the wait. Every
 * wakeup is only a hint; the thread looks at its state again after each one. None of the releasers can release
 * it without bound (the app_server releases it once per frame taken, the others once per event), so a
 * spurious count can't turn a wait into a busy loop.
 */
class CaptureWakeup {
public:
	enum class Result { Woken, TimedOut, Stopped };

	CaptureWakeup() : sem(create_sem(0, "haiku-capture wakeup")) {}
	~CaptureWakeup()
	{
		if (sem >= 0) {
			delete_sem(sem);
		}
	}
	CaptureWakeup(const CaptureWakeup &) = delete;
	CaptureWakeup &operator=(const CaptureWakeup &) = delete;

	/* create_sem() can fail when the system is out of semaphores */
	bool IsValid() const { return sem >= 0; }
	/* For BScreenCapture::SetFrameNotification(): a semaphore of this team, as the app_server requires */
	sem_id Semaphore() const { return sem; }

	/* Before the thread starts: forget an earlier Stop() and releases nobody waited for */
	void Reset()
	{
		stopped = false;
		int32 count = 0;
		if (sem >= 0 && get_sem_count(sem, &count) == B_OK && count > 0) {
			acquire_sem_etc(sem, count, B_RELATIVE_TIMEOUT, 0);
		}
	}

	void Stop()
	{
		stopped = true; /* before the release: a thread that wakes sees it */
		Wake();
	}

	void Wake()
	{
		if (sem >= 0) {
			release_sem_etc(sem, 1, B_DO_NOT_RESCHEDULE);
		}
	}

	bool Stopped() const { return stopped; }

	/* Waits until woken or until os_gettime_ns() reaches `deadline` */
	Result WaitUntil(uint64_t deadline)
	{
		if (stopped) {
			return Result::Stopped;
		}
		const uint64_t now = os_gettime_ns();
		status_t status = B_TIMED_OUT;
		if (sem < 0) {
			/* Not expected (the sources refuse to start without it); at least don't spin */
			snooze((bigtime_t)std::min<uint64_t>(deadline > now ? (deadline - now) / 1000 : 0, 100000));
		} else if (deadline > now) {
			const bigtime_t timeout = (bigtime_t)((deadline - now + 999) / 1000);
			do {
				status = acquire_sem_etc(sem, 1, B_RELATIVE_TIMEOUT, timeout);
			} while (status == B_INTERRUPTED);
		}
		if (stopped) {
			return Result::Stopped;
		}
		return status == B_OK ? Result::Woken : Result::TimedOut;
	}

	Result WaitFor(uint64_t ns) { return WaitUntil(os_gettime_ns() + ns); }

private:
	const sem_id sem;
	std::atomic_bool stopped = false;
};

/*
 * The user's permission to record the screen, for the whole process (the app_server decides per application
 * signature, so one answer covers every source).
 *
 * It is asked with BScreenCapture::RequestPermission() (F-58, OBS Studio's O-7), which returns at once; the
 * answer comes as a SCREEN_CAPTURE_PERMISSION_ANSWER message to a small BLooper of this module (created on
 * first need, quit and joined in obs_module_unload()). Only after a "yes" do the sources start their sessions,
 * with a permission timeout of 0, so none of them ever waits for the prompt: hide, show and destroy on the
 * graphics thread never block on it. One question at a time is in flight for the whole module.
 */
enum class CapturePermission {
	Unknown,
	Asking,      /* a request is out; the prompt may be on screen */
	Granted,     /* as far as we know: it can be revoked at any time */
	Denied,      /* "Don't allow" (now or remembered), or nobody could ask the user */
	NotAnswered, /* the prompt was closed without an answer (B_CANCELED) */
	Unavailable, /* the request could not be sent (no BApplication, no looper, app_server error) */
};

/* The current state. Sends the request if the state is Unknown; never blocks on the user. */
CapturePermission capture_permission_poll();
/* A session start was refused (B_NOT_ALLOWED) or wants to ask (B_TIMED_OUT) although permission had been
 * granted: it was changed since. Ask again. */
void capture_permission_invalidate();
/* "Try again": ask again after a denial, an unanswered prompt or a failed request. */
void capture_permission_retry();
/* From obs_module_unload(): quits and joins the looper. No question is asked afterwards. */
void capture_permission_shutdown();
/* Every capture thread is woken when the permission changes (and by capture_wake_all()). */
void capture_register_wakeup(CaptureWakeup *wakeup);
void capture_unregister_wakeup(CaptureWakeup *wakeup);
void capture_wake_all();

class CaptureSource {
public:
	enum class State {
		Idle, /* not shown: no thread, no session */
		WaitingForPermission,
		Capturing,
		Denied,
		NotAnswered,   /* the prompt was closed without an answer */
		StoppedByUser, /* from the Screen recording indicator or preferences, or the permission was revoked */
		SourceMissing, /* the window isn't open, the region is outside the screen */
		Failed,
	};

	CaptureSource(obs_source_t *source, const char *what);
	virtual ~CaptureSource();

	/* Start and stop the capture thread. Hide() must have returned before the object is destroyed. */
	void Show();
	void Hide();

	/* The "Try again"/"Start capture" button. "Start capture" restarts every source of this module that the
	 * user stopped, not only this one: one Stop in the Screen recording indicator stops all of OBS's sessions,
	 * so one click undoes it. */
	void Restart();

	/* Reads the settings both sources have (frame rate, cursor). */
	void UpdateCommon(obs_data_t *settings);
	static void CommonDefaults(obs_data_t *settings);
	/* The state text and the restart button, when the state calls for them. `self` may be null. */
	static void AddStatusProperties(obs_properties_t *props, CaptureSource *self);
	static void AddCommonProperties(obs_properties_t *props);

protected:
	/* All of these run on the capture thread. */

	/* Starts the session, with a permission timeout of 0. B_ENTRY_NOT_FOUND: the source isn't there now (tried
	 * again once a second); B_NOT_ALLOWED: permission was revoked; B_TIMED_OUT: the app_server wants to ask
	 * the user again (the permission was set back to "ask"). */
	virtual status_t StartSession(BScreenCapture &capture) = 0;
	/* Before each frame: whether to restart the session (another window). */
	virtual bool WantsRestart(uint64_t now)
	{
		(void)now;
		return false;
	}
	/* Whether a frame that was just read may be shown, given NextFrame()'s SCREEN_CAPTURE_FRAME_* flags (always
	 * 0 for screen and region sessions). A source that refuses frames keeps showing its last one. */
	virtual bool AcceptFrame(uint32 flags)
	{
		(void)flags;
		return true;
	}
	/* NextFrame() reported that the captured window is gone. */
	virtual void SessionLost() {}
	virtual const char *MissingText() const = 0;

	/* SCREEN_CAPTURE_INCLUDE_CURSOR if the cursor is to be captured. */
	uint32 CommonFlags() const;
	/* Session parameters changed: start a new session (the flags and the source are fixed when it starts). */
	void RequestRestart()
	{
		session_generation++;
		wakeup.Wake();
	}
	/* Stop showing the last frame. */
	void ClearOutput();

	obs_source_t *const source;
	const char *const what; /* for the log */

private:
	void Run();
	bool SetState(State state); /* whether it changed */

	std::mutex lifecycle_mutex;
	std::thread thread;
	CaptureWakeup wakeup;

	std::atomic<State> state = State::Idle;
	std::atomic_bool user_stopped = false;
	/* restart_generation (capture-source.cpp) when user_stopped was set: the source starts again once any
	 * source's "Start capture" button has moved the module-wide counter on */
	std::atomic_uint32_t stopped_generation = 0;
	std::atomic_bool capture_cursor = true;
	std::atomic_int fps = 30;
	std::atomic_uint32_t session_generation = 0;
};
