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

/* See browser-audio.hpp for what this does and why. */

#include "browser-audio.hpp"

#include <util/platform.h>
#include <util/threading.h>
#include <util/util_uint64.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include <string.h>

#include <MediaAddOn.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <TimeSource.h>

/* Renga private header (headers/private/media, header-only on public Media Kit API) */
#include <AudioStreamControl.h>

#define ALOG(level, fmt, ...) blog(level, "[haiku-browser] %s: " fmt, state->name.c_str(), ##__VA_ARGS__)

/* How often an idle worker checks that its loopback still exists (get_port_info(), no round trip to the media
 * server): the media services may have been restarted, and the new mixer knows neither the loopback nor the
 * loopback-only team. */
#define CHECK_INTERVAL_MS 2000
/* how long to wait before trying again what could not be set up (loopback, loopback-only team) */
#define RETRY_INTERVAL_US 2000000LL

/* a converted time further than this from now is not believed (libobs gives up at 2 s, MAX_TS_VAR) */
#define MAX_CLOCK_OFFSET_US 1000000
/* how often a missing or changed time source is looked up again */
#define CLOCK_LOOKUP_INTERVAL_US 1000000

struct BrowserAudioReroute::State {
	std::string name; /* for the log; set once */

	/* --- worker control, under `mutex` --- */
	std::mutex mutex;
	std::condition_variable cond;
	bool quit = false;
	bool wake = false;
	team_id team = -1;

	/* set by the worker as its last action; the registry joins finished workers */
	std::atomic_bool done{false};

	/* the destructor ran without HelperClosing(): take the team off the loopback-only list on the way out */
	bool restore = true;

	/* --- what the worker hands out, under `output_mutex` --- */
	std::mutex output_mutex;
	/* both cleared once the instance let go: nothing is output and nobody is called after that */
	obs_source_t *source = nullptr;
	std::function<void(team_id)> ready;
};

namespace {

using State = BrowserAudioReroute::State;

/*
 * Converts buffer start times from the mixer's time source to real time (system_time()). The same conversion
 * as plugins/haiku-media/loopback-audio.cpp (MixerClock), copied so this module stays self-contained.
 *
 * The mixer stamps each loopback buffer with the performance time at which its first frame reaches the
 * output, in the mixer's time source (MixerCore::_FeedTaps(), MixerTap::_SendBuffers()). The time source is
 * the default output's sound card and changes with it; MakeTimeSourceFor() on the mixer returns a
 * reference-counted shadow object that reads its shared area, so a conversion costs no round trip. Used from
 * the recorder's node thread only.
 */
class MixerClock {
public:
	~MixerClock()
	{
		if (source)
			source->Release();
	}

	bool RealTimeFor(const media_header &header, bigtime_t now, bigtime_t *real)
	{
		if (header.time_source <= 0)
			return false;

		if (!source || source->ID() != header.time_source) {
			/* asks the mixer and the media server: not more than once a second */
			if (last_lookup && now - last_lookup < CLOCK_LOOKUP_INTERVAL_US)
				return false;
			last_lookup = now;
			if (source) {
				source->Release();
				source = nullptr;
			}
			BMediaRoster *roster = BMediaRoster::CurrentRoster();
			media_node mixer;
			if (!roster || roster->GetAudioMixer(&mixer) != B_OK)
				return false;
			source = roster->MakeTimeSourceFor(mixer);
			roster->ReleaseNode(mixer);
			if (!source || source->ID() != header.time_source)
				return false;
		}

		/* a shadow object whose area could not be cloned reports "not running" (and GetTime() would call
		 * debugger()); a stopped time source has no meaningful times either */
		if (!source->IsRunning())
			return false;

		/* BTimeSource::RealTimeFor(), spelled out to refuse a drift of zero instead of dividing by it */
		bigtime_t performance = 0, at = 0;
		float drift = 0.0f;
		if (source->GetTime(&performance, &at, &drift) != B_OK || !(drift > 0.0f))
			return false;
		const bigtime_t converted = at + (bigtime_t)((double)(header.start_time - performance) / drift);
		if (converted < now - MAX_CLOCK_OFFSET_US || converted > now + MAX_CLOCK_OFFSET_US)
			return false;

		*real = converted;
		return true;
	}

private:
	BTimeSource *source = nullptr;
	bigtime_t last_lookup = 0;
};

/* What the recorder's node thread works with. Allocated apart from the recorder and deleted only after it:
 * ~BMediaRecorder() releases its node, whose BMediaEventLooper::DeleteHook() ends the node thread that calls
 * BufferReceived() (the same arrangement as HaikuRecorder::Deleter in plugins/haiku-media). */
struct Delivery {
	State *state;
	float rate = 0.0f;
	std::atomic_bool on{true};

	MixerClock clock;
	bool have_offset = false;
	int64_t last_offset_us = 0;
	int clock_state = -1;

	void Buffer(const void *data, size_t size, const media_header &header);
};

class LoopbackRecorder : public BMediaRecorder {
public:
	explicit LoopbackRecorder(Delivery *delivery_)
		: BMediaRecorder("OBS browser source audio", B_MEDIA_RAW_AUDIO),
		  delivery(delivery_)
	{
	}

	/* the format negotiated by Connect() (Format() is only the accepted one) */
	media_format Negotiated() const { return MediaInput().format; }

protected:
	void BufferReceived(void *buffer, size_t size, const media_header &header) override
	{
		Delivery *const d = delivery;
		if (d->on)
			d->Buffer(buffer, size, header);
	}

private:
	Delivery *const delivery;
};

/* recorder node thread */
void Delivery::Buffer(const void *data, size_t size, const media_header &header)
{
	/* float stereo, host order: checked when connecting (Loopback::Open()) */
	const size_t frame_size = 2 * sizeof(float);
	if (size < frame_size)
		return;

	struct obs_source_audio audio = {};
	audio.data[0] = static_cast<const uint8_t *>(data);
	audio.frames = (uint32_t)(size / frame_size);
	audio.speakers = SPEAKERS_STEREO;
	audio.format = AUDIO_FORMAT_FLOAT;
	audio.samples_per_sec = (uint32_t)rate;

	/* Both clocks are read here so that only their difference matters: the buffer's real time is in
	 * system_time(), OBS's time stamps are os_gettime_ns(). As in loopback-audio.cpp: a buffer is stamped
	 * with the moment it is heard; while the time source is being looked up again, the last offset is kept
	 * rather than switching to arrival stamps, which would make the stamps jump by a latency. */
	const uint64_t now_ns = os_gettime_ns();
	const bigtime_t now = system_time();
	bigtime_t real = 0;
	const bool exact = clock.RealTimeFor(header, now, &real);
	if (exact) {
		last_offset_us = (int64_t)(real - now);
		have_offset = true;
	}
	int clock_now;
	if (have_offset) {
		const int64_t offset_ns = last_offset_us * 1000;
		audio.timestamp = offset_ns >= 0 ? now_ns + (uint64_t)offset_ns : now_ns - (uint64_t)(-offset_ns);
		clock_now = exact ? 2 : 1;
	} else {
		audio.timestamp = now_ns - util_mul_div64(audio.frames, 1000000000ULL, audio.samples_per_sec);
		clock_now = 0;
	}
	if (clock_state != clock_now) {
		clock_state = clock_now;
		static const char *const names[] = {"the arrival time of the buffers",
						    "the mixer's time source, as last converted",
						    "the mixer's time source"};
		ALOG(LOG_INFO, "page audio time stamps from %s", names[clock_now]);
	}

	std::lock_guard<std::mutex> lock(state->output_mutex);
	if (state->source)
		obs_source_output_audio(state->source, &audio);
}

/* The rate to ask the loopback for: the rate the mixer mixes at (its output connection's,
 * MixerCore::_ApplyOutputFormat()), so that MixerTap copies instead of resampling in the mix thread with the
 * core locked (MixerTap::Push()); libobs resamples with its own resampler. As in plugins/haiku-media. */
status_t mixer_rate(BMediaRoster *roster, float *rate)
{
	media_node mixer;
	status_t err = roster->GetAudioMixer(&mixer);
	if (err != B_OK)
		return err;

	media_output output;
	int32 count = 0;
	err = roster->GetConnectedOutputsFor(mixer, &output, 1, &count);
	roster->ReleaseNode(mixer);
	if (err != B_OK)
		return err;
	if (count < 1 || output.format.type != B_MEDIA_RAW_AUDIO)
		return B_MEDIA_NOT_CONNECTED;

	*rate = output.format.u.raw_audio.frame_rate;
	return B_OK;
}

/* The source's loopback and its recorder; worker thread only. */
class Loopback {
public:
	explicit Loopback(State *state_) : state(state_) {}
	~Loopback() { Close(); }

	bool IsOpen() const { return recorder != nullptr; }
	team_id Team() const { return team; }

	/* false with `reason` set if it cannot be set up now */
	bool Open(BMediaRoster *roster, team_id new_team, const char **reason);
	void Close();
	bool SetTeam(team_id new_team);

	/* the node lives as long as its control port (BMediaNode deletes it on destruction) and with it the
	 * media add-on server that hosts it */
	bool NodeAlive() const
	{
		port_info info;
		return have_node && get_port_info(node.port, &info) == B_OK;
	}

private:
	State *const state;
	media_node node;
	bool have_node = false;
	team_id team = -1;
	LoopbackRecorder *recorder = nullptr;
	Delivery *delivery = nullptr;
};

bool Loopback::Open(BMediaRoster *roster, team_id new_team, const char **reason)
{
	Close();

	/* The system mixer has to be there first: a loopback attaches itself to the mixer when it is registered
	 * (MixerTap::NodeRegistered() -> AudioMixer::AttachLoopback()) and is not picked up later. */
	float rate = 0.0f;
	if (mixer_rate(roster, &rate) != B_OK) {
		*reason = "the system mixer is not running (yet)";
		return false;
	}
	/* what MixerTap accepts (format_supported() in MixerTap.cpp); a mixer without a sane rate gets OBS's */
	if (!(rate >= 4000.0f && rate <= 384000.0f))
		rate = (float)audio_output_get_sample_rate(obs_get_audio());

	/* Not AudioStreamControl::StartLoopback(): that selects by signature, and an empty one means every
	 * stream. The team is set before anything is connected, so the loopback never records anything else. */
	dormant_node_info info;
	if (BPrivate::AudioStreamControl::FindLoopbackFlavor(&info) != B_OK ||
	    roster->InstantiateDormantNode(info, &node, B_FLAVOR_IS_GLOBAL) != B_OK) {
		*reason = "the system mixer's loopback (\"" RENGA_LOOPBACK_FLAVOR_NAME "\") is not available";
		return false;
	}
	have_node = true;

	if (BPrivate::AudioStreamControl::SetLoopbackTeam(node, new_team) != B_OK) {
		*reason = "the loopback cannot select a team (renga:loopback_team, Renga F-58)";
		Close();
		return false;
	}
	team = new_team;

	media_output output;
	int32 count = 0;
	if (roster->GetFreeOutputsFor(node, &output, 1, &count, B_MEDIA_RAW_AUDIO) != B_OK || count < 1) {
		*reason = "the loopback has no free output";
		Close();
		return false;
	}

	delivery = new Delivery;
	delivery->state = state;
	recorder = new LoopbackRecorder(delivery);
	if (recorder->InitCheck() != B_OK) {
		*reason = "media recorder init failed";
		Close();
		return false;
	}

	/* Float stereo: the tap holds the mix as float and fills two channels whatever the connection has
	 * (MixerTap::Push()), so this format is copied as it is and OBS takes it without conversion. The buffer
	 * size stays a wildcard. */
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = media_raw_audio_format::wildcard;
	format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.u.raw_audio.channel_count = 2;
	format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
	format.u.raw_audio.frame_rate = rate;
	recorder->SetAcceptedFormat(format);
	if (recorder->Connect(node, &output, &format) != B_OK) {
		*reason = "connecting to the loopback failed";
		Close();
		return false;
	}

	const media_raw_audio_format got = recorder->Negotiated().u.raw_audio;
	if (got.format != media_raw_audio_format::B_AUDIO_FLOAT || got.channel_count != 2 ||
	    got.byte_order != B_MEDIA_HOST_ENDIAN || !(got.frame_rate >= 4000.0f && got.frame_rate <= 384000.0f)) {
		*reason = "the loopback did not accept float stereo";
		Close();
		return false;
	}
	delivery->rate = got.frame_rate;

	/* The loopback is no time source, so this is BMediaRoster::StartNode(); the mixer feeds a loopback
	 * silence while its team plays nothing. */
	if (recorder->Start() != B_OK) {
		*reason = "starting the loopback failed";
		Close();
		return false;
	}

	ALOG(LOG_INFO, "recording the page's audio from the system mixer's loopback (team %d, %.0f Hz)", (int)team,
	     got.frame_rate);
	return true;
}

bool Loopback::SetTeam(team_id new_team)
{
	if (!have_node || BPrivate::AudioStreamControl::SetLoopbackTeam(node, new_team) != B_OK)
		return false;
	team = new_team;
	ALOG(LOG_INFO, "page audio now follows browser helper team %d", (int)team);
	return true;
}

void Loopback::Close()
{
	if (recorder) {
		delivery->on = false;
		if (recorder->IsRunning())
			recorder->Stop(true);
		if (recorder->IsConnected())
			recorder->Disconnect();
		delete recorder; /* ends its node thread: no BufferReceived() after this */
		recorder = nullptr;
	}
	delete delivery;
	delivery = nullptr;

	if (have_node) {
		/* the only reference: the media add-on server deletes the instance */
		if (BMediaRoster *roster = BMediaRoster::CurrentRoster())
			roster->ReleaseNode(node);
		have_node = false;
	}
	team = -1;
}

/*
 * The helper's team on the mixer's loopback-only list (O-18, AudioStreamControl::SetTeamLoopbackOnly()). It is
 * set for the current team only; a previous helper's team stays listed while that helper shuts down and is
 * forgotten by the mixer when it has quit. Worker thread only.
 */
class TeamRoute {
public:
	explicit TeamRoute(State *state_) : state(state_) {}

	/* The mixer lost its list (the media services were restarted): set it again. */
	void Forget()
	{
		applied = -1;
		given_up = -1;
		next_try = 0;
	}

	/* Puts `team` on the list unless it is there; then calls `ready` once per team, whether that worked or
	 * not (the page must not stay blank because of a missing mixer). */
	void Apply(team_id team, bigtime_t now)
	{
		if (team != wanted) {
			wanted = team;
			next_try = 0;
			notified = false;
		}
		if (applied != team && given_up != team && now >= next_try) {
			/* Update() reads the mixer's parameter web, which SetTeamLoopbackOnly() needs (_FindKind()) */
			status_t status = control.Update();
			if (status == B_OK)
				status = control.SetTeamLoopbackOnly(team, true);
			if (status == B_OK) {
				applied = team;
				ALOG(LOG_INFO,
				     "browser helper team %d plays loopback-only (not on the speakers, not in "
				     "Desktop Audio)",
				     (int)team);
			} else if (status == B_BAD_VALUE) {
				/* the team no longer runs (stopped while hidden, or crashed): its successor is reported
				 * with the started callback */
				given_up = team;
				ALOG(LOG_INFO, "browser helper team %d no longer runs", (int)team);
			} else if (status == B_ENTRY_NOT_FOUND) {
				/* the mixer has no "renga:loopback_only_teams" (older than F-58 batch 2) */
				given_up = team;
				ALOG(LOG_WARNING,
				     "cannot make browser helper team %d loopback-only (the system mixer does not support "
				     "it): the page's audio may also play on the speakers and in Desktop Audio",
				     (int)team);
			} else {
				/* the mixer is not there (yet), or the media server did not answer: again later */
				next_try = now + RETRY_INTERVAL_US;
				if (!notified)
					ALOG(LOG_WARNING,
					     "cannot make browser helper team %d loopback-only yet (%s); retrying",
					     (int)team, strerror(status));
			}
		}
		if (!notified) {
			notified = true;
			std::lock_guard<std::mutex> lock(state->output_mutex);
			if (state->ready)
				state->ready(team);
		}
	}

	/* The option was turned off: the page plays as its own settings say again. */
	void Restore()
	{
		if (applied <= 0)
			return;
		status_t status = control.Update();
		if (status == B_OK)
			status = control.SetTeamLoopbackOnly(applied, false);
		if (status == B_OK)
			ALOG(LOG_INFO, "browser helper team %d plays on the speakers again", (int)applied);
		else
			ALOG(LOG_WARNING, "cannot take browser helper team %d off the loopback-only list: %s",
			     (int)applied, strerror(status));
		applied = -1;
	}

private:
	State *const state;
	BPrivate::AudioStreamControl control; /* not thread safe: this worker's own */
	team_id wanted = -1;
	team_id applied = -1;
	team_id given_up = -1;
	bigtime_t next_try = 0;
	bool notified = false;
};

void run_worker(std::shared_ptr<State> state)
{
	os_set_thread_name("haiku-browser-audio");
	{
		Loopback loopback(state.get());
		TeamRoute route(state.get());
		bigtime_t next_open = 0;
		const char *last_reason = nullptr;
		bool restore;

		for (;;) {
			team_id team;
			{
				/* Only a retry or the loopback check needs a timeout; otherwise this waits for a new team
				 * or the end. */
				std::unique_lock<std::mutex> lock(state->mutex);
				state->cond.wait_for(lock, std::chrono::milliseconds(CHECK_INTERVAL_MS),
						     [&]() { return state->quit || state->wake; });
				if (state->quit) {
					restore = state->restore;
					break;
				}
				state->wake = false;
				team = state->team;
			}

			BMediaRoster *roster = BMediaRoster::Roster();
			if (!roster)
				continue;
			bigtime_t now = system_time();

			/* the media services were restarted: the instance went with the media add-on server, and the
			 * new mixer has an empty loopback-only list */
			if (loopback.IsOpen() && !loopback.NodeAlive()) {
				ALOG(LOG_WARNING, "the page audio loopback went away (media services restarted?); "
						  "setting it up again");
				loopback.Close();
				route.Forget();
				next_open = 0;
			}

			if (team <= 0)
				continue;

			/* first: the page is held back until this is done */
			route.Apply(team, now);
			now = system_time();

			if (!loopback.IsOpen()) {
				const char *reason = nullptr;
				if (now >= next_open && !loopback.Open(roster, team, &reason)) {
					if (reason != last_reason)
						ALOG(LOG_WARNING, "cannot record the page's audio: %s; retrying",
						     reason);
					last_reason = reason;
					next_open = now + RETRY_INTERVAL_US;
				} else if (loopback.IsOpen()) {
					last_reason = nullptr;
				}
			} else if (loopback.Team() != team && !loopback.SetTeam(team)) {
				/* set it up anew with the new team at the next turn */
				loopback.Close();
			}
		}

		if (restore)
			route.Restore();
		/* ~Loopback() disconnects and releases the loopback here, on this thread */
	}
	state->done = true;
}

/* Workers outlive their BrowserAudioReroute so that its destructor never waits for the media server; they
 * are joined at module unload (or, failing that, when the module's statics are destroyed: a joinable
 * std::thread must not be destroyed). Finished ones are joined (at once) whenever a new one starts, so the
 * list stays short. */
class Workers {
public:
	~Workers() { Shutdown(); }

	void Start(const std::shared_ptr<State> &state)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (shut_down)
			return;
		for (auto it = list.begin(); it != list.end();) {
			if (it->state->done) {
				it->thread.join();
				it = list.erase(it);
			} else {
				++it;
			}
		}
		/* Called from a C callback (the source's create or update): an exception must not leave it */
		try {
			list.push_back({std::thread(run_worker, state), state});
		} catch (const std::system_error &error) {
			blog(LOG_ERROR,
			     "[haiku-browser] %s: cannot start the audio thread (%s); the page's audio is not recorded",
			     state->name.c_str(), error.what());
		}
	}

	void Shutdown()
	{
		std::list<Entry> remaining;
		{
			std::lock_guard<std::mutex> lock(mutex);
			shut_down = true;
			remaining.swap(list);
		}
		for (Entry &entry : remaining) {
			{
				/* OBS is quitting and the helpers go with it: leave their teams loopback-only, as
				 * HelperClosing() does, so that no page is heard while it closes */
				std::lock_guard<std::mutex> lock(entry.state->mutex);
				entry.state->quit = true;
				entry.state->restore = false;
			}
			entry.state->cond.notify_one();
			entry.thread.join();
		}
	}

private:
	struct Entry {
		std::thread thread;
		std::shared_ptr<State> state;
	};

	std::mutex mutex;
	std::list<Entry> list;
	bool shut_down = false;
};

Workers &workers()
{
	static Workers instance;
	return instance;
}

} // namespace

BrowserAudioReroute::BrowserAudioReroute(obs_source_t *source, std::function<void(team_id)> ready)
	: state(std::make_shared<State>())
{
	const char *name = obs_source_get_name(source);
	state->name = name ? name : "browser";
	state->source = source;
	state->ready = std::move(ready);

	workers().Start(state);
}

BrowserAudioReroute::~BrowserAudioReroute()
{
	/* After this no audio reaches the source and `ready` is not called; a call in progress finishes first. */
	{
		std::lock_guard<std::mutex> lock(state->output_mutex);
		state->source = nullptr;
		state->ready = nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->quit = true;
	}
	state->cond.notify_one();
}

void BrowserAudioReroute::SetHelperTeam(team_id team)
{
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (state->team == team)
			return;
		state->team = team;
		state->wake = true;
	}
	state->cond.notify_one();
}

void BrowserAudioReroute::HelperClosing()
{
	std::lock_guard<std::mutex> lock(state->mutex);
	state->restore = false;
}

void haiku_browser_audio_shutdown(void)
{
	workers().Shutdown();
}
