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
/* value of the video "frame_rate" setting that keeps the rate the device picks for the size */
#define HAIKU_MEDIA_DEFAULT_RATE "default"

/* The system mixer's loopback flavor (Renga, PORTING.md "Per-application audio, routing and loopback"):
 * RENGA_LOOPBACK_FLAVOR_NAME of <AudioStreamControl.h>. It is a B_PHYSICAL_INPUT so that recorders list it;
 * here it is left out of the sound card list, because Desktop Audio and Application Audio Capture are the
 * sources for it and each needs an instance of its own. */
#define HAIKU_MEDIA_LOOPBACK_FLAVOR "What you hear (loopback)"

/* What a HaikuRecorder/HaikuCapture connects to. */
enum class HaikuCaptureKind {
	/* A physical device as published by a Media Kit add-on; `device_id` is its flavor name or
	 * HAIKU_MEDIA_DEFAULT_DEVICE. */
	Device,
	/* A new instance of the system mixer's loopback; `device_id` is the signature of the application whose
	 * streams it records, empty for everything that plays. */
	Loopback,
};

/* A physical capture device as published by a Media Kit add-on (e.g. "HDA audio input", "USB Webcam"). */
struct HaikuMediaDevice {
	/* Persistent id: the flavor name. Renga keeps a device's flavor name across plug-ins and boots and
	 * numbers identical devices ("QEMU USB Audio 2"), while flavor ids change (PORTING.md "Sound cards and
	 * cameras that come and go"). */
	std::string id;
	std::string name; /* display name */
};

/* A video frame size to ask the producer for; 0 x 0 means "whatever the device picks". */
struct HaikuVideoSize {
	uint32_t width = 0;
	uint32_t height = 0;

	bool IsDefault() const { return width == 0 || height == 0; }
	bool operator==(const HaikuVideoSize &o) const { return width == o.width && height == o.height; }
};

/* What a HaikuRecorder opens a device with, besides the device itself. */
struct HaikuOpenParams {
	/* video: the frame size to ask for (default: the device's own) */
	HaikuVideoSize size;
	/* video: the frame rate to ask for, as the connection's field_rate (0: the device's default for the size).
	 * Renga's camera producers use their nearest rate and put it in the connected format (F-58, O-5). */
	float field_rate = 0.0f;
	/* loopback: signatures of the applications it leaves out, one per line (renga:loopback_exclude, F-58,
	 * O-1); empty: none */
	std::string loopback_exclude;
	/* loopback: also leave out this program by the signature the registrar has for it (HaikuOwnSignature(),
	 * looked up on the capture's thread) */
	bool loopback_exclude_self = false;

	bool operator==(const HaikuOpenParams &o) const
	{
		return size == o.size && field_rate == o.field_rate && loopback_exclude == o.loopback_exclude &&
		       loopback_exclude_self == o.loopback_exclude_self;
	}
};

/* One line of a camera's "renga:video_modes" parameter (Renga F-58, O-5; PORTING.md "Sound cards and cameras
 * that come and go"): a frame size, what crosses the bus for it ("YUY2", "MJPEG"; the demo and virtual cameras
 * say "RGB32") and the frame rates, fastest first, as the device writes them ("30", "29.97", "7.5"). */
struct HaikuVideoMode {
	HaikuVideoSize size;
	std::string encoding;
	std::vector<std::string> rates;
};

/* What a video device says it can do, read when it was last opened (HaikuRecorder::Open()). */
struct HaikuVideoModes {
	/* the device has a renga:video_modes parameter; `modes` is what it said (in its order) */
	bool listed = false;
	std::vector<HaikuVideoMode> modes;
	/* without the parameter: the format the producer suggests (BMediaRoster::GetFormatFor() of its output,
	 * i.e. FormatSuggestionRequested()); 0 x 0 if it suggests none */
	HaikuVideoSize suggested;
	float suggested_rate = 0.0f;

	bool operator==(const HaikuVideoModes &o) const;
};

/* The signature of this program as the registrar (and so the system mixer) knows it; empty if it has none.
 * A registrar round trip the first time, cached afterwards. */
std::string HaikuOwnSignature();

/* Lists physical input devices that produce raw buffers of the given type (without the mixer's loopback). */
std::vector<HaikuMediaDevice> HaikuEnumerateCaptureDevices(media_type type);

/* Fills an OBS list property with "Default" plus the available devices. `current` (a saved device id, may be
 * NULL) is listed as not connected if it is not among them. */
void HaikuFillDeviceList(obs_property_t *list, media_type type, const char *current);

/* "640x480" -> 640 x 480; anything else (HAIKU_MEDIA_DEFAULT_SIZE, "") is the device default. */
HaikuVideoSize HaikuParseVideoSize(const char *text);

/* "29.97" -> 29.97; anything else (HAIKU_MEDIA_DEFAULT_RATE, "", nonsense) is 0, the device default. */
float HaikuParseFrameRate(const char *text);

/* Fills an OBS list property with "Device default" plus the frame sizes the device listed when it was last
 * opened; nothing more while it has not been opened yet in this session or has no list (then the default's
 * label shows the size the device suggests, if it does). The `current` setting is always listed so that it
 * isn't lost. Reads the module's cache only: no Media Kit call, so it can run on the UI thread. */
void HaikuFillVideoSizeList(obs_property_t *list, const std::string &device_id, const char *current);

/* The same for the frame rates the device listed for the frame size `resolution` ("Device default" only for
 * the default size). */
void HaikuFillFrameRateList(obs_property_t *list, const std::string &device_id, const char *resolution,
			    const char *current);

/* Outcome of HaikuRecorder::Open(); the caller decides what to log (it retries, see HaikuCapture). */
struct HaikuOpenResult {
	status_t error = B_OK;
	std::string reason;         /* what failed, for the log */
	bool used_fallback = false; /* the named device wasn't there; the Media preferences default is used */
	bool size_refused = false;  /* the requested frame size was refused; the device default is used */
	bool modes_changed = false; /* video: what the device lists differs from what was known before */
	float requested_rate = 0.0f; /* video: the field_rate asked for (0: the device's default) */
};

/* BMediaRecorder that forwards buffers together with the format negotiated for the connection. Not
 * thread-safe by itself: HaikuCapture only uses it from its supervisor thread.
 *
 * Always held in a HaikuRecorderPtr: what the Media Kit's recorder thread reads when a buffer arrives lives
 * in a separate block that is freed only after ~BMediaRecorder() has stopped that thread (it joins it when
 * it releases its node). As plain members they would be destroyed before the base class destructor runs,
 * while a buffer could still be on its way through BufferReceived(). */
class HaikuRecorder : public BMediaRecorder {
public:
	/* `header` is the buffer's: start_time is in the performance time of the node header.time_source */
	using BufferFunc = std::function<void(const void *data, size_t size, const media_format &format,
					      const media_header &header)>;

	HaikuRecorder(const char *name, media_type type, HaikuCaptureKind kind, BufferFunc func);
	~HaikuRecorder() override;

	struct Deleter {
		void operator()(HaikuRecorder *recorder) const;
	};

	/* Device: acquires the device's node (or, unless `allow_fallback` is false, the system default for the
	 * type if the named device isn't plugged in), connects to a free output and starts the producer. For
	 * video, it first reads what the device lists (HaikuVideoModes, for HaikuFillVideoSizeList()), then asks
	 * for `params.size` and `params.field_rate` (the device default if the size is refused).
	 * Loopback: instantiates a loopback node, selects the application `device_id`, leaves out
	 * `params.loopback_exclude` and connects with float stereo at the mixer's rate. */
	HaikuOpenResult Open(const std::string &device_id, const HaikuOpenParams &params = HaikuOpenParams(),
			     bool allow_fallback = true);
	void Close();

	bool IsOpen() const { return opened; }
	/* false: buffers are dropped instead of handed to the BufferFunc (a recorder that is not in use yet) */
	void SetDelivering(bool on);
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
	HaikuOpenResult OpenLoopback(const std::string &signature, const HaikuOpenParams &params);
	void SetConnectedFormat(const media_format &format);
	bool ReadVideoModes(const std::string &key, const media_output &output);
	bool IsVirtualCamera() const;

	/* read by the recorder thread; see the class comment */
	struct Shared {
		BufferFunc func;
		std::atomic_bool delivering{true};
		std::mutex format_mutex;
		media_format connected_format;
	};
	Shared *const shared;

	const media_type type;
	const HaikuCaptureKind kind;
	port_id consumer_port = -1; /* our consumer's input port, in the Media Kit notifications it causes */
	media_node node;
	bool have_node = false;
	dormant_node_info dormant;
	bool have_dormant = false;
	bool opened = false;
};

using HaikuRecorderPtr = std::unique_ptr<HaikuRecorder, HaikuRecorder::Deleter>;

/* Owns a HaikuRecorder and keeps it connected while the source wants it: Media Kit work happens on a
 * supervisor thread, so the OBS UI thread never blocks on the media server and every failure is retried.
 *
 *  - Opening fails (device busy in another application, not plugged in, media server down): retried after
 *    1 s, doubling up to 10 s, sooner when the Media Kit reports something that may have freed or added a
 *    device (a connection broken, add-on flavors changed, media server started).
 *  - An open device goes away (producer node deleted, add-on server or media server restarted, device
 *    unplugged): the Media Kit's notifications say so (B_MEDIA_FLAVORS_CHANGED, B_MEDIA_NODE_DELETED,
 *    B_MEDIA_SERVER_QUIT; see the watcher in media-device.cpp for where Renga sends them), the open device
 *    is checked when one arrives, and the device is reopened with the same backoff. There is no periodic
 *    health check any more: Renga's media services report hot-plug.
 *  - A named device that was missing (so the Media preferences default is used) is switched to when it
 *    appears; the default stays open until the named device has opened, and is kept if it can't be.
 *  - When the set of devices changes, or a camera opened lists other modes than known before, the source's
 *    properties are reloaded so that an open properties window lists the devices and modes there are now.
 * Each failure streak is logged once, and its end. */
class HaikuCapture {
public:
	using OpenedFunc = std::function<void(const media_format &format, const HaikuOpenResult &result)>;
	using ClosedFunc = std::function<void()>;

	/* the callbacks run on the supervisor thread; on_buffer on the Media Kit's recorder thread */
	HaikuCapture(obs_source_t *source, const char *recorder_name, media_type type, HaikuCaptureKind kind,
		     HaikuRecorder::BufferFunc on_buffer, OpenedFunc on_opened, ClosedFunc on_closed);
	~HaikuCapture();

	HaikuCapture(const HaikuCapture &) = delete;
	HaikuCapture &operator=(const HaikuCapture &) = delete;

	/* Any thread; returns immediately. A change of device or parameters reopens the device; `active` false
	 * closes it. */
	void Configure(const std::string &device_id, const HaikuOpenParams &params, bool active);

	/* Media Kit notification relay (media-device.cpp); `retry` events may have made a device available,
	 * `broken` (another application's connection broken) may have freed a busy one: it brings a pending
	 * retry forward at most once per failure streak */
	void Wake(bool retry, bool server_quit, bool broken = false);
	/* Media Kit notification relay: devices were plugged in or removed (B_MEDIA_FLAVORS_CHANGED) */
	void DevicesChanged();

private:
	struct Config {
		std::string device_id;
		HaikuOpenParams params;
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
	const HaikuCaptureKind kind;
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
	HaikuRecorderPtr recorder;
	uint64_t applied_generation = UINT64_MAX;
	bigtime_t backoff = 0;
	bigtime_t next_attempt = 0;
	bigtime_t last_attempt = 0;
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
