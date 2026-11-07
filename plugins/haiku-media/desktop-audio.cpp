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

/* Background app (no Deskbar entry). Executables built without resources carry no app flags; they live in a
 * file attribute, so setting them once per installation is enough. */
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
	uint32 flags = 0;
	if (info.GetAppFlags(&flags) != B_OK || !(flags & B_BACKGROUND_APP))
		info.SetAppFlags(B_SINGLE_LAUNCH | B_BACKGROUND_APP);
}

void reap_helper()
{
	std::lock_guard<std::mutex> lock(spawn_mutex);
	if (helper_pid > 0 && waitpid(helper_pid, nullptr, WNOHANG) == helper_pid)
		helper_pid = -1;
}

bool spawn_helper()
{
	std::lock_guard<std::mutex> lock(spawn_mutex);
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

speaker_layout speakers_for(uint32_t channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
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

struct DesktopAudio {
	obs_source_t *source = nullptr;
	std::thread thread;
	std::atomic_bool stop{false};

	area_id clone = -1;
	const obs_tap_shm *shm = nullptr;
	volatile int64_t *heartbeat = nullptr;
	int64_t read_pos = 0;
	int32_t format_serial = -1;
	bool format_ok = false;
	bool warned_format = false;
	std::vector<uint8_t> chunk;

	void Run();
	bool Attach();
	void Detach();
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
	if (mapped->magic != OBS_TAP_MAGIC || mapped->version != OBS_TAP_VERSION ||
	    mapped->ring_size != OBS_TAP_RING_SIZE) {
		blog(LOG_WARNING, "[haiku-media] desktop audio: helper protocol mismatch (magic %08x, version %u)",
		     mapped->magic, mapped->version);
		delete_area(clone);
		clone = -1;
		return false;
	}

	shm = mapped;
	heartbeat = &mapped->client_heartbeat;
	*heartbeat = system_time();
	read_pos = shm->write_pos; /* start with what is played from now on */
	format_serial = -1;
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

void DesktopAudio::Pump()
{
	*heartbeat = system_time();

	const int32_t serial = shm->format_serial;
	if (serial != format_serial) {
		format_serial = serial;
		format_ok = obs_format_for(shm->format) != AUDIO_FORMAT_UNKNOWN &&
			    speakers_for(shm->channels) != SPEAKERS_UNKNOWN && shm->frame_rate > 0.0f &&
			    shm->byte_order == B_MEDIA_HOST_ENDIAN;
		if (!format_ok && !warned_format) {
			blog(LOG_WARNING,
			     "[haiku-media] desktop audio: unsupported mixer format (format 0x%x, %u channels, %.0f Hz)",
			     shm->format, shm->channels, shm->frame_rate);
			warned_format = true;
		}
		/* bytes of the old format are meaningless now */
		read_pos = shm->write_pos;
		return;
	}
	if (!format_ok)
		return;

	__sync_synchronize();
	const int64_t write_pos = shm->write_pos;
	const uint32_t ring = shm->ring_size;
	const uint32_t channels = shm->channels;
	const size_t sample_size = shm->format & media_raw_audio_format::B_AUDIO_SIZE_MASK;
	const size_t frame_size = sample_size * channels;
	const uint32_t rate = (uint32_t)shm->frame_rate;

	int64_t avail = write_pos - read_pos;
	if (avail < 0 || avail > (int64_t)ring / 2) {
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

	/* the writer may have lapped us while copying */
	__sync_synchronize();
	if (shm->write_pos - read_pos > (int64_t)ring) {
		read_pos = shm->write_pos;
		return;
	}
	read_pos += avail;

	if (shm->format == media_raw_audio_format::B_AUDIO_CHAR)
		for (uint8_t &b : chunk)
			b ^= 0x80;

	const uint32_t frames = (uint32_t)(avail / (int64_t)frame_size);

	struct obs_source_audio audio = {};
	audio.data[0] = chunk.data();
	audio.frames = frames;
	audio.speakers = speakers_for(channels);
	audio.format = obs_format_for(shm->format);
	audio.samples_per_sec = rate;
	audio.timestamp = os_gettime_ns() - util_mul_div64(frames, 1000000000ULL, rate);
	obs_source_output_audio(source, &audio);
}

void DesktopAudio::Run()
{
	os_set_thread_name("haiku-desktop-audio");

	bigtime_t last_spawn = 0;
	while (!stop) {
		if (!shm) {
			reap_helper();
			if (!Attach()) {
				const bigtime_t now = system_time();
				if (now - last_spawn > HELPER_START_TIMEOUT_US) {
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

		if (!shm->helper_alive && system_time() - *heartbeat > 1000000) {
			/* the helper is shutting down (or failed to insert the tap) */
			blog(LOG_INFO, "[haiku-media] '%s': desktop audio tap went away", obs_source_get_name(source));
			Detach();
			continue;
		}

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
		reap_helper();
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
