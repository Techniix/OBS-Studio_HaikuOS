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
 * "browser_source" for Haiku: the same source id and settings as obs-browser, so scene collections move
 * between platforms unchanged, rendered by HaikuWebKit in the obs-haiku-browser helper.
 */

#include "browser-audio.hpp"
#include "browser-client.hpp"
#include "browser-input.hpp"

#include <obs-module.h>
#include <obs-interaction.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <atomic>
#include <ctype.h>
#include <string.h>
#include <memory>
#include <mutex>
#include <string>

#define T_(text) obs_module_text(text)

#define S_IS_LOCAL_FILE "is_local_file"
#define S_LOCAL_FILE "local_file"
#define S_URL "url"
#define S_WIDTH "width"
#define S_HEIGHT "height"
#define S_FPS_CUSTOM "fps_custom"
#define S_FPS "fps"
#define S_CSS "css"
#define S_SHUTDOWN "shutdown"
#define S_RESTART_ACTIVE "restart_when_active"
#define S_REROUTE_AUDIO "reroute_audio"
#define S_CONTROL_LEVEL "webpage_control_level"

/* obs-browser's ControlLevel::ReadObs, its default; the level window.obsstudio reports on Haiku */
#define CONTROL_LEVEL_READ_OBS 1

/* the obs-browser default */
#define DEFAULT_CSS "body { background-color: rgba(0, 0, 0, 0); margin: 0px auto; overflow: hidden; }"

namespace {

struct BrowserSource {
	obs_source_t *source = nullptr;
	std::unique_ptr<HaikuBrowserClient> client;
	std::atomic_bool shutdown_when_hidden{false};
	std::atomic_bool restart_when_active{false};
	std::atomic_bool showing{false};

	/* "Control audio via OBS": present while reroute_audio is on. Created and destroyed in browser_update()
	 * (UI thread), told the helper's team from the client's I/O thread; neither waits for the media server
	 * (browser-audio.hpp). */
	std::mutex audio_mutex;
	std::unique_ptr<BrowserAudioReroute> audio;

	void OutputFrame(const HaikuBrowserFrame &frame);
	void HelperStarted(pid_t team);
	void SetRerouteAudio(bool on);
};

std::string file_url(const char *path)
{
	/* file:// plus a percent-encoded absolute path */
	std::string url = "file://";
	static const char hex[] = "0123456789ABCDEF";
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		if (isalnum(*p) || strchr("/-_.~", *p)) {
			url += (char)*p;
		} else {
			url += '%';
			url += hex[*p >> 4];
			url += hex[*p & 15];
		}
	}
	return url;
}

/* On the client's I/O thread, when the helper published a new frame: OBS keeps showing the last one, so a page
 * that did not repaint costs no copy and no upload. obs_source_output_video() copies the frame. */
void BrowserSource::OutputFrame(const HaikuBrowserFrame &frame)
{
	struct obs_source_frame out = {};
	out.data[0] = const_cast<uint8_t *>(frame.pixels);
	out.linesize[0] = frame.stride;
	out.width = frame.width;
	out.height = frame.height;
	out.format = frame.has_alpha ? VIDEO_FORMAT_BGRA : VIDEO_FORMAT_BGRX;
	out.timestamp = os_gettime_ns();
	out.full_range = true;
	video_format_get_parameters_for_format(VIDEO_CS_SRGB, VIDEO_RANGE_FULL, out.format, out.color_matrix,
					       out.color_range_min, out.color_range_max);
	obs_source_output_video(source, &out);
}

/* On the client's I/O thread, for every helper started */
void BrowserSource::HelperStarted(pid_t team)
{
	std::lock_guard<std::mutex> lock(audio_mutex);
	if (audio)
		audio->SetHelperTeam(team);
}

void BrowserSource::SetRerouteAudio(bool on)
{
	{
		std::lock_guard<std::mutex> lock(audio_mutex);
		if (on && !audio) {
			/* No team yet: a helper that is running has the other signature and is replaced (the
			 * client restarts it whenever reroute_audio changes, and starts the first one only after
			 * this); HelperStarted() passes the new one's team. */
			audio = std::make_unique<BrowserAudioReroute>(source);
		} else if (!on) {
			audio.reset(); /* returns at once; its thread releases the loopback */
		}
	}
	/* as obs-browser: the source is listed in the audio mixer only while it has audio */
	obs_source_set_audio_active(source, on);
}

const char *browser_get_name(void *)
{
	return T_("BrowserSource");
}

void browser_update(void *data, obs_data_t *settings)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);

	HaikuBrowserConfig cfg;
	if (obs_data_get_bool(settings, S_IS_LOCAL_FILE)) {
		const char *path = obs_data_get_string(settings, S_LOCAL_FILE);
		cfg.url = *path ? file_url(path) : std::string("about:blank");
	} else {
		cfg.url = obs_data_get_string(settings, S_URL);
		if (cfg.url.empty())
			cfg.url = "about:blank";
	}
	cfg.width = (int)obs_data_get_int(settings, S_WIDTH);
	cfg.height = (int)obs_data_get_int(settings, S_HEIGHT);
	if (obs_data_get_bool(settings, S_FPS_CUSTOM)) {
		cfg.fps = (int)obs_data_get_int(settings, S_FPS);
	} else {
		struct obs_video_info ovi;
		cfg.fps = obs_get_video_info(&ovi) && ovi.fps_den ? (int)(ovi.fps_num / ovi.fps_den) : 30;
	}
	cfg.css = obs_data_get_string(settings, S_CSS);
	cfg.transparent = true;
	cfg.reroute_audio = obs_data_get_bool(settings, S_REROUTE_AUDIO);
	/* before Configure(): the loopback must exist when the helper that Configure() restarts reports its team */
	bs->SetRerouteAudio(cfg.reroute_audio);

	const bool was_shutdown = bs->shutdown_when_hidden;
	bs->shutdown_when_hidden = obs_data_get_bool(settings, S_SHUTDOWN);
	bs->restart_when_active = obs_data_get_bool(settings, S_RESTART_ACTIVE);

	if (bs->shutdown_when_hidden && !bs->showing) {
		/* also when the option was just turned on for a hidden source; it starts when it is shown, with this
		 * configuration. Stop() first, so that no helper is started for it now, and Stop() returns after the
		 * last frame callback, so the cleared frame stays cleared. */
		bs->client->Stop();
		bs->client->Configure(cfg);
		if (!was_shutdown)
			obs_source_output_video(bs->source, nullptr);
		return;
	}

	bs->client->Configure(cfg);
	/* no-op unless it was stopped, e.g. shut down while hidden before the option was turned off */
	bs->client->Start();
}

void *browser_create(obs_data_t *settings, obs_source_t *source)
{
	BrowserSource *bs = new BrowserSource;
	bs->source = source;
	/*
	 * Show each frame at the next render instead of pacing frames by their timestamps, as the capture sources
	 * do (plugins/haiku-capture). Only frames the page repainted are output (OutputFrame()), and libobs's
	 * pacing (ready_async_frame() in obs-source.c) only moves its clock on (last_frame_ts += sys_offset) while
	 * frames are queued: after a static page, the first frame of a change and every frame after it would be
	 * shown late by the length of the pause (up to MAX_TS_VAR, 2 s, after which it resynchronises). Frames
	 * are stamped when the I/O thread has copied them out of the helper's buffer (OutputFrame()), not with a
	 * time of their own, so buffering has nothing to smooth out here.
	 */
	obs_source_set_async_unbuffered(source, true);
	HaikuBrowserClient::Callbacks callbacks;
	callbacks.frame = [bs](const HaikuBrowserFrame &frame) { bs->OutputFrame(frame); };
	callbacks.started = [bs](pid_t team) { bs->HelperStarted(team); };
	bs->client = std::make_unique<HaikuBrowserClient>(obs_source_get_name(source), std::move(callbacks));
	browser_update(bs, settings);
	return bs;
}

void browser_destroy(void *data)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	bs->client.reset(); /* joins its I/O thread: no frame or started callback runs after this */
	bs->audio.reset();  /* no audio is output after this; the loopback is released by its own thread */
	delete bs;
}

void browser_show(void *data)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	bs->showing = true;
	if (bs->shutdown_when_hidden)
		bs->client->Start(); /* with the configuration last given to Configure() */
	bs->client->SetVisibility(true);
}

void browser_hide(void *data)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	bs->showing = false;
	bs->client->SetVisibility(false);
	if (bs->shutdown_when_hidden) {
		bs->client->Stop();
		obs_source_output_video(bs->source, nullptr);
	}
}

void browser_activate(void *data)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	if (bs->restart_when_active)
		bs->client->Reload(false);
	bs->client->SetActive(true);
}

void browser_deactivate(void *data)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	bs->client->SetActive(false);
}

int32_t buttons_for(int32_t type)
{
	switch (type) {
	case MOUSE_LEFT:
		return B_PRIMARY_MOUSE_BUTTON;
	case MOUSE_MIDDLE:
		return B_TERTIARY_MOUSE_BUTTON;
	case MOUSE_RIGHT:
		return B_SECONDARY_MOUSE_BUTTON;
	default:
		return B_PRIMARY_MOUSE_BUTTON;
	}
}

void browser_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			 uint32_t click_count)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	const int32 mods = haiku_modifiers_from_interact(event->modifiers);
	if (mouse_up)
		bs->client->MouseUp(event->x, event->y, buttons_for(type), (int32)click_count, mods);
	else
		bs->client->MouseDown(event->x, event->y, buttons_for(type), (int32)click_count, mods);
}

void browser_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	bs->client->MouseMove(event->x, event->y, haiku_modifiers_from_interact(event->modifiers), mouse_leave);
}

void browser_mouse_wheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	/* OBS passes Qt angle deltas (120 per notch, positive = up/left); Haiku wheel steps are positive down */
	bs->client->MouseWheel(-x_delta / 120.0f, -y_delta / 120.0f, haiku_modifiers_from_interact(event->modifiers));
}

void browser_focus(void *data, bool focus)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	bs->client->Focus(focus);
}

void browser_key_click(void *data, const struct obs_key_event *event, bool key_up)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	/* the Haiku frontend puts the Haiku raw key code in native_vkey (OBSBasicInteraction.cpp) */
	const std::string bytes = haiku_key_bytes((int32_t)event->native_vkey, event->text);
	bs->client->Key(!key_up, bytes, (int32_t)event->native_vkey, haiku_modifiers_from_interact(event->modifiers));
}

bool refresh_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	BrowserSource *bs = static_cast<BrowserSource *>(data);
	bs->client->Reload(true);
	return false;
}

bool is_local_file_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool local = obs_data_get_bool(settings, S_IS_LOCAL_FILE);
	obs_property_set_visible(obs_properties_get(props, S_URL), !local);
	obs_property_set_visible(obs_properties_get(props, S_LOCAL_FILE), local);
	return true;
}

bool fps_custom_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_set_visible(obs_properties_get(props, S_FPS), obs_data_get_bool(settings, S_FPS_CUSTOM));
	return true;
}

obs_properties_t *browser_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_bool(props, S_IS_LOCAL_FILE, T_("LocalFile"));
	obs_property_set_modified_callback(p, is_local_file_modified);
	obs_properties_add_path(props, S_LOCAL_FILE, T_("Local file"), OBS_PATH_FILE,
				"HTML (*.html *.htm *.xhtml);;All files (*.*)", nullptr);
	obs_properties_add_text(props, S_URL, T_("URL"), OBS_TEXT_DEFAULT);

	obs_properties_add_int(props, S_WIDTH, T_("Width"), 1, 8192, 1);
	obs_properties_add_int(props, S_HEIGHT, T_("Height"), 1, 8192, 1);

	p = obs_properties_add_bool(props, S_FPS_CUSTOM, T_("CustomFrameRate"));
	obs_property_set_modified_callback(p, fps_custom_modified);
	obs_properties_add_int(props, S_FPS, T_("FPS"), 1, 120, 1);

	p = obs_properties_add_text(props, S_CSS, T_("CSS"), OBS_TEXT_MULTILINE);
	obs_property_text_set_monospace(p, true);

	obs_properties_add_bool(props, S_SHUTDOWN, T_("ShutdownSourceNotVisible"));
	obs_properties_add_bool(props, S_RESTART_ACTIVE, T_("RefreshBrowserActive"));

	/* Page permissions need the obs-browser JavaScript bridge to the frontend, which the Haiku helper does not
	 * have: shown disabled so the saved value is kept for other platforms. */
	p = obs_properties_add_list(props, S_CONTROL_LEVEL, T_("WebpageControlLevel"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("WebpageControlLevel.Level.None"), 0);
	obs_property_list_add_int(p, T_("WebpageControlLevel.Level.ReadObs"), CONTROL_LEVEL_READ_OBS);
	obs_property_list_add_int(p, T_("WebpageControlLevel.Level.ReadUser"), 2);
	obs_property_list_add_int(p, T_("WebpageControlLevel.Level.Basic"), 3);
	obs_property_list_add_int(p, T_("WebpageControlLevel.Level.Advanced"), 4);
	obs_property_list_add_int(p, T_("WebpageControlLevel.Level.All"), 5);
	obs_property_set_enabled(p, false);
	obs_property_set_long_description(p, T_("WebpageControlLevel.Haiku"));

	p = obs_properties_add_bool(props, S_REROUTE_AUDIO, T_("RerouteAudio"));
	obs_property_set_long_description(p, T_("RerouteAudio.Description"));

	obs_properties_add_button2(props, "refreshnocache", T_("RefreshNoCache"), refresh_clicked, data);
	return props;
}

void browser_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_URL, "https://obsproject.com/browser-source");
	obs_data_set_default_int(settings, S_WIDTH, 800);
	obs_data_set_default_int(settings, S_HEIGHT, 600);
	obs_data_set_default_int(settings, S_FPS, 30);
	obs_data_set_default_bool(settings, S_FPS_CUSTOM, false);
	obs_data_set_default_bool(settings, S_SHUTDOWN, false);
	obs_data_set_default_bool(settings, S_RESTART_ACTIVE, false);
	obs_data_set_default_int(settings, S_CONTROL_LEVEL, CONTROL_LEVEL_READ_OBS);
	obs_data_set_default_bool(settings, S_REROUTE_AUDIO, false);
	obs_data_set_default_string(settings, S_CSS, DEFAULT_CSS);
}

} // namespace

void haiku_register_browser_source(void)
{
	struct obs_source_info info = {};
	info.id = "browser_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	/* OBS_SOURCE_AUDIO always, as obs-browser: a source without the flag at registration can never output
	 * audio; without "Control audio via OBS" it simply outputs none and is kept out of the audio mixer
	 * (obs_source_set_audio_active() in SetRerouteAudio()). */
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_INTERACTION |
			    OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_BROWSER;
	info.get_name = browser_get_name;
	info.create = browser_create;
	info.destroy = browser_destroy;
	info.update = browser_update;
	info.get_defaults = browser_defaults;
	info.get_properties = browser_properties;
	info.show = browser_show;
	info.hide = browser_hide;
	info.activate = browser_activate;
	info.deactivate = browser_deactivate;
	info.mouse_click = browser_mouse_click;
	info.mouse_move = browser_mouse_move;
	info.mouse_wheel = browser_mouse_wheel;
	info.focus = browser_focus;
	info.key_click = browser_key_click;
	obs_register_source(&info);
}
