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
 * Audio monitoring through Renga's system mixer.
 *
 * Every monitored source plays as a plain BSoundPlayer stream into the system mixer, which mixes, resamples
 * and follows the default output. The monitoring device is chosen with the mixer's per-stream routing: each
 * stream has a B_OUTPUT_MUX parameter "Output" (0 the default output, n the n-th output device), and the
 * mixer plays a routed stream through a sink of its own on that device (AudioMixer::_RouteOnce()). Nothing
 * here opens a sound card, so there is no direct-to-card player any more, no format conversion, and a device
 * that is unplugged or busy just means the stream plays on the default output until it is back (the mixer
 * handles that; PORTING.md "Sound cards and cameras that come and go").
 *
 * How the mixer keys a route, read in the mixer add-on: the route is a property of each mixer input
 * (MixerInput::SetRoute()), set through that input's "Output" parameter (AudioMixer::SetParameterValue()),
 * and saved in the mixer's settings entry for the input's application signature
 * (MixerSettings::SaveConnectionSettings() and _FindInputSetting()). A new input of the same application gets
 * the saved route when it connects (AudioMixer::Connected() -> LoadConnectionSettings()). So:
 *  - all of OBS's monitor streams share one route (they have one signature), which is what OBS wants;
 *  - setting it on one stream does not move OBS's other streams that are already connected, so
 *    obs_haiku_route_streams() sets it on every stream of OBS;
 *  - a stream that appears later inherits the route the mixer last saved for OBS; it is set again anyway
 *    after each player starts (haiku-output.c), so that a route changed elsewhere (Media preferences) is
 *    brought back to OBS's setting.
 *
 * Setting a route reads the mixer's whole parameter web and sets a parameter on the mixer: round trips that
 * audio_monitor_init() would otherwise make on whatever thread creates or resets a monitor (the UI thread,
 * for a source added or a monitoring device changed). So obs_haiku_route_streams_async() hands them to one
 * worker thread, which does the latest request only: a device change resets every monitor, and the requests
 * that come in meanwhile all ask for the same thing.
 */

#include "haiku-sound-player.h"

#include "../../util/base.h"
#include "../../util/threading.h"

#include <condition_variable>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <string.h>

#include <MediaAddOn.h>
#include <MediaDefs.h>
#include <MediaRoster.h>
#include <OS.h>
#include <Roster.h>
#include <SoundPlayer.h>

/* Renga private header (headers/private/media; header-only on public Media Kit API): the system mixer's
 * per-application streams */
#include <AudioStreamControl.h>

#define MAX_DEVICES 32

struct obs_haiku_sound_player {
	obs_haiku_fill_cb cb;
	void *param;
	BSoundPlayer *player;
	uint32 channels; /* the channel count cb produces */
	media_node mixer; /* our reference to the system mixer, for the player's lifetime */
	bool have_mixer;
};

/* ------------------------------------------------------------------------- */
/* Devices and routing                                                       */

/* The devices exactly as the mixer lists them for routing: AudioMixer::_ScanOutputDevices() takes the
 * dormant nodes that are B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT with a raw audio input, in the order
 * GetDormantNodes() returns them, each name once. Renga numbers identical cards (" 2", " 3"), so names are
 * unique and stable across plug-ins and boots, and they are what the mixer saves a route as. Reading them
 * here rather than from the mixer's parameter web means the list is there even before any stream exists
 * (only stream groups carry the "Output" parameter). */
static std::vector<std::string> output_devices(BMediaRoster *roster)
{
	std::vector<std::string> names;
	dormant_node_info infos[MAX_DEVICES];
	int32 count = MAX_DEVICES;
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = media_raw_audio_format::wildcard;
	if (roster->GetDormantNodes(infos, &count, &format, NULL, NULL, B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT, 0) !=
	    B_OK)
		return names;
	for (int32 i = 0; i < count && i < MAX_DEVICES; i++) {
		bool seen = false;
		for (const std::string &name : names)
			seen = seen || name == infos[i].name;
		if (!seen)
			names.push_back(infos[i].name);
	}
	return names;
}

extern "C" void obs_haiku_enum_output_devices(obs_haiku_device_cb cb, void *param)
{
	/* Without the media server every roster call would wait for its timeout; this check is local. */
	if (!BMediaRoster::IsRunning())
		return;
	BMediaRoster *roster = BMediaRoster::Roster();
	if (!roster)
		return;

	for (const std::string &name : output_devices(roster))
		if (!cb(param, name.c_str(), name.c_str()))
			break;
}

/* The signature the mixer knows OBS's streams by: AudioMixer::_IdentifyApplication() asks the registrar for
 * the application of the team that owns the stream's port. */
static bool own_signature(std::string &signature)
{
	thread_info thread;
	app_info app;
	if (!be_roster || get_thread_info(find_thread(NULL), &thread) != B_OK ||
	    be_roster->GetRunningAppInfo(thread.team, &app) != B_OK || !app.signature[0])
		return false;
	signature = app.signature;
	return true;
}

static std::mutex warn_mutex;
static std::string warned_id;

/* Logs a routing failure once per device until routing to it works again: every monitored source and
 * every monitoring reset would otherwise repeat it. */
static void warn_route(const char *id, const char *reason)
{
	std::lock_guard<std::mutex> lock(warn_mutex);
	if (warned_id == id)
		return;
	warned_id = id;
	blog(LOG_WARNING, "[audio monitoring] Cannot route monitoring to '%s': %s; it plays on the default output", id,
	     reason);
}

static void route_ok()
{
	std::lock_guard<std::mutex> lock(warn_mutex);
	warned_id.clear();
}

extern "C" bool obs_haiku_route_streams(const char *device_id)
{
	if (!device_id || !*device_id || strcmp(device_id, OBS_HAIKU_MONITOR_DEFAULT_ID) == 0)
		device_id = "";
	const char *label = *device_id ? device_id : OBS_HAIKU_MONITOR_DEFAULT_ID;

	if (!BMediaRoster::IsRunning())
		return false;

	std::string self;
	if (!own_signature(self)) {
		/* the mixer then has no signature for OBS's streams either, and cannot be asked about them */
		warn_route(label, "OBS has no application signature the system mixer could know its streams by");
		return false;
	}

	/* one object per call: AudioStreamControl is not thread safe */
	BPrivate::AudioStreamControl control;
	if (control.Update() != B_OK) {
		warn_route(label, "the system mixer's controls cannot be read");
		return false;
	}

	/* index 0 is the default output; the device's index in the list the mixer offers its streams */
	int32 index = 0;
	if (*device_id) {
		index = control.Outputs().IndexOf(device_id);
		if (index <= 0) {
			/* The parameter only takes devices that are there now; a stream routed to a device that
			 * goes away later does come back to it by itself. */
			warn_route(label, "the system mixer does not list this device now");
			return false;
		}
	}

	int routed = 0, failed = 0;
	for (int32 i = 0; i < control.CountStreams(); i++) {
		const BPrivate::AudioStream *stream = control.StreamAt(i);
		if (stream->signature != self.c_str() || !stream->hasOutput)
			continue;
		if (stream->output == index) {
			routed++;
			continue;
		}
		if (control.SetOutput(stream->id, index) == B_OK)
			routed++;
		else
			failed++;
	}

	if (failed || !routed) {
		warn_route(label, routed || failed ? "the system mixer refused the route"
						   : "the system mixer has no stream of OBS (yet)");
		return false;
	}
	route_ok();
	return true;
}

/* The routing worker. It lives while there are players (or a request): it returns by itself when the last
 * player is gone and nothing is pending, so neither destroying a player nor creating one ever waits for a
 * round trip to the mixer. The state is never destroyed, so no joinable std::thread is left to a static
 * destructor at exit. */
struct RouteWorker {
	std::mutex mutex;
	std::condition_variable cond;
	std::thread thread;
	bool running = false; /* the thread is in its loop: a request needs no new thread */
	bool pending = false;
	std::string device_id;
	int players = 0;
};

static RouteWorker &route_worker()
{
	static RouteWorker *worker = new RouteWorker;
	return *worker;
}

static void route_worker_run()
{
	os_set_thread_name("obs-haiku-monitor-route");
	RouteWorker &w = route_worker();
	std::unique_lock<std::mutex> lock(w.mutex);
	for (;;) {
		if (!w.pending) {
			if (w.players == 0)
				break;
			w.cond.wait(lock);
			continue;
		}
		w.pending = false;
		const std::string id = w.device_id;
		lock.unlock();
		obs_haiku_route_streams(id.c_str());
		lock.lock();
	}
	/* Under the lock, so that a request after this point starts a new thread */
	w.running = false;
}

/* With the lock held. A thread that has left its loop is joined first: it is returning, not waiting. */
static void route_worker_ensure(RouteWorker &w)
{
	if (w.running)
		return;
	if (w.thread.joinable())
		w.thread.join();
	try {
		w.thread = std::thread(route_worker_run);
		w.running = true;
	} catch (const std::system_error &error) {
		blog(LOG_WARNING, "[audio monitoring] Cannot start the routing thread: %s", error.what());
	}
}

extern "C" void obs_haiku_route_streams_async(const char *device_id)
{
	RouteWorker &w = route_worker();
	std::lock_guard<std::mutex> lock(w.mutex);
	w.device_id = device_id ? device_id : "";
	w.pending = true;
	route_worker_ensure(w);
	w.cond.notify_all();
}

static void route_worker_player_added()
{
	RouteWorker &w = route_worker();
	std::lock_guard<std::mutex> lock(w.mutex);
	w.players++;
}

static void route_worker_player_removed()
{
	RouteWorker &w = route_worker();
	std::lock_guard<std::mutex> lock(w.mutex);
	if (w.players > 0)
		w.players--;
	/* an idle worker leaves when it wakes and finds no players */
	w.cond.notify_all();
}

/* ------------------------------------------------------------------------- */
/* Players                                                                   */

static void play_buffer(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format)
{
	obs_haiku_sound_player *p = static_cast<obs_haiku_sound_player *>(cookie);

	/* The format was checked in obs_haiku_sound_player_create(); cb writes p->channels per frame, so
	 * anything else would overrun the buffer. */
	if (format.format != media_raw_audio_format::B_AUDIO_FLOAT || format.channel_count != p->channels ||
	    p->channels == 0) {
		memset(buffer, 0, size);
		return;
	}

	const size_t frame_bytes = sizeof(float) * p->channels;
	const size_t frames = size / frame_bytes;
	p->cb(p->param, static_cast<float *>(buffer), frames);
	/* a partial trailing frame (shouldn't happen) */
	memset(static_cast<uint8 *>(buffer) + frames * frame_bytes, 0, size % frame_bytes);
}

static void free_player(obs_haiku_sound_player *p)
{
	/* Stop without blocking: Stop(true, ...) polls IsPlaying() for up to 500 ms and then snoozes
	 * Latency() + 2 ms so queued buffers finish playing, which is only about letting the tail play
	 * out (and ~BSoundPlayer would do the same if the player were still started, hence the explicit
	 * non-blocking Stop first). Callback safety comes from the destructor: it releases the
	 * SoundPlayNode, whose DeleteHook (BMediaEventLooper) calls Quit(), which closes the control port
	 * and wait_for_thread()s the control thread -- the only thread that calls PlayBuffer -- before the
	 * node is deleted. So the buffer hook cannot run once delete returns. */
	if (p->player) {
		p->player->Stop(false, false);
		delete p->player;
		p->player = NULL;
	}
	/* BSoundPlayer only releases a mixer it looked up itself (F_MUST_RELEASE_MIXER), not one it was
	 * given, so this reference is ours to drop, after the player has disconnected */
	if (p->have_mixer) {
		if (BMediaRoster *roster = BMediaRoster::CurrentRoster())
			roster->ReleaseNode(p->mixer);
		p->have_mixer = false;
	}
}

/* The channel mask of OBS's layout for `channels` (speaker_layout values are channel counts), in Media Kit
 * terms: channels are interleaved in mask bit order. The mixer keeps a mask with as many bits as channels
 * (MixerUtils.cpp) and otherwise assumes its own defaults, which differ from OBS's for 3 to 5 channels
 * (OBS 2.1 = FL FR LFE, 4.0 = FL FR FC RC, 4.1 = FL FR FC LFE RC). */
static uint32 obs_channel_mask(uint32 channels)
{
	const uint32 L = B_CHANNEL_LEFT, R = B_CHANNEL_RIGHT, C = B_CHANNEL_CENTER, S = B_CHANNEL_SUB,
		     RL = B_CHANNEL_REARLEFT, RR = B_CHANNEL_REARRIGHT, BC = B_CHANNEL_BACK_CENTER;
	switch (channels) {
	case 2:
		return L | R;
	case 3:
		return L | R | S;
	case 4:
		return L | R | C | BC;
	case 5:
		return L | R | C | S | BC;
	case 6:
		return L | R | C | S | RL | RR;
	case 8:
		return L | R | C | S | RL | RR | B_CHANNEL_SIDE_LEFT | B_CHANNEL_SIDE_RIGHT;
	default:
		return 0; /* mono, and anything OBS has no layout for: the mixer's default */
	}
}

extern "C" obs_haiku_sound_player *obs_haiku_sound_player_create(const char *name, uint32_t sample_rate,
								 uint32_t channels, size_t buffer_frames,
								 obs_haiku_fill_cb cb, void *param)
{
	/* Fail fast instead of waiting for roster timeouts when the media server is down. */
	BMediaRoster *roster = BMediaRoster::IsRunning() ? BMediaRoster::Roster() : NULL;
	if (!roster)
		return NULL;
	if (channels < 1 || channels > 8 || channels == 7)
		return NULL;

	obs_haiku_sound_player *p = new (std::nothrow) obs_haiku_sound_player();
	if (!p)
		return NULL;
	p->cb = cb;
	p->param = param;
	p->channels = channels;

	/* The node constructor, because only it takes a media_multi_audio_format, which has the channel
	 * mask; given the system mixer it connects just as the plain constructor does. */
	if (roster->GetAudioMixer(&p->mixer) != B_OK) {
		delete p;
		return NULL;
	}
	p->have_mixer = true;

	media_multi_audio_format format = media_multi_audio_format::wildcard;
	format.channel_mask = obs_channel_mask(channels);
	format.frame_rate = (float)sample_rate;
	format.channel_count = channels;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.byte_order = B_MEDIA_HOST_ENDIAN;
	/* A fixed buffer length keeps monitoring latency independent of the device's period. Renga's
	 * low-latency mode (switched on for OBS in Media preferences > Audio settings > Applications, or with
	 * RENGA_AUDIO_LOW_LATENCY=1) still shortens the default output's period; only BSoundPlayer's own
	 * one-period buffer choice is skipped because the size is given (SoundPlayer.cpp _Init()). */
	format.buffer_size = buffer_frames * channels * sizeof(float);

	/* The constructor connects to the system mixer: the stream exists once it returns. */
	p->player = new (std::nothrow) BSoundPlayer(p->mixer, &format, name, NULL, play_buffer, NULL, p);
	if (!p->player || p->player->InitCheck() != B_OK) {
		free_player(p);
		delete p;
		return NULL;
	}

	const media_raw_audio_format fmt = p->player->Format();
	if (fmt.format != media_raw_audio_format::B_AUDIO_FLOAT || fmt.channel_count != channels) {
		blog(LOG_WARNING,
		     "[audio monitoring] The system mixer negotiated %u channels (format 0x%x) instead of "
		     "%u float channels",
		     (unsigned)fmt.channel_count, (unsigned)fmt.format, (unsigned)channels);
		free_player(p);
		delete p;
		return NULL;
	}
	route_worker_player_added();
	return p;
}

extern "C" bool obs_haiku_sound_player_start(obs_haiku_sound_player *p)
{
	if (!p || p->player->Start() != B_OK)
		return false;
	p->player->SetHasData(true);
	return true;
}

extern "C" void obs_haiku_sound_player_destroy(obs_haiku_sound_player *p)
{
	if (!p)
		return;
	free_player(p);
	delete p;
	route_worker_player_removed();
}
