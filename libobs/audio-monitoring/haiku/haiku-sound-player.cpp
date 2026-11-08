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
 * Two kinds of player:
 *
 * - System mixer ("default"): one BSoundPlayer per monitored source, connected to the mixer, which mixes,
 *   resamples and plays to the output chosen in Media preferences.
 *
 * - Hardware output: the player connects straight to a physical output node, bypassing the mixer.
 *   multi_audio offers one input per bus (one for a stereo card) and accepts only its hardware format,
 *   so all monitored sources on a device share one BSoundPlayer (struct device_output); its buffer hook
 *   mixes the sources' float audio and converts to the device's sample format.
 */

#include "haiku-sound-player.h"

#include "../../util/base.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <string.h>

#include <MediaAddOn.h>
#include <MediaDefs.h>
#include <MediaRoster.h>
#include <SoundPlayer.h>

/* Ids of hardware outputs carry a prefix so a device can never collide with "default". */
#define DEVICE_ID_PREFIX "node:"
#define MAX_DEVICES 64
/* Frames mixed per pass in a device's buffer hook */
#define MIX_CHUNK_FRAMES 512

struct device_output;

struct obs_haiku_sound_player {
	obs_haiku_fill_cb cb;
	void *param;

	/* exactly one of these */
	BSoundPlayer *player;  /* system mixer */
	device_output *device; /* shared hardware output */

	uint32 channels; /* system mixer: the channel count cb produces */
};

struct device_output {
	std::string id;
	BSoundPlayer *player = NULL;
	media_node node;
	bool release_node = false;
	int refs = 0; /* players created on it (guarded by registry_mutex) */

	/* negotiated connection format */
	uint32 sample_format = 0;
	uint32 channels = 0;
	size_t sample_size = 0;

	/* players currently started on it; the buffer hook holds clients_mutex while calling them */
	std::mutex clients_mutex;
	std::vector<obs_haiku_sound_player *> clients;
	std::vector<float> mix, scratch; /* buffer hook only */
};

/* open_devices holds the usable entries by id. An entry found dead is removed from it but lives on until
 * its last player releases it (release_device() then closes it), so players never see it freed. */
static std::mutex registry_mutex;
static std::condition_variable registry_cv; /* signalled when an id leaves opening_ids */
static std::vector<device_output *> open_devices;
static std::vector<std::string> opening_ids; /* being opened by some thread, registry_mutex not held */

static bool is_opening(const std::string &id)
{
	return std::find(opening_ids.begin(), opening_ids.end(), id) != opening_ids.end();
}

/* ------------------------------------------------------------------------- */
/* Device list                                                               */

struct output_device {
	dormant_node_info info;
	std::string id;
	std::string name;
};

/* The physical audio outputs, as listed by Haiku's Media preferences (dormant B_PHYSICAL_OUTPUT nodes that
 * consume raw audio), each with a stable id. */
static std::vector<output_device> list_output_devices(BMediaRoster *roster)
{
	std::vector<output_device> devices;

	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = media_raw_audio_format::wildcard;

	std::vector<dormant_node_info> nodes(MAX_DEVICES);
	int32 count = MAX_DEVICES;
	if (roster->GetDormantNodes(nodes.data(), &count, &format, NULL, NULL, B_BUFFER_CONSUMER | B_PHYSICAL_OUTPUT) !=
		    B_OK ||
	    count < 0)
		return devices;
	count = std::min<int32>(count, MAX_DEVICES);

	/* Node ids change on every boot; names don't. Number repeated names (two identical USB headsets) in
	 * the order the add-ons report them. */
	std::map<std::string, int> seen;
	for (int32 i = 0; i < count; i++) {
		output_device dev;
		dev.info = nodes[i];
		dev.name = nodes[i].name;
		dev.id = DEVICE_ID_PREFIX + dev.name;

		int n = ++seen[dev.name];
		if (n > 1) {
			dev.id += "#" + std::to_string(n);
			dev.name += " (" + std::to_string(n) + ")";
		}

		devices.push_back(dev);
	}

	return devices;
}

/* Gets the running instance of a device node, if any. The caller must ReleaseNode() it. */
static bool get_running_instance(BMediaRoster *roster, const dormant_node_info &info, media_node *node)
{
	media_node_id id;
	int32 count = 1;
	return roster->GetInstancesFor(info.addon, info.flavor_id, &id, &count) == B_OK && count > 0 &&
	       roster->GetNodeFor(id, node) == B_OK;
}

/* The node the system mixer plays to (Media preferences' "Audio output"). */
static bool is_system_output(BMediaRoster *roster, const media_node &node)
{
	media_node output;
	if (roster->GetAudioOutput(&output) != B_OK)
		return false;

	bool same = output.node == node.node;
	roster->ReleaseNode(output);
	return same;
}

static bool has_free_input(BMediaRoster *roster, const media_node &node)
{
	media_input input;
	int32 count = 0;
	return roster->GetFreeInputsFor(node, &input, 1, &count, B_MEDIA_RAW_AUDIO) == B_OK && count > 0;
}

static bool device_is_open(const std::string &id)
{
	std::lock_guard<std::mutex> lock(registry_mutex);
	if (is_opening(id))
		return true;
	for (device_output *d : open_devices) {
		if (d->id == id)
			return true;
	}
	return false;
}

extern "C" void obs_haiku_enum_output_devices(obs_haiku_device_cb cb, void *param)
{
	/* Without the media server every roster call would wait for its timeout; this check is local. */
	if (!BMediaRoster::IsRunning())
		return;

	BMediaRoster *roster = BMediaRoster::Roster();
	if (!roster)
		return;

	for (const output_device &dev : list_output_devices(roster)) {
		/* A device nobody has opened is free, and one OBS already plays to stays listed. Another running
		 * one is skipped when the mixer owns it (playing to one of its other inputs would hit a different
		 * bus, e.g. the rear speakers) or when all of its inputs are taken (the connection would fail).
		 * Dormant devices are not instantiated here, so listing never opens hardware. */
		media_node node;
		if (!device_is_open(dev.id) && get_running_instance(roster, dev.info, &node)) {
			bool usable = (node.kind & B_BUFFER_CONSUMER) != 0 && !is_system_output(roster, node) &&
				      has_free_input(roster, node);
			roster->ReleaseNode(node);
			if (!usable)
				continue;
		}

		if (!cb(param, dev.name.c_str(), dev.id.c_str()))
			break;
	}
}

/* ------------------------------------------------------------------------- */
/* Fallback logging                                                          */

static std::mutex warn_mutex;
static std::string warned_id;

/* Logs a fallback to the mixer once per device (until that device works again): every monitored source
 * and every monitoring reset would otherwise repeat it. */
static void warn_fallback(const char *id, const char *reason)
{
	std::lock_guard<std::mutex> lock(warn_mutex);
	if (warned_id == id)
		return;

	warned_id = id;
	blog(LOG_WARNING, "[audio monitoring] Device '%s' %s; monitoring through the system mixer instead", id,
	     reason);
}

static void device_ok(const char *id)
{
	std::lock_guard<std::mutex> lock(warn_mutex);
	if (warned_id == id)
		warned_id.clear();
}

/* ------------------------------------------------------------------------- */
/* Hardware output (shared)                                                  */

static inline float clamp_sample(float v)
{
	return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

static void convert_samples(uint32 format, const float *src, size_t count, void *dst)
{
	size_t i;

	switch (format) {
	case media_raw_audio_format::B_AUDIO_FLOAT:
		for (i = 0; i < count; i++)
			((float *)dst)[i] = clamp_sample(src[i]);
		break;
	case media_raw_audio_format::B_AUDIO_INT:
		/* 2147483647 isn't representable as a float; scale in double */
		for (i = 0; i < count; i++)
			((int32 *)dst)[i] = (int32)((double)clamp_sample(src[i]) * 2147483647.0);
		break;
	case media_raw_audio_format::B_AUDIO_SHORT:
		for (i = 0; i < count; i++)
			((int16 *)dst)[i] = (int16)(clamp_sample(src[i]) * 32767.0f);
		break;
	case media_raw_audio_format::B_AUDIO_UCHAR:
		for (i = 0; i < count; i++)
			((uint8 *)dst)[i] = (uint8)(clamp_sample(src[i]) * 127.0f + 128.0f);
		break;
	}
}

static void device_play_buffer(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format)
{
	device_output *d = static_cast<device_output *>(cookie);
	uint8 silence = d->sample_format == media_raw_audio_format::B_AUDIO_UCHAR ? 0x80 : 0;

	/* The format is fixed at connection time; anything else would be a negotiation bug. */
	if (format.format != d->sample_format || format.channel_count != d->channels) {
		memset(buffer, silence, size);
		return;
	}

	const size_t frame_bytes = d->sample_size * d->channels;
	size_t frames = size / frame_bytes;
	uint8 *out = static_cast<uint8 *>(buffer);

	std::lock_guard<std::mutex> lock(d->clients_mutex);

	while (frames > 0) {
		const size_t n = std::min<size_t>(frames, MIX_CHUNK_FRAMES);
		const size_t samples = n * d->channels;

		std::fill(d->mix.begin(), d->mix.begin() + samples, 0.0f);
		for (obs_haiku_sound_player *c : d->clients) {
			c->cb(c->param, d->scratch.data(), n);
			for (size_t i = 0; i < samples; i++)
				d->mix[i] += d->scratch[i];
		}

		convert_samples(d->sample_format, d->mix.data(), samples, out);
		out += n * frame_bytes;
		frames -= n;
	}

	/* a partial trailing frame (shouldn't happen) */
	memset(out, silence, size % frame_bytes);
}

static size_t sample_size_for(uint32 format)
{
	switch (format) {
	case media_raw_audio_format::B_AUDIO_FLOAT:
	case media_raw_audio_format::B_AUDIO_INT:
		return 4;
	case media_raw_audio_format::B_AUDIO_SHORT:
		return 2;
	case media_raw_audio_format::B_AUDIO_UCHAR:
		return 1;
	default: /* B_AUDIO_DOUBLE, B_AUDIO_CHAR: not handled */
		return 0;
	}
}

static bool supported_channels(uint32 channels)
{
	/* What libobs' speaker layouts can express */
	return (channels >= 1 && channels <= 6) || channels == 8;
}

static void close_device(device_output *d)
{
	/* See free_player() for why stopping without blocking and then deleting is safe. */
	if (d->player) {
		d->player->Stop(false, false);
		delete d->player; /* disconnects from the device */
		d->player = NULL;
	}

	/* Only now that nothing is connected: if this reference is the last one (we instantiated the node),
	 * the media add-on server closes the device again. */
	if (d->release_node) {
		BMediaRoster *roster = BMediaRoster::CurrentRoster();
		if (roster)
			roster->ReleaseNode(d->node);
		d->release_node = false;
	}

	delete d;
}

/* Connects to a hardware output. On failure returns NULL and a reason for the log. */
static device_output *open_device(BMediaRoster *roster, const char *id, const char **reason)
{
	const output_device *dev = NULL;
	std::vector<output_device> devices = list_output_devices(roster);
	for (const output_device &o : devices) {
		if (o.id == id) {
			dev = &o;
			break;
		}
	}
	if (!dev) {
		*reason = "was not found";
		return NULL;
	}

	device_output *d = new (std::nothrow) device_output();
	if (!d) {
		*reason = "could not be opened";
		return NULL;
	}
	d->id = id;

	/* Reuse the running instance (e.g. opened by another app) rather than creating a second one; a
	 * dormant one is instantiated globally, like Media preferences does, and released in close_device()
	 * so it doesn't outlive OBS's use of it. */
	if (!get_running_instance(roster, dev->info, &d->node) &&
	    roster->InstantiateDormantNode(dev->info, &d->node, B_FLAVOR_IS_GLOBAL) != B_OK) {
		*reason = "could not be opened";
		delete d;
		return NULL;
	}
	d->release_node = true;

	if ((d->node.kind & B_BUFFER_CONSUMER) == 0) {
		/* BSoundPlayer would call debugger() */
		*reason = "does not accept audio";
	} else if (is_system_output(roster, d->node)) {
		*reason = "is the system audio output (used by the mixer)";
	} else {
		/* All wildcards: the device's AcceptFormat() fills in its own rate, channel count, sample format
		 * and buffer size (multi_audio accepts nothing but its hardware format). */
		media_multi_audio_format format = media_multi_audio_format::wildcard;
		std::string name = "OBS Monitor: " + std::string(dev->name);
		d->player = new (std::nothrow)
			BSoundPlayer(d->node, &format, name.c_str(), NULL, device_play_buffer, NULL, d);

		if (!d->player || d->player->InitCheck() != B_OK) {
			/* GetFreeInputsFor()/Connect() failed: typically every input is already connected */
			*reason = "could not be connected to (all of its inputs may be in use)";
		} else {
			const media_raw_audio_format fmt = d->player->Format();
			d->sample_format = fmt.format;
			d->channels = fmt.channel_count;
			d->sample_size = sample_size_for(fmt.format);

			if (d->sample_size == 0 || fmt.byte_order != B_MEDIA_HOST_ENDIAN ||
			    !supported_channels(fmt.channel_count) || fmt.frame_rate < 1000.0f ||
			    fmt.frame_rate > 768000.0f) {
				*reason = "uses an audio format OBS can't produce";
			} else {
				d->mix.resize((size_t)MIX_CHUNK_FRAMES * d->channels);
				d->scratch.resize(d->mix.size());

				if (d->player->Start() == B_OK) {
					d->player->SetHasData(true);
					return d;
				}
				*reason = "could not be started";
			}
		}
	}

	close_device(d);
	return NULL;
}

/* Whether an open device can still play: its node may have gone away (USB output unplugged, media server
 * restarted), which leaves the player connected to nothing. One media server query. */
static bool device_alive(BMediaRoster *roster, device_output *d)
{
	live_node_info info;
	return d->player && d->player->InitCheck() == B_OK && roster->GetLiveNodeInfo(d->node, &info) == B_OK;
}

/* Gets (opening if needed) the shared output for id, taking a reference. */
static device_output *acquire_device(BMediaRoster *roster, const char *id, const char **reason)
{
	std::unique_lock<std::mutex> lock(registry_mutex);

	/* Another thread is opening this id: wait for it rather than opening a second connection, which a
	 * single-input device would refuse. */
	registry_cv.wait(lock, [id] { return !is_opening(id); });

	for (auto it = open_devices.begin(); it != open_devices.end(); ++it) {
		device_output *d = *it;
		if (d->id != id)
			continue;
		if (device_alive(roster, d)) {
			d->refs++;
			return d;
		}
		/* Dead: stop handing it out. Its current players keep it (silent) until they are destroyed;
		 * refs > 0 here, since entries are closed and removed when refs drops to 0. */
		blog(LOG_INFO, "[audio monitoring] Device '%s' went away; reopening it", id);
		open_devices.erase(it);
		break;
	}

	/* Instantiating and connecting a node can take a while (the add-on may open hardware); don't block
	 * device listing and other players' teardown meanwhile. opening_ids keeps the id reserved. */
	opening_ids.push_back(id);
	lock.unlock();

	device_output *d = open_device(roster, id, reason);

	lock.lock();
	opening_ids.erase(std::find(opening_ids.begin(), opening_ids.end(), std::string(id)));
	if (d) {
		d->refs = 1;
		open_devices.push_back(d);
	}
	lock.unlock();
	registry_cv.notify_all();
	return d;
}

static void release_device(device_output *d)
{
	std::lock_guard<std::mutex> lock(registry_mutex);

	if (--d->refs > 0)
		return;

	open_devices.erase(std::remove(open_devices.begin(), open_devices.end(), d), open_devices.end());
	close_device(d);
}

/* ------------------------------------------------------------------------- */
/* System mixer                                                              */

static void mixer_play_buffer(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format)
{
	obs_haiku_sound_player *p = static_cast<obs_haiku_sound_player *>(cookie);

	/* The mixer accepts the format we asked for (checked in open_mixer()); cb writes p->channels per frame,
	 * so anything else would overrun the buffer. */
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
}

static bool open_mixer(obs_haiku_sound_player *p, const char *name, uint32_t sample_rate, uint32_t channels,
		       size_t buffer_frames)
{
	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.frame_rate = (float)sample_rate;
	format.channel_count = channels;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.byte_order = B_MEDIA_HOST_ENDIAN;
	format.buffer_size = buffer_frames * channels * sizeof(float);

	p->channels = channels;
	p->player = new (std::nothrow) BSoundPlayer(&format, name, mixer_play_buffer, NULL, p);
	if (!p->player || p->player->InitCheck() != B_OK) {
		free_player(p);
		return false;
	}

	const media_raw_audio_format fmt = p->player->Format();
	if (fmt.format != media_raw_audio_format::B_AUDIO_FLOAT || fmt.channel_count != channels) {
		blog(LOG_WARNING, "[audio monitoring] The system mixer negotiated %u channels (format 0x%x) instead of "
				  "%u float channels",
		     (unsigned)fmt.channel_count, (unsigned)fmt.format, (unsigned)channels);
		free_player(p);
		return false;
	}
	return true;
}

/* ------------------------------------------------------------------------- */

extern "C" obs_haiku_sound_player *obs_haiku_sound_player_create(const char *device_id, const char *name,
								 uint32_t sample_rate, uint32_t channels,
								 size_t buffer_frames, obs_haiku_fill_cb cb,
								 void *param, obs_haiku_output_format *out)
{
	/* Fail fast instead of waiting for roster timeouts when the media server is down. */
	if (!BMediaRoster::IsRunning())
		return NULL;

	BMediaRoster *roster = BMediaRoster::Roster();
	if (!roster)
		return NULL;

	obs_haiku_sound_player *p = new (std::nothrow) obs_haiku_sound_player();
	if (!p)
		return NULL;
	p->cb = cb;
	p->param = param;

	if (device_id && *device_id && strcmp(device_id, OBS_HAIKU_MONITOR_DEFAULT_ID) != 0) {
		const char *reason = "is not a Haiku audio output";
		if (strncmp(device_id, DEVICE_ID_PREFIX, strlen(DEVICE_ID_PREFIX)) == 0)
			p->device = acquire_device(roster, device_id, &reason);

		if (p->device) {
			device_ok(device_id);
			out->sample_rate = (uint32_t)(p->device->player->Format().frame_rate + 0.5f);
			out->channels = p->device->channels;
			return p;
		}
		warn_fallback(device_id, reason);
	}

	if (!open_mixer(p, name, sample_rate, channels, buffer_frames)) {
		delete p;
		return NULL;
	}
	out->sample_rate = sample_rate;
	out->channels = channels;
	return p;
}

extern "C" bool obs_haiku_sound_player_start(obs_haiku_sound_player *p)
{
	if (!p)
		return false;

	if (p->device) {
		std::lock_guard<std::mutex> lock(p->device->clients_mutex);
		p->device->clients.push_back(p);
		return true;
	}

	if (p->player->Start() != B_OK)
		return false;
	p->player->SetHasData(true);
	return true;
}

extern "C" void obs_haiku_sound_player_destroy(obs_haiku_sound_player *p)
{
	if (!p)
		return;

	if (p->device) {
		{
			/* Waits for a buffer hook in progress; afterwards p's callback is never called again. */
			std::lock_guard<std::mutex> lock(p->device->clients_mutex);
			auto &c = p->device->clients;
			c.erase(std::remove(c.begin(), c.end(), p), c.end());
		}
		release_device(p->device);
		p->device = NULL;
	}

	free_player(p);
	delete p;
}
