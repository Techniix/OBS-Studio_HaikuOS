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
 * "Desktop Audio" and "Application Audio Capture": what the system mixer plays, recorded through its
 * loopback (Renga roadmap F-31; PORTING.md "Per-application audio, routing and loopback").
 *
 * Renga's mixer add-on has a second flavor, "What you hear (loopback)": a producer node the mixer's mix thread
 * feeds with every stream (before the master volume), or with the streams of one application chosen by its
 * signature. Each source here instantiates a loopback of its own and reads it like a sound card input, with
 * the same recorder and supervisor as the other sources (media-device.hpp). Nothing is inserted into the
 * system's audio path, so there is no helper process, nothing to repair after a crash, and the default
 * output can change freely: the mixer keeps feeding the loopback.
 *
 * Desktop Audio leaves OBS's own streams out (the loopback's "renga:loopback_exclude", Renga F-58 / O-1): they
 * are audio monitoring, i.e. sources OBS records directly already. So monitored sources are not recorded twice,
 * and Desktop Audio itself can be monitored without feeding back into itself.
 *
 * Time stamps: the mixer stamps each loopback buffer with the performance time at which its first frame
 * reaches the output, in the mixer's time source (MixerCore::_FeedTaps() passes its event time and time
 * source; MixerTap::_SendBuffers() writes both into the buffer header and advances the time by the frames
 * sent). MixerClock turns that into OBS's clock, so the audio is stamped with the moment it is heard and
 * not with the moment its buffer happened to arrive.
 */

#include "audio-convert.hpp"
#include "media-device.hpp"

#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/util_uint64.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <List.h>
#include <MediaRoster.h>
#include <OS.h>
#include <Roster.h>
#include <TimeSource.h>

/* Renga private header (headers/private/media, header-only on public Media Kit API) */
#include <AudioStreamControl.h>

#define T_(text) obs_module_text(text)

/* The frontend's Settings > Audio > Desktop Audio list reads a "device_id" list from the platform's output
 * capture source and stores its value (frontend/settings/OBSBasicSettings.cpp); the system mixer is the one
 * thing there is to capture, whatever devices its streams are routed to. */
#define S_DEVICE_ID "device_id"
#define S_APPLICATION "application"
#define S_REFRESH "refresh"

/* OBS's application signature: OBS_HAIKU_APP_SIGNATURE in frontend/cmake/os-haiku.cmake, which goes into the
 * executable's resources (frontend/cmake/haiku/obs-studio.rdef.in). The mixer knows OBS's monitoring streams
 * by it (AudioMixer::_IdentifyApplication() asks the registrar). */
#define OBS_APP_SIGNATURE "application/x-vnd.obs-studio"

/* a converted time further than this from now is not believed (libobs gives up at 2 s, MAX_TS_VAR) */
#define MAX_CLOCK_OFFSET_US 1000000
/* how often a missing or changed time source is looked up again */
#define CLOCK_LOOKUP_INTERVAL_US 1000000

namespace {

/* Converts buffer start times from the mixer's time source to real time (system_time()). Used from the
 * recorder thread only. */
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
			/* The mixer's time source is the default output's sound card and changes with it. There
			 * is no public way from a node id to a BTimeSource, but the mixer's time source is the one
			 * the buffers name (MixerCore passes fTimeSource->ID()), and MakeTimeSourceFor() returns
			 * a reference-counted shadow object that reads the time source's shared area, so the
			 * conversion below costs no round trip. The lookup itself asks the mixer and the media
			 * server; hence not more than once a second. */
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

		/* A shadow object whose area could not be cloned reports "not running" here, and calls
		 * debugger() in GetTime(); a stopped time source has no meaningful times either. */
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

struct LoopbackAudio {
	obs_source_t *source;
	const bool per_application; /* Application Audio Capture; false: Desktop Audio */
	HaikuAudioConverter converter;

	/* recorder thread */
	MixerClock clock;
	int clock_state = -1; /* last logged: 2 mixer time source, 1 last offset, 0 arrival time */
	/* real time of the last buffer converted through the time source minus its arrival (system_time()) */
	bool have_offset = false;
	int64_t last_offset_us = 0;

	/* opens the loopback and keeps it open, retrying while the media services are down (media-device.hpp).
	 * Declared last: it is destroyed first, which stops the threads that call into this object. */
	std::unique_ptr<HaikuCapture> capture;

	LoopbackAudio(obs_source_t *source_, bool per_application_)
		: source(source_),
		  per_application(per_application_),
		  converter(source_)
	{
	}

	void OnOpened(const media_format &format);
	void OnBuffer(const void *data, size_t size, const media_format &format, const media_header &header);
};

/* supervisor thread */
void LoopbackAudio::OnOpened(const media_format &format)
{
	const media_raw_audio_format &raw = format.u.raw_audio;
	converter.Reset();
	blog(LOG_INFO, "[haiku-media] '%s': recording the system mixer's loopback (%u ch, %.0f Hz, format 0x%x)",
	     obs_source_get_name(source), raw.channel_count, raw.frame_rate, raw.format);
}

/* recorder thread */
void LoopbackAudio::OnBuffer(const void *data, size_t size, const media_format &format, const media_header &header)
{
	struct obs_source_audio audio = {};
	if (!converter.Convert(data, size, format, audio))
		return;

	/* Both clocks are read here so that only their difference matters: the buffer's real time is in
	 * system_time(), OBS's time stamps are os_gettime_ns(). */
	const uint64_t now_ns = os_gettime_ns();
	const bigtime_t now = system_time();
	bigtime_t real = 0;
	const bool exact = clock.RealTimeFor(header, now, &real);
	if (exact) {
		/* usually a little ahead of now: the mixer mixes a buffer its downstream latency before the
		 * sound card plays it */
		last_offset_us = (int64_t)(real - now);
		have_offset = true;
	}
	int state;
	if (have_offset) {
		/* Not exact: the time source is being looked up again (the default output changed, so the mixer's
		 * time source did; MixerClock tries once a second) or gave an implausible time. Keep the last
		 * offset instead of switching to arrival stamps, which lie about a latency earlier and would make
		 * the stamps jump back and forth by that much; the next good conversion corrects it. */
		const int64_t offset_ns = last_offset_us * 1000;
		audio.timestamp = offset_ns >= 0 ? now_ns + (uint64_t)offset_ns : now_ns - (uint64_t)(-offset_ns);
		state = exact ? 2 : 1;
	} else {
		/* no usable time source yet: stamped on arrival, like a sound card input */
		audio.timestamp = now_ns - util_mul_div64(audio.frames, 1000000000ULL, audio.samples_per_sec);
		state = 0;
	}
	if (clock_state != state) {
		clock_state = state;
		static const char *const names[] = {"the arrival time of the buffers",
						    "the mixer's time source, as last converted",
						    "the mixer's time source"};
		blog(LOG_INFO, "[haiku-media] '%s': time stamps from %s", obs_source_get_name(source), names[state]);
	}

	obs_source_output_audio(source, &audio);
}

/* What Desktop Audio leaves out: OBS's streams, which are its audio monitoring (libobs/audio-monitoring/haiku),
 * by the signature OBS is built with; HaikuRecorder::OpenLoopback() adds the one the registrar has for this team
 * if it differs (an executable run without its resources gets one made up by Qt). The browser helper
 * (application/x-vnd.obs-studio-browser-helper) is not left out: a browser source's page audio plays through
 * the system mixer and is meant to reach recordings through Desktop Audio, as on other platforms with "Control
 * audio via OBS" off. */
HaikuOpenParams desktop_audio_params()
{
	HaikuOpenParams params;
	params.loopback_exclude = OBS_APP_SIGNATURE "\n";
	params.loopback_exclude_self = true;
	return params;
}

void loopback_update(void *data, obs_data_t *settings)
{
	LoopbackAudio *la = static_cast<LoopbackAudio *>(data);
	/* no Media Kit work here: the capture's thread (re)opens the loopback */
	if (!la->per_application) {
		la->capture->Configure(std::string(), desktop_audio_params(), true);
		return;
	}

	/* An empty signature means "everything" to the loopback, which is Desktop Audio's job: without an
	 * application this source is silent. So is OBS itself: capturing its own monitoring playback would
	 * feed a monitored source back into itself. */
	std::string signature = obs_data_get_string(settings, S_APPLICATION);
	if (!signature.empty() && signature == HaikuOwnSignature()) {
		blog(LOG_WARNING, "[haiku-media] '%s': not capturing OBS's own audio", obs_source_get_name(la->source));
		signature.clear();
	}
	/* No exclusions: the loopback records the one application chosen, never OBS (refused above). */
	la->capture->Configure(signature, HaikuOpenParams(), !signature.empty());
}

void *loopback_create(obs_data_t *settings, obs_source_t *source, bool per_application)
{
	LoopbackAudio *la = new LoopbackAudio(source, per_application);
	la->capture = std::make_unique<HaikuCapture>(
		source, per_application ? "OBS Application Audio" : "OBS Desktop Audio", B_MEDIA_RAW_AUDIO,
		HaikuCaptureKind::Loopback,
		[la](const void *data, size_t size, const media_format &format, const media_header &header) {
			la->OnBuffer(data, size, format, header);
		},
		[la](const media_format &format, const HaikuOpenResult &) { la->OnOpened(format); }, nullptr);
	loopback_update(la, settings);
	return la;
}

void loopback_destroy(void *data)
{
	/* ~LoopbackAudio() stops the capture (supervisor and recorder threads) before the rest goes away */
	delete static_cast<LoopbackAudio *>(data);
}

/* The list of applications for the properties (below) */
void application_list_register(obs_source_t *source);
void application_list_unregister(obs_source_t *source);

/* ------------------------------------------------------------------------------------------------------ */
/* Desktop Audio                                                                                          */

const char *desktop_audio_get_name(void *)
{
	return T_("DesktopAudio");
}

void *desktop_audio_create(obs_data_t *settings, obs_source_t *source)
{
	return loopback_create(settings, source, false);
}

void desktop_audio_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_DEVICE_ID, "default");
}

obs_properties_t *desktop_audio_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *list = obs_properties_add_list(props, S_DEVICE_ID, T_("Device"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, T_("SystemMixer"), "default");
	obs_property_set_long_description(list, T_("SystemMixer.Description"));
	return props;
}

/* ------------------------------------------------------------------------------------------------------ */
/* Application Audio Capture                                                                              */

const char *application_audio_get_name(void *)
{
	return T_("ApplicationAudio");
}

void *application_audio_create(obs_data_t *settings, obs_source_t *source)
{
	void *data = loopback_create(settings, source, true);
	application_list_register(source);
	return data;
}

void application_audio_destroy(void *data)
{
	application_list_unregister(static_cast<LoopbackAudio *>(data)->source);
	loopback_destroy(data);
}

void application_audio_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, S_APPLICATION, "");
}

/*
 * The choices: applications that play something now (the system mixer's streams, by name), the other running
 * applications (they may start playing later; the loopback follows an application by its signature, also
 * across the application quitting and starting again: the mix thread compares every stream's signature with
 * the loopback's each cycle, MixerCore::_InputFade()), and the saved application if it is neither.
 *
 * Reading them takes round trips to the mixer (AudioStreamControl::Update() fetches its whole parameter web)
 * and to the registrar, which libobs's get_properties and button callbacks would make on the UI thread. So a
 * thread of this file keeps the list: it reads it when asked (a properties dialog opening with an old list,
 * the Refresh button) and every few seconds while an Application Audio source exists, and reloads the
 * sources' properties (obs_source_update_properties(), queued to the UI thread by the frontend) when the list
 * changed or Refresh was pressed. The properties are built from the last list, without waiting.
 */
struct ApplicationEntry {
	std::string signature;
	std::string name;
	bool playing; /* name: the stream's display name; else the application's file name */

	bool operator==(const ApplicationEntry &other) const
	{
		return signature == other.signature && name == other.name && playing == other.playing;
	}
};

/* Every few seconds while a source exists: streams come and go without a dialog being reopened */
const uint64_t kListRefreshInterval = 3000000000ULL;
/* A dialog opened within this time of the last read uses that list without asking for another */
const uint64_t kListFreshFor = 2000000000ULL;

class ApplicationList {
public:
	void Register(obs_source_t *source)
	{
		std::lock_guard<std::mutex> lock(mutex);
		sources.push_back(source);
		if (!stop && !thread.joinable()) {
			try {
				thread = std::thread([this]() { Run(); });
			} catch (const std::system_error &error) {
				blog(LOG_ERROR, "[haiku-media] cannot start the application list thread: %s",
				     error.what());
			}
		}
		requested = true;
		cond.notify_all();
	}

	/* Before the source goes away: afterwards the thread doesn't reload its properties any more */
	void Unregister(obs_source_t *source)
	{
		std::lock_guard<std::mutex> lock(mutex);
		sources.erase(std::remove(sources.begin(), sources.end(), source), sources.end());
	}

	/* From get_properties: the list as it is; asks for a new one if it is old. */
	std::vector<ApplicationEntry> Get()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!have_list || os_gettime_ns() - read_at >= kListFreshFor) {
			requested = true;
			cond.notify_all();
		}
		return entries;
	}

	/* The Refresh button: read now, and reload the properties when done even if nothing changed */
	void Refresh()
	{
		std::lock_guard<std::mutex> lock(mutex);
		requested = true;
		notify_always = true;
		cond.notify_all();
	}

	/* obs_module_unload(): every source is gone by then; waits for at most one read */
	void Shutdown()
	{
		std::thread worker;
		{
			std::lock_guard<std::mutex> lock(mutex);
			stop = true;
			worker = std::move(thread);
		}
		cond.notify_all();
		if (worker.joinable())
			worker.join();
	}

private:
	void Run()
	{
		os_set_thread_name("haiku-app-audio-list");
		/* The object is not thread safe: this thread is its only user */
		BPrivate::AudioStreamControl control;

		std::unique_lock<std::mutex> lock(mutex);
		while (!stop) {
			if (!requested) {
				if (sources.empty())
					cond.wait(lock);
				else
					cond.wait_for(lock, std::chrono::nanoseconds(kListRefreshInterval));
				if (stop)
					break;
				if (!requested && sources.empty())
					continue;
			}
			requested = false;
			const bool always = notify_always;
			notify_always = false;

			lock.unlock();
			std::vector<ApplicationEntry> fresh = Read(control);
			lock.lock();

			const bool changed = !have_list || fresh != entries;
			entries = std::move(fresh);
			have_list = true;
			read_at = os_gettime_ns();
			if (changed || always) {
				/* Under the lock: Unregister() can't let a source go meanwhile. The frontend queues the
				 * reload to the UI thread, so this doesn't call back into Get(). */
				for (obs_source_t *source : sources)
					obs_source_update_properties(source);
			}
		}
	}

	static std::vector<ApplicationEntry> Read(BPrivate::AudioStreamControl &control)
	{
		const std::string self = HaikuOwnSignature();
		std::vector<ApplicationEntry> list;

		auto wanted = [&](const char *signature) {
			return signature && *signature && self != signature &&
			       std::find_if(list.begin(), list.end(), [&](const ApplicationEntry &entry) {
				       return entry.signature == signature;
			       }) == list.end();
		};

		/* Each stream is a group of the mixer's parameter web: its display name, and the application's
		 * signature as a hidden parameter of kind "renga:app_signature". Streams of the media add-on
		 * server and of programs without a signature have none and cannot be told apart by the loopback. */
		if (BMediaRoster::IsRunning() && control.Update() == B_OK) {
			for (int32 i = 0; i < control.CountStreams(); i++) {
				const BPrivate::AudioStream *stream = control.StreamAt(i);
				if (wanted(stream->signature.String()))
					list.push_back({stream->signature.String(), stream->name.String(), true});
			}
		}

		if (be_roster) {
			BList teams;
			be_roster->GetAppList(&teams);
			for (int32 i = 0; i < teams.CountItems(); i++) {
				app_info app;
				if (be_roster->GetRunningAppInfo((team_id)(addr_t)teams.ItemAt(i), &app) != B_OK)
					continue;
				/* servers and other programs without a face are only listed while they play (above) */
				if ((app.flags & B_BACKGROUND_APP) || !wanted(app.signature))
					continue;
				list.push_back({app.signature, app.ref.name ? app.ref.name : app.signature, false});
			}
		}
		return list;
	}

	std::mutex mutex;
	std::condition_variable cond;
	std::vector<obs_source_t *> sources; /* the Application Audio sources */
	std::vector<ApplicationEntry> entries;
	bool have_list = false;
	uint64_t read_at = 0;
	bool requested = false;
	bool notify_always = false;
	bool stop = false;
	std::thread thread;
};

/* Never destroyed: obs_module_unload() joins its thread; a static destructor must not find it joinable */
ApplicationList &application_list()
{
	static ApplicationList *list = new ApplicationList;
	return *list;
}

void application_list_register(obs_source_t *source)
{
	application_list().Register(source);
}

void application_list_unregister(obs_source_t *source)
{
	application_list().Unregister(source);
}

void fill_application_list(obs_property_t *list, const char *current)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, T_("Application.Select"), "");

	struct dstr label = {0};
	bool current_listed = false;
	for (const ApplicationEntry &entry : application_list().Get()) {
		if (entry.playing) {
			dstr_printf(&label, T_("Application.Playing"), entry.name.c_str());
			obs_property_list_add_string(list, label.array, entry.signature.c_str());
		} else {
			obs_property_list_add_string(list, entry.name.c_str(), entry.signature.c_str());
		}
		current_listed = current_listed || entry.signature == current;
	}

	if (current && *current && !current_listed && HaikuOwnSignature() != current) {
		dstr_printf(&label, T_("Application.NotRunning"), current);
		obs_property_list_add_string(list, label.array, current);
	}
	dstr_free(&label);
}

bool application_audio_refresh(obs_properties_t *, obs_property_t *, void *)
{
	/* Read on the list's thread; it reloads the properties when it has the new list */
	application_list().Refresh();
	return false;
}

obs_properties_t *application_audio_properties(void *data)
{
	LoopbackAudio *la = static_cast<LoopbackAudio *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_APPLICATION, T_("Application"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(list, T_("Application.Description"));
	obs_properties_add_button2(props, S_REFRESH, T_("Refresh"), application_audio_refresh, data);

	std::string current;
	if (la) {
		obs_data_t *settings = obs_source_get_settings(la->source);
		current = obs_data_get_string(settings, S_APPLICATION);
		obs_data_release(settings);
	}
	fill_application_list(list, current.c_str());
	return props;
}

} // namespace

void haiku_register_desktop_audio(void)
{
	struct obs_source_info info = {};
	/* the id the frontend creates its global Desktop Audio devices with (OUTPUT_AUDIO_SOURCE in
	 * frontend/OBSApp.cpp) and that existing scene collections name */
	info.id = "haiku_desktop_audio_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	/* DO_NOT_SELF_MONITOR marks it as an "audio output capture" for libobs, which then asks the platform's
	 * devices_match() whether monitoring plays into it (feedback, and deduplication of monitored sources).
	 * On Renga it never does, OBS's streams being excluded (libobs/audio-monitoring/haiku/haiku-output.c). */
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR;
	info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
	info.get_name = desktop_audio_get_name;
	info.create = desktop_audio_create;
	info.destroy = loopback_destroy;
	info.update = loopback_update;
	info.get_defaults = desktop_audio_defaults;
	info.get_properties = desktop_audio_properties;
	obs_register_source(&info);
}

void haiku_register_application_audio(void)
{
	struct obs_source_info info = {};
	info.id = "haiku_application_audio_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	/* Monitoring it is safe: the monitor plays as OBS's stream, which this source never records. */
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT;
	info.get_name = application_audio_get_name;
	info.create = application_audio_create;
	info.destroy = application_audio_destroy;
	info.update = loopback_update;
	info.get_defaults = application_audio_defaults;
	info.get_properties = application_audio_properties;
	obs_register_source(&info);
}

void haiku_application_audio_shutdown(void)
{
	application_list().Shutdown();
}
