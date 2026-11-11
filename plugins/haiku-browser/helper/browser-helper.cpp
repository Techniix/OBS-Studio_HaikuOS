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
 * obs-haiku-browser: renders one web page for an OBS browser source or browser dock with HaikuWebKit.
 *
 *   obs-haiku-browser --ipc <socket fd> --wake <eventfd> [--storage <dir>]
 *
 * One process per browser instance: a page that hangs or crashes WebKit only takes its own helper down, and
 * the OBS side restarts it. See ../browser-ipc.hpp for the protocol; both descriptors are inherited from OBS.
 *
 * The page lives in a borderless window parked far outside the screen. It must stay shown, because hiding a
 * BWebView marks the page invisible and WebCore then stops timers and animations. HaikuWebKit renders the
 * page into the view's offscreen bitmap; a pump thread copies that bitmap into a sealed memfd shared with OBS
 * at the requested frame rate.
 */

#include "../browser-ipc.hpp"
#include "webkit-abi.h"

#include <Application.h>
#include <Bitmap.h>
#include <Directory.h>
#include <Messenger.h>
#include <FindDirectory.h>
#include <Path.h>
#include <String.h>
#include <View.h>
#include <Window.h>

#include <WebPage.h>
#include <WebSettings.h>
#include <WebView.h>
#include <WebViewConstants.h>
#include <WebWindow.h>

#include <image.h>

#include <algorithm>
#include <atomic>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#define LOG(...) fprintf(stderr, "obs-haiku-browser: " __VA_ARGS__)

/* far enough outside any realistic desktop that the window never shows */
#define PARK_X -30000.0f
#define PARK_Y -30000.0f

enum {
	/* window -> app: page (re)loaded, inject CSS and window.obsstudio; "committed" bool: a new main-frame
	 * document, "finished" bool: its DOM is ready (runs the startup script once per load) */
	MSG_INJECT = 'inj_',
};

/* Bumped whenever the offscreen bitmap may have changed. Everything that draws into it runs on the
 * application thread while it dispatches a message (BWebPage::paint() for HANDLE_DRAW and visibility
 * changes, BWebPage::scroll() which blits synchronously from inside WebCore timers, input and script), or in
 * BWebView::FrameResized() on the window thread. HelperApp::DispatchMessage() bumps it after each message,
 * HelperView::FrameResized() after a resize; the pump only copies a frame when it changed. A page with
 * nothing going on (no timers, animations or input) dispatches nothing, so a static overlay costs one copy. */
static std::atomic<uint32> paint_serial{1};

/* Page script support that mirrors what obs-browser offers pages (the subset that needs no frontend round
 * trip): window.obsstudio.pluginVersion and the visibility/activity events. */
static const char *kObsStudioObject =
	"(function(){if(window.obsstudio)return;"
	"window.obsstudio={pluginVersion:'2.24.0-haiku',"
	"getControlLevel:function(cb){if(cb)cb(1);},"
	"getStatus:function(cb){if(cb)cb({recording:false,streaming:false,recordingPaused:false,"
	"replaybuffer:false,virtualcam:false});}};})();";

static void escape_js_string(const std::string &in, std::string &out)
{
	out.reserve(in.size() + 16);
	for (char c : in) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case '\'':
			out += "\\'";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '<':
			out += "\\x3c"; /* keep "</script>" sequences inert */
			break;
		default:
			out += c;
		}
	}
}

/*
 * The helper's end of the connection to OBS. Sends come from three threads (the application thread: hello,
 * pings, page information; the window thread: page information; the pump: frame buffers), so they are
 * serialized. The socket blocks: OBS's I/O thread always reads, and if it ever stopped, a blocked helper is
 * exactly what its ping timeout is there to find.
 */
class Channel {
public:
	explicit Channel(int fd) : fd(fd) {}

	/* `pass_fd` >= 0: also passes that descriptor (SCM_RIGHTS) with the first byte */
	bool Send(uint32_t what, int32_t value, const std::string &text = std::string(), int pass_fd = -1)
	{
		obs_browser_event event = {what, value};
		const uint32_t length = (uint32_t)(sizeof(event) + text.size());
		if (length > OBS_BROWSER_MAX_EVENT)
			return false;
		std::vector<uint8_t> bytes(sizeof(length) + length);
		memcpy(bytes.data(), &length, sizeof(length));
		memcpy(bytes.data() + sizeof(length), &event, sizeof(event));
		if (!text.empty())
			memcpy(bytes.data() + sizeof(length) + sizeof(event), text.data(), text.size());

		std::lock_guard<std::mutex> lock(mutex);
		size_t offset = 0;
		while (offset < bytes.size()) {
			struct iovec iov = {bytes.data() + offset, bytes.size() - offset};
			struct msghdr message = {};
			message.msg_iov = &iov;
			message.msg_iovlen = 1;
			union {
				char bytes[CMSG_SPACE(sizeof(int))];
				struct cmsghdr align;
			} control;
			if (pass_fd >= 0 && offset == 0) {
				memset(&control, 0, sizeof(control));
				message.msg_control = control.bytes;
				message.msg_controllen = sizeof(control.bytes);
				struct cmsghdr *header = CMSG_FIRSTHDR(&message);
				header->cmsg_level = SOL_SOCKET;
				header->cmsg_type = SCM_RIGHTS;
				header->cmsg_len = CMSG_LEN(sizeof(int));
				memcpy(CMSG_DATA(header), &pass_fd, sizeof(int));
			}
			/* MSG_NOSIGNAL: OBS having gone away is handled by the reader, not by SIGPIPE */
			const ssize_t sent = sendmsg(fd, &message, MSG_NOSIGNAL);
			if (sent < 0 && errno == EINTR)
				continue;
			if (sent <= 0)
				return false;
			offset += (size_t)sent;
		}
		return true;
	}

	const int fd;

private:
	std::mutex mutex;
};

static Channel *channel = nullptr;

static void send_info(const BString &title, const BString &url)
{
	std::string text(title.String());
	text += '\0';
	text += url.String();
	text += '\0';
	/* both are bounded by the event size; titles and URLs that long are cut, never dropped */
	if (text.size() > OBS_BROWSER_MAX_EVENT - sizeof(obs_browser_event)) {
		std::string t(title.String(), std::min<size_t>(title.Length(), 1024));
		std::string u(url.String(), std::min<size_t>(url.Length(), 8192));
		text = t + '\0' + u + '\0';
	}
	channel->Send(OBS_BROWSER_EVENT_INFO, 0, text);
}

/* BWebView allocates its offscreen bitmap in FrameResized(), which runs asynchronously after the view gets a
 * real size; until then getOffscreen() would dereference a null view. */
class HelperView : public BWebView {
public:
	HelperView() : BWebView("obs browser view") {}

	void FrameResized(float width, float height) override
	{
		BWebView::FrameResized(width, height);
		if (width >= 0.0f && height >= 0.0f)
			offscreen_ready = true;
		paint_serial++; /* the offscreen bitmap was reallocated or its exposed part cleared */
	}

	std::atomic_bool offscreen_ready{false};
};

class HelperWindow : public BWebWindow {
public:
	HelperWindow(BRect frame)
		: BWebWindow(frame, "OBS browser source", B_NO_BORDER_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
			     B_AVOID_FOCUS | B_NOT_MOVABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE |
				     B_NO_WORKSPACE_ACTIVATION | B_ASYNCHRONOUS_CONTROLS,
			     B_ALL_WORKSPACES)
	{
	}

	/* BWebWindow only forwards these for the main frame (FrameLoaderClientHaiku drops child-frame load
	 * messages); LoadFinished is LOAD_DOC_COMPLETED, which also comes for in-page navigation (fragments) */
	void LoadCommitted(const BString &, BWebView *) override
	{
		BMessage msg(MSG_INJECT);
		msg.AddBool("committed", true);
		be_app->PostMessage(&msg);
	}
	void LoadFinished(const BString &, BWebView *) override
	{
		BMessage msg(MSG_INJECT);
		msg.AddBool("finished", true);
		be_app->PostMessage(&msg);
	}

	/* Both run on the window thread, which holds the window lock: CurrentWebView() is safe to use */
	void TitleChanged(const BString &title, BWebView *) override
	{
		this->title = title;
		if (CurrentWebView())
			url = CurrentWebView()->MainFrameURL();
		send_info(this->title, url);
	}

	void NavigationRequested(const BString &url, BWebView *view) override
	{
		BWebWindow::NavigationRequested(url, view);
		this->url = url;
		send_info(title, this->url);
	}

	/* popups open in the same view rather than in new windows nobody can see */
	void NewWindowRequested(const BString &url, bool) override
	{
		if (CurrentWebView())
			CurrentWebView()->LoadURL(url.String(), false);
	}

	void CloseWindowRequested(BWebView *) override {}
	void ResizeRequested(float, float, BWebView *) override {}

	bool AuthenticationChallenge(BString, BString &, BString &, bool &, uint32, BWebView *) override
	{
		return false; /* no UI to ask; fail the challenge */
	}

	void MessageReceived(BMessage *message) override
	{
		if (message->what == JAVASCRIPT_WINDOW_OBJECT_CLEARED) {
			be_app->PostMessage(MSG_INJECT);
			return;
		}
		BWebWindow::MessageReceived(message);
	}

	bool QuitRequested() override
	{
		be_app->PostMessage(B_QUIT_REQUESTED);
		return false; /* the application closes us */
	}

private:
	BString title, url;
};

class HelperApp : public BApplication {
public:
	/* storage: the resolved storage directory (see resolve_storage_dir()); empty if there is none */
	HelperApp(int wake_fd, const std::string &storage)
		: BApplication(OBS_BROWSER_HELPER_SIGNATURE),
		  wake_fd(wake_fd),
		  storage(storage)
	{
	}

	void ReadyToRun() override
	{
		BWebPage::InitializeOnce();
		BWebPage::SetCacheModel(B_WEBKIT_CACHE_MODEL_WEB_BROWSER);

		if (!storage.empty())
			BWebSettings::SetPersistentStoragePath(storage.c_str());

		window = new HelperWindow(BRect(PARK_X, PARK_Y, PARK_X + width - 1, PARK_Y + height - 1));
		view = new HelperView();
		window->AddChild(view);
		window->SetCurrentWebView(view);
		view->ResizeTo(width - 1, height - 1);
		window->Show();
		/* the window avoids focus and is never activated by the app_server; tell the page it is active so
		 * carets, focus rings and keyboard input behave as in a focused browser */
		if (window->Lock()) {
			view->WindowActivated(true);
			window->Unlock();
		}

		channel->Send(OBS_BROWSER_EVENT_HELLO, OBS_BROWSER_PROTOCOL);

		pump = spawn_thread(PumpEntry, "obs browser frame pump", B_DISPLAY_PRIORITY, this);
		resume_thread(pump);
	}

	bool QuitRequested() override
	{
		quitting = true;
		if (pump >= 0) {
			status_t ret;
			wait_for_thread(pump, &ret);
			pump = -1;
		}
		if (window && window->Lock()) {
			if (view)
				view->Shutdown();
			window->Quit();
			window = nullptr;
		}
		BWebPage::ShutdownOnce();
		if (buffer) {
			munmap(buffer, buffer_size);
			buffer = nullptr;
		}
		return true;
	}

	void MessageReceived(BMessage *message) override
	{
		switch (message->what) {
		case OBS_BROWSER_MSG_CONFIG:
			ApplyConfig(message);
			return;
		case OBS_BROWSER_MSG_PING:
			/* answered here, on WebCore's main thread: a page stuck in a script delays it */
			channel->Send(OBS_BROWSER_EVENT_PONG, message->GetInt32("serial", 0));
			return;
		case OBS_BROWSER_MSG_RELOAD:
			Reload(message->GetBool("nocache", false));
			return;
		case OBS_BROWSER_MSG_EXECUTE_JS: {
			const char *script;
			if (message->FindString("script", &script) == B_OK)
				RunScript(script);
			return;
		}
		case OBS_BROWSER_MSG_STARTUP_SCRIPT: {
			const char *script;
			if (message->FindString("startup", &script) == B_OK)
				startup_script = script;
			return;
		}
		case OBS_BROWSER_MSG_VISIBILITY:
			Visibility(message);
			return;
		case OBS_BROWSER_MSG_MOUSE_DOWN:
		case OBS_BROWSER_MSG_MOUSE_UP:
		case OBS_BROWSER_MSG_MOUSE_MOVE:
		case OBS_BROWSER_MSG_MOUSE_WHEEL:
			Mouse(message);
			return;
		case OBS_BROWSER_MSG_KEY_DOWN:
		case OBS_BROWSER_MSG_KEY_UP:
			Key(message);
			return;
		case OBS_BROWSER_MSG_FOCUS:
			if (view && window->Lock()) {
				const bool focus = message->GetBool("focus", true);
				view->MakeFocus(focus);
				view->WindowActivated(true);
				window->Unlock();
			}
			return;
		case OBS_BROWSER_MSG_ZOOM:
			if (view && window->Lock()) {
				const int32 dir = message->GetInt32("direction", 0);
				if (dir > 0)
					view->IncreaseZoomFactor(false);
				else if (dir < 0)
					view->DecreaseZoomFactor(false);
				else
					view->ResetZoomFactor();
				window->Unlock();
			}
			return;
		case MSG_INJECT:
			if (message->GetBool("committed", false))
				startup_pending = true;
			Inject();
			if (message->GetBool("finished", false) && startup_pending) {
				startup_pending = false;
				if (!startup_script.empty())
					RunScript(startup_script.c_str());
			}
			return;
		}
		BApplication::MessageReceived(message);
	}

	void DispatchMessage(BMessage *message, BHandler *handler) override
	{
		/* the message is gone after dispatching (BWebPage coalesces queued HANDLE_DRAW messages) */
		const bool may_paint = message->what != OBS_BROWSER_MSG_STARTUP_SCRIPT && message->what != OBS_BROWSER_MSG_PING;
		BApplication::DispatchMessage(message, handler);
		if (may_paint)
			paint_serial++;
	}

private:
	static status_t PumpEntry(void *data)
	{
		static_cast<HelperApp *>(data)->Pump();
		return B_OK;
	}

	void ApplyConfig(BMessage *message)
	{
		const int32 w = message->GetInt32("width", width);
		const int32 h = message->GetInt32("height", height);
		int32 rate = message->GetInt32("fps", fps.load());
		if (rate < 1)
			rate = 1;
		if (rate > 120)
			rate = 120;
		fps = rate;
		transparent = message->GetBool("transparent", transparent);
		css = message->GetString("css", css.c_str());
		const char *url = message->GetString("url", "");

		if (!window || !window->Lock())
			return;

		if (w > 0 && h > 0 && (w != width || h != height)) {
			width = w;
			height = h;
			window->ResizeTo(width - 1, height - 1);
			view->ResizeTo(width - 1, height - 1);
		}

		if (*url && current_url != url) {
			current_url = url;
			view->LoadURL(url, false);
		}
		window->Unlock();

		Inject(); /* transparency and CSS may have changed */
	}

	/* HaikuWebKit has no cache-bypassing reload (BWebPage/BWebFrame::Reload() is a normal reload that
	 * revalidates) and no call that clears the cache. Dropping the memory cache to zero capacity and back
	 * evicts what no page uses any more; resources the current page still holds are revalidated. */
	void Reload(bool nocache)
	{
		if (nocache) {
			if (!warned_nocache) {
				LOG("HaikuWebKit cannot bypass its cache; the page reloads normally (unused cached resources "
				    "are dropped, the rest is revalidated)\n");
				warned_nocache = true;
			}
			BWebPage::SetCacheModel(B_WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
			BWebPage::SetCacheModel(B_WEBKIT_CACHE_MODEL_WEB_BROWSER);
		}
		if (view && window->Lock()) {
			view->Reload();
			window->Unlock();
		}
	}

	void RunScript(const char *script)
	{
		BWebFrame *frame = (view && view->WebPage()) ? view->WebPage()->MainFrame() : nullptr;
		if (!frame)
			return;
		JSGlobalContextRef ctx = frame->GlobalContext();
		if (!ctx)
			return;
		JSStringRef source = JSStringCreateWithUTF8CString(script);
		JSValueRef exception = nullptr;
		JSEvaluateScript(ctx, source, nullptr, nullptr, 1, &exception);
		JSStringRelease(source);
	}

	/* Runs on the application thread, which is WebCore's main thread (BWebPage::InitializeOnce attaches the
	 * WebCore run loop to be_app). Idempotent, so it runs whenever a window object may be new and after every
	 * configuration; the startup script runs once per main-frame load instead (MSG_INJECT). */
	void Inject()
	{
		BWebFrame *frame = (view && view->WebPage()) ? view->WebPage()->MainFrame() : nullptr;
		if (!frame)
			return;

		frame->SetTransparent(transparent);
		RunScript(kObsStudioObject);

		if (!css.empty()) {
			std::string escaped;
			escape_js_string(css, escaped);
			std::string js = "(function(){var d=document;if(!d||!d.documentElement)return;"
					 "var s=d.getElementById('__obs_haiku_css');"
					 "if(!s){s=d.createElement('style');s.id='__obs_haiku_css';"
					 "(d.head||d.documentElement).appendChild(s);}s.textContent='" +
					 escaped + "';})();";
			RunScript(js.c_str());
		}
	}

	/* the same events and payloads as obs-browser: detail is {"visible":bool} / {"active":bool} */
	void Visibility(BMessage *message)
	{
		bool value;
		if (message->FindBool("visible", &value) == B_OK) {
			std::string js = std::string("window.dispatchEvent(new CustomEvent('obsSourceVisibleChanged',"
						     "{detail:{visible:") +
					 (value ? "true" : "false") + "}}));";
			RunScript(js.c_str());
		}
		if (message->FindBool("active", &value) == B_OK) {
			std::string js = std::string("window.dispatchEvent(new CustomEvent('obsSourceActiveChanged',"
						     "{detail:{active:") +
					 (value ? "true" : "false") + "}}));";
			RunScript(js.c_str());
		}
	}

	/* Input goes straight to the BWebPage handler (it lives in this looper). BWebView's own hooks read the
	 * window's current message and, for the wheel, the real pointer position, neither of which exists for
	 * synthetic events. BWebPage inherits BHandler privately; the C-style cast is the one conversion C++
	 * allows to an inaccessible base. */
	BHandler *PageHandler() const
	{
		return (view && view->WebPage()) ? (BHandler *)view->WebPage() : nullptr;
	}

	void Mouse(BMessage *in)
	{
		BHandler *page = PageHandler();
		if (!page)
			return;

		const BPoint where(in->GetInt32("x", 0), in->GetInt32("y", 0));
		const BPoint screen(where.x + PARK_X, where.y + PARK_Y);
		const int32 modifiers = in->GetInt32("modifiers", 0);

		/* A secondary-button press would make BWebPage open its native context menu, synchronously, on this
		 * thread (WebCore's main thread), at a position on the real screen. Deliver the DOM events instead. */
		const int32 pressed = in->GetInt32("buttons", B_PRIMARY_MOUSE_BUTTON);
		if ((in->what == OBS_BROWSER_MSG_MOUSE_DOWN || in->what == OBS_BROWSER_MSG_MOUSE_UP) &&
		    (pressed & B_SECONDARY_MOUSE_BUTTON)) {
			char js[512];
			snprintf(js, sizeof(js),
				 "(function(x,y){var e=document.elementFromPoint(x,y)||document.body;if(!e)return;"
				 "function f(t){e.dispatchEvent(new MouseEvent(t,{bubbles:true,cancelable:true,"
				 "clientX:x,clientY:y,button:2,buttons:%d}));}%s})(%d,%d);",
				 in->what == OBS_BROWSER_MSG_MOUSE_DOWN ? 2 : 0,
				 in->what == OBS_BROWSER_MSG_MOUSE_DOWN ? "f('mousedown');"
									: "f('mouseup');f('contextmenu');",
				 (int)where.x, (int)where.y);
			RunScript(js);
			return;
		}

		BMessage out;
		switch (in->what) {
		case OBS_BROWSER_MSG_MOUSE_DOWN: {
			out.what = B_MOUSE_DOWN;
			buttons |= pressed;
			out.AddInt32("buttons", buttons);
			out.AddInt32("clicks", in->GetInt32("clicks", 1));
			if (view && window->Lock()) {
				view->MakeFocus(true);
				window->Unlock();
			}
			break;
		}
		case OBS_BROWSER_MSG_MOUSE_UP: {
			out.what = B_MOUSE_UP;
			out.AddInt32("previous buttons", pressed);
			buttons &= ~pressed;
			out.AddInt32("buttons", buttons);
			out.AddInt32("clicks", in->GetInt32("clicks", 1));
			break;
		}
		case OBS_BROWSER_MSG_MOUSE_MOVE:
			out.what = B_MOUSE_MOVED;
			out.AddInt32("buttons", buttons);
			out.AddInt32("be:transit", in->GetBool("leave", false) ? B_EXITED_VIEW : B_INSIDE_VIEW);
			break;
		case OBS_BROWSER_MSG_MOUSE_WHEEL:
			out.what = B_MOUSE_WHEEL_CHANGED;
			out.AddFloat("be:wheel_delta_x", in->GetFloat("dx", 0.0f));
			out.AddFloat("be:wheel_delta_y", in->GetFloat("dy", 0.0f));
			break;
		default:
			return;
		}

		out.AddInt64("when", system_time());
		out.AddPoint("where", where);
		out.AddPoint("be:view_where", where);
		out.AddPoint("screen_where", screen);
		out.AddInt32("modifiers", modifiers);
		out.AddPointer("target", view->WebPage());
		PostMessage(&out, page);
	}

	void Key(BMessage *in)
	{
		BHandler *page = PageHandler();
		if (!page)
			return;

		const char *bytes = in->GetString("bytes", "");
		if (!*bytes)
			return; /* WebCore derives everything from the bytes */

		BMessage out(in->what == OBS_BROWSER_MSG_KEY_DOWN ? B_KEY_DOWN : B_KEY_UP);
		out.AddInt64("when", system_time());
		out.AddString("bytes", bytes);
		for (const char *b = bytes; *b; b++)
			out.AddInt8("byte", *b);
		out.AddInt32("key", in->GetInt32("key", 0));
		out.AddInt32("raw_char", (uint8)bytes[0]);
		out.AddInt32("modifiers", in->GetInt32("modifiers", 0));
		PostMessage(&out, page);
	}

	void Pump()
	{
		bigtime_t next = system_time();
		bool warned_alpha = false;
		uint32 copied_serial = 0; /* paint_serial starts at 1: the first tick always copies */

		while (!quitting) {
			const bigtime_t now = system_time();
			const int32 rate = fps.load();
			const bigtime_t interval = 1000000 / (rate > 0 ? rate : 30);
			next += interval;
			if (next < now)
				next = now + interval; /* we fell behind; don't try to catch up */

			/* Read the serial before getOffscreen(), which syncs the offscreen view: anything drawn after
			 * this read bumps it again and is copied on the next tick. */
			const uint32 serial = paint_serial.load();
			if (serial != copied_serial && view->offscreen_ready && window->LockWithTimeout(interval) == B_OK) {
				BBitmap *bitmap = WebCore::DumpRenderTreeClient::getOffscreen(view);
				if (bitmap && bitmap->Lock()) {
					if (CopyFrame(bitmap, warned_alpha))
						copied_serial = serial;
					bitmap->Unlock();
				}
				window->Unlock();
			}

			const bigtime_t wait = next - system_time();
			if (wait > 0)
				snooze(wait);
		}
	}

	/*
	 * Makes sure the frame buffer can hold `bytes` of pixels, replacing it (and telling OBS) if not. A buffer is
	 * a memfd (Renga: libgnu memfd_create(), 3rdparty/ce/PORTING.md "memfd_create"), sealed once it is sized:
	 * F_SEAL_SHRINK | F_SEAL_GROW so that no mapping of it can ever fault, F_SEAL_FUTURE_WRITE so that OBS
	 * (and anyone it might leak to) can only map it read-only while our own writable mapping keeps working
	 * (memfd_test.cpp "future_write"), and F_SEAL_SEAL. Buffers grow with headroom and shrink only when a
	 * quarter would do, so resizing a source does not make one per step.
	 */
	bool EnsureBuffer(size_t bytes)
	{
		const size_t needed = OBS_BROWSER_FRAME_HEADER_SIZE + bytes;
		if (buffer && needed <= buffer_size && needed * 4 > buffer_size)
			return true;

		const size_t size = (needed + needed / 4 + 65535) & ~(size_t)65535;
		const int fd = memfd_create("obs-browser-frames", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (fd < 0) {
			LOG("memfd_create failed: %s\n", strerror(errno));
			return false;
		}
		void *address = MAP_FAILED;
		if (ftruncate(fd, (off_t)size) == 0)
			address = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (address == MAP_FAILED ||
		    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL) != 0) {
			LOG("cannot set up a %zu byte frame buffer: %s\n", size, strerror(errno));
			if (address != MAP_FAILED)
				munmap(address, size);
			close(fd);
			return false;
		}

		obs_browser_frame_header *header = static_cast<obs_browser_frame_header *>(address);
		header->magic = OBS_BROWSER_FRAME_MAGIC; /* the rest is zero: ftruncate() fills with zeros */

		const bool sent = channel->Send(OBS_BROWSER_EVENT_BUFFER, 0, std::string(), fd);
		close(fd); /* our mapping keeps the file; OBS has its own descriptor now */
		if (!sent) {
			munmap(address, size);
			return false; /* OBS is gone; the socket reader is about to quit */
		}

		if (buffer)
			munmap(buffer, buffer_size);
		buffer = static_cast<uint8_t *>(address);
		buffer_size = size;
		return true;
	}

	/* false if nothing was published (and the same picture has to be copied again later) */
	bool CopyFrame(BBitmap *bitmap, bool &warned_alpha)
	{
		const BRect bounds = bitmap->Bounds();
		int32 w = (int32)bounds.IntegerWidth() + 1;
		int32 h = (int32)bounds.IntegerHeight() + 1;
		/* the offscreen bitmap only grows; the visible page is the view size (`width`/`height` are written on
		 * the application thread with the window locked, and we hold that lock) */
		if (w > width)
			w = width;
		if (h > height)
			h = height;
		if (w <= 0 || h <= 0)
			return false;

		const color_space cs = bitmap->ColorSpace();
		if (cs != B_RGB32 && cs != B_RGBA32)
			return false;

		const uint32 dst_stride = (uint32)w * 4;
		if (!EnsureBuffer((size_t)dst_stride * (size_t)h))
			return false;

		obs_browser_frame_header *header = reinterpret_cast<obs_browser_frame_header *>(buffer);
		const uint8 *src = (const uint8 *)bitmap->Bits();
		const int32 src_stride = bitmap->BytesPerRow();
		uint8 *dst = buffer + OBS_BROWSER_FRAME_HEADER_SIZE;

		/* sequence lock: odd while writing (the release fences order the pixel stores against it) */
		__atomic_store_n(&header->seq, header->seq + 1, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_RELEASE);

		bool any_alpha = false;
		for (int32 y = 0; y < h; y++) {
			const uint8 *row = src + (size_t)y * src_stride;
			uint8 *out = dst + (size_t)y * dst_stride;
			memcpy(out, row, dst_stride);
			if (transparent && !any_alpha) {
				for (int32 x = 3; x < (int32)dst_stride; x += 4)
					if (out[x]) {
						any_alpha = true;
						break;
					}
			}
		}

		/* B_RGB32 does not promise a meaningful alpha channel. If a transparent page comes back with
		 * alpha 0 everywhere the channel was not written at all; show the page opaque instead of
		 * invisible. */
		const bool has_alpha = transparent && any_alpha;
		if (transparent && !any_alpha && !warned_alpha) {
			LOG("page rendered without an alpha channel; showing it opaque\n");
			warned_alpha = true;
		}

		header->width = (uint32_t)w;
		header->height = (uint32_t)h;
		header->stride = dst_stride;
		header->has_alpha = has_alpha ? 1 : 0;
		header->frame_count++;
		__atomic_store_n(&header->seq, header->seq + 1, __ATOMIC_RELEASE); /* even: complete */

		/* Renga eventfd: wakes OBS's reader; the counter coalesces and never blocks us (non-blocking) */
		eventfd_write(wake_fd, 1);
		return true;
	}

	const int wake_fd;
	std::string storage;

	/* the current frame memfd's writable mapping; pump thread only (and QuitRequested() after it ended) */
	uint8_t *buffer = nullptr;
	size_t buffer_size = 0;

	HelperWindow *window = nullptr;
	HelperView *view = nullptr;
	thread_id pump = -1;
	std::atomic_bool quitting{false};

	int32 width = 800;
	int32 height = 600;
	std::atomic<int32> fps{30};
	bool transparent = true;
	std::string css;
	std::string current_url;
	std::string startup_script;
	bool startup_pending = false; /* a main-frame load committed and its startup script has not run yet */
	bool warned_nocache = false;
	int32 buttons = 0;
};

/* --storage <dir> (a browser dock's cookie profile, from the QCef cookie manager) or, for browser sources, one
 * directory shared by all of them. Created if needed; empty if it cannot be. */
static std::string resolve_storage_dir(const char *storage)
{
	BPath path;
	if (storage && *storage)
		path.SetTo(storage);
	else if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK)
		path.Append("obs-studio/plugin_config/obs-browser/haiku-webkit");
	if (path.InitCheck() != B_OK || create_directory(path.Path(), 0700) != B_OK)
		return std::string();
	return path.Path();
}

static bool curl_backend_loaded()
{
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		const char *leaf = strrchr(info.name, '/');
		if (!strncmp(leaf ? leaf + 1 : info.name, "libcurl", 7))
			return true;
	}
	return false;
}

/*
 * WebCore's CookieJarDB opens the shared database without a busy timeout, and treats any failure while opening
 * it (including SQLITE_BUSY from "PRAGMA journal_mode=WAL" or the quick_check while another helper or the
 * panel's cookie manager holds a lock) as corruption: it deletes the database and every login with it
 * (CookieJarDB::openDatabase() -> deleteAllDatabaseFiles()). Several helpers start together when OBS starts.
 *
 * sqlite3_auto_extension() registers an entry point that SQLite calls for every connection opened afterwards in
 * this process, at the end of sqlite3_open_v2() and so before WebKit's first statement. It is registered in the
 * library instance WebKit links: dlopen() of the soname WebKit already loaded returns that same image.
 */
namespace sqlite_busy {

struct sqlite3;
typedef int (*busy_timeout_fn)(sqlite3 *, int);
typedef int (*auto_extension_fn)(void (*)(void));

constexpr int kTimeoutMs = 5000;
busy_timeout_fn busy_timeout = nullptr;

/* the documented entry point signature: int xEntryPoint(sqlite3 *, char **pzErrMsg,
 * const sqlite3_api_routines *); a non-zero return would make the open fail */
extern "C" int entry_point(sqlite3 *db, char **, const void *)
{
	if (busy_timeout)
		busy_timeout(db, kTimeoutMs);
	return 0; /* SQLITE_OK */
}

static bool already_loaded()
{
	image_info info;
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
		const char *leaf = strrchr(info.name, '/');
		if (!strncmp(leaf ? leaf + 1 : info.name, "libsqlite3", 10))
			return true;
	}
	return false;
}

static void install()
{
	/* the same names browser-panel.cpp resolves */
	const bool loaded = already_loaded();
	void *lib = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_GLOBAL);
	if (!lib)
		lib = dlopen("libsqlite3.so", RTLD_NOW | RTLD_GLOBAL);
	if (!lib) {
		LOG("libsqlite3 is not available; the cookie database is opened without a busy timeout\n");
		return;
	}
	if (!loaded)
		LOG("libsqlite3 was not loaded with HaikuWebKit; if WebKit has its own copy, the cookie database "
		    "is opened without a busy timeout\n");
	busy_timeout = reinterpret_cast<busy_timeout_fn>(dlsym(lib, "sqlite3_busy_timeout"));
	auto auto_extension = reinterpret_cast<auto_extension_fn>(dlsym(lib, "sqlite3_auto_extension"));
	if (!busy_timeout || !auto_extension ||
	    auto_extension(reinterpret_cast<void (*)(void)>(entry_point)) != 0) {
		busy_timeout = nullptr;
		LOG("libsqlite3 cannot register a busy timeout; the cookie database is opened without one\n");
	}
	/* the library stays loaded: the entry point is called for as long as the process lives */
}

} // namespace sqlite_busy

/*
 * Persistent, shared cookies. HaikuWebKit (1.9.19 and 1.10 as packaged) uses WebCore's curl network backend;
 * its cookie store is an SQLite database (CookieJarDB) at $CURL_COOKIE_JAR_PATH, or "cookie.jar.db" in the
 * current directory without it (WebCore's NetworkStorageSessionCurl.cpp; WebPositive sets the same variable).
 * Every request and every document.cookie access queries that database, and SQLite (WAL journal) arbitrates
 * between processes, so all helpers given the same storage directory share one live cookie jar: a login in
 * one dock is visible to the others on their next request. The BUrlContext/BNetworkCookieJar a BWebView can
 * be given is not consulted by curl builds (FrameNetworkingContextHaiku ignores it under USE(CURL)).
 *
 * Must run before WebKit is initialized: the storage session reads the variable when it is first created.
 */
static void setup_cookie_store(const std::string &dir)
{
	/* The database holds login tokens; so do local storage and the cache. Nothing this process creates is
	 * meant for other users: SQLite creates files 0644 & ~umask and gives -wal/-shm the database's mode. */
	umask(077);

	if (dir.empty() || access(dir.c_str(), W_OK) != 0) {
		/* the in-memory jar of earlier versions, never a stray file in the current directory */
		LOG("no usable storage directory; cookies are kept in memory for this page only\n");
		setenv("CURL_COOKIE_JAR_PATH", ":memory:", 1);
		return;
	}

	/* tighten what earlier versions or another umask left behind (best effort: we may not own it) */
	chmod(dir.c_str(), 0700);
	const std::string db = dir + "/" OBS_BROWSER_COOKIE_DB;
	for (const char *suffix : {"", "-wal", "-shm", "-journal"}) {
		const std::string file = db + suffix;
		if (chmod(file.c_str(), 0600) != 0 && errno != ENOENT)
			LOG("cannot restrict permissions of %s: %s\n", file.c_str(), strerror(errno));
	}

	/* before WebKit's storage session first opens the database (lazily, on the first cookie access of a
	 * load). Opening is not serialized across helpers with a lock: that first access happens inside WebCore,
	 * with no hook before or after it; the busy timeout makes concurrent opens wait instead of failing. */
	sqlite_busy::install();

	setenv("CURL_COOKIE_JAR_PATH", db.c_str(), 1);
	/* A WebKit that stops honouring the variable falls back to "cookie.jar.db" in the current directory,
	 * which is then the same file. The helper only uses absolute paths itself. */
	if (chdir(dir.c_str()) != 0)
		LOG("cannot change to %s: %s\n", dir.c_str(), strerror(errno));

	if (!curl_backend_loaded())
		LOG("libcurl is not loaded: if this HaikuWebKit does not use the curl network backend, cookies are "
		    "kept in memory per page and not persisted\n");
}

/*
 * Reads OBS's messages and posts them to the application. Its own thread, blocking in poll(): messages that
 * arrive before the BApplication runs wait in the socket. End-of-file means OBS closed our socket (it wants us
 * gone) or died: either way we quit. If the application thread does not get to it (a page stuck in a script),
 * the process exits after a grace period; OBS kills it a moment later if even that fails (browser-client.cpp,
 * REAP_GRACE).
 */
static status_t socket_reader(void *)
{
	BMessenger app(be_app);
	std::vector<uint8_t> rx;
	for (;;) {
		uint8_t chunk[16384];
		const ssize_t got = recv(channel->fd, chunk, sizeof(chunk), 0);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		rx.insert(rx.end(), chunk, chunk + got);

		size_t offset = 0;
		bool bad = false;
		while (rx.size() - offset >= sizeof(uint32_t)) {
			uint32_t length;
			memcpy(&length, rx.data() + offset, sizeof(length));
			if (length == 0 || length > OBS_BROWSER_MAX_MESSAGE) {
				bad = true;
				break;
			}
			if (rx.size() - offset - sizeof(length) < length)
				break;
			/* BLooper::PostMessage() gives up at once when the looper's port is full (it sends with a zero
			 * timeout); waiting here instead pushes back to OBS through the socket, which holds the rest. */
			BMessage message;
			if (message.Unflatten((const char *)rx.data() + offset + sizeof(length)) == B_OK)
				app.SendMessage(&message, (BHandler *)nullptr, B_INFINITE_TIMEOUT);
			offset += sizeof(length) + length;
		}
		if (bad) {
			LOG("invalid message from OBS\n");
			break;
		}
		rx.erase(rx.begin(), rx.begin() + offset);
	}

	BMessage quit(B_QUIT_REQUESTED);
	app.SendMessage(&quit, (BHandler *)nullptr, 1000000);
	snooze(2000000);
	LOG("did not quit in time, exiting\n");
	_exit(0);
	return B_OK;
}

static int descriptor_arg(const char *value)
{
	char *end = nullptr;
	const long fd = strtol(value, &end, 10);
	return (end && !*end && fd >= 0 && fd < 65536) ? (int)fd : -1;
}

int main(int argc, char **argv)
{
	int ipc_fd = -1, wake_fd = -1;
	const char *storage = nullptr;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--ipc") && i + 1 < argc)
			ipc_fd = descriptor_arg(argv[++i]);
		else if (!strcmp(argv[i], "--wake") && i + 1 < argc)
			wake_fd = descriptor_arg(argv[++i]);
		else if (!strcmp(argv[i], "--storage") && i + 1 < argc)
			storage = argv[++i];
	}

	struct stat st;
	if (ipc_fd < 0 || wake_fd < 0 || fstat(ipc_fd, &st) != 0 || !S_ISSOCK(st.st_mode)) {
		fprintf(stderr, "usage: %s --ipc <fd> --wake <fd> [--storage <dir>]\n", argv[0]);
		fprintf(stderr, "This helper is started by OBS Studio's browser source; it is not meant to be run by hand.\n");
		return 1;
	}
	/* Inherited without close-on-exec; nothing this process starts may keep OBS's socket open (OBS would not
	 * see us die) */
	fcntl(ipc_fd, F_SETFD, FD_CLOEXEC);
	fcntl(wake_fd, F_SETFD, FD_CLOEXEC);

	const std::string storage_dir = resolve_storage_dir(storage);
	setup_cookie_store(storage_dir);

	Channel connection(ipc_fd);
	channel = &connection;

	HelperApp app(wake_fd, storage_dir);
	const thread_id reader = spawn_thread(socket_reader, "obs browser ipc", B_NORMAL_PRIORITY, nullptr);
	resume_thread(reader);
	app.Run();
	/* the reader may still be blocked in recv(); exiting ends it with the process */
	return 0;
}
