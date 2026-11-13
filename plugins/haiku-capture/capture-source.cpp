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

#include "capture-source.hpp"

#include <util/threading.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include <Application.h>
#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <Roster.h>

#define S_CAPTURE_CURSOR "capture_cursor"
#define S_FPS "fps"

namespace {

/* Waits for the user, for the permission answer or for a button end on a wakeup (CaptureWakeup); these
 * timeouts only bound them, in case a wakeup is ever missed. */
const uint64_t kStatePoll = 1000000000ULL;
const uint64_t kPromptGrace = 300000000ULL;    /* an answer within this time didn't come from the user */
const uint64_t kRetryInterval = 1000000000ULL; /* a source that isn't there, a failed start */
/*
 * The longest a running session sleeps without taking a frame. The frame notification covers everything that
 * changes the source's area of the screen, including a window being hidden, minimized, uncovered or moved to
 * another workspace (each of them redraws that area). It doesn't cover a window that is closed while it isn't
 * on screen (on another workspace, minimized: nothing is drawn), and a window source looks for its selected
 * window once a second while it captures a stand-in. A frame without changes costs one app_server round trip.
 */
const uint64_t kIdleWake = 1000000000ULL;
const uint64_t kStatsInterval = 10000000000ULL;

std::string application_signature()
{
	app_info info;
	if (be_app && be_app->GetAppInfo(&info) == B_OK && info.signature[0]) {
		return info.signature;
	}
	return "?";
}

class PermissionLooper;

struct PermissionGate {
	std::mutex mutex;
	CapturePermission state = CapturePermission::Unknown;
	/* Created on first need; quit (which deletes it) in capture_permission_shutdown() */
	PermissionLooper *looper = nullptr;
	bool shut_down = false;
	bool unavailable_logged = false;
	/* After a failed request, no new one before this (os_gettime_ns()): the sources ask again once a second */
	uint64_t not_before = 0;
	/* Every source's wakeup, registered for the source's whole lifetime: woken under `mutex`, so a source
	 * that is being destroyed (it unregisters under `mutex` before its semaphore goes) is never touched. */
	std::vector<CaptureWakeup *> wakeups;
};

/* Never destroyed: a source destroyed after obs_module_unload() (libobs frees modules before the remaining
 * sources, obs_shutdown()) still unregisters, and a static destructor must not run before that. */
PermissionGate &permission_gate()
{
	static PermissionGate *gate = new PermissionGate;
	return *gate;
}

void wake_all_locked(PermissionGate &gate)
{
	for (CaptureWakeup *wakeup : gate.wakeups) {
		wakeup->Wake();
	}
}

void permission_answer(status_t status);

/* Receives the app_server's SCREEN_CAPTURE_PERMISSION_ANSWER ('ScPa': "status", "request", "sources";
 * headers/private/app/ScreenCaptureDefs.h) for BScreenCapture::RequestPermission(). */
class PermissionLooper : public BLooper {
public:
	PermissionLooper() : BLooper("haiku-capture permission") {}

	void MessageReceived(BMessage *message) override
	{
		if (message->what != BPrivate::SCREEN_CAPTURE_PERMISSION_ANSWER) {
			BLooper::MessageReceived(message);
			return;
		}
		permission_answer((status_t)message->GetInt32("status", B_ERROR));
	}
};

/* gate.mutex held. The answer comes to the looper; for a decision the app_server already knows (remembered,
 * or "allowed" earlier in this run) it is sent right away. */
status_t send_request(PermissionGate &gate)
{
	return BScreenCapture::RequestPermission(BMessenger(gate.looper), BPrivate::SCREEN_CAPTURE_ANY_MASK);
}

/*
 * On the looper's thread.
 *
 * Every answer belongs to the request it was given for (F-58 batch 2, O-16; ScreenCaptureManager::
 * _DropSettledNo() and _AnswerRequest()): after a "Don't allow" that was not remembered, or a prompt closed
 * without an answer, the next RequestPermission() shows the prompt again. So "Try again" is one request; a
 * remembered "Don't allow" is answered no at once (request -1) and stays denied.
 */
void permission_answer(status_t status)
{
	PermissionGate &gate = permission_gate();
	std::lock_guard<std::mutex> lock(gate.mutex);
	if (gate.shut_down || gate.state != CapturePermission::Asking) {
		/* Unloading, or an answer nobody waits for (only one request is ever out) */
		return;
	}

	const std::string signature = application_signature();
	switch (status) {
	case B_OK:
		gate.state = CapturePermission::Granted;
		blog(LOG_INFO, "[haiku-capture] Screen recording is allowed for this application (signature '%s')",
		     signature.c_str());
		break;
	case B_CANCELED:
		gate.state = CapturePermission::NotAnswered;
		blog(LOG_WARNING,
		     "[haiku-capture] The Screen recording prompt was closed without an answer; press \"Try again\" "
		     "in the source's properties to be asked again");
		break;
	case B_NOT_ALLOWED:
		gate.state = CapturePermission::Denied;
		blog(LOG_WARNING,
		     "[haiku-capture] Screen recording is not allowed. To allow it, open the Screen recording "
		     "preferences (" SCREEN_CAPTURE_AGENT_PATH ") and allow this application, or run "
		     "'ScreenRecording --set %s allow'; then press \"Try again\" in the source's properties.",
		     signature.c_str());
		break;
	default:
		/* Not one of the documented answers; the sources show "failed" and ask again in a second */
		gate.state = CapturePermission::Unknown;
		gate.not_before = os_gettime_ns() + kRetryInterval;
		blog(LOG_ERROR, "[haiku-capture] Asking for the screen recording permission failed: %s",
		     strerror(status));
		break;
	}
	wake_all_locked(gate);
}

/* Moved on by any source's "Start capture" button: see CaptureSource::Restart() */
std::atomic_uint32_t restart_generation = 0;

/* Bitmaps only grow, in steps: a window being resized doesn't get a new one every frame. */
int32 round_up(int32 value)
{
	return (value + 63) & ~63;
}

struct Stats {
	uint64_t since = 0;
	uint32_t wakeups = 0; /* frame notifications (and other wakeups) of a running session */
	uint32_t frames = 0;  /* NextFrame() calls */
	uint32_t changed = 0;
	uint32_t output = 0;
	uint32_t refused = 0; /* changed frames not output: the window doesn't show (AcceptFrame()) */
	uint64_t next_frame_ns = 0;
	uint64_t next_frame_max_ns = 0;
	uint64_t output_ns = 0;
};

} // namespace

CapturePermission capture_permission_poll()
{
	PermissionGate &gate = permission_gate();
	std::lock_guard<std::mutex> lock(gate.mutex);
	if (gate.state != CapturePermission::Unknown) {
		return gate.state;
	}
	if (gate.shut_down || os_gettime_ns() < gate.not_before) {
		return CapturePermission::Unavailable;
	}

	/* Every BScreenCapture call goes through be_app's app_server connection (AppServerLink) */
	if (!be_app) {
		if (!gate.unavailable_logged) {
			blog(LOG_ERROR,
			     "[haiku-capture] No BApplication: unable to ask for the screen recording permission");
		}
		gate.unavailable_logged = true;
		return CapturePermission::Unavailable;
	}

	if (!gate.looper) {
		PermissionLooper *looper = new PermissionLooper;
		const thread_id thread = looper->Run();
		if (thread < 0) {
			blog(LOG_ERROR, "[haiku-capture] Unable to start the permission looper: %s", strerror(thread));
			/* Quit() deletes it, also when Run() failed (BLooper::Quit()) */
			if (looper->Lock()) {
				looper->Quit();
			}
			return CapturePermission::Unavailable;
		}
		gate.looper = looper;
	}

	gate.state = CapturePermission::Asking;
	const status_t status = send_request(gate);
	if (status != B_OK) {
		gate.state = CapturePermission::Unknown;
		gate.not_before = os_gettime_ns() + kRetryInterval;
		if (!gate.unavailable_logged) {
			blog(LOG_ERROR, "[haiku-capture] Asking for the screen recording permission failed: %s",
			     strerror(status));
		}
		gate.unavailable_logged = true;
		return CapturePermission::Unavailable;
	}
	gate.unavailable_logged = false;
	return gate.state;
}

void capture_permission_invalidate()
{
	PermissionGate &gate = permission_gate();
	std::lock_guard<std::mutex> lock(gate.mutex);
	if (gate.state == CapturePermission::Granted) {
		gate.state = CapturePermission::Unknown;
	}
}

void capture_permission_retry()
{
	PermissionGate &gate = permission_gate();
	std::lock_guard<std::mutex> lock(gate.mutex);
	if (gate.state == CapturePermission::Denied || gate.state == CapturePermission::NotAnswered) {
		/* The next capture_permission_poll() sends one new request, which prompts again (O-16) */
		gate.state = CapturePermission::Unknown;
	}
}

void capture_permission_shutdown()
{
	PermissionGate &gate = permission_gate();
	PermissionLooper *looper;
	{
		std::lock_guard<std::mutex> lock(gate.mutex);
		gate.shut_down = true;
		looper = gate.looper;
		gate.looper = nullptr;
	}
	/*
	 * Without gate.mutex: the looper may be waiting for it in permission_answer(), which returns at once now
	 * that shut_down is set. Quit() from another thread lets the looper finish the messages it has, then
	 * waits for its thread (BLooper::Quit()); the looper deletes itself. An answer the app_server sends later
	 * goes to a port that no longer exists, which it tolerates (a SendMessage() with a timeout).
	 */
	if (looper && looper->Lock()) {
		looper->Quit();
	}
}

void capture_register_wakeup(CaptureWakeup *wakeup)
{
	PermissionGate &gate = permission_gate();
	std::lock_guard<std::mutex> lock(gate.mutex);
	gate.wakeups.push_back(wakeup);
}

void capture_unregister_wakeup(CaptureWakeup *wakeup)
{
	PermissionGate &gate = permission_gate();
	std::lock_guard<std::mutex> lock(gate.mutex);
	gate.wakeups.erase(std::remove(gate.wakeups.begin(), gate.wakeups.end(), wakeup), gate.wakeups.end());
}

void capture_wake_all()
{
	PermissionGate &gate = permission_gate();
	std::lock_guard<std::mutex> lock(gate.mutex);
	wake_all_locked(gate);
}

CaptureSource::CaptureSource(obs_source_t *source, const char *what) : source(source), what(what)
{
	/*
	 * Show each frame at the next render instead of pacing frames by their timestamps. libobs's pacing
	 * (ready_async_frame() in obs-source.c) only advances its clock while frames are queued; this source
	 * outputs nothing while the screen doesn't change, so with pacing the first frame after a pause of up to
	 * two seconds, and every frame after it, would be shown late by the length of the pause. A capture of the
	 * live screen has nothing to gain from the buffering either.
	 */
	obs_source_set_async_unbuffered(source, true);
	capture_register_wakeup(&wakeup);
}

CaptureSource::~CaptureSource()
{
	/* The thread calls virtual functions: the subclass's owner must have called Hide() */
	if (thread.joinable()) {
		blog(LOG_ERROR, "[haiku-capture] %s: destroyed while capturing", what);
		wakeup.Stop();
		thread.join();
	}
	/* Before `wakeup` (and its semaphore) goes: the permission looper wakes registered sources */
	capture_unregister_wakeup(&wakeup);
}

void CaptureSource::Show()
{
	std::lock_guard<std::mutex> lock(lifecycle_mutex);
	if (thread.joinable()) {
		return;
	}

	wakeup.Reset();
	try {
		thread = std::thread(&CaptureSource::Run, this);
	} catch (const std::system_error &error) {
		blog(LOG_ERROR, "[haiku-capture] %s: unable to start the capture thread: %s", what, error.what());
	}
}

void CaptureSource::Hide()
{
	std::lock_guard<std::mutex> lock(lifecycle_mutex);
	if (!thread.joinable()) {
		return;
	}

	/* Called on the graphics thread. Stop() releases the semaphore the capture thread sleeps on, so it wakes
	 * at once; it is at most inside one NextFrame(), one Start with a zero timeout or one RequestPermission()
	 * (a round trip to the app_server each), or copying one frame into libobs. */
	wakeup.Stop();
	thread.join();

	/* Don't keep showing stale content when the source is shown again */
	obs_source_output_video(source, nullptr);
	/* Not announced: nothing shows the properties of a hidden source, and this also runs while the source is
	 * being destroyed. */
	state = State::Idle;
}

void CaptureSource::Restart()
{
	/* Also every other source that the user stopped: their capture threads see the new generation */
	restart_generation++;
	user_stopped = false;
	capture_permission_retry();
	capture_wake_all();
}

void CaptureSource::UpdateCommon(obs_data_t *settings)
{
	const int rate = (int)obs_data_get_int(settings, S_FPS);
	fps = rate > 0 ? rate : 30;

	const bool cursor = obs_data_get_bool(settings, S_CAPTURE_CURSOR);
	if (capture_cursor.exchange(cursor) != cursor) {
		RequestRestart();
	}
}

void CaptureSource::CommonDefaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_CAPTURE_CURSOR, true);
	obs_data_set_default_int(settings, S_FPS, 30);
}

uint32 CaptureSource::CommonFlags() const
{
	return capture_cursor ? BPrivate::SCREEN_CAPTURE_INCLUDE_CURSOR : 0;
}

void CaptureSource::ClearOutput()
{
	obs_source_output_video(source, nullptr);
}

static bool restart_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	if (data) {
		static_cast<CaptureSource *>(data)->Restart();
	}
	/* The capture thread reloads the properties when the state changes */
	return false;
}

void CaptureSource::AddStatusProperties(obs_properties_t *props, CaptureSource *self)
{
	if (!self) {
		return;
	}

	const State current = self->state;
	const char *text = nullptr;
	enum obs_text_info_type type = OBS_TEXT_INFO_NORMAL;

	switch (current) {
	case State::WaitingForPermission:
		text = T_("Status.WaitingForPermission");
		break;
	case State::Denied:
		text = T_("Status.Denied");
		type = OBS_TEXT_INFO_WARNING;
		break;
	case State::NotAnswered:
		text = T_("Status.NotAnswered");
		type = OBS_TEXT_INFO_WARNING;
		break;
	case State::StoppedByUser:
		text = T_("Status.StoppedByUser");
		type = OBS_TEXT_INFO_WARNING;
		break;
	case State::SourceMissing:
		text = self->MissingText();
		break;
	case State::Failed:
		text = T_("Status.Failed");
		type = OBS_TEXT_INFO_ERROR;
		break;
	case State::Idle:
	case State::Capturing:
		break;
	}

	if (text) {
		/* An info text without a value shows its description across the dialog */
		obs_property_t *status = obs_properties_add_text(props, "status", text, OBS_TEXT_INFO);
		obs_property_text_set_info_type(status, type);
	}
	if (current == State::Denied || current == State::NotAnswered || current == State::StoppedByUser) {
		obs_properties_add_button2(props, "restart",
					   current == State::StoppedByUser ? T_("StartCapture") : T_("TryAgain"),
					   restart_clicked, self);
	}
}

void CaptureSource::AddCommonProperties(obs_properties_t *props)
{
	obs_properties_add_bool(props, S_CAPTURE_CURSOR, T_("CaptureCursor"));
	obs_property_t *rate = obs_properties_add_int_slider(props, S_FPS, T_("FrameRate"), 1, 60, 1);
	obs_property_int_set_suffix(rate, " FPS");
}

bool CaptureSource::SetState(State new_state)
{
	if (state.exchange(new_state) == new_state) {
		return false;
	}

	static const char *const names[] = {
		"idle",
		"waiting for the user's permission",
		"capturing",
		"not allowed",
		"the Screen recording prompt was closed without an answer",
		"stopped by the user (Screen recording indicator or preferences)",
		"source not available",
		"failed",
	};
	blog(new_state == State::Capturing || new_state == State::WaitingForPermission ? LOG_INFO : LOG_WARNING,
	     "[haiku-capture] %s: %s", what, names[(int)new_state]);

	/* An open properties dialog builds its properties again (queued to the UI thread by the frontend), so
	 * the state text and the restart button follow what happens */
	obs_source_update_properties(source);
	return true;
}

void CaptureSource::Run()
{
	os_set_thread_name("haiku-capture");

	/* BScreenCapture talks to the app_server through the application's connection */
	if (!be_app) {
		blog(LOG_ERROR, "[haiku-capture] %s: no BApplication, unable to capture", what);
		SetState(State::Failed);
		return;
	}
	if (!wakeup.IsValid()) {
		blog(LOG_ERROR, "[haiku-capture] %s: unable to create a semaphore, unable to capture", what);
		SetState(State::Failed);
		return;
	}

	/* Used by this thread only, as the class requires */
	BScreenCapture capture;
	std::unique_ptr<BBitmap> bitmap;
	int32 bitmap_width = 0;
	int32 bitmap_height = 0;
	/* Show the next frame even if nothing changed since the one before: that one wasn't shown */
	bool force_output = true;
	/* Take a frame without waiting for the notification (a new session, a bitmap that was too small) */
	bool take_frame = false;
	uint32_t generation = session_generation;
	uint64_t retry_at = 0;
	/* The frame rate cap: no NextFrame() before this */
	uint64_t next_due = 0;
	uint64_t asking_since = 0;
	bool frame_error_logged = false;
	Stats stats;

	while (!wakeup.Stopped()) {
		/* The user said stop: only the button in the properties starts a capture again, not hiding and
		 * showing the source (a scene switch) or a settings change. */
		if (user_stopped && stopped_generation != restart_generation) {
			user_stopped = false; /* "Start capture" on another source */
		}
		if (user_stopped) {
			SetState(State::StoppedByUser);
			wakeup.WaitFor(kStatePoll);
			continue;
		}

		const uint32_t wanted = session_generation;
		if (wanted != generation) {
			generation = wanted;
			retry_at = 0;
			capture.Stop();
		}

		if (!capture.IsRunning()) {
			const CapturePermission permission = capture_permission_poll();
			if (permission == CapturePermission::Denied || permission == CapturePermission::NotAnswered) {
				asking_since = 0;
				SetState(permission == CapturePermission::Denied ? State::Denied : State::NotAnswered);
				wakeup.WaitFor(kStatePoll); /* "Try again" wakes it */
				continue;
			}
			if (permission == CapturePermission::Unavailable) {
				asking_since = 0;
				if (SetState(State::Failed)) {
					ClearOutput();
				}
				wakeup.WaitFor(kRetryInterval);
				continue;
			}
			if (permission != CapturePermission::Granted) {
				/* A decision the app_server knows is answered within milliseconds: only say "waiting"
				 * once it is clear that the user is being asked. The answer wakes this thread. */
				const uint64_t asking_now = os_gettime_ns();
				if (!asking_since) {
					asking_since = asking_now;
				}
				const bool prompting = asking_now - asking_since >= kPromptGrace;
				if (prompting) {
					SetState(State::WaitingForPermission);
				}
				wakeup.WaitFor(prompting ? kStatePoll : kPromptGrace);
				continue;
			}
			asking_since = 0;

			const uint64_t now = os_gettime_ns();
			if (now < retry_at) {
				wakeup.WaitUntil(retry_at);
				continue;
			}

			const status_t status = StartSession(capture);
			if (status == B_NOT_ALLOWED || status == B_TIMED_OUT) {
				/* Changed since it was granted. B_NOT_ALLOWED: set to "deny" (the request below is answered
				 * no at once). B_TIMED_OUT: set back to "ask", and this zero-timeout start has made the
				 * app_server show the prompt (BScreenCapture::_Start()); the request below joins it, and the
				 * properties say "waiting" rather than "not allowed". */
				blog(LOG_INFO, "[haiku-capture] %s: the session was refused (%s); asking again", what,
				     status == B_TIMED_OUT ? "the user is being asked" : "not allowed");
				capture_permission_invalidate();
				continue;
			}
			if (status != B_OK) {
				/* Logged and cleared once, not at every retry */
				const bool missing = status == B_ENTRY_NOT_FOUND;
				if (SetState(missing ? State::SourceMissing : State::Failed)) {
					ClearOutput();
					if (!missing) {
						blog(LOG_ERROR, "[haiku-capture] %s: starting the capture failed: %s",
						     what, strerror(status));
					}
				}
				retry_at = now + kRetryInterval;
				continue;
			}

			/* F-58 (O-8): the app_server releases the semaphore when the source changes after a frame was
			 * taken (ScreenCaptureManager::ScreenDamaged()), and when the session is stopped from the
			 * indicator (_Stop()) or its window is gone (_Frame()). The semaphore must belong to this team
			 * (_SetNotification() checks). No SetFrameNotification(-1) is needed before Stop(): a stopped
			 * session is removed with its notification. */
			const status_t notify = capture.SetFrameNotification(wakeup.Semaphore());
			if (notify != B_OK) {
				blog(LOG_ERROR, "[haiku-capture] %s: SetFrameNotification failed: %s", what,
				     strerror(notify));
				capture.Stop();
				if (SetState(State::Failed)) {
					ClearOutput();
				}
				retry_at = now + kRetryInterval;
				continue;
			}

			SetState(State::Capturing);
			force_output = true;
			take_frame = true;
			next_due = 0;
			stats = Stats();
			stats.since = os_gettime_ns();
		}

		if (!take_frame) {
			/* Sleep until the source changes (or Stop(), a restart, a button) */
			const CaptureWakeup::Result result = wakeup.WaitFor(kIdleWake);
			if (result == CaptureWakeup::Result::Stopped) {
				break;
			}
			if (result == CaptureWakeup::Result::Woken) {
				stats.wakeups++;
			}
			take_frame = true;
			continue; /* look at the state again first: a wakeup may have been a restart or a stop */
		}

		/*
		 * The frame rate cap: not faster than the source's frame rate. Changes in the meantime are not lost:
		 * the app_server collects them for the next NextFrame(), and doesn't release the semaphore again
		 * before that (once per frame), so this wait isn't cut short by drawing on the desktop.
		 */
		uint64_t now = os_gettime_ns();
		if (now < next_due) {
			if (wakeup.WaitUntil(next_due) == CaptureWakeup::Result::Stopped) {
				break;
			}
			continue;
		}

		if (WantsRestart(now)) {
			capture.Stop();
			continue;
		}

		/* The app_server fills the bitmap from its top left and clips to its size (ServerBitmap::ImportBits()
		 * -> BPrivate::ConvertBits()), so a larger bitmap than the frame is fine, and a smaller one is safe but
		 * incomplete: it is replaced below. */
		BRect frame = capture.Frame();
		int32 width = frame.IntegerWidth() + 1;
		int32 height = frame.IntegerHeight() + 1;
		if (!bitmap || bitmap_width < width || bitmap_height < height) {
			bitmap_width = round_up(std::max(width, bitmap_width));
			bitmap_height = round_up(std::max(height, bitmap_height));
			bitmap.reset(); /* first: never two screen-sized bitmaps at once */
			bitmap = std::make_unique<BBitmap>(BRect(0, 0, bitmap_width - 1, bitmap_height - 1), B_RGB32);
			force_output = true;
			if (!bitmap->IsValid()) {
				blog(LOG_ERROR, "[haiku-capture] %s: unable to allocate a %dx%d bitmap", what,
				     (int)bitmap_width, (int)bitmap_height);
				bitmap.reset();
				bitmap_width = bitmap_height = 0;
				capture.Stop();
				SetState(State::Failed);
				retry_at = now + kRetryInterval;
				continue;
			}
		}

		bool changed = false;
		bigtime_t time = 0;
		uint32 flags = 0;
		const uint64_t before = os_gettime_ns();
		/* F-58 (O-9): the overload with flags; window sessions report whether the window shows */
		const status_t status = capture.NextFrame(bitmap.get(), &changed, nullptr, &time, &flags);
		const uint64_t after = os_gettime_ns();
		const uint64_t took = after - before;
		take_frame = false;

		if (status == B_CANCELED) {
			/* Stopped from the indicator or the preferences, or the permission was revoked. Give the
			 * session up (the app_server keeps a stopped one until then) and wait for the button; the
			 * picture goes too, the user doesn't expect it to stay on stream. */
			capture.Stop();
			ClearOutput();
			stopped_generation = restart_generation.load();
			user_stopped = true;
			continue;
		}
		if (status == B_ENTRY_NOT_FOUND) {
			/* The window is gone (or entirely outside the screen; a region after a mode change). The
			 * session is dead on the app_server's side as well. */
			capture.Stop();
			ClearOutput();
			SessionLost();
			continue; /* straight to StartSession(), which looks for the source again */
		}
		if (status != B_OK) {
			/* B_ERROR: no reply, or the app_server could not lock its drawing engine; B_BAD_VALUE: it
			 * doesn't know the session or the bitmap. Start over. */
			if (!frame_error_logged) {
				blog(LOG_ERROR, "[haiku-capture] %s: reading a frame failed: %s", what,
				     strerror(status));
			}
			frame_error_logged = true;
			capture.Stop();
			SetState(State::Failed);
			retry_at = now + kRetryInterval;
			continue;
		}

		frame_error_logged = false;
		stats.frames++;
		stats.next_frame_ns += took;
		stats.next_frame_max_ns = std::max(stats.next_frame_max_ns, took);
		if (changed) {
			stats.changed++;
		}

		/*
		 * The next frame is due one frame interval after this one started, and not before as long again as
		 * this one took: the app_server copies while it holds its drawing lock (DrawingEngine::
		 * LockExclusiveAccess() in ScreenCaptureManager::_Frame()), and nothing on the desktop is drawn
		 * meanwhile. Normally that is a short copy from its back buffer, but where a frame takes longer than
		 * the frame interval (a large screen that changes all over, read from a slow front buffer), reading
		 * back to back would hold that lock all the time. So the desktop always gets at least half of it.
		 */
		const int rate = fps;
		next_due = std::max<uint64_t>(before + 1000000000ULL / (uint64_t)(rate > 0 ? rate : 30), after + took);

		/* Frame() is now the frame this bitmap was filled for. If the window grew (or the screen mode did)
		 * beyond the bitmap, this frame is cut off: read it again into a larger one. */
		frame = capture.Frame();
		width = frame.IntegerWidth() + 1;
		height = frame.IntegerHeight() + 1;
		if (width > bitmap_width || height > bitmap_height) {
			take_frame = true;
			continue;
		}

		if ((changed || force_output) && width > 0 && height > 0) {
			if (AcceptFrame(flags)) {
				const uint64_t output_start = os_gettime_ns();
				struct obs_source_frame out = {};
				out.data[0] = (uint8_t *)bitmap->Bits();
				out.linesize[0] = (uint32_t)bitmap->BytesPerRow();
				out.width = (uint32_t)width;
				out.height = (uint32_t)height;
				out.format = VIDEO_FORMAT_BGRX; /* B_RGB32 on little endian */
				out.full_range = true;
				/* When the app_server took the frame: system_time() in microseconds, the same clock as
				 * os_gettime_ns() (CLOCK_MONOTONIC) */
				out.timestamp = (uint64_t)time * 1000ULL;
				obs_source_output_video(source, &out);

				force_output = false;
				stats.output++;
				stats.output_ns += os_gettime_ns() - output_start;
			} else {
				/* The bitmap now holds something that wasn't shown: show the next frame the source
				 * accepts even if nothing changes in it */
				force_output = true;
				if (changed) {
					stats.refused++;
				}
			}
		}

		now = os_gettime_ns();
		if (now - stats.since >= kStatsInterval) {
			/* For before/after numbers. A frame costs one NextFrame() (a round trip to the app_server plus
			 * its copy of what changed) and, when output, libobs's copy of the whole frame. Wakeups are
			 * frame notifications; frames read beyond them are the idle checks (kIdleWake) and first
			 * frames. */
			blog(LOG_DEBUG,
			     "[haiku-capture] %s: %dx%d, in %.1f s: %" PRIu32 " wakeups, %" PRIu32
			     " frames read (%" PRIu32 " changed), %" PRIu32 " output, %" PRIu32
			     " not shown (window not visible); NextFrame avg "
			     "%.2f ms, max %.2f ms; output avg %.2f ms",
			     what, (int)width, (int)height, (now - stats.since) / 1e9, stats.wakeups, stats.frames,
			     stats.changed, stats.output, stats.refused,
			     stats.frames ? stats.next_frame_ns / 1e6 / stats.frames : 0.0,
			     stats.next_frame_max_ns / 1e6, stats.output ? stats.output_ns / 1e6 / stats.output : 0.0);
			stats = Stats();
			stats.since = now;
		}
	}

	capture.Stop();
}
