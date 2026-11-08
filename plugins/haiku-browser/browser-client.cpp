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

#include "browser-client.hpp"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <AppFileInfo.h>
#include <File.h>
#include <Messenger.h>
#include <Roster.h>

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <thread>

extern char **environ;

#define BLOG(level, fmt, ...) blog(level, "[haiku-browser] %s: " fmt, name.c_str(), ##__VA_ARGS__)

/* the helper must answer within this long after being spawned, or it is considered hung */
#define HELPER_START_TIMEOUT 15000000LL
#define HELPER_STALL_TIMEOUT 10000000LL

namespace {

std::atomic_int area_counter{0};

std::string helper_path()
{
	std::string path;
	if (char *p = os_get_executable_path_ptr(OBS_BROWSER_HELPER_NAME)) {
		path = p;
		bfree(p);
	}
	if (path.empty() || access(path.c_str(), X_OK) != 0)
		path = "/boot/home/config/non-packaged/bin/" OBS_BROWSER_HELPER_NAME;
	return path;
}

/* Marks the helper as a background application (no Deskbar entry) that may run several times. Executables
 * built without a resource file default to neither; the flags live in a file attribute, so this only needs
 * to succeed once per installation. */
void mark_background_app(const std::string &path)
{
	static std::once_flag once;
	std::call_once(once, [&path]() {
		BFile file(path.c_str(), B_READ_WRITE);
		if (file.InitCheck() != B_OK)
			return;
		BAppFileInfo info(&file);
		if (info.InitCheck() != B_OK)
			return;
		uint32 flags = 0;
		const uint32 wanted = B_MULTIPLE_LAUNCH | B_BACKGROUND_APP;
		if (info.GetAppFlags(&flags) != B_OK || (flags & (B_LAUNCH_MASK | B_BACKGROUND_APP)) != wanted)
			info.SetAppFlags(wanted);
		char signature[B_MIME_TYPE_LENGTH];
		if (info.GetSignature(signature) != B_OK || strcmp(signature, OBS_BROWSER_HELPER_SIGNATURE) != 0)
			info.SetSignature(OBS_BROWSER_HELPER_SIGNATURE);
	});
}

} // namespace

HaikuBrowserClient::HaikuBrowserClient(const char *log_name, const std::string &storage_path)
	: name(log_name ? log_name : "browser"),
	  storage(storage_path)
{
}

HaikuBrowserClient::~HaikuBrowserClient()
{
	std::lock_guard<std::mutex> lock(mutex);
	StopHelper();
	DestroyArea();
}

/* Areas are allocated with headroom (at least 1920x1080, rounded up to 64 pixels) so that resizing a source or
 * a dock rarely needs a new one. */
static uint32_t area_dimension(uint32_t value, uint32_t minimum)
{
	value = std::max(value, minimum);
	return (value + 63u) & ~63u;
}

bool HaikuBrowserClient::CreateArea(uint32_t width, uint32_t height)
{
	width = area_dimension(width, 1920);
	height = area_dimension(height, 1080);

	char area_name[B_OS_NAME_LENGTH];
	snprintf(area_name, sizeof(area_name), "obs browser %d.%d", (int)getpid(), area_counter++);

	void *addr = nullptr;
	area_id id = create_area(area_name, &addr, B_ANY_ADDRESS, obs_browser_area_size(width, height), B_NO_LOCK,
				 B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (id < 0) {
		BLOG(LOG_WARNING, "create_area(%ux%u) failed: %s", width, height, strerror(id));
		return false;
	}

	obs_browser_shm *mapped = static_cast<obs_browser_shm *>(addr);
	memset(mapped, 0, OBS_BROWSER_HEADER_SIZE);
	mapped->magic = OBS_BROWSER_MAGIC;
	mapped->version = OBS_BROWSER_VERSION;
	mapped->capacity = obs_browser_area_size(width, height) - OBS_BROWSER_HEADER_SIZE;
	mapped->client_heartbeat = system_time();

	if (shm && helper > 0 && HelperAlive()) {
		/* The running (or starting) helper keeps using its area until it has switched. Seed the new header
		 * with its state and a heartbeat so that nothing reading it before the switch sees a dead helper;
		 * helper_team stays 0 until the helper has attached (that is how the switch is confirmed). */
		const obs_browser_shm *state = HelperShm();
		mapped->helper_state = state->helper_state;
		mapped->helper_heartbeat = system_time();
		if (attach_pending) {
			/* the current area was never handed to the helper: replace it, keep the one it uses */
			delete_area(area);
			area = -1;
			shm = nullptr;
		} else {
			RetireArea();
		}
		area = id;
		shm = mapped;
		area_fresh = false;
		attach_pending = true;
		TrySendAttach();
	} else {
		DestroyArea();
		area = id;
		shm = mapped;
		area_fresh = true;
	}

	area_width = width;
	area_height = height;
	last_frame = -1;
	return true;
}

/* Only called while no switch is in flight (see AttachInFlight()), so there is no older retired area that the
 * helper might still be using. */
void HaikuBrowserClient::RetireArea()
{
	if (retired_area >= 0)
		delete_area(retired_area);
	retired_area = area;
	retired_shm = shm;
	area = -1;
	shm = nullptr;
}

bool HaikuBrowserClient::TrySendAttach()
{
	if (!attach_pending || !ready)
		return false;
	BMessage msg(OBS_BROWSER_MSG_ATTACH_AREA);
	msg.AddInt32("area", area);
	if (!SendLocked(msg))
		return false;
	/* the retired area stays the helper's until it confirms the switch in the new header (Poll()) */
	attach_pending = false;
	attach_sent = system_time();
	return true;
}

bool HaikuBrowserClient::AttachInFlight() const
{
	return retired_shm && !attach_pending;
}

bool HaikuBrowserClient::NeedsBiggerArea() const
{
	return !shm || (uint32_t)config.width > area_width || (uint32_t)config.height > area_height;
}

obs_browser_shm *HaikuBrowserClient::HelperShm() const
{
	return retired_shm ? retired_shm : shm;
}

void HaikuBrowserClient::DestroyArea()
{
	if (area >= 0)
		delete_area(area);
	if (retired_area >= 0)
		delete_area(retired_area);
	area = retired_area = -1;
	shm = retired_shm = nullptr;
	attach_pending = false;
	resize_deferred = false;
	area_fresh = false;
	area_width = area_height = 0;
}

bool HaikuBrowserClient::HelperAlive() const
{
	return helper > 0 && kill(helper, 0) == 0;
}

bool HaikuBrowserClient::SpawnHelper()
{
	const std::string path = helper_path();
	if (access(path.c_str(), X_OK) != 0) {
		if (!spawn_failing)
			BLOG(LOG_ERROR, "helper %s is missing; browser sources cannot render", path.c_str());
		spawn_failing = true;
		ScheduleRestart(system_time());
		return false;
	}
	mark_background_app(path);

	/* A new helper always starts on an area no earlier helper was given: one that was just stopped (or
	 * killed after a stall) can keep running for a moment, and two writers would break the sequence lock.
	 * Dropping our handles to the old areas is safe; a helper still running keeps its own clone. */
	if (!area_fresh &&
	    !CreateArea(std::max<uint32_t>(config.width, area_width), std::max<uint32_t>(config.height, area_height))) {
		ScheduleRestart(system_time());
		return false;
	}

	/* ...with clean state and a single area */
	if (retired_area >= 0) {
		delete_area(retired_area);
		retired_area = -1;
		retired_shm = nullptr;
	}
	attach_pending = false;
	shm->seq = 0;
	shm->frame_count = 0;
	shm->helper_state = OBS_BROWSER_STATE_STARTING;
	shm->helper_heartbeat = 0;
	shm->helper_team = 0;
	shm->client_heartbeat = system_time();
	last_frame = -1;
	__sync_synchronize();

	char area_arg[16];
	snprintf(area_arg, sizeof(area_arg), "%d", (int)area);
	char *argv[6] = {const_cast<char *>(path.c_str()), const_cast<char *>("--area"), area_arg, nullptr, nullptr,
			 nullptr};
	if (!storage.empty()) {
		argv[3] = const_cast<char *>("--storage");
		argv[4] = const_cast<char *>(storage.c_str());
	}

	pid_t pid = -1;
	const int err = posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv, environ);
	if (err != 0) {
		if (!spawn_failing)
			BLOG(LOG_ERROR, "could not start %s: %s", path.c_str(), strerror(err));
		spawn_failing = true;
		ScheduleRestart(system_time());
		return false;
	}

	spawn_failing = false;
	area_fresh = false;
	helper = pid;
	helper_started = system_time();
	ready = false;
	config_dirty = true;
	BLOG(LOG_INFO, "started browser helper (team %d)", (int)pid);
	return true;
}

void HaikuBrowserClient::ScheduleRestart(bigtime_t now)
{
	const bigtime_t backoff = std::min<bigtime_t>(500000LL << std::min(restarts, 5), 10000000LL);
	next_restart = now + backoff;
	restarts++;
}

/* Never blocks the caller (often the UI or graphics thread): asks the helper to quit and leaves waiting for
 * it, and killing it if it doesn't, to a short-lived reaper thread. */
void HaikuBrowserClient::StopHelper()
{
	if (helper <= 0)
		return;

	const pid_t pid = helper;
	/* a clean quit lets WebKit flush its storage; without a messenger (helper never became ready) SIGTERM
	 * is fine, the helper owns no system state */
	BMessage quit(B_QUIT_REQUESTED);
	if (!messenger.IsValid() || messenger.SendMessage(&quit, (BHandler *)nullptr, 0) != B_OK)
		kill(pid, SIGTERM);

	std::thread([pid]() {
		for (int i = 0; i < 30; i++) {
			if (waitpid(pid, nullptr, WNOHANG) != 0)
				return; /* exited (or not our child any more) */
			snooze(50000);
		}
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);
	}).detach();

	helper = -1;
	ready = false;
	messenger = BMessenger();
}

bool HaikuBrowserClient::SendLocked(BMessage &message)
{
	if (!ready || helper <= 0)
		return false;
	if (!messenger.IsValid()) {
		/* a registrar round trip; done once per helper, not per message */
		messenger = BMessenger(OBS_BROWSER_HELPER_SIGNATURE, helper);
		if (!messenger.IsValid())
			return false;
	}
	/* never wait: a busy helper (page loading, long JS) drops input rather than stalling OBS; configuration
	 * and area switches are retried from Poll() */
	return messenger.SendMessage(&message, (BHandler *)nullptr, 0) == B_OK;
}

void HaikuBrowserClient::SendConfigLocked()
{
	if (resize_deferred)
		return; /* the new size is sent once the area that fits it exists */
	if (attach_pending && !TrySendAttach())
		return; /* the helper must be on the new area before it is told the new size */

	BMessage msg(OBS_BROWSER_MSG_CONFIG);
	msg.AddString("url", config.url.c_str());
	msg.AddInt32("width", config.width);
	msg.AddInt32("height", config.height);
	msg.AddInt32("fps", config.fps);
	msg.AddString("css", config.css.c_str());
	msg.AddBool("transparent", config.transparent);
	if (!SendLocked(msg))
		return;
	config_dirty = false;

	if (!startup_script.empty()) {
		BMessage s(OBS_BROWSER_MSG_STARTUP_SCRIPT);
		s.AddString("startup", startup_script.c_str());
		SendLocked(s);
	}
}

void HaikuBrowserClient::Configure(const HaikuBrowserConfig &cfg)
{
	std::lock_guard<std::mutex> lock(mutex);
	config = cfg;
	config.width = std::clamp(config.width, 1, 8192);
	config.height = std::clamp(config.height, 1, 8192);
	config.fps = std::clamp(config.fps, 1, 120);
	config_dirty = true;

	if (NeedsBiggerArea()) {
		/* one switch at a time: while one is unconfirmed, Poll() makes the bigger area after it */
		if (AttachInFlight())
			resize_deferred = true;
		else
			CreateArea(std::max<uint32_t>(config.width, area_width),
				   std::max<uint32_t>(config.height, area_height));
	}

	if (!shm)
		return;
	if (!stopped && helper <= 0)
		SpawnHelper();
	else if (ready)
		SendConfigLocked();
}

void HaikuBrowserClient::Stop()
{
	std::lock_guard<std::mutex> lock(mutex);
	stopped = true;
	StopHelper();
}

void HaikuBrowserClient::Start()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!stopped)
		return; /* keep a running restart backoff */
	stopped = false;
	restarts = 0;
	next_restart = 0;
}

bool HaikuBrowserClient::Running() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return ready && HelperAlive();
}

void HaikuBrowserClient::Poll()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!shm || stopped)
		return;

	const bigtime_t now = system_time();
	shm->client_heartbeat = now;
	if (retired_shm)
		retired_shm->client_heartbeat = now;

	if (helper > 0 && !HelperAlive()) {
		int status = 0;
		waitpid(helper, &status, WNOHANG);
		BLOG(LOG_WARNING, "browser helper (team %d) exited unexpectedly (status %d)", (int)helper, status);
		helper = -1;
		ready = false;
		messenger = BMessenger();
		ScheduleRestart(now);
	}

	if (helper <= 0) {
		if (now >= next_restart)
			SpawnHelper();
		return;
	}

	obs_browser_shm *state = HelperShm();

	if (!ready) {
		if (state->helper_state == OBS_BROWSER_STATE_READY) {
			ready = true;
			SendConfigLocked();
		} else if (now - helper_started > HELPER_START_TIMEOUT) {
			BLOG(LOG_WARNING, "browser helper did not start in time, restarting it");
			StopHelper();
			next_restart = now;
		}
		return;
	}

	if (now - helper_started > 30000000LL)
		restarts = 0;

	if (AttachInFlight()) {
		if (shm->helper_team != 0) {
			/* the helper has switched to the current area; its clone of the old one lives as long as
			 * it needs it */
			delete_area(retired_area);
			retired_area = -1;
			retired_shm = nullptr;
			state = shm;
		} else if (now - attach_sent > HELPER_STALL_TIMEOUT) {
			BLOG(LOG_WARNING, "browser helper did not switch to the new frame area, restarting it");
			StopHelper();
			next_restart = now;
			return;
		}
	}

	const bigtime_t last_sign_of_life = std::max<bigtime_t>((bigtime_t)state->helper_heartbeat, helper_started);
	if (now - last_sign_of_life > HELPER_STALL_TIMEOUT) {
		BLOG(LOG_WARNING, "browser helper stopped responding, restarting it");
		StopHelper();
		next_restart = now;
		return;
	}

	if (resize_deferred && !AttachInFlight()) {
		resize_deferred = false;
		if (NeedsBiggerArea())
			CreateArea(std::max<uint32_t>(config.width, area_width),
				   std::max<uint32_t>(config.height, area_height));
	}
	if (attach_pending)
		TrySendAttach();
	if (config_dirty)
		SendConfigLocked();
}

bool HaikuBrowserClient::ReadFrame(HaikuBrowserFrame &frame)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!shm || !ready)
		return false;

	for (int attempt = 0; attempt < 3; attempt++) {
		const int32_t seq1 = shm->seq;
		if (seq1 & 1) {
			snooze(500);
			continue;
		}
		__sync_synchronize();

		const int64_t count = shm->frame_count;
		if (count == last_frame)
			return false;

		const uint32_t w = (uint32_t)shm->width;
		const uint32_t h = (uint32_t)shm->height;
		const uint32_t stride = (uint32_t)shm->stride;
		const bool alpha = shm->has_alpha != 0;
		if (!w || !h || stride < w * 4 || (uint64_t)stride * h > shm->capacity)
			return false;

		frame.pixels.resize((size_t)stride * h);
		memcpy(frame.pixels.data(), obs_browser_pixels(shm), frame.pixels.size());

		__sync_synchronize();
		if (shm->seq != seq1)
			continue; /* torn: the helper wrote while we copied */

		frame.width = w;
		frame.height = h;
		frame.stride = stride;
		frame.has_alpha = alpha;
		last_frame = count;
		return true;
	}
	return false;
}

bool HaikuBrowserClient::ReadInfo(std::string &title, std::string &url)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!shm)
		return false;
	const int32_t serial = shm->info_serial;
	if (serial == last_info)
		return false;
	__sync_synchronize();
	char t[OBS_BROWSER_TITLE_LEN], u[OBS_BROWSER_URL_LEN];
	memcpy(t, shm->title, sizeof(t));
	memcpy(u, shm->url, sizeof(u));
	t[sizeof(t) - 1] = 0;
	u[sizeof(u) - 1] = 0;
	title = t;
	url = u;
	last_info = serial;
	return true;
}

void HaikuBrowserClient::Reload(bool nocache)
{
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(OBS_BROWSER_MSG_RELOAD);
	msg.AddBool("nocache", nocache);
	SendLocked(msg);
}

void HaikuBrowserClient::ExecuteJavaScript(const std::string &script)
{
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(OBS_BROWSER_MSG_EXECUTE_JS);
	msg.AddString("script", script.c_str());
	SendLocked(msg);
}

void HaikuBrowserClient::SetStartupScript(const std::string &script)
{
	std::lock_guard<std::mutex> lock(mutex);
	startup_script = script;
	BMessage msg(OBS_BROWSER_MSG_STARTUP_SCRIPT);
	msg.AddString("startup", script.c_str());
	SendLocked(msg);
}

void HaikuBrowserClient::SetVisibility(bool visible)
{
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(OBS_BROWSER_MSG_VISIBILITY);
	msg.AddBool("visible", visible);
	SendLocked(msg);
}

void HaikuBrowserClient::SetActive(bool active)
{
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(OBS_BROWSER_MSG_VISIBILITY);
	msg.AddBool("active", active);
	SendLocked(msg);
}

void HaikuBrowserClient::Zoom(int direction)
{
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(OBS_BROWSER_MSG_ZOOM);
	msg.AddInt32("direction", direction);
	SendLocked(msg);
}

void HaikuBrowserClient::MouseDown(int x, int y, int32 buttons, int32 clicks, int32 modifiers)
{
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(OBS_BROWSER_MSG_MOUSE_DOWN);
	msg.AddInt32("x", x);
	msg.AddInt32("y", y);
	msg.AddInt32("buttons", buttons);
	msg.AddInt32("clicks", clicks);
	msg.AddInt32("modifiers", modifiers);
	SendLocked(msg);
}

void HaikuBrowserClient::MouseUp(int x, int y, int32 buttons, int32 clicks, int32 modifiers)
{
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(OBS_BROWSER_MSG_MOUSE_UP);
	msg.AddInt32("x", x);
	msg.AddInt32("y", y);
	msg.AddInt32("buttons", buttons);
	msg.AddInt32("clicks", clicks);
	msg.AddInt32("modifiers", modifiers);
	SendLocked(msg);
}

void HaikuBrowserClient::MouseMove(int x, int y, int32 modifiers, bool leave)
{
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(OBS_BROWSER_MSG_MOUSE_MOVE);
	msg.AddInt32("x", x);
	msg.AddInt32("y", y);
	msg.AddInt32("modifiers", modifiers);
	msg.AddBool("leave", leave);
	SendLocked(msg);
}

void HaikuBrowserClient::MouseWheel(float dx, float dy, int32 modifiers)
{
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(OBS_BROWSER_MSG_MOUSE_WHEEL);
	msg.AddFloat("dx", dx);
	msg.AddFloat("dy", dy);
	msg.AddInt32("modifiers", modifiers);
	SendLocked(msg);
}

void HaikuBrowserClient::Key(bool down, const std::string &bytes, int32 key, int32 modifiers)
{
	if (bytes.empty())
		return;
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(down ? OBS_BROWSER_MSG_KEY_DOWN : OBS_BROWSER_MSG_KEY_UP);
	msg.AddString("bytes", bytes.c_str());
	msg.AddInt32("key", key);
	msg.AddInt32("modifiers", modifiers);
	SendLocked(msg);
}

void HaikuBrowserClient::Focus(bool focus)
{
	std::lock_guard<std::mutex> lock(mutex);
	BMessage msg(OBS_BROWSER_MSG_FOCUS);
	msg.AddBool("focus", focus);
	SendLocked(msg);
}
