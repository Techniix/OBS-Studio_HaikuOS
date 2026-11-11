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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>

extern char **environ;

#define BLOG(level, fmt, ...) blog(level, "[haiku-browser] %s: " fmt, name.c_str(), ##__VA_ARGS__)

/* How often the helper's application thread is asked for a sign of life, and how long it may stay silent.
 * Generous: WebCore runs page script on that thread, and a slow page is not a hung one. A helper that died
 * is not found this way but at once, by the end-of-file on its socket. */
#define PING_INTERVAL 5000000LL
#define STALL_TIMEOUT 30000000LL

/* Bytes of messages waiting for a helper that does not read them; more are dropped (it is hung, and the
 * helper that replaces it gets the configuration anew) */
#define QUEUE_LIMIT (2 * (size_t)OBS_BROWSER_MAX_MESSAGE)

/* A helper told to go (its socket was closed) gets this long before it is killed. The helper gives itself
 * two seconds (browser-helper.cpp, SocketReader). */
#define REAP_GRACE 3000000LL

/* Renga's debug_server writes one report per crashing program here (roadmap F-51,
 * src/servers/debug/DebugServer.cpp, TeamDebugHandler::_WriteCrashReport()): "app-<program>-<date>.txt",
 * containing a "team: <id>" line. The System logs application lists them. */
#define CRASH_REPORT_DIR "/boot/system/var/log/crash"

namespace {

enum { EVENT_WAKE = 1, EVENT_SOCKET = 2, EVENT_FRAME = 3 };

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

/* The crash report debug_server wrote for `team` since `not_before`, or empty. One directory scan and, for the
 * few files that are recent enough, one small read; only called after a helper died or stopped answering. */
std::string find_crash_report(pid_t team, time_t not_before)
{
	static const char prefix[] = "app-" OBS_BROWSER_HELPER_NAME "-";
	DIR *dir = opendir(CRASH_REPORT_DIR);
	if (!dir)
		return std::string();

	char needle[32];
	snprintf(needle, sizeof(needle), "\nteam: %d\n", (int)team);

	std::string found;
	time_t found_time = 0;
	while (struct dirent *entry = readdir(dir)) {
		if (strncmp(entry->d_name, prefix, sizeof(prefix) - 1) != 0)
			continue;
		const std::string path = std::string(CRASH_REPORT_DIR "/") + entry->d_name;
		struct stat st;
		/* a little slack: the report's time is debug_server's, taken before it wrote the file */
		if (stat(path.c_str(), &st) != 0 || st.st_mtime + 2 < not_before || st.st_mtime < found_time)
			continue;
		/* the header is: title, time, program (a path), command line (at most 64 bytes), team */
		char head[2048];
		const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		const ssize_t got = read(fd, head, sizeof(head) - 1);
		close(fd);
		if (got <= 0)
			continue;
		head[got] = 0;
		if (strstr(head, needle)) {
			found = path;
			found_time = st.st_mtime;
		}
	}
	closedir(dir);
	return found;
}

/*
 * Waits for helpers that were told to go, so that no caller has to: closing a helper's socket makes it quit
 * (it flushes WebKit's storage first), which can take a moment that neither the UI thread nor a client's I/O
 * thread should spend. One thread for the whole module, joined in obs_module_unload(), instead of a detached
 * thread per helper that could still be running when the module's code is unloaded.
 */
class Reaper {
public:
	~Reaper() { Shutdown(); }

	void Add(pid_t pid)
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (done) {
			/* after module unload began; nothing may be left running */
			lock.unlock();
			kill(pid, SIGKILL);
			waitpid(pid, nullptr, 0);
			return;
		}
		pending.push_back({pid, system_time() + REAP_GRACE, false});
		if (!thread.joinable())
			thread = std::thread([this]() { Run(); });
		cond.notify_one();
	}

	/* returns once every helper handed over has exited (at most REAP_GRACE plus the time SIGKILL takes) */
	void Shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			done = true;
		}
		cond.notify_one();
		if (thread.joinable())
			thread.join();
	}

private:
	struct Entry {
		pid_t pid;
		bigtime_t deadline;
		bool killed;
	};

	void Run()
	{
		os_set_thread_name("haiku-browser-reaper");
		std::unique_lock<std::mutex> lock(mutex);
		for (;;) {
			if (pending.empty()) {
				if (done)
					return;
				cond.wait(lock);
				continue;
			}
			const bigtime_t now = system_time();
			for (auto it = pending.begin(); it != pending.end();) {
				/* non-zero: it exited, or it is not our child (any more) */
				if (waitpid(it->pid, nullptr, WNOHANG) != 0) {
					it = pending.erase(it);
					continue;
				}
				if (now >= it->deadline && !it->killed) {
					kill(it->pid, SIGKILL);
					it->killed = true;
				}
				++it;
			}
			if (!pending.empty())
				cond.wait_for(lock, std::chrono::milliseconds(50));
		}
	}

	std::mutex mutex;
	std::condition_variable cond;
	std::vector<Entry> pending;
	std::thread thread;
	bool done = false;
};

Reaper &reaper()
{
	static Reaper instance;
	return instance;
}

/* Serializes helper starts of all clients: while one is being started, its ends of the socket and the eventfd
 * are inheritable, and a helper started at the same moment by another client's thread would inherit them too
 * (and then keep the first helper's socket open after that one died). */
std::mutex spawn_mutex;

} // namespace

void haiku_browser_shutdown(void)
{
	reaper().Shutdown();
}

HaikuBrowserClient::HaikuBrowserClient(const char *log_name, Callbacks callbacks_, const std::string &storage_path)
	: name(log_name ? log_name : "browser"),
	  storage(storage_path),
	  callbacks(std::move(callbacks_))
{
	/* Renga: epoll and eventfd (libgnu; 3rdparty/ce/PORTING.md "epoll", "eventfd") */
	epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	struct epoll_event event = {};
	event.events = EPOLLIN;
	event.data.u32 = EVENT_WAKE;
	if (epoll_fd < 0 || wake_fd < 0 || epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &event) != 0) {
		BLOG(LOG_ERROR, "cannot set up the helper event loop (%s); this browser stays empty", strerror(errno));
		return;
	}
	thread = std::thread([this]() { Run(); });
}

HaikuBrowserClient::~HaikuBrowserClient()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		quit = true;
	}
	Wake();
	if (thread.joinable())
		thread.join(); /* the thread closes the helper's socket, which ends the helper */
	if (wake_fd >= 0)
		close(wake_fd);
	if (epoll_fd >= 0)
		close(epoll_fd);
}

void HaikuBrowserClient::Wake()
{
	if (wake_fd >= 0)
		eventfd_write(wake_fd, 1);
}

/* ---------------------------------------------------------------------------------------------------------- */
/* any thread */

bool HaikuBrowserClient::EnqueueLocked(std::vector<uint8_t> &&bytes, bool mouse_move)
{
	if (bytes.empty() || !connected)
		return false;

	/* Pointer positions the helper has not even been sent yet are superseded by the newest one */
	if (mouse_move && queue_tail_is_move && !queue.empty()) {
		queue_bytes -= queue.back().size();
		queue_bytes += bytes.size();
		queue.back() = std::move(bytes);
		return true;
	}

	if (queue_bytes + bytes.size() > QUEUE_LIMIT) {
		if (!queue_overflow_logged)
			BLOG(LOG_WARNING, "browser helper is not reading its messages; dropping input");
		queue_overflow_logged = true;
		return false;
	}
	queue_bytes += bytes.size();
	queue.push_back(std::move(bytes));
	queue_tail_is_move = mouse_move;
	return true;
}

void HaikuBrowserClient::Post(const BMessage &message, bool mouse_move)
{
	std::vector<uint8_t> bytes = obs_browser_frame_message(message);
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!EnqueueLocked(std::move(bytes), mouse_move))
			return;
	}
	Wake();
}

void HaikuBrowserClient::QueueConfigLocked()
{
	BMessage msg(OBS_BROWSER_MSG_CONFIG);
	msg.AddString("url", config.url.c_str());
	msg.AddInt32("width", config.width);
	msg.AddInt32("height", config.height);
	msg.AddInt32("fps", config.fps);
	msg.AddString("css", config.css.c_str());
	msg.AddBool("transparent", config.transparent);
	EnqueueLocked(obs_browser_frame_message(msg), false);
}

void HaikuBrowserClient::Configure(const HaikuBrowserConfig &cfg)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		config = cfg;
		config.width = std::clamp(config.width, 1, OBS_BROWSER_MAX_DIMENSION);
		config.height = std::clamp(config.height, 1, OBS_BROWSER_MAX_DIMENSION);
		config.fps = std::clamp(config.fps, 1, 120);
		have_config = true;
		QueueConfigLocked(); /* no-op without a helper: one started later is sent `config` first thing */
	}
	Wake();
}

void HaikuBrowserClient::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopped = true;
	}
	Wake();
	/* a callback that began before `stopped` was set finishes first; later ones see the flag */
	std::lock_guard<std::mutex> wait(deliver_mutex);
}

void HaikuBrowserClient::Start()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!stopped)
			return; /* keep a running restart backoff */
		stopped = false;
		reset_backoff = true;
	}
	Wake();
}

void HaikuBrowserClient::Reload(bool nocache)
{
	BMessage msg(OBS_BROWSER_MSG_RELOAD);
	msg.AddBool("nocache", nocache);
	Post(msg);
}

void HaikuBrowserClient::ExecuteJavaScript(const std::string &script)
{
	BMessage msg(OBS_BROWSER_MSG_EXECUTE_JS);
	msg.AddString("script", script.c_str());
	Post(msg);
}

void HaikuBrowserClient::SetStartupScript(const std::string &script)
{
	BMessage msg(OBS_BROWSER_MSG_STARTUP_SCRIPT);
	msg.AddString("startup", script.c_str());
	std::vector<uint8_t> bytes = obs_browser_frame_message(msg);
	{
		std::lock_guard<std::mutex> lock(mutex);
		startup_script = script;
		if (!EnqueueLocked(std::move(bytes), false))
			return;
	}
	Wake();
}

void HaikuBrowserClient::SetVisibility(bool visible)
{
	BMessage msg(OBS_BROWSER_MSG_VISIBILITY);
	msg.AddBool("visible", visible);
	Post(msg);
}

void HaikuBrowserClient::SetActive(bool active)
{
	BMessage msg(OBS_BROWSER_MSG_VISIBILITY);
	msg.AddBool("active", active);
	Post(msg);
}

void HaikuBrowserClient::Zoom(int direction)
{
	BMessage msg(OBS_BROWSER_MSG_ZOOM);
	msg.AddInt32("direction", direction);
	Post(msg);
}

void HaikuBrowserClient::MouseDown(int x, int y, int32 buttons, int32 clicks, int32 modifiers)
{
	BMessage msg(OBS_BROWSER_MSG_MOUSE_DOWN);
	msg.AddInt32("x", x);
	msg.AddInt32("y", y);
	msg.AddInt32("buttons", buttons);
	msg.AddInt32("clicks", clicks);
	msg.AddInt32("modifiers", modifiers);
	Post(msg);
}

void HaikuBrowserClient::MouseUp(int x, int y, int32 buttons, int32 clicks, int32 modifiers)
{
	BMessage msg(OBS_BROWSER_MSG_MOUSE_UP);
	msg.AddInt32("x", x);
	msg.AddInt32("y", y);
	msg.AddInt32("buttons", buttons);
	msg.AddInt32("clicks", clicks);
	msg.AddInt32("modifiers", modifiers);
	Post(msg);
}

void HaikuBrowserClient::MouseMove(int x, int y, int32 modifiers, bool leave)
{
	BMessage msg(OBS_BROWSER_MSG_MOUSE_MOVE);
	msg.AddInt32("x", x);
	msg.AddInt32("y", y);
	msg.AddInt32("modifiers", modifiers);
	msg.AddBool("leave", leave);
	Post(msg, !leave);
}

void HaikuBrowserClient::MouseWheel(float dx, float dy, int32 modifiers)
{
	BMessage msg(OBS_BROWSER_MSG_MOUSE_WHEEL);
	msg.AddFloat("dx", dx);
	msg.AddFloat("dy", dy);
	msg.AddInt32("modifiers", modifiers);
	Post(msg);
}

void HaikuBrowserClient::Key(bool down, const std::string &bytes, int32 key, int32 modifiers)
{
	if (bytes.empty())
		return;
	BMessage msg(down ? OBS_BROWSER_MSG_KEY_DOWN : OBS_BROWSER_MSG_KEY_UP);
	msg.AddString("bytes", bytes.c_str());
	msg.AddInt32("key", key);
	msg.AddInt32("modifiers", modifiers);
	Post(msg);
}

void HaikuBrowserClient::Focus(bool focus)
{
	BMessage msg(OBS_BROWSER_MSG_FOCUS);
	msg.AddBool("focus", focus);
	Post(msg);
}

/* ---------------------------------------------------------------------------------------------------------- */
/* I/O thread */

void HaikuBrowserClient::Run()
{
	os_set_thread_name("haiku-browser-io");

	for (;;) {
		bool want_helper;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (quit)
				break;
			want_helper = have_config && !stopped;
			if (reset_backoff) {
				reset_backoff = false;
				restarts = 0;
				next_restart = 0;
			}
		}

		bigtime_t now = system_time();
		if (!want_helper && helper > 0)
			CloseHelper();
		if (want_helper && helper <= 0 && now >= next_restart)
			Spawn(now);
		if (helper > 0)
			Housekeeping(now);
		if (helper > 0)
			Flush(now);

		/* Nothing to do until a descriptor says so, except the slow ping while a helper runs and the
		 * restart backoff while none does. */
		int timeout = -1;
		if (helper > 0)
			timeout = (int)(PING_INTERVAL / 2000);
		else if (want_helper)
			timeout = (int)std::max<bigtime_t>((next_restart - system_time()) / 1000 + 1, 1);

		struct epoll_event events[4];
		const int count = epoll_wait(epoll_fd, events, 4, timeout);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			BLOG(LOG_ERROR, "epoll_wait failed (%s); this browser stops updating", strerror(errno));
			break;
		}

		now = system_time();
		for (int i = 0; i < count; i++) {
			eventfd_t value;
			switch (events[i].data.u32) {
			case EVENT_WAKE:
				eventfd_read(wake_fd, &value);
				break;
			case EVENT_SOCKET:
				/* also for hang-up and error: the read then reports the end-of-file. Skipped if an
				 * earlier event of this batch already closed the helper. */
				if (sock >= 0)
					ReadSocket(now);
				break;
			case EVENT_FRAME:
				if (frame_fd >= 0) {
					eventfd_read(frame_fd, &value);
					ReadFrame();
				}
				break;
			}
		}
	}

	CloseHelper();
}

bool HaikuBrowserClient::Spawn(bigtime_t now)
{
	const std::string path = helper_path();
	if (access(path.c_str(), X_OK) != 0) {
		if (!spawn_failing)
			BLOG(LOG_ERROR, "helper %s is missing; browser sources cannot render", path.c_str());
		spawn_failing = true;
		ScheduleRestart(now);
		return false;
	}
	mark_background_app(path);

	/* SOCK_STREAM: see browser-ipc.hpp. Both ends close-on-exec; the helper's is made inheritable only
	 * while it is started. The helper adds to the eventfd, we read it; O_NONBLOCK belongs to the open file,
	 * so the helper's copy never blocks either. */
	int pair[2] = {-1, -1};
	int event_fd = -1;
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0 ||
	    fcntl(pair[0], F_SETFL, fcntl(pair[0], F_GETFL) | O_NONBLOCK) != 0 ||
	    (event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
		if (!spawn_failing)
			BLOG(LOG_ERROR, "cannot create the helper's socket and eventfd: %s", strerror(errno));
		spawn_failing = true;
		if (pair[0] >= 0) {
			close(pair[0]);
			close(pair[1]);
		}
		ScheduleRestart(now);
		return false;
	}

	pid_t pid = -1;
	int err;
	{
		/*
		 * No posix_spawn file actions: with any, Haiku's posix_spawn() fork()s the calling process
		 * (src/system/libroot/posix/spawn.cpp, do_posix_spawn()), and OBS is a large multi-threaded
		 * process with graphics driver state. Without them it uses load_image(), whose new team inherits
		 * every descriptor that is not close-on-exec (src/system/kernel/team.cpp, load_image_internal():
		 * vfs_new_io_context(parent, true)). So the helper's two descriptors are made inheritable for the
		 * duration of the call and named on the command line.
		 */
		std::lock_guard<std::mutex> lock(spawn_mutex);
		const int child_event = dup(event_fd); /* dup() clears close-on-exec */
		if (child_event < 0 || fcntl(pair[1], F_SETFD, 0) != 0) {
			err = errno;
		} else {
			char ipc_arg[16], wake_arg[16];
			snprintf(ipc_arg, sizeof(ipc_arg), "%d", pair[1]);
			snprintf(wake_arg, sizeof(wake_arg), "%d", child_event);
			char *argv[8] = {const_cast<char *>(path.c_str()),
					 const_cast<char *>("--ipc"),
					 ipc_arg,
					 const_cast<char *>("--wake"),
					 wake_arg,
					 nullptr,
					 nullptr,
					 nullptr};
			if (!storage.empty()) {
				argv[5] = const_cast<char *>("--storage");
				argv[6] = const_cast<char *>(storage.c_str());
			}
			err = posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv, environ);
		}
		if (child_event >= 0)
			close(child_event);
		close(pair[1]);
	}

	struct epoll_event socket_event = {};
	socket_event.events = EPOLLIN;
	socket_event.data.u32 = EVENT_SOCKET;
	struct epoll_event frame_event = {};
	frame_event.events = EPOLLIN;
	frame_event.data.u32 = EVENT_FRAME;
	if (err == 0 && (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pair[0], &socket_event) != 0 ||
			 epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &frame_event) != 0)) {
		err = errno;
		reaper().Add(pid); /* closing its socket below ends it */
	}
	if (err != 0) {
		if (!spawn_failing)
			BLOG(LOG_ERROR, "could not start %s: %s", path.c_str(), strerror(err));
		spawn_failing = true;
		close(pair[0]); /* also removes it from the epoll set */
		close(event_fd);
		ScheduleRestart(now);
		return false;
	}

	spawn_failing = false;
	sock = pair[0];
	sock_writable_watched = false;
	frame_fd = event_fd;
	helper = pid;
	helper_started = last_alive = last_ping = now;
	helper_started_wall = time(nullptr);
	last_crash_check = now;
	ping_outstanding = false;
	hello = false;

	{
		/* The helper reads these once its application runs; until then they wait in the socket. */
		std::lock_guard<std::mutex> lock(mutex);
		queue.clear();
		queue_bytes = 0;
		queue_tail_is_move = false;
		queue_overflow_logged = false;
		connected = true;
		if (!startup_script.empty()) {
			BMessage script(OBS_BROWSER_MSG_STARTUP_SCRIPT);
			script.AddString("startup", startup_script.c_str());
			EnqueueLocked(obs_browser_frame_message(script), false);
		}
		QueueConfigLocked();
	}

	BLOG(LOG_INFO, "started browser helper (team %d)", (int)pid);
	return true;
}

void HaikuBrowserClient::ScheduleRestart(bigtime_t now)
{
	const bigtime_t backoff = std::min<bigtime_t>(500000LL << std::min(restarts, 5), 10000000LL);
	next_restart = now + backoff;
	restarts++;
}

/* Forgets the current helper. Closing the socket is the request to quit: the helper sees end-of-file, shuts
 * WebKit down and exits; the reaper waits for it (and kills it if it does not). Never blocks. */
void HaikuBrowserClient::CloseHelper()
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		connected = false;
		queue.clear();
		queue_bytes = 0;
		queue_tail_is_move = false;
	}
	sending.clear();
	sending_offset = 0;
	rx.clear();
	for (int fd : rx_fds)
		close(fd);
	rx_fds.clear();

	/* closing a descriptor takes it out of the epoll set (PORTING.md "epoll": registrations go away with
	 * the last descriptor of the open file; ours are the only ones in this process) */
	if (sock >= 0)
		close(sock);
	if (frame_fd >= 0)
		close(frame_fd);
	sock = frame_fd = -1;
	sock_writable_watched = false;
	UnmapBuffer();

	if (helper > 0)
		reaper().Add(helper);
	helper = -1;
}

void HaikuBrowserClient::HelperExited(bigtime_t now, pid_t pid, int status)
{
	char how[96];
	if (WIFSIGNALED(status))
		snprintf(how, sizeof(how), "was ended by signal %d (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
	else
		snprintf(how, sizeof(how), "exited with status %d", WEXITSTATUS(status));

	/* Renga F-51: say where the crash report is instead of only "helper died" */
	const std::string report = find_crash_report(pid, helper_started_wall);
	if (!report.empty())
		BLOG(LOG_WARNING, "browser helper (team %d) crashed (%s); crash report: %s (also in System logs)",
		     (int)pid, how, report.c_str());
	else
		BLOG(LOG_WARNING, "browser helper (team %d) %s unexpectedly; no crash report for it in %s", (int)pid,
		     how, CRASH_REPORT_DIR);

	helper = -1; /* already waited for */
	CloseHelper();
	ScheduleRestart(now);
}

/* End-of-file on the socket: the helper is gone or going. */
void HaikuBrowserClient::HelperDied(bigtime_t now)
{
	const pid_t pid = helper;
	/* its descriptors are closed a moment before it can be waited for */
	for (int i = 0; i < 50; i++) {
		int status = 0;
		const pid_t result = waitpid(pid, &status, WNOHANG);
		if (result == pid) {
			HelperExited(now, pid, status);
			return;
		}
		if (result < 0)
			break;
		snooze(20000);
	}

	const std::string report = find_crash_report(pid, helper_started_wall);
	if (!report.empty())
		BLOG(LOG_WARNING, "browser helper (team %d) crashed; crash report: %s (also in System logs)", (int)pid,
		     report.c_str());
	else
		BLOG(LOG_WARNING, "browser helper (team %d) closed its connection unexpectedly", (int)pid);
	CloseHelper();
	ScheduleRestart(now);
}

void HaikuBrowserClient::KillHelper(bigtime_t now, const char *why, const std::string &crash_report)
{
	if (!crash_report.empty())
		BLOG(LOG_WARNING, "browser helper (team %d) %s; crash report: %s (also in System logs); restarting it",
		     (int)helper, why, crash_report.c_str());
	else
		BLOG(LOG_WARNING, "browser helper (team %d) %s; restarting it", (int)helper, why);
	kill(helper, SIGKILL);
	CloseHelper();
	ScheduleRestart(now);
}

void HaikuBrowserClient::Housekeeping(bigtime_t now)
{
	if (hello && restarts > 0 && now - helper_started > 30000000LL)
		restarts = 0;

	if (!ping_outstanding) {
		if (now - last_ping >= PING_INTERVAL) {
			BMessage ping(OBS_BROWSER_MSG_PING);
			ping.AddInt32("serial", ++ping_serial);
			Post(ping);
			ping_outstanding = true;
			last_ping = now;
		}
		return;
	}

	/* A ping has been unanswered for a while: busy, hung, or crashed. A crashed team does not exit by
	 * itself: debug_server writes its report and then shows its alert, with the team stopped and its socket
	 * still open. The report tells the two apart without waiting for the full timeout. */
	if (now - last_ping >= PING_INTERVAL && now - last_crash_check >= PING_INTERVAL) {
		last_crash_check = now;

		/* gone although the socket is open (a descriptor leaked into some other child process)? */
		int status = 0;
		if (waitpid(helper, &status, WNOHANG) == helper) {
			HelperExited(now, helper, status);
			return;
		}

		const std::string report = find_crash_report(helper, helper_started_wall);
		if (!report.empty()) {
			KillHelper(now, "crashed", report);
			return;
		}
	}

	if (now - last_alive > STALL_TIMEOUT)
		KillHelper(now, "stopped responding");
}

template<typename Call> void HaikuBrowserClient::Deliver(Call &&call)
{
	std::lock_guard<std::mutex> delivering(deliver_mutex);
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopped || quit)
			return;
	}
	call();
}

void HaikuBrowserClient::ReadSocket(bigtime_t now)
{
	for (;;) {
		uint8_t buffer[4096];
		/* room for the descriptor of a buffer event (and a few more, which a correct helper never sends) */
		union {
			char bytes[CMSG_SPACE(4 * sizeof(int))];
			struct cmsghdr align;
		} control;
		struct iovec iov = {buffer, sizeof(buffer)};
		struct msghdr message = {};
		message.msg_iov = &iov;
		message.msg_iovlen = 1;
		message.msg_control = control.bytes;
		message.msg_controllen = sizeof(control.bytes);

		const ssize_t got = recvmsg(sock, &message, MSG_CMSG_CLOEXEC);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			HelperDied(now);
			return;
		}
		if (got == 0) {
			HelperDied(now);
			return;
		}

		/* SCM_RIGHTS (3rdparty/ce/tests/posix_compat/memfd_test.cpp "scm_rights"). On a stream socket the
		 * descriptors arrive with the read that takes the first byte of the message they were sent with
		 * (UnixFifo.cpp), that is, never later than the event they belong to is complete. */
		for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
			if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
				continue;
			const size_t fds = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			for (size_t i = 0; i < fds; i++) {
				int fd;
				memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(fd));
				rx_fds.push_back(fd);
			}
		}
		rx.insert(rx.end(), buffer, buffer + got);

		size_t offset = 0;
		while (rx.size() - offset >= sizeof(uint32_t)) {
			uint32_t length;
			memcpy(&length, rx.data() + offset, sizeof(length));
			const bool valid = length >= sizeof(obs_browser_event) && length <= OBS_BROWSER_MAX_EVENT;
			if (valid && rx.size() - offset - sizeof(length) < length)
				break; /* the rest is still to come */
			if (!valid || !HandleEvent(rx.data() + offset + sizeof(length), length, now)) {
				KillHelper(now, "sent an invalid message");
				return;
			}
			if (helper <= 0)
				return; /* the event made us drop the helper */
			offset += sizeof(length) + length;
		}
		rx.erase(rx.begin(), rx.begin() + offset);

		/* one buffer event can be in flight; anything beyond is a helper leaking descriptors into OBS */
		if (rx_fds.size() > 4) {
			KillHelper(now, "sent unexpected file descriptors");
			return;
		}
	}
}

/* false: protocol violation */
bool HaikuBrowserClient::HandleEvent(const uint8_t *payload, size_t size, bigtime_t now)
{
	obs_browser_event event;
	memcpy(&event, payload, sizeof(event));
	const char *text = (const char *)payload + sizeof(event);
	const size_t text_size = size - sizeof(event);

	switch (event.what) {
	case OBS_BROWSER_EVENT_HELLO:
		if (event.value != OBS_BROWSER_PROTOCOL) {
			if (!spawn_failing)
				BLOG(LOG_ERROR,
				     "%s speaks protocol %d, this OBS speaks %d: it is a leftover from another OBS "
				     "version; browser sources cannot render",
				     helper_path().c_str(), (int)event.value, OBS_BROWSER_PROTOCOL);
			spawn_failing = true;
			CloseHelper();
			restarts = 5; /* look again every ten seconds, quietly */
			ScheduleRestart(now);
			return true;
		}
		hello = true;
		last_alive = now;
		return true;

	case OBS_BROWSER_EVENT_PONG:
		if (event.value == ping_serial) {
			ping_outstanding = false;
			last_alive = now;
		}
		return true;

	case OBS_BROWSER_EVENT_BUFFER: {
		if (rx_fds.empty())
			return false;
		const int fd = rx_fds.front();
		rx_fds.pop_front();
		return MapBuffer(fd);
	}

	case OBS_BROWSER_EVENT_INFO: {
		/* two NUL-terminated strings */
		const char *title_end = (const char *)memchr(text, 0, text_size);
		if (!title_end)
			return false;
		const char *url = title_end + 1;
		const char *url_end = (const char *)memchr(url, 0, text_size - (size_t)(url - text));
		if (!url_end)
			return false;
		if (callbacks.info) {
			const std::string title(text), address(url);
			Deliver([&]() { callbacks.info(title, address); });
		}
		return true;
	}
	}
	return true; /* unknown events are ignored */
}

/* Takes ownership of `fd`, a frame memfd from the helper. */
bool HaikuBrowserClient::MapBuffer(int fd)
{
	/*
	 * A mapping of a file faults when the file is truncated under it, and the helper is not trusted that
	 * far. Renga's memfds can be sealed like Linux's (PORTING.md "memfd_create"; memfd_test.cpp
	 * "shrink_grow_sealed"): once F_SEAL_SHRINK is set, nobody can make the file smaller again. The helper
	 * sets it; if it did not, set it here (seals are shared by every descriptor of the file), and refuse
	 * anything that cannot be sealed: F_GET_SEALS fails with EINVAL on files that are not memfds.
	 */
	int seals = fcntl(fd, F_GET_SEALS);
	if (seals >= 0 && !(seals & F_SEAL_SHRINK) && fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0)
		seals |= F_SEAL_SHRINK;

	const uint64_t max_size = (uint64_t)OBS_BROWSER_FRAME_HEADER_SIZE +
				  (uint64_t)OBS_BROWSER_MAX_DIMENSION * OBS_BROWSER_MAX_DIMENSION * 4 + 4096;
	struct stat st;
	void *address = MAP_FAILED;
	if (seals >= 0 && (seals & F_SEAL_SHRINK) && fstat(fd, &st) == 0 &&
	    (uint64_t)st.st_size > OBS_BROWSER_FRAME_HEADER_SIZE && (uint64_t)st.st_size <= max_size)
		address = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd); /* the mapping keeps the file (memfd_test.cpp "mapping_outlives_fd") */
	if (address == MAP_FAILED) {
		BLOG(LOG_WARNING, "cannot use the helper's frame buffer");
		return false;
	}

	UnmapBuffer();
	map = static_cast<const uint8_t *>(address);
	map_size = (size_t)st.st_size;
	ReadFrame(); /* its first frame may be complete already; its wake-up may have been consumed earlier */
	return true;
}

void HaikuBrowserClient::UnmapBuffer()
{
	if (map)
		munmap(const_cast<uint8_t *>(map), map_size);
	map = nullptr;
	map_size = 0;
	have_last_frame = false;
}

/* Copies the newest frame out of the shared buffer if there is a complete one that was not delivered yet. */
void HaikuBrowserClient::ReadFrame()
{
	if (!map || !callbacks.frame)
		return;

	/* the helper writes this memory while we read it */
	const volatile obs_browser_frame_header *header = reinterpret_cast<const volatile obs_browser_frame_header *>(map);

	const uint32_t seq = __atomic_load_n(&header->seq, __ATOMIC_ACQUIRE);
	if (seq & 1)
		return; /* being written; the eventfd is signalled again when it is complete */

	const uint64_t count = header->frame_count;
	if (header->magic != OBS_BROWSER_FRAME_MAGIC || count == 0 || (have_last_frame && count == last_frame))
		return;

	/* every field is checked against our own mapping, never trusted */
	const uint32_t width = header->width;
	const uint32_t height = header->height;
	const uint32_t stride = header->stride;
	const bool alpha = header->has_alpha != 0;
	if (!width || !height || width > OBS_BROWSER_MAX_DIMENSION || height > OBS_BROWSER_MAX_DIMENSION ||
	    stride < width * 4 || stride > OBS_BROWSER_MAX_DIMENSION * 4 ||
	    (uint64_t)stride * height > map_size - OBS_BROWSER_FRAME_HEADER_SIZE)
		return;

	copy.resize((size_t)stride * height);
	memcpy(copy.data(), map + OBS_BROWSER_FRAME_HEADER_SIZE, copy.size());

	if (__atomic_load_n(&header->seq, __ATOMIC_ACQUIRE) != seq)
		return; /* torn by a newer frame, which announces itself */

	last_frame = count;
	have_last_frame = true;

	HaikuBrowserFrame frame;
	frame.pixels = copy.data();
	frame.width = width;
	frame.height = height;
	frame.stride = stride;
	frame.has_alpha = alpha;
	Deliver([&]() { callbacks.frame(frame); });
}

void HaikuBrowserClient::WatchSocket(bool writable)
{
	if (writable == sock_writable_watched)
		return;
	struct epoll_event event = {};
	event.events = EPOLLIN | (writable ? EPOLLOUT : 0);
	event.data.u32 = EVENT_SOCKET;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, sock, &event) == 0)
		sock_writable_watched = writable;
}

/* Writes queued messages until the queue is empty or the socket is full; in the latter case epoll reports
 * when there is room again. A stream socket may take part of a message: the rest stays in `sending`. */
void HaikuBrowserClient::Flush(bigtime_t now)
{
	for (;;) {
		if (sending_offset == sending.size()) {
			std::lock_guard<std::mutex> lock(mutex);
			if (queue.empty()) {
				sending.clear();
				sending_offset = 0;
				break;
			}
			queue_bytes -= queue.front().size();
			sending = std::move(queue.front());
			queue.pop_front();
			if (queue.empty())
				queue_tail_is_move = false;
			sending_offset = 0;
		}

		/* MSG_NOSIGNAL: a helper that just died must not raise SIGPIPE in OBS */
		const ssize_t sent =
			send(sock, sending.data() + sending_offset, sending.size() - sending_offset, MSG_NOSIGNAL);
		if (sent > 0) {
			sending_offset += (size_t)sent;
			continue;
		}
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			WatchSocket(true);
			return;
		}
		HelperDied(now);
		return;
	}
	WatchSocket(false);
}
