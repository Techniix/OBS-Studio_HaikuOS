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
 * "Desktop Audio (Media Kit)": captures what the system mixer plays.
 *
 * The capture itself happens in the obs-haiku-audio-tap helper (see audio-tap/tap-app.cpp), which this source
 * starts on demand. The helper copies the mixer output into a cloneable area; a reader thread here clones
 * that area, converts the ring contents to obs_source_audio and keeps the helper alive with a heartbeat.
 * If the helper dies or the area goes away the reader detaches and starts it again.
 */

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/util_uint64.h>

#include <AppFileInfo.h>
#include <File.h>
#include <MediaDefs.h>
#include <OS.h>
#include <Roster.h>

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio-channels.hpp"
#include "audio-tap/audio-tap-shm.h"

extern char **environ;

#define T_(text) obs_module_text(text)

#define HELPER_NAME "obs-haiku-audio-tap"

/* read granularity: 10 ms keeps latency low without burning CPU */
#define POLL_INTERVAL_US 10000
/* how long to wait for a freshly started helper before trying again */
#define HELPER_START_TIMEOUT_US 3000000

namespace {

/* Number of desktop audio sources alive; the last one to go stops the helper so playback goes straight to
 * the card again immediately instead of after the heartbeat timeout. */
std::atomic_int active_sources{0};
std::mutex spawn_mutex;
pid_t helper_pid = -1;

std::string helper_path()
{
	std::string path;
	if (char *p = os_get_executable_path_ptr(HELPER_NAME)) {
		path = p;
		bfree(p);
	}
	if (path.empty() || access(path.c_str(), X_OK) != 0)
		path = "/boot/home/config/non-packaged/bin/" HELPER_NAME;
	return path;
}

/* Background app (no Deskbar entry), multiple launch. Executables built without resources carry no app flags;
 * they live in a file attribute, so setting them once per installation is enough. Not B_SINGLE_LAUNCH: the
 * registrar would turn away a helper started while the previous one is still restoring the routing, and
 * `--repair` while a helper runs; the helper has its own single-instance lock (tap-app.cpp). */
void mark_background_app(const std::string &path)
{
	static bool done = false;
	if (done)
		return;
	done = true;
	BFile file(path.c_str(), B_READ_WRITE);
	BAppFileInfo info(&file);
	if (file.InitCheck() != B_OK || info.InitCheck() != B_OK)
		return;
	const uint32 wanted = B_MULTIPLE_LAUNCH | B_BACKGROUND_APP;
	const uint32 managed = B_LAUNCH_MASK | B_BACKGROUND_APP;
	uint32 flags = 0;
	if (info.GetAppFlags(&flags) != B_OK)
		flags = 0;
	else if ((flags & managed) == wanted)
		return;
	/* also rewrites the B_SINGLE_LAUNCH flags that older builds of this plugin set */
	info.SetAppFlags((flags & ~managed) | wanted);
}

/* spawn_mutex held */
void reap_helper_locked()
{
	if (helper_pid > 0 && waitpid(helper_pid, nullptr, WNOHANG) == helper_pid)
		helper_pid = -1;
}

void reap_helper()
{
	std::lock_guard<std::mutex> lock(spawn_mutex);
	reap_helper_locked();
}

bool spawn_helper()
{
	std::lock_guard<std::mutex> lock(spawn_mutex);
	reap_helper_locked(); /* a zombie still passes kill(pid, 0) */
	if (helper_pid > 0 && kill(helper_pid, 0) == 0)
		return true; /* still starting up */

	const std::string path = helper_path();
	mark_background_app(path);
	char *argv[] = {const_cast<char *>(path.c_str()), nullptr};
	pid_t pid = -1;
	int err = posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv, environ);
	if (err != 0) {
		blog(LOG_WARNING, "[haiku-media] desktop audio: could not start %s: %s", path.c_str(), strerror(err));
		return false;
	}
	helper_pid = pid;
	blog(LOG_INFO, "[haiku-media] desktop audio: started %s (team %d)", path.c_str(), (int)pid);
	return true;
}

audio_format obs_format_for(uint32_t format)
{
	switch (format) {
	case media_raw_audio_format::B_AUDIO_FLOAT:
		return AUDIO_FORMAT_FLOAT;
	case media_raw_audio_format::B_AUDIO_INT:
		return AUDIO_FORMAT_32BIT;
	case media_raw_audio_format::B_AUDIO_SHORT:
		return AUDIO_FORMAT_16BIT;
	case media_raw_audio_format::B_AUDIO_UCHAR:
	case media_raw_audio_format::B_AUDIO_CHAR:
		return AUDIO_FORMAT_U8BIT;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

/* The mixer format as published by the helper, copied out of the shared area in one consistent piece */
struct TapFormat {
	float frame_rate;
	uint32_t channels;
	uint32_t format;
	uint32_t byte_order;
	uint32_t channel_mask;
};

struct DesktopAudio {
	obs_source_t *source = nullptr;
	std::thread thread;
	std::atomic_bool stop{false};

	area_id clone = -1;
	const obs_tap_shm *shm = nullptr;
	int64_t *heartbeat = nullptr;
	int64_t read_pos = 0;
	bigtime_t attached_at = 0;
	bool warned_protocol = false;

	/* cached when format_serial changes; Pump() never re-reads the shared format fields */
	int32_t format_serial = -1;
	bool format_ok = false;
	bool warned_format = false;
	uint32_t rate = 0;
	size_t sample_size = 0;
	size_t frame_size = 0;
	audio_format obs_format = AUDIO_FORMAT_UNKNOWN;
	bool signed_8bit = false;
	AudioChannelMap channel_map;

	std::vector<uint8_t> chunk;
	std::vector<uint8_t> packed;

	void Run();
	bool Attach();
	void Detach();
	bool ReadFormat(int32_t *serial, TapFormat *fmt) const;
	void UpdateFormat();
	void Pump();
};

bool DesktopAudio::Attach()
{
	area_id area = find_area(OBS_TAP_AREA_NAME);
	if (area < 0)
		return false;

	void *addr = nullptr;
	clone = clone_area("obs desktop audio reader", &addr, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, area);
	if (clone < 0) {
		blog(LOG_WARNING, "[haiku-media] desktop audio: clone_area failed: %s", strerror(clone));
		clone = -1;
		return false;
	}

	obs_tap_shm *mapped = static_cast<obs_tap_shm *>(addr);
	/* the helper stores the magic last (release): seeing it means the header is initialized. Areas are at
	 * least a page, so the header fields can be read before the size is known to be right. */
	area_info ainfo;
	const bool size_ok = get_area_info(clone, &ainfo) == B_OK && ainfo.size >= obs_tap_area_size();
	const uint32_t magic = __atomic_load_n(&mapped->magic, __ATOMIC_ACQUIRE);
	if (magic != OBS_TAP_MAGIC || mapped->version != OBS_TAP_VERSION || mapped->ring_size != OBS_TAP_RING_SIZE ||
	    !size_ok) {
		/* magic 0: the helper is still setting up. Otherwise a helper from another build; it is not fed
		 * heartbeats, so it gives up within OBS_TAP_CLIENT_TIMEOUT and a current one takes over. */
		if (magic != 0 && !warned_protocol) {
			blog(LOG_WARNING,
			     "[haiku-media] desktop audio: helper protocol mismatch (magic %08x, version %u, "
			     "expected %u); waiting for it to exit",
			     magic, mapped->version, OBS_TAP_VERSION);
			warned_protocol = true;
		}
		delete_area(clone);
		clone = -1;
		return false;
	}
	warned_protocol = false;

	shm = mapped;
	heartbeat = &mapped->client_heartbeat;
	__atomic_store_n(heartbeat, (int64_t)system_time(), __ATOMIC_RELEASE);
	attached_at = system_time();
	read_pos = __atomic_load_n(&shm->write_pos, __ATOMIC_ACQUIRE); /* start with what is played from now on */
	format_serial = -1;
	format_ok = false;
	blog(LOG_INFO, "[haiku-media] '%s': attached to the desktop audio tap", obs_source_get_name(source));
	return true;
}

void DesktopAudio::Detach()
{
	if (clone >= 0) {
		delete_area(clone);
		clone = -1;
	}
	shm = nullptr;
	heartbeat = nullptr;
}

/* seqlock reader (see audio-tap-shm.h) */
bool DesktopAudio::ReadFormat(int32_t *serial, TapFormat *fmt) const
{
	for (int tries = 0; tries < 64; tries++) {
		const int32_t s1 = __atomic_load_n(&shm->format_serial, __ATOMIC_ACQUIRE);
		if (s1 & 1)
			continue; /* the writer is in the middle of an update; it only takes a few stores */
		fmt->frame_rate = shm->frame_rate;
		fmt->channels = shm->channels;
		fmt->format = shm->format;
		fmt->byte_order = shm->byte_order;
		fmt->channel_mask = shm->channel_mask;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&shm->format_serial, __ATOMIC_RELAXED) == s1) {
			*serial = s1;
			return true;
		}
	}
	return false;
}

void DesktopAudio::UpdateFormat()
{
	int32_t serial;
	TapFormat f;
	if (!ReadFormat(&serial, &f))
		return; /* try again on the next poll */

	format_serial = serial;
	obs_format = obs_format_for(f.format);
	sample_size = f.format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
	signed_8bit = f.format == media_raw_audio_format::B_AUDIO_CHAR;
	const bool rate_ok = f.frame_rate >= 1.0f && f.frame_rate <= 1000000.0f; /* also false for NaN */
	rate = rate_ok ? (uint32_t)f.frame_rate : 0;
	const bool channels_ok = f.channels >= 1 && f.channels <= 8;
	channel_map = channels_ok ? haiku_audio_channel_map(f.channels, f.channel_mask) : AudioChannelMap();
	frame_size = sample_size * (channels_ok ? f.channels : 0);

	format_ok = obs_format != AUDIO_FORMAT_UNKNOWN && sample_size > 0 && channels_ok && rate_ok &&
		    channel_map.Valid() && f.byte_order == B_MEDIA_HOST_ENDIAN;

	/* serial 0 is the zeroed header before the tap's first connection: nothing to complain about yet */
	if (!format_ok && serial != 0 && !warned_format) {
		blog(LOG_WARNING,
		     "[haiku-media] desktop audio: unsupported mixer format (format 0x%x, %u channels, %.0f Hz, "
		     "byte order %u)",
		     f.format, f.channels, f.frame_rate, f.byte_order);
		warned_format = true;
	}
	if (format_ok) {
		warned_format = false;
		if (channel_map.dropped_mask)
			blog(LOG_WARNING,
			     "[haiku-media] desktop audio: channel mask 0x%x has channels OBS has no slot for (0x%x); "
			     "they are dropped",
			     f.channel_mask, channel_map.dropped_mask);
		if (!channel_map.identity)
			blog(LOG_INFO,
			     "[haiku-media] desktop audio: %u channels (mask 0x%x) are remapped to an OBS %u-channel "
			     "layout",
			     f.channels, f.channel_mask, channel_map.out_channels);
	}

	/* bytes of the old format are meaningless now */
	read_pos = __atomic_load_n(&shm->write_pos, __ATOMIC_ACQUIRE);
}

void DesktopAudio::Pump()
{
	__atomic_store_n(heartbeat, (int64_t)system_time(), __ATOMIC_RELEASE);

	if (__atomic_load_n(&shm->format_serial, __ATOMIC_ACQUIRE) != format_serial) {
		UpdateFormat();
		return;
	}
	if (!format_ok)
		return;

	/* The ring size is checked at Attach(). The writer copies at most OBS_TAP_MAX_WRITE bytes before it
	 * publishes them, so bytes up to that far beyond write_pos may already be changing. */
	const int64_t ring = OBS_TAP_RING_SIZE;
	const int64_t safe = ring - OBS_TAP_MAX_WRITE;
	const int64_t write_pos = __atomic_load_n(&shm->write_pos, __ATOMIC_ACQUIRE);

	int64_t avail = write_pos - read_pos;
	if (avail < 0 || avail > safe) {
		/* we fell behind (or the helper restarted): skip ahead instead of reading overwritten data */
		read_pos = write_pos;
		return;
	}

	avail -= avail % (int64_t)frame_size;
	if (avail <= 0)
		return;

	chunk.resize((size_t)avail);
	const size_t offset = (size_t)(read_pos % ring);
	const size_t first = std::min((size_t)avail, (size_t)ring - offset);
	memcpy(chunk.data(), shm->ring + offset, first);
	if (first < (size_t)avail)
		memcpy(chunk.data() + first, shm->ring, (size_t)avail - first);

	/* the writer may have lapped us while copying, or the format may have changed under us */
	const int64_t write_after = __atomic_load_n(&shm->write_pos, __ATOMIC_ACQUIRE);
	if (write_after - read_pos > safe) {
		read_pos = write_after;
		return;
	}
	if (__atomic_load_n(&shm->format_serial, __ATOMIC_ACQUIRE) != format_serial)
		return; /* UpdateFormat() on the next poll drops these bytes */
	read_pos += avail;

	const uint32_t frames = (uint32_t)(avail / (int64_t)frame_size);
	const uint8_t *samples = chunk.data();
	if (!channel_map.identity) {
		/* Haiku channel order -> OBS layout, converting signed 8-bit on the way */
		packed.resize((size_t)frames * channel_map.out_channels * sample_size);
		haiku_repack_channels(chunk.data(), packed.data(), frames, sample_size, channel_map, signed_8bit);
		samples = packed.data();
	} else if (signed_8bit) {
		for (uint8_t &b : chunk)
			b ^= 0x80;
	}

	struct obs_source_audio audio = {};
	audio.data[0] = samples;
	audio.frames = frames;
	audio.speakers = channel_map.layout;
	audio.format = obs_format;
	audio.samples_per_sec = rate;
	audio.timestamp = os_gettime_ns() - util_mul_div64(frames, 1000000000ULL, rate);
	obs_source_output_audio(source, &audio);
}

void DesktopAudio::Run()
{
	os_set_thread_name("haiku-desktop-audio");

	bigtime_t last_spawn = 0;
	/* grows while the helper keeps failing (no sound card, media server down) so the log isn't flooded */
	bigtime_t spawn_interval = HELPER_START_TIMEOUT_US;
	while (!stop) {
		if (!shm) {
			reap_helper();
			if (!Attach()) {
				const bigtime_t now = system_time();
				if (now - last_spawn > spawn_interval) {
					if (last_spawn)
						spawn_interval = std::min<bigtime_t>(spawn_interval * 2, 60000000);
					spawn_helper();
					last_spawn = now;
				}
				snooze(100000);
				continue;
			}
		}

		const int32_t team = shm->helper_team;
		if (team > 0 && kill(team, 0) != 0 && errno == ESRCH) {
			/* killed without cleanup; the next helper repairs the mixer connection */
			blog(LOG_WARNING, "[haiku-media] '%s': desktop audio helper died", obs_source_get_name(source));
			Detach();
			continue;
		}

		/* not (or no longer) inserted; allow a freshly started helper a moment to insert the tap */
		const bool alive = __atomic_load_n(&shm->helper_alive, __ATOMIC_ACQUIRE) != 0;
		if (!alive && system_time() - attached_at > 2000000) {
			/* the helper is shutting down (or failed to insert the tap) */
			blog(LOG_INFO, "[haiku-media] '%s': desktop audio tap went away", obs_source_get_name(source));
			Detach();
			continue;
		}

		if (alive)
			spawn_interval = HELPER_START_TIMEOUT_US;

		area_info info;
		if (get_area_info(clone, &info) != B_OK) {
			Detach();
			continue;
		}

		Pump();
		snooze(POLL_INTERVAL_US);
	}

	Detach();
}

const char *desktop_audio_get_name(void *)
{
	return T_("DesktopAudio");
}

/* The frontend's Settings > Audio > Desktop Audio list reads a "device_id" list from the platform's output
 * capture source; Haiku has a single system mixer, so the only device is "default". */
#define S_DEVICE_ID "device_id"

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
	return props;
}

void *desktop_audio_create(obs_data_t *, obs_source_t *source)
{
	DesktopAudio *da = new DesktopAudio;
	da->source = source;
	active_sources++;
	da->thread = std::thread([da]() { da->Run(); });
	return da;
}

void desktop_audio_destroy(void *data)
{
	DesktopAudio *da = static_cast<DesktopAudio *>(data);
	da->stop = true;
	if (da->thread.joinable())
		da->thread.join();

	if (--active_sources == 0) {
		/* ask the helper to restore the mixer -> sound card connection right away */
		area_id area = find_area(OBS_TAP_AREA_NAME);
		void *addr = nullptr;
		area_id c = area >= 0 ? clone_area("obs desktop audio stop", &addr, B_ANY_ADDRESS, B_READ_AREA, area)
				      : -1;
		if (c >= 0) {
			const int32_t team = static_cast<obs_tap_shm *>(addr)->helper_team;
			if (team > 0)
				kill(team, SIGTERM);
			delete_area(c);
		}
		/* Reap our helper so it doesn't linger as a zombie, but don't hold up the caller for long: give it a
		 * moment to restore the routing and exit, otherwise the next source's thread (reap_helper() in
		 * Run()) or the next spawn_helper() collects it. No detached reaper thread: it could outlive this
		 * module's code at shutdown. */
		for (int i = 0; i < 50; i++) {
			{
				std::lock_guard<std::mutex> lock(spawn_mutex);
				reap_helper_locked();
				if (helper_pid <= 0)
					break;
			}
			snooze(10000);
		}
	}

	delete da;
}

} // namespace

void haiku_register_desktop_audio(void)
{
	struct obs_source_info info = {};
	info.id = "haiku_desktop_audio_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	/* DO_NOT_SELF_MONITOR: monitoring plays into the same mixer the tap captures; without it monitoring
	 * desktop audio would feed back into itself */
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR;
	info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
	info.get_name = desktop_audio_get_name;
	info.create = desktop_audio_create;
	info.destroy = desktop_audio_destroy;
	info.get_defaults = desktop_audio_defaults;
	info.get_properties = desktop_audio_properties;
	obs_register_source(&info);
}
