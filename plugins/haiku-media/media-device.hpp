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

#pragma once

#include <obs-module.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <MediaAddOn.h>
#include <MediaDefs.h>
#include <MediaNode.h>
#include <MediaRecorder.h>

#define HAIKU_MEDIA_DEFAULT_DEVICE "default"
/* value of the video "resolution" setting that keeps the size the device picks itself */
#define HAIKU_MEDIA_DEFAULT_SIZE "default"

/* A physical capture device as published by a Media Kit add-on (e.g. "HDA audio input", "USB Webcam"). */
struct HaikuMediaDevice {
	std::string id;   /* persistent id: the add-on's node name */
	std::string name; /* display name */
};

/* A video frame size to ask the producer for; 0 x 0 means "whatever the device picks". */
struct HaikuVideoSize {
	uint32_t width = 0;
	uint32_t height = 0;

	bool IsDefault() const { return width == 0 || height == 0; }
	bool operator==(const HaikuVideoSize &o) const { return width == o.width && height == o.height; }
};

/* Lists physical input devices that produce raw buffers of the given type. */
std::vector<HaikuMediaDevice> HaikuEnumerateCaptureDevices(media_type type);

/* Fills an OBS list property with "Default" plus the available devices. */
void HaikuFillDeviceList(obs_property_t *list, media_type type);

/* "640x480" -> 640 x 480; anything else (HAIKU_MEDIA_DEFAULT_SIZE, "") is the device default. */
HaikuVideoSize HaikuParseVideoSize(const char *text);

/* Fills an OBS list property with "Device default" plus the frame sizes for a device: the ones the device
 * accepted when it was last opened (probed, see HaikuRecorder::Open()), or a list of common sizes if it has
 * not been opened yet. The `current` setting is always listed so it isn't lost. */
void HaikuFillVideoSizeList(obs_property_t *list, const std::string &device_id, const char *current);

/* Outcome of HaikuRecorder::Open(); the caller decides what to log (it retries, see HaikuCapture). */
struct HaikuOpenResult {
	status_t error = B_OK;
	std::string reason;         /* what failed, for the log */
	bool used_fallback = false; /* the named device wasn't there; the Media preferences default is used */
	bool size_refused = false;  /* the requested frame size was refused; the device default is used */
};

/* BMediaRecorder that forwards buffers together with the format negotiated for the connection. Not
 * thread-safe by itself: HaikuCapture only uses it from its supervisor thread. */
class HaikuRecorder : public BMediaRecorder {
public:
	using BufferFunc = std::function<void(const void *data, size_t size, const media_format &format)>;

	HaikuRecorder(const char *name, media_type type, BufferFunc func);
	~HaikuRecorder() override;

	/* Acquires the device's node (or, unless `allow_fallback` is false, the system default for `type` if the
	 * named device isn't plugged in), connects to a free output and starts the producer. For video, `size`
	 * asks for a frame size (falls back to the device default if refused) and `probe` first tries a list of
	 * common sizes and remembers which the device accepts (for HaikuFillVideoSizeList()), unless the
	 * device actually acquired was already probed this session. */
	HaikuOpenResult Open(const std::string &device_id, media_type type, HaikuVideoSize size = HaikuVideoSize(),
			     bool probe = false, bool allow_fallback = true);
	void Close();

	bool IsOpen() const { return opened; }
	/* false: buffers are dropped instead of handed to the BufferFunc (a recorder that is not in use yet) */
	void SetDelivering(bool on) { delivering = on; }
	media_format ConnectedFormat();

	/* false once the producer node is gone (its control port deleted: add-on server or media server
	 * quit, node deleted) */
	bool NodeAlive() const;
	/* false once the add-on no longer publishes the producer's flavor (device unplugged); true if the
	 * producer isn't an add-on node or the question can't be answered */
	bool DevicePublished() const;

protected:
	void BufferReceived(void *buffer, size_t size, const media_header &header) override;

private:
	void ProbeSizes(const std::string &key, media_output &output, const media_format &format);

	BufferFunc func;
	std::atomic_bool delivering{true};
	port_id consumer_port = -1; /* our consumer's input port, in the Media Kit notifications it causes */
	media_node node;
	bool have_node = false;
	dormant_node_info dormant;
	bool have_dormant = false;
	bool opened = false;
	media_format connected_format;
	std::mutex format_mutex;
};

/* Owns a HaikuRecorder and keeps it connected while the source wants it: Media Kit work happens on a
 * supervisor thread, so the OBS UI thread never blocks on the media server and every failure is retried.
 *
 *  - Opening fails (device busy in another application, not plugged in, media server down): retried after
 *    1 s, doubling up to 10 s, sooner when the Media Kit reports something that may have freed or added a
 *    device (a connection broken, add-on flavors changed, media server started).
 *  - An open device goes away (producer node deleted, add-on server or media server restarted, webcam
 *    unplugged): noticed through Media Kit notifications where Haiku sends them, and in any case by a
 *    check every second; the recorder is recreated and the device reopened with the same backoff.
 *  - A named device that was missing (so the Media preferences default is used) is switched to when it
 *    appears; the default stays open until the named device has opened, and is kept if it can't be.
 * Each failure streak is logged once, and its end. */
class HaikuCapture {
public:
	using OpenedFunc = std::function<void(const media_format &format, const HaikuOpenResult &result)>;
	using ClosedFunc = std::function<void()>;

	/* the callbacks run on the supervisor thread; on_buffer on the Media Kit's recorder thread */
	HaikuCapture(obs_source_t *source, const char *recorder_name, media_type type,
		     HaikuRecorder::BufferFunc on_buffer, OpenedFunc on_opened, ClosedFunc on_closed);
	~HaikuCapture();

	HaikuCapture(const HaikuCapture &) = delete;
	HaikuCapture &operator=(const HaikuCapture &) = delete;

	/* Any thread; returns immediately. A change of device or size reopens the device; `active` false
	 * closes it. */
	void Configure(const std::string &device_id, HaikuVideoSize size, bool active);

	/* Media Kit notification relay (media-device.cpp); `retry` events may have made a device available,
	 * `broken` (another application's connection broken) may have freed a busy one: it brings a pending
	 * retry forward at most once per failure streak */
	void Wake(bool retry, bool server_quit, bool broken = false);

private:
	struct Config {
		std::string device_id;
		HaikuVideoSize size;
		bool active = false;
	};

	void Run();
	bigtime_t Step(const Config &want, uint64_t generation, bool retry_event, bool broken_event, bool check_event,
		       bool server_quit);
	void Opened(const Config &want, const HaikuOpenResult &result);
	bool SwitchFromFallback(const Config &want);
	void CloseRecorder(bool notify = true);
	void StartStreak(const char *what, const Config &want, const HaikuOpenResult *result);

	obs_source_t *source;
	const std::string recorder_name;
	const media_type type;
	HaikuRecorder::BufferFunc on_buffer;
	OpenedFunc on_opened;
	ClosedFunc on_closed;

	/* shared with Configure()/Wake() */
	std::mutex mutex;
	std::condition_variable cv;
	Config desired;
	uint64_t generation = 0;
	bool stop = false;
	bool changed = false;
	bool retry_event = false;
	bool broken_event = false;
	bool check_event = false;
	bool server_quit_event = false;

	/* supervisor thread only */
	std::unique_ptr<HaikuRecorder> recorder;
	uint64_t applied_generation = UINT64_MAX;
	bigtime_t backoff = 0;
	bigtime_t next_attempt = 0;
	bigtime_t last_attempt = 0;
	bigtime_t last_publish_check = 0;
	bigtime_t opened_at = 0;
	bool in_streak = false;       /* failing since the last stable open; the streak has been logged */
	bool recovery_logged = false; /* "open again" logged for this streak */
	int unpublished_seen = 0;     /* consecutive checks that found the device's flavor gone */
	bool using_fallback = false;
	bool broken_pulled = false;        /* a connection-broken event brought a retry forward in this streak */
	bigtime_t switch_backoff = 0;      /* on the fallback: delay before trying the named device again */
	bigtime_t switch_next = 0;
	bool switch_failed_logged = false; /* "keeps the default device" logged since the fallback opened */

	std::thread thread;
};
