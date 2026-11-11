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

#include <Application.h>
#include <Roster.h>

#define S_CAPTURE_CURSOR "capture_cursor"
#define S_FPS "fps"

namespace {

/* How long the user gets to answer the prompt (the API's default). After that the state is "denied" and the
 * properties offer "Try again"; a late answer in the prompt still counts then (the app_server keeps the request). */
const uint64_t kPermissionTimeout = 120000000000ULL;
/* The permission thread asks in slices of this length, so that it can be stopped within about this time */
const bigtime_t kPermissionSlice = 1000000;
/* See prompt_on_screen() */
const uint64_t kPromptSettle = 250000000ULL;

const uint64_t kStatePoll = 250000000ULL;      /* how often a thread that waits for the user looks again */
const uint64_t kPermissionPoll = 100000000ULL; /* ... and while the prompt is up */
const uint64_t kPromptGrace = 300000000ULL;    /* an answer within this time didn't come from the user */
const uint64_t kRetryInterval = 1000000000ULL; /* a source that isn't there, a failed start */
const uint64_t kStatsInterval = 10000000000ULL;

struct PermissionGate {
	std::mutex mutex;
	CapturePermission state = CapturePermission::Unknown;
	std::thread thread;
	StopSignal stop;
	bool shut_down = false;
};

/* Never destroyed: obs_module_unload() joins the thread, but a static destructor running after that (or
 * without it, at exit()) must not find a joinable std::thread. */
PermissionGate &permission_gate()
{
	static PermissionGate *gate = new PermissionGate;
	return *gate;
}

std::string application_signature()
{
	app_info info;
	if (be_app && be_app->GetAppInfo(&info) == B_OK && info.signature[0]) {
		return info.signature;
	}
	return "?";
}

/*
 * Whether a Screen recording prompt ("ScreenRecording --prompt <request>", launched by the app_server:
 * ScreenCaptureManager::_LaunchAgent()) is running.
 *
 * Needed because BScreenCapture::_Start() returns B_NOT_ALLOWED both when the user said no and when its own
 * timeout ran out while the app_server still answered B_WOULD_BLOCK (src/kits/interface/ScreenCapture.cpp).
 * A timeout leaves the request on the app_server, so asking again just goes on waiting for it. But a "no"
 * without "Remember my decision" is consumed by the Start that returned it (ScreenCaptureManager::_Start()),
 * and asking again would show a second prompt. A slice that ends at its timeout can also have read such a no
 * (the reply to its last round trip, which starts after the deadline), so the return value alone can't tell.
 * The prompt quits as soon as it is answered (PromptWindow in src/preferences/screenrecording), so: prompt
 * still up after a short settle time -> the request is pending; gone -> it was answered, and the answer was
 * the B_NOT_ALLOWED we got. (In the unlikely case that the user answers in the few milliseconds between that
 * reply and this check, the answer is still on the app_server and "Try again" picks it up.)
 *
 * Any application's prompt counts (the request id isn't returned to the client): with two applications asking
 * at once this can take another application's prompt for ours and ask once more. Renga request: a way for
 * Start() to report "still pending" distinctly (B_WOULD_BLOCK for a zero timeout) would replace this.
 */
bool prompt_running()
{
	static const char kPrompt[] = SCREEN_CAPTURE_AGENT_PATH " --prompt";
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		/* args: the command line, cut to 63 characters; the prefix fits */
		if (strncmp(info.args, kPrompt, sizeof(kPrompt) - 1) == 0) {
			return true;
		}
	}
	return false;
}

bool prompt_on_screen(StopSignal &stop)
{
	if (!prompt_running()) {
		return false;
	}
	/* Answered just now: give the prompt the time to quit (it posts B_QUIT_REQUESTED right after answering) */
	stop.WaitFor(kPromptSettle);
	return prompt_running();
}

/*
 * Asks through a session of its own for one pixel, which is stopped again at once: the permission belongs to
 * the application, not to a session, so the sources' own sessions start without a prompt afterwards.
 * B_NOT_ALLOWED stands for "Don't allow" as well as for no answer within kPermissionTimeout.
 *
 * Starting that session shows the Screen recording indicator for a moment (ScreenCaptureManager::_Start() ->
 * _EnsureIndicator()); the sources' sessions start right after it, so it normally just stays up.
 */
void permission_thread()
{
	os_set_thread_name("haiku-capture-permission");

	PermissionGate &gate = permission_gate();
	BScreenCapture probe;
	const uint64_t deadline = os_gettime_ns() + kPermissionTimeout;
	status_t status = B_NOT_ALLOWED;

	while (!gate.stop.Stopped()) {
		/* Every BScreenCapture call goes through be_app's app_server connection (AppServerLink) */
		if (!be_app) {
			status = B_NO_INIT;
			break;
		}
		const uint64_t before = os_gettime_ns();
		status = probe.StartRegion(BRect(0, 0, 0, 0), 0, kPermissionSlice);
		const bool slice_ran_out = os_gettime_ns() - before >= (uint64_t)kPermissionSlice * 1000ULL;
		if (status != B_NOT_ALLOWED || !slice_ran_out || os_gettime_ns() >= deadline) {
			break;
		}
		if (!prompt_on_screen(gate.stop)) {
			break;
		}
	}
	/* Only a running session costs a round trip */
	if (be_app) {
		probe.Stop();
	}

	if (gate.stop.Stopped()) {
		return; /* unloading: nobody reads the state any more */
	}

	const std::string signature = application_signature();
	if (status == B_OK) {
		blog(LOG_INFO, "[haiku-capture] Screen recording is allowed for this application (signature '%s')",
		     signature.c_str());
	} else {
		blog(LOG_WARNING,
		     "[haiku-capture] Screen recording is not allowed (%s). To allow it, open the Screen recording "
		     "preferences (" SCREEN_CAPTURE_AGENT_PATH ") and allow this application, or run "
		     "'ScreenRecording --set %s allow'; then press \"Try again\" in the source's properties.",
		     strerror(status), signature.c_str());
	}

	std::lock_guard<std::mutex> lock(gate.mutex);
	gate.state = status == B_OK ? CapturePermission::Granted : CapturePermission::Denied;
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
	uint32_t polls = 0;
	uint32_t changed = 0;
	uint64_t next_frame_ns = 0;
	uint64_t next_frame_max_ns = 0;
	uint64_t output_ns = 0;
};

} // namespace

CapturePermission capture_permission_poll()
{
	PermissionGate &gate = permission_gate();
	std::lock_guard<std::mutex> lock(gate.mutex);
	if (gate.state == CapturePermission::Unknown && !gate.shut_down) {
		/* A thread from an earlier question has set the state (its last action) and is returning or has
		 * returned: this join doesn't wait for anything that needs the lock held here */
		if (gate.thread.joinable()) {
			gate.thread.join();
		}
		gate.state = CapturePermission::Asking;
		try {
			gate.thread = std::thread(permission_thread);
		} catch (const std::system_error &error) {
			blog(LOG_ERROR, "[haiku-capture] Unable to start the permission thread: %s", error.what());
			gate.state = CapturePermission::Denied;
		}
	}
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
	if (gate.state == CapturePermission::Denied) {
		gate.state = CapturePermission::Unknown;
	}
}

void capture_permission_shutdown()
{
	PermissionGate &gate = permission_gate();
	std::thread thread;
	{
		std::lock_guard<std::mutex> lock(gate.mutex);
		gate.shut_down = true;
		thread = std::move(gate.thread);
	}
	/* The thread returns after the round trip it is in, at most one slice (kPermissionSlice) */
	gate.stop.Stop();
	if (thread.joinable()) {
		thread.join();
	}
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
}

CaptureSource::~CaptureSource()
{
	/* The thread calls virtual functions: the subclass's owner must have called Hide() */
	if (thread.joinable()) {
		blog(LOG_ERROR, "[haiku-capture] %s: destroyed while capturing", what);
		stop.Stop();
		thread.join();
	}
}

void CaptureSource::Show()
{
	std::lock_guard<std::mutex> lock(lifecycle_mutex);
	if (thread.joinable()) {
		return;
	}

	stop.Reset();
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

	/* Called on the graphics thread. The capture thread is at most inside one NextFrame() or one Start with
	 * a zero timeout (a round trip to the app_server each), or copying one frame into libobs. */
	stop.Stop();
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
	if (current == State::Denied || current == State::StoppedByUser) {
		obs_properties_add_button2(props, "restart",
					   current == State::Denied ? T_("TryAgain") : T_("StartCapture"),
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

	/* Used by this thread only, as the class requires */
	BScreenCapture capture;
	std::unique_ptr<BBitmap> bitmap;
	int32 bitmap_width = 0;
	int32 bitmap_height = 0;
	/* Show the next frame even if nothing changed since the one before: that one wasn't shown */
	bool force_output = true;
	uint32_t generation = session_generation;
	uint64_t retry_at = 0;
	uint64_t next_frame = os_gettime_ns();
	uint64_t last_took = 0;
	uint64_t asking_since = 0;
	bool frame_error_logged = false;
	Stats stats;

	while (!stop.Stopped()) {
		/* The user said stop: only the button in the properties starts a capture again, not hiding and
		 * showing the source (a scene switch) or a settings change. */
		if (user_stopped && stopped_generation != restart_generation) {
			user_stopped = false; /* "Start capture" on another source */
		}
		if (user_stopped) {
			SetState(State::StoppedByUser);
			stop.WaitFor(kStatePoll);
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
			if (permission == CapturePermission::Denied) {
				asking_since = 0;
				SetState(State::Denied);
				stop.WaitFor(kStatePoll);
				continue;
			}
			if (permission != CapturePermission::Granted) {
				/* With a remembered decision the answer is there within milliseconds: only say
				 * "waiting" once it is clear that the user is being asked */
				const uint64_t asking_now = os_gettime_ns();
				if (!asking_since) {
					asking_since = asking_now;
				}
				const bool prompting = asking_now - asking_since >= kPromptGrace;
				if (prompting) {
					SetState(State::WaitingForPermission);
				}
				stop.WaitFor(prompting ? kPermissionPoll : kPromptGrace / 10);
				continue;
			}
			asking_since = 0;

			const uint64_t now = os_gettime_ns();
			if (now < retry_at) {
				stop.WaitUntil(std::min(retry_at, now + kStatePoll));
				continue;
			}

			const status_t status = StartSession(capture);
			if (status == B_NOT_ALLOWED) {
				/* Revoked since it was granted. If the setting is "ask" now, this start has made the
				 * app_server show the prompt, and the permission thread picks that request up. */
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

			SetState(State::Capturing);
			force_output = true;
			next_frame = os_gettime_ns();
			stats = Stats();
			stats.since = next_frame;
		}

		uint64_t now = os_gettime_ns();
		const Poll poll = BeforeFrame(now);
		if (poll == Poll::Restart) {
			capture.Stop();
			continue;
		}

		if (poll == Poll::Capture) {
			/* The app_server fills the bitmap from its top left and clips to its size (ServerBitmap::
			 * ImportBits() -> BPrivate::ConvertBits()), so a larger bitmap than the frame is fine, and a
			 * smaller one is safe but incomplete: it is replaced below. */
			BRect frame = capture.Frame();
			int32 width = frame.IntegerWidth() + 1;
			int32 height = frame.IntegerHeight() + 1;
			if (!bitmap || bitmap_width < width || bitmap_height < height) {
				bitmap_width = round_up(std::max(width, bitmap_width));
				bitmap_height = round_up(std::max(height, bitmap_height));
				bitmap.reset(); /* first: never two screen-sized bitmaps at once */
				bitmap = std::make_unique<BBitmap>(BRect(0, 0, bitmap_width - 1, bitmap_height - 1),
								   B_RGB32);
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
			const uint64_t before = os_gettime_ns();
			const status_t status = capture.NextFrame(bitmap.get(), &changed, nullptr, &time);
			const uint64_t took = os_gettime_ns() - before;

			if (status == B_CANCELED) {
				/* Stopped from the indicator or the preferences, or the permission was revoked. Give
				 * the session up (the app_server keeps a stopped one until then) and wait for the
				 * button; the picture goes too, the user doesn't expect it to stay on stream. */
				capture.Stop();
				ClearOutput();
				stopped_generation = restart_generation.load();
				user_stopped = true;
				continue;
			}
			if (status == B_ENTRY_NOT_FOUND) {
				/* The window is gone (or entirely outside the screen; a region after a mode change).
				 * The session is dead on the app_server's side as well. */
				capture.Stop();
				ClearOutput();
				SessionLost();
				continue; /* straight to StartSession(), which looks for the source again */
			}
			if (status != B_OK) {
				/* B_ERROR: no reply, or the app_server could not lock its drawing engine;
				 * B_BAD_VALUE: it doesn't know the session or the bitmap. Start over. */
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
			last_took = took;
			stats.polls++;
			stats.next_frame_ns += took;
			stats.next_frame_max_ns = std::max(stats.next_frame_max_ns, took);

			/* Frame() is now the frame this bitmap was filled for. If the window grew (or the screen
			 * mode did) beyond the bitmap, this frame is cut off: read it again into a larger one. */
			frame = capture.Frame();
			width = frame.IntegerWidth() + 1;
			height = frame.IntegerHeight() + 1;
			if (width > bitmap_width || height > bitmap_height) {
				continue;
			}

			if ((changed || force_output) && width > 0 && height > 0) {
				if (AcceptFrame(os_gettime_ns())) {
					const uint64_t output_start = os_gettime_ns();
					struct obs_source_frame out = {};
					out.data[0] = (uint8_t *)bitmap->Bits();
					out.linesize[0] = (uint32_t)bitmap->BytesPerRow();
					out.width = (uint32_t)width;
					out.height = (uint32_t)height;
					out.format = VIDEO_FORMAT_BGRX; /* B_RGB32 on little endian */
					out.full_range = true;
					/* When the app_server took the frame: system_time() in microseconds, the same
					 * clock as os_gettime_ns() (CLOCK_MONOTONIC) */
					out.timestamp = (uint64_t)time * 1000ULL;
					obs_source_output_video(source, &out);

					force_output = false;
					stats.changed++;
					stats.output_ns += os_gettime_ns() - output_start;
				} else {
					/* The bitmap now holds something that wasn't shown */
					force_output = true;
				}
			}

			now = os_gettime_ns();
			if (now - stats.since >= kStatsInterval) {
				/* For before/after numbers: the cost of a frame is one NextFrame() (a round trip to
				 * the app_server plus its copy of what changed) and, for changed frames, libobs's
				 * copy of the whole frame */
				blog(LOG_DEBUG,
				     "[haiku-capture] %s: %dx%d, %" PRIu32 " frames polled in %.1f s, %" PRIu32
				     " changed; NextFrame avg %.2f ms, max %.2f ms; output avg %.2f ms",
				     what, (int)width, (int)height, stats.polls, (now - stats.since) / 1e9,
				     stats.changed, stats.polls ? stats.next_frame_ns / 1e6 / stats.polls : 0.0,
				     stats.next_frame_max_ns / 1e6,
				     stats.changed ? stats.output_ns / 1e6 / stats.changed : 0.0);
				stats = Stats();
				stats.since = now;
			}
		}

		/*
		 * A steady schedule; a late frame isn't made up for. The app_server copies while it holds its
		 * drawing lock (DrawingEngine::LockExclusiveAccess() in ScreenCaptureManager::_Frame()): nothing on
		 * the desktop is drawn meanwhile. Normally that is a short copy from its back buffer, but where a
		 * frame takes longer than the frame interval (a large screen that changes all over, read from a
		 * slow front buffer), polling back to back would hold that lock all the time. So wait at least as
		 * long as the last read took: the desktop always gets half of the time.
		 */
		const int rate = fps;
		next_frame += 1000000000ULL / (uint64_t)(rate > 0 ? rate : 30);
		now = os_gettime_ns();
		if (next_frame < now + last_took) {
			next_frame = now + last_took;
		}
		last_took = 0;
		stop.WaitUntil(next_frame);
	}

	capture.Stop();
}
