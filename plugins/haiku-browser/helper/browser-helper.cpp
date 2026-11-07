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
 *   obs-haiku-browser --area <area_id> [--storage <dir>]
 *
 * One process per browser instance: a page that hangs or crashes WebKit only takes its own helper down, and
 * the OBS side restarts it. See ../browser-shm.h for the protocol.
 *
 * The page lives in a borderless window parked far outside the screen. It must stay shown, because hiding a
 * BWebView marks the page invisible and WebCore then stops timers and animations. HaikuWebKit renders the
 * page into the view's offscreen bitmap; a pump thread copies that bitmap into the shared area at the
 * requested frame rate.
 */

#include "../browser-shm.h"
#include "webkit-abi.h"

#include <Application.h>
#include <Bitmap.h>
#include <Directory.h>
#include <Locker.h>
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

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#define LOG(...) fprintf(stderr, "obs-haiku-browser: " __VA_ARGS__)

/* far enough outside any realistic desktop that the window never shows */
#define PARK_X -30000.0f
#define PARK_Y -30000.0f

enum {
	MSG_INJECT = 'inj_', /* window -> app: page (re)loaded, inject CSS / scripts */
};

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

	void LoadCommitted(const BString &, BWebView *) override { be_app->PostMessage(MSG_INJECT); }
	void LoadFinished(const BString &, BWebView *) override { be_app->PostMessage(MSG_INJECT); }

	void TitleChanged(const BString &title, BWebView *) override
	{
		if (!shm)
			return;
		strlcpy(shm->title, title.String(), sizeof(shm->title));
		if (CurrentWebView())
			strlcpy(shm->url, CurrentWebView()->MainFrameURL().String(), sizeof(shm->url));
		__sync_synchronize();
		atomic_add((int32 *)&shm->info_serial, 1);
	}

	void NavigationRequested(const BString &url, BWebView *view) override
	{
		BWebWindow::NavigationRequested(url, view);
		if (!shm)
			return;
		strlcpy(shm->url, url.String(), sizeof(shm->url));
		__sync_synchronize();
		atomic_add((int32 *)&shm->info_serial, 1);
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

	obs_browser_shm *shm = nullptr;
};

class HelperApp : public BApplication {
public:
	HelperApp(area_id source_area, const char *storage)
		: BApplication(OBS_BROWSER_HELPER_SIGNATURE),
		  source_area(source_area),
		  storage(storage ? storage : "")
	{
	}

	void ReadyToRun() override
	{
		if (!AttachArea(source_area)) {
			Quit();
			return;
		}

		BWebPage::InitializeOnce();
		BWebPage::SetCacheModel(B_WEBKIT_CACHE_MODEL_WEB_BROWSER);

		BPath path;
		if (!storage.empty()) {
			path.SetTo(storage.c_str());
		} else if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
			path.Append("obs-studio/plugin_config/obs-browser/haiku-webkit");
		}
		if (path.InitCheck() == B_OK) {
			create_directory(path.Path(), 0755);
			BWebSettings::SetPersistentStoragePath(path.Path());
		}

		window = new HelperWindow(BRect(PARK_X, PARK_Y, PARK_X + width - 1, PARK_Y + height - 1));
		window->shm = shm;
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

		shm->helper_team = (int32_t)Team();
		shm->helper_heartbeat = system_time();
		__sync_synchronize();
		shm->helper_state = OBS_BROWSER_STATE_READY;

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
		return true;
	}

	void MessageReceived(BMessage *message) override
	{
		switch (message->what) {
		case OBS_BROWSER_MSG_CONFIG:
			ApplyConfig(message);
			return;
		case OBS_BROWSER_MSG_ATTACH_AREA: {
			int32 area;
			if (message->FindInt32("area", &area) == B_OK)
				AttachArea(area);
			return;
		}
		case OBS_BROWSER_MSG_RELOAD:
			if (view && window->Lock()) {
				view->Reload();
				window->Unlock();
			}
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
			Inject();
			return;
		}
		BApplication::MessageReceived(message);
	}

private:
	static status_t PumpEntry(void *data)
	{
		static_cast<HelperApp *>(data)->Pump();
		return B_OK;
	}

	bool AttachArea(area_id source)
	{
		void *addr = nullptr;
		area_id clone = clone_area("obs browser frames", &addr, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA,
					   source);
		if (clone < 0) {
			LOG("cannot clone area %d: %s\n", (int)source, strerror(clone));
			return false;
		}
		obs_browser_shm *mapped = static_cast<obs_browser_shm *>(addr);
		if (mapped->magic != OBS_BROWSER_MAGIC || mapped->version != OBS_BROWSER_VERSION) {
			LOG("area %d is not an OBS browser area\n", (int)source);
			delete_area(clone);
			return false;
		}

		/* the pump reads `shm` without the app lock; swap under frame_lock */
		frame_lock.Lock();
		obs_browser_shm *old = shm;
		const area_id old_area = area;
		if (old) {
			mapped->helper_team = old->helper_team;
			mapped->helper_state = old->helper_state;
		}
		mapped->helper_heartbeat = system_time();
		mapped->helper_team = (int32_t)Team();
		shm = mapped;
		area = clone;
		if (window && window->Lock()) {
			window->shm = shm;
			window->Unlock();
		}
		frame_lock.Unlock();

		if (old_area >= 0)
			delete_area(old_area);
		return true;
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
	 * WebCore run loop to be_app). */
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

		if (!startup_script.empty())
			RunScript(startup_script.c_str());
	}

	void Visibility(BMessage *message)
	{
		bool value;
		if (message->FindBool("visible", &value) == B_OK) {
			std::string js = std::string("window.dispatchEvent(new CustomEvent('obsSourceVisibleChanged',"
						     "{detail:") +
					 (value ? "true" : "false") + "}));";
			RunScript(js.c_str());
		}
		if (message->FindBool("active", &value) == B_OK) {
			std::string js = std::string("window.dispatchEvent(new CustomEvent('obsSourceActiveChanged',"
						     "{detail:") +
					 (value ? "true" : "false") + "}));";
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

		while (!quitting) {
			const bigtime_t now = system_time();
			const int32 rate = fps.load();
			const bigtime_t interval = 1000000 / (rate > 0 ? rate : 30);
			next += interval;
			if (next < now)
				next = now + interval; /* we fell behind; don't try to catch up */

			frame_lock.Lock();
			obs_browser_shm *s = shm;
			s->helper_heartbeat = now;

			if (now - s->client_heartbeat > OBS_BROWSER_CLIENT_TIMEOUT) {
				frame_lock.Unlock();
				LOG("client went away, exiting\n");
				PostMessage(B_QUIT_REQUESTED);
				return;
			}

			if (window && view->offscreen_ready && window->LockWithTimeout(interval) == B_OK) {
				BBitmap *bitmap = WebCore::DumpRenderTreeClient::getOffscreen(view);
				if (bitmap && bitmap->Lock()) {
					CopyFrame(s, bitmap, warned_alpha);
					bitmap->Unlock();
				}
				window->Unlock();
			}
			frame_lock.Unlock();

			const bigtime_t wait = next - system_time();
			if (wait > 0)
				snooze(wait);
		}
	}

	void CopyFrame(obs_browser_shm *s, BBitmap *bitmap, bool &warned_alpha)
	{
		const BRect bounds = bitmap->Bounds();
		int32 w = (int32)bounds.IntegerWidth() + 1;
		int32 h = (int32)bounds.IntegerHeight() + 1;
		/* the offscreen bitmap only grows; the visible page is the view size */
		if (w > width)
			w = width;
		if (h > height)
			h = height;
		if (w <= 0 || h <= 0)
			return;

		const color_space cs = bitmap->ColorSpace();
		if (cs != B_RGB32 && cs != B_RGBA32)
			return;

		const uint32 dst_stride = (uint32)w * 4;
		if ((uint64)dst_stride * (uint64)h > s->capacity)
			return; /* OBS is about to hand us a bigger area */

		const uint8 *src = (const uint8 *)bitmap->Bits();
		const int32 src_stride = bitmap->BytesPerRow();
		uint8 *dst = obs_browser_pixels(s);

		atomic_add((int32 *)&s->seq, 1); /* odd: writing */
		__sync_synchronize();

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

		s->width = w;
		s->height = h;
		s->stride = (int32_t)dst_stride;
		s->has_alpha = has_alpha ? 1 : 0;
		s->frame_count++;
		__sync_synchronize();
		atomic_add((int32 *)&s->seq, 1); /* even: complete */
	}

	area_id source_area;
	std::string storage;

	area_id area = -1;
	obs_browser_shm *shm = nullptr;
	BLocker frame_lock{"obs browser frame lock"};

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
	int32 buttons = 0;
};

int main(int argc, char **argv)
{
	area_id area = -1;
	const char *storage = nullptr;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--area") && i + 1 < argc)
			area = (area_id)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--storage") && i + 1 < argc)
			storage = argv[++i];
	}

	if (area < 0) {
		fprintf(stderr, "usage: %s --area <area_id> [--storage <dir>]\n", argv[0]);
		fprintf(stderr, "This helper is started by OBS Studio's browser source; it is not meant to be run by hand.\n");
		return 1;
	}

	HelperApp app(area, storage);
	app.Run();
	return 0;
}
