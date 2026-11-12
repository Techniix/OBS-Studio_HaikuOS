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

#include "media-device.hpp"

#include <util/dstr.h>
#include <util/threading.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <system_error>

#include <Looper.h>
#include <MediaAddOn.h>
#include <MediaRoster.h>
#include <Messenger.h>
#include <OS.h>
#include <ParameterWeb.h>
#include <Roster.h>

#include <strings.h>

/* Renga private header (headers/private/media, header-only on public Media Kit API): the system mixer's
 * per-application streams and the loopback flavor. PORTING.md, "Per-application audio, routing and loopback". */
#include <AudioStreamControl.h>
/* Renga private header (headers/private/media; F-58, O-12): only its constants are used here, so nothing of
 * the feeder (memfd, sockets) is compiled in */
#include <VirtualCamera.h>

#define T_(text) obs_module_text(text)

/* retry delays for a device that can't be opened */
#define RETRY_FIRST_US 1000000
#define RETRY_MAX_US 10000000
/* an open device whose flavor was missing from the device list is looked up once more after this */
#define RECHECK_US 1000000
/* how long the supervisor sleeps when it has nothing scheduled; Configure() and Media Kit events wake it */
#define IDLE_US 60000000
/* a device open this long counts as working again: its next failure is logged and retried quickly */
#define STABLE_US 10000000

static media_format raw_format(media_type type)
{
	media_format format;
	format.type = type;

	if (type == B_MEDIA_RAW_AUDIO)
		format.u.raw_audio = media_raw_audio_format::wildcard;
	else if (type == B_MEDIA_RAW_VIDEO)
		format.u.raw_video = media_raw_video_format::wildcard;

	return format;
}

static std::vector<dormant_node_info> dormant_capture_nodes(media_type type)
{
	std::vector<dormant_node_info> nodes;

	status_t err;
	BMediaRoster *roster = BMediaRoster::Roster(&err);
	if (!roster)
		return nodes;

	media_format format = raw_format(type);

	/* Sound cards are the physical inputs with a raw audio output. Cameras are listed the way PORTING.md
	 * says ("Sound cards and cameras that come and go", as CodyCam's Camera menu does): every producer with
	 * a raw video output. That includes "Demo Video Producer", which is not a physical input
	 * (video_producer_demo/AddOn.cpp) and is how a setup is tested without a camera. */
	const uint64 kinds = type == B_MEDIA_RAW_AUDIO ? B_BUFFER_PRODUCER | B_PHYSICAL_INPUT : B_BUFFER_PRODUCER;

	int32 count = 64;
	nodes.resize((size_t)count);
	err = roster->GetDormantNodes(nodes.data(), &count, NULL, &format, NULL, kinds);
	if (err != B_OK || count < 0)
		count = 0;
	nodes.resize((size_t)std::min<int32>(count, 64));

	/* The mixer's loopback is a physical input too (MixerAddOn.cpp); it has sources of its own. Renga's
	 * "Virtual Camera" (virtual_camera add-on, VIRTUAL_CAMERA_FLAVOR_NAME, its only flavor) shows what OBS
	 * itself feeds it (plugins/haiku-virtualcam): capturing it would loop OBS's output back into itself, so
	 * it is no camera to choose here. */
	nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
				   [type](const dormant_node_info &info) {
					   return strcmp(info.name, HAIKU_MEDIA_LOOPBACK_FLAVOR) == 0 ||
						  (type == B_MEDIA_RAW_VIDEO &&
						   strcmp(info.name, VIRTUAL_CAMERA_FLAVOR_NAME) == 0);
				   }),
		    nodes.end());
	return nodes;
}

static bool is_virtual_camera_id(const std::string &device_id, media_type type)
{
	return type == B_MEDIA_RAW_VIDEO && device_id == VIRTUAL_CAMERA_FLAVOR_NAME;
}

static bool device_listed(const std::string &device_id, media_type type)
{
	for (const dormant_node_info &info : dormant_capture_nodes(type))
		if (device_id == info.name)
			return true;
	return false;
}

std::vector<HaikuMediaDevice> HaikuEnumerateCaptureDevices(media_type type)
{
	std::vector<HaikuMediaDevice> devices;

	for (const dormant_node_info &info : dormant_capture_nodes(type))
		devices.push_back({info.name, info.name});

	return devices;
}

void HaikuFillDeviceList(obs_property_t *list, media_type type, const char *current)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, T_("Default"), HAIKU_MEDIA_DEFAULT_DEVICE);

	bool found = !current || !*current || strcmp(current, HAIKU_MEDIA_DEFAULT_DEVICE) == 0;
	for (const HaikuMediaDevice &device : HaikuEnumerateCaptureDevices(type)) {
		obs_property_list_add_string(list, device.name.c_str(), device.id.c_str());
		found = found || device.id == current;
	}

	if (!found && is_virtual_camera_id(current, type)) {
		/* a saved source that points to OBS's own virtual camera: shown, never opened (acquire_node()) */
		struct dstr label = {0};
		dstr_printf(&label, T_("VirtualCamera.NotAvailable"), current);
		const size_t index = obs_property_list_add_string(list, label.array, current);
		obs_property_list_item_disable(list, index, true);
		dstr_free(&label);
	} else if (!found) {
		/* Devices are remembered by flavor name; one that is unplugged keeps its place in the list (the
		 * capture uses the default device meanwhile and returns to it when it is back). */
		struct dstr label = {0};
		dstr_printf(&label, T_("DeviceNotConnected"), current);
		const size_t index = obs_property_list_add_string(list, label.array, current);
		obs_property_list_item_disable(list, index, true);
		dstr_free(&label);
	}
}

/* ------------------------------------------------------------------------------------------------------ */
/* Frame sizes                                                                                            */

/* Renga's camera producers list what they can send in a hidden, read-only text parameter of kind
 * "renga:video_modes" (F-58, OBS Studio's O-5; usb_webcam/Producer.cpp P_MODES from UVCCamDevice::VideoModes(),
 * video_producer_demo and virtual_camera have one too), one line per frame size and encoding:
 * "<w>x<h> <encoding> <fps>,<fps>,...", fastest first. A connection's field_rate picks the device's nearest rate
 * for the size (UVCCamDevice::SetFrameRate() -> _ChooseInterval(); 0: the descriptor's default) and the rate
 * granted is in the connected format.
 *
 * Reading the parameter is a round trip to the node (GetParameterWebFor(), then GetValue()), so it is done by
 * the capture's supervisor thread when it opens the device (HaikuRecorder::ReadVideoModes()), and the
 * properties, built on the UI thread, read this cache. A device that has not been opened in this session yet
 * offers "Device default" only until it is (the properties are reloaded then, HaikuCapture::Opened()). */
static std::mutex modes_mutex;
static std::map<std::string, HaikuVideoModes> modes_cache; /* device id -> what it listed when last opened */

bool HaikuVideoModes::operator==(const HaikuVideoModes &o) const
{
	if (listed != o.listed || !(suggested == o.suggested) || suggested_rate != o.suggested_rate ||
	    modes.size() != o.modes.size())
		return false;
	for (size_t i = 0; i < modes.size(); i++)
		if (!(modes[i].size == o.modes[i].size) || modes[i].encoding != o.modes[i].encoding ||
		    modes[i].rates != o.modes[i].rates)
			return false;
	return true;
}

static bool cached_modes(const std::string &device_id, HaikuVideoModes &modes)
{
	std::lock_guard<std::mutex> lock(modes_mutex);
	auto it = modes_cache.find(device_id);
	if (it == modes_cache.end())
		return false;
	modes = it->second;
	return true;
}

/* true if this changed what was known */
static bool cache_modes(const std::string &device_id, const HaikuVideoModes &modes)
{
	std::lock_guard<std::mutex> lock(modes_mutex);
	auto it = modes_cache.find(device_id);
	if (it != modes_cache.end() && it->second == modes)
		return false;
	modes_cache[device_id] = modes;
	return true;
}

/* A rate as the device wrote it: digits with at most one decimal point, above 0. */
static bool valid_rate(const std::string &rate)
{
	if (rate.empty() || rate.size() > 15)
		return false;
	int points = 0;
	for (char c : rate) {
		if (c == '.')
			points++;
		else if (c < '0' || c > '9')
			return false;
	}
	return points <= 1 && strtod(rate.c_str(), nullptr) > 0.0;
}

/* The parameter's text; lines that don't parse are skipped (and so is the whole list if none does) */
static std::vector<HaikuVideoMode> parse_video_modes(const char *text)
{
	std::vector<HaikuVideoMode> modes;
	for (const char *line = text; line && *line;) {
		const char *end = strchr(line, '\n');
		const std::string entry(line, end ? (size_t)(end - line) : strlen(line));
		line = end ? end + 1 : nullptr;

		unsigned w = 0, h = 0;
		char encoding[16] = {0}, rates[512] = {0};
		if (sscanf(entry.c_str(), "%ux%u %15s %511s", &w, &h, encoding, rates) != 4 || !w || !h ||
		    w > 16384 || h > 16384)
			continue;
		HaikuVideoMode mode;
		mode.size.width = w;
		mode.size.height = h;
		mode.encoding = encoding;
		for (const char *rate = rates; *rate;) {
			const char *comma = strchr(rate, ',');
			const std::string value(rate, comma ? (size_t)(comma - rate) : strlen(rate));
			if (valid_rate(value) &&
			    std::find(mode.rates.begin(), mode.rates.end(), value) == mode.rates.end())
				mode.rates.push_back(value);
			if (!comma)
				break;
			rate = comma + 1;
		}
		if (!mode.rates.empty())
			modes.push_back(std::move(mode));
	}
	return modes;
}

HaikuVideoSize HaikuParseVideoSize(const char *text)
{
	HaikuVideoSize size;
	unsigned w = 0, h = 0;
	char extra;
	if (text && sscanf(text, "%ux%u%c", &w, &h, &extra) == 2 && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
		size.width = w;
		size.height = h;
	}
	return size;
}

float HaikuParseFrameRate(const char *text)
{
	if (!text || !valid_rate(text))
		return 0.0f;
	const double rate = strtod(text, nullptr);
	return rate <= 1000.0 ? (float)rate : 0.0f;
}

static std::string size_value(const HaikuVideoSize &size)
{
	char value[32];
	snprintf(value, sizeof(value), "%ux%u", size.width, size.height);
	return value;
}

static void add_size(obs_property_t *list, const HaikuVideoSize &size)
{
	char label[32];
	snprintf(label, sizeof(label), "%u x %u", size.width, size.height);
	obs_property_list_add_string(list, label, size_value(size).c_str());
}

void HaikuFillVideoSizeList(obs_property_t *list, const std::string &device_id, const char *current)
{
	HaikuVideoModes modes;
	cached_modes(device_id, modes);

	obs_property_list_clear(list);
	if (!modes.listed && !modes.suggested.IsDefault()) {
		/* no list, but the producer says what its default is */
		struct dstr label = {0};
		dstr_printf(&label, T_("DeviceDefaultSize.Suggested"), modes.suggested.width, modes.suggested.height);
		obs_property_list_add_string(list, label.array, HAIKU_MEDIA_DEFAULT_SIZE);
		dstr_free(&label);
	} else {
		obs_property_list_add_string(list, T_("DeviceDefault"), HAIKU_MEDIA_DEFAULT_SIZE);
	}

	/* Each size once (a camera can send one as YUY2 and as MJPEG), largest first */
	std::vector<HaikuVideoSize> sizes;
	for (const HaikuVideoMode &mode : modes.modes)
		if (std::find(sizes.begin(), sizes.end(), mode.size) == sizes.end())
			sizes.push_back(mode.size);
	std::stable_sort(sizes.begin(), sizes.end(), [](const HaikuVideoSize &a, const HaikuVideoSize &b) {
		return (uint64_t)a.width * a.height > (uint64_t)b.width * b.height;
	});
	for (const HaikuVideoSize &size : sizes)
		add_size(list, size);

	const HaikuVideoSize cur = HaikuParseVideoSize(current);
	if (!cur.IsDefault() && std::find(sizes.begin(), sizes.end(), cur) == sizes.end())
		add_size(list, cur); /* keep the setting visible; a device that refuses it uses its default */
}

void HaikuFillFrameRateList(obs_property_t *list, const std::string &device_id, const char *resolution,
			    const char *current)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, T_("DeviceDefault"), HAIKU_MEDIA_DEFAULT_RATE);

	std::vector<std::string> rates;
	const HaikuVideoSize size = HaikuParseVideoSize(resolution);
	HaikuVideoModes modes;
	if (!size.IsDefault() && cached_modes(device_id, modes)) {
		/* The rates of the first line with this size: usb_webcam sends a size as YUY2 if the camera has it
		 * so, MJPEG only otherwise (UVCCamDevice::AcceptVideoFrame()), and chooses among the rates of the
		 * encoding it uses (_ChooseInterval() on _CurrentFrame()); VideoModes() lists YUY2 first. */
		for (const HaikuVideoMode &mode : modes.modes) {
			if (mode.size == size) {
				rates = mode.rates;
				break;
			}
		}
	}

	struct dstr label = {0};
	for (const std::string &rate : rates) {
		dstr_printf(&label, T_("FrameRate.Fps"), rate.c_str());
		obs_property_list_add_string(list, label.array, rate.c_str());
	}
	if (!size.IsDefault() && current && HaikuParseFrameRate(current) > 0.0f &&
	    std::find(rates.begin(), rates.end(), std::string(current)) == rates.end()) {
		/* keep the setting visible; the device uses its nearest rate */
		dstr_printf(&label, T_("FrameRate.Fps"), current);
		obs_property_list_add_string(list, label.array, current);
	}
	dstr_free(&label);
}

/* ------------------------------------------------------------------------------------------------------ */
/* This program's signature                                                                               */

/* The signature of this program, as the system mixer sees it: the mixer names a stream's application with
 * be_roster->GetRunningAppInfo() of the team that owns the stream's port (AudioMixer::_IdentifyApplication()). */
static std::string lookup_own_signature()
{
	thread_info thread;
	app_info app;
	if (!be_roster || get_thread_info(find_thread(NULL), &thread) != B_OK ||
	    be_roster->GetRunningAppInfo(thread.team, &app) != B_OK)
		return std::string();
	return app.signature;
}

/* A registrar round trip, so only once; the signature doesn't change while the program runs. Not cached
 * while there is no be_roster yet (nothing to compare with then). */
std::string HaikuOwnSignature()
{
	static std::mutex mutex;
	static std::string signature;
	static bool known = false;
	std::lock_guard<std::mutex> lock(mutex);
	if (!known) {
		signature = lookup_own_signature();
		known = be_roster != nullptr;
	}
	return signature;
}

/* ------------------------------------------------------------------------------------------------------ */
/* HaikuRecorder                                                                                          */

static status_t acquire_dormant(BMediaRoster *roster, const dormant_node_info &info, media_node *node)
{
	/* Reuse a running instance (e.g. the system's audio input) rather than creating a second one */
	media_node_id id;
	int32 count = 1;
	if (roster->GetInstancesFor(info.addon, info.flavor_id, &id, &count) == B_OK && count > 0)
		return roster->GetNodeFor(id, node);

	return roster->InstantiateDormantNode(info, node, B_FLAVOR_IS_GLOBAL);
}

/* Gets a reference to the node for a device; the caller must ReleaseNode() it. */
static status_t acquire_node(const std::string &device_id, media_type type, media_node *node, bool *fallback,
			     bool allow_fallback)
{
	status_t err;
	BMediaRoster *roster = BMediaRoster::Roster(&err);
	if (!roster)
		return err;

	/* OBS's own virtual camera: not opened, and no stand-in either (the source stays black) */
	if (is_virtual_camera_id(device_id, type))
		return B_NOT_ALLOWED;

	if (device_id.empty() || device_id == HAIKU_MEDIA_DEFAULT_DEVICE) {
		/* The devices selected in Haiku's Media preferences */
		return type == B_MEDIA_RAW_AUDIO ? roster->GetAudioInput(node) : roster->GetVideoInput(node);
	}

	for (const dormant_node_info &info : dormant_capture_nodes(type))
		if (device_id == info.name)
			return acquire_dormant(roster, info, node);

	if (!allow_fallback)
		return ENODEV;
	/* not plugged in (yet): use the default device meanwhile; HaikuCapture switches when it appears */
	*fallback = true;
	return acquire_node(HAIKU_MEDIA_DEFAULT_DEVICE, type, node, fallback, false);
}

/* The consumer ports of this module's recorders. BMediaRoster::Disconnect() sends B_MEDIA_CONNECTION_BROKEN
 * for every disconnect, ours included (each Close()), so the watcher must not take
 * those for another application letting go of a device. A port stays listed for a while after its recorder
 * is gone: the notification travels through the media server. Port ids are not reused soon. */
namespace {

#define OWN_PORT_LINGER_US 30000000

std::mutex own_ports_mutex;
std::map<port_id, bigtime_t> own_ports; /* port -> when its recorder went away, 0 while it exists */

void prune_own_ports(bigtime_t now)
{
	for (auto it = own_ports.begin(); it != own_ports.end();) {
		if (it->second && now - it->second > OWN_PORT_LINGER_US)
			it = own_ports.erase(it);
		else
			++it;
	}
}

bool own_port(port_id port)
{
	std::lock_guard<std::mutex> lock(own_ports_mutex);
	return own_ports.count(port) != 0;
}

} // namespace

HaikuRecorder::HaikuRecorder(const char *name, media_type type_, HaikuCaptureKind kind_, BufferFunc func_)
	: BMediaRecorder(name, type_),
	  shared(new Shared),
	  type(type_),
	  kind(kind_)
{
	shared->func = std::move(func_);
	if (InitCheck() != B_OK)
		return; /* no node; MediaInput() would dereference it */
	/* BMediaRecorderNode's input destination is its control port, set in its constructor */
	consumer_port = MediaInput().destination.port;
	std::lock_guard<std::mutex> lock(own_ports_mutex);
	prune_own_ports(system_time());
	own_ports[consumer_port] = 0;
}

HaikuRecorder::~HaikuRecorder()
{
	shared->delivering = false;
	Close();
	if (consumer_port >= 0) {
		std::lock_guard<std::mutex> lock(own_ports_mutex);
		own_ports[consumer_port] = system_time();
	}
}

void HaikuRecorder::Deleter::operator()(HaikuRecorder *recorder) const
{
	if (!recorder)
		return;
	Shared *shared = recorder->shared;
	/* ~BMediaRecorder() releases its node, whose DeleteHook (BMediaEventLooper) closes the control port and
	 * joins the control thread, the only caller of BufferReceived(). Only then may `shared` go. */
	delete recorder;
	delete shared;
}

void HaikuRecorder::SetDelivering(bool on)
{
	shared->delivering = on;
}

void HaikuRecorder::SetConnectedFormat(const media_format &format)
{
	std::lock_guard<std::mutex> lock(shared->format_mutex);
	shared->connected_format = format;
}

static void set_size(media_format &format, const HaikuVideoSize &size)
{
	format.u.raw_video.display.line_width = size.width;
	format.u.raw_video.display.line_count = size.height;
	format.u.raw_video.display.bytes_per_row = 0; /* wildcard: follows from the size */
}

/* Supervisor thread, with the node acquired. */
bool HaikuRecorder::IsVirtualCamera() const
{
	if (have_dormant && strcmp(dormant.name, VIRTUAL_CAMERA_FLAVOR_NAME) == 0)
		return true;
	BMediaRoster *roster = BMediaRoster::CurrentRoster();
	BParameterWeb *web = nullptr;
	bool found = false;
	if (roster && roster->GetParameterWebFor(node, &web) == B_OK && web) {
		for (int32 i = 0; !found && i < web->CountParameters(); i++)
			found = strcmp(web->ParameterAt(i)->Kind(), VIRTUAL_CAMERA_STATUS_KIND) == 0;
	}
	delete web;
	return found;
}

/* Supervisor thread, with the device's node acquired and a free output found, before connecting. Reads the
 * camera's "renga:video_modes" (by kind, as PORTING.md says; it is a B_TEXT_PARAMETER) and caches it under `key`.
 * A producer without it gets its suggested format instead: Renga's usb_webcam answers
 * FormatSuggestionRequested() for raw video now (B_MEDIA_UNKNOWN_TYPE, which BMediaRoster::GetFormatFor() of an
 * output sends, included); the demo producer still answers for B_MEDIA_ENCODED_VIDEO only, but it has the
 * parameter. Returns true if this changed what was known about the device. */
bool HaikuRecorder::ReadVideoModes(const std::string &key, const media_output &output)
{
	BMediaRoster *roster = BMediaRoster::CurrentRoster();
	if (!roster)
		return false;

	HaikuVideoModes modes;
	BParameterWeb *web = nullptr;
	if (roster->GetParameterWebFor(node, &web) == B_OK && web) {
		for (int32 i = 0; i < web->CountParameters(); i++) {
			BParameter *parameter = web->ParameterAt(i);
			if (parameter->Type() != BParameter::B_TEXT_PARAMETER ||
			    strcmp(parameter->Kind(), "renga:video_modes") != 0)
				continue;
			/* usb_webcam's parameter holds up to 4096 bytes, the others 256 */
			const size_t max_bytes = ((BTextParameter *)parameter)->MaxBytes();
			std::vector<char> text(std::max<size_t>(max_bytes, 4096) + 1, 0);
			size_t size = text.size() - 1;
			bigtime_t when;
			if (parameter->GetValue(text.data(), &size, &when) == B_OK) {
				text[std::min(size, text.size() - 1)] = '\0';
				modes.listed = true;
				modes.modes = parse_video_modes(text.data());
			}
			break;
		}
	}
	delete web;

	if (!modes.listed) {
		media_format suggested;
		if (roster->GetFormatFor(output, &suggested) == B_OK && suggested.type == B_MEDIA_RAW_VIDEO) {
			modes.suggested.width = suggested.u.raw_video.display.line_width;
			modes.suggested.height = suggested.u.raw_video.display.line_count;
			modes.suggested_rate = suggested.u.raw_video.field_rate;
		}
	}

	const bool changed = cache_modes(key, modes);
	if (changed) {
		if (modes.listed)
			blog(LOG_INFO, "[haiku-media] '%s' lists %zu video modes", key.c_str(), modes.modes.size());
		else if (!modes.suggested.IsDefault())
			blog(LOG_INFO, "[haiku-media] '%s' lists no video modes; it suggests %ux%u at %.2f fps",
			     key.c_str(), modes.suggested.width, modes.suggested.height, modes.suggested_rate);
		else
			blog(LOG_INFO, "[haiku-media] '%s' lists no video modes", key.c_str());
	}
	return changed;
}

/* The rate to ask the loopback for. MixerTap converts the mix to whatever rate the connection has, but with
 * a linear-interpolation resampler that runs in the mixer's mix thread with the mixer core locked
 * (MixerTap::Push()). Asking for the rate the mixer mixes at makes that a plain copy, and libobs resamples
 * to its own rate with its (better) resampler where the two differ. The mix rate is the rate of the mixer's
 * output connection (MixerCore::_ApplyOutputFormat()). */
static status_t mixer_rate(BMediaRoster *roster, float *rate)
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

HaikuOpenResult HaikuRecorder::OpenLoopback(const std::string &signature, const HaikuOpenParams &params)
{
	HaikuOpenResult result;
	BMediaRoster *roster = BMediaRoster::CurrentRoster(); /* Open() made sure there is one */

	/* The system mixer has to be there first: a loopback attaches itself to it when it is registered
	 * (MixerTap::NodeRegistered() -> AudioMixer::AttachLoopback()) and is not picked up later if there was
	 * none (after the media services were restarted, say); it would connect fine and stay silent. */
	float rate = 0.0f;
	status_t err = mixer_rate(roster, &rate);
	if (err != B_OK) {
		result.error = err;
		result.reason = "the system mixer is not running (yet)";
		return result;
	}
	/* what MixerTap accepts (format_supported() in MixerTap.cpp); a mixer without a sane rate gets OBS's */
	if (!(rate >= 4000.0f && rate <= 384000.0f))
		rate = (float)audio_output_get_sample_rate(obs_get_audio());

	/* Finds the flavor among the physical inputs, instantiates it in the media add-on server
	 * (B_FLAVOR_IS_GLOBAL; each instance is independent) and sets its text parameter
	 * "renga:loopback_signature"; releases the node again if that fails. */
	err = BPrivate::AudioStreamControl::StartLoopback(signature.c_str(), &node);
	if (err != B_OK) {
		result.error = err;
		result.reason = "the system mixer's loopback (\"" RENGA_LOOPBACK_FLAVOR_NAME "\") is not available";
		return result;
	}
	have_node = true;
	have_dormant = roster->GetDormantNodeFor(node, &dormant) == B_OK;

	/* Before any buffer: the loopback's "renga:loopback_exclude" parameter (F-58, O-1), set through
	 * AudioStreamControl::SetLoopbackExclusions(), which finds it by kind. BParameter::SetValue() waits for the
	 * node's reply, which comes after MixerTap::SetParameterValue() has taken the list, so B_OK means it
	 * applies. Without it the source must not run: Desktop Audio leaves OBS out so that monitoring (which
	 * libobs/audio-monitoring/haiku now allows on it) does not feed back into it. */
	std::string exclude = params.loopback_exclude;
	if (params.loopback_exclude_self) {
		/* this thread may ask the registrar; the UI thread that configured the source should not */
		const std::string self = HaikuOwnSignature();
		bool listed = self.empty();
		for (size_t start = 0; !listed && start < exclude.size();) {
			size_t end = exclude.find('\n', start);
			if (end == std::string::npos)
				end = exclude.size();
			listed = end - start == self.size() &&
				 strncasecmp(exclude.c_str() + start, self.c_str(), self.size()) == 0;
			start = end + 1;
		}
		if (!listed)
			exclude += self + "\n";
	}
	if (!exclude.empty()) {
		err = BPrivate::AudioStreamControl::SetLoopbackExclusions(node, exclude.c_str());
		if (err != B_OK) {
			result.error = err;
			result.reason = "the loopback cannot leave applications out (renga:loopback_exclude)";
			Close();
			return result;
		}
	}

	media_output output;
	int32 count = 0;
	err = roster->GetFreeOutputsFor(node, &output, 1, &count, B_MEDIA_RAW_AUDIO);
	if (err != B_OK || count < 1) {
		result.error = err != B_OK ? err : B_BUSY;
		result.reason = "the loopback has no free output";
		Close();
		return result;
	}

	/* Float stereo: the tap holds the mix as float and only ever fills two channels, whatever the
	 * connection has (MixerTap::Push() zeroes the others and averages for mono), so this is the one format
	 * that is copied as it is (memcpy in MixerTap::_SendBuffers()) and that OBS takes without conversion.
	 * The buffer size stays a wildcard: the tap fills in its preferred 1024 frames. */
	media_format format = raw_format(B_MEDIA_RAW_AUDIO);
	format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.u.raw_audio.channel_count = 2;
	format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
	format.u.raw_audio.frame_rate = rate;

	SetAcceptedFormat(format);
	err = BMediaRecorder::Connect(node, &output, &format);
	if (err != B_OK) {
		result.error = err;
		result.reason = "connecting to the loopback failed";
		Close();
		return result;
	}
	/* MediaInput() carries the format that was actually negotiated */
	SetConnectedFormat(MediaInput().format);

	/* The loopback is no time source, so this is BMediaRoster::StartNode(); MixerTap::Start() runs at once
	 * whatever the time given, and the mixer mixes silence for a loopback while nothing plays. */
	err = Start();
	if (err != B_OK) {
		result.error = err;
		result.reason = "starting the loopback failed";
		Close();
		return result;
	}

	opened = true;
	return result;
}

HaikuOpenResult HaikuRecorder::Open(const std::string &device_id, const HaikuOpenParams &params, bool allow_fallback)
{
	HaikuOpenResult result;
	Close();

	status_t err = InitCheck();
	if (err != B_OK) {
		result.error = err;
		result.reason = "media recorder init failed (is the media server running?)";
		return result;
	}

	BMediaRoster *roster = BMediaRoster::Roster(&err);
	if (!roster) {
		result.error = err;
		result.reason = "no media roster";
		return result;
	}

	if (kind == HaikuCaptureKind::Loopback)
		return OpenLoopback(device_id, params);

	err = acquire_node(device_id, type, &node, &result.used_fallback, allow_fallback);
	if (err != B_OK) {
		result.error = err;
		result.reason = err == B_NOT_ALLOWED ? "it is Renga's Virtual Camera, which shows OBS's own output"
						     : "no capture device available";
		return result;
	}
	have_node = true;
	have_dormant = roster->GetDormantNodeFor(node, &dormant) == B_OK;

	if (have_dormant && !DevicePublished()) {
		/* The node of an unplugged device: usb_webcam keeps it alive, and the media server's default may
		 * still name it. Plugged in again, the device comes back as a new flavor of the same add-on with
		 * the same name. */
		const dormant_node_info stale = dormant;
		roster->ReleaseNode(node);
		have_node = have_dormant = false;
		err = ENODEV;
		for (const dormant_node_info &info : dormant_capture_nodes(type)) {
			if (info.addon == stale.addon && strcmp(info.name, stale.name) == 0) {
				err = acquire_dormant(roster, info, &node);
				break;
			}
		}
		if (err != B_OK) {
			result.error = err;
			result.reason = "the device is not plugged in";
			return result;
		}
		have_node = true;
		have_dormant = roster->GetDormantNodeFor(node, &dormant) == B_OK;
	}

	/* The media server may make the Virtual Camera the default video input (VirtualCameraAddOn.cpp), and a
	 * fallback lands on the default: refuse it by its flavor name, and by the status parameter only its node
	 * has (VIRTUAL_CAMERA_STATUS_KIND), in case the node is not an add-on flavor we can name. */
	if (type == B_MEDIA_RAW_VIDEO && IsVirtualCamera()) {
		result.error = B_NOT_ALLOWED;
		result.reason = "it is Renga's Virtual Camera, which shows OBS's own output";
		Close();
		return result;
	}

	media_output output;
	int32 count = 0;
	err = roster->GetFreeOutputsFor(node, &output, 1, &count, type);
	if (err != B_OK || count < 1) {
		result.error = err != B_OK ? err : B_BUSY;
		result.reason = "the device has no free output; it may be in use by another application";
		Close();
		return result;
	}

	media_format format = raw_format(type);
	if (type == B_MEDIA_RAW_AUDIO)
		format.u.raw_audio = output.format.u.raw_audio;
	else
		format.u.raw_video = output.format.u.raw_video;

	if (type == B_MEDIA_RAW_VIDEO) {
		/* Keyed on the device actually acquired, by the id the device list uses (its node name): with the
		 * fallback that is the default device, not the one asked for. "default" is also kept under its own
		 * id: the properties ask by the setting's value. */
		const std::string key = have_dormant ? std::string(dormant.name) : device_id;
		result.modes_changed = ReadVideoModes(key, output);
		if (!result.used_fallback && key != device_id) {
			HaikuVideoModes modes;
			if (cached_modes(key, modes))
				result.modes_changed = cache_modes(device_id, modes) || result.modes_changed;
		}
	}

	err = B_ERROR;
	if (type == B_MEDIA_RAW_VIDEO) {
		/* Accept any size and any rate: field_rate must be a wildcard here. The output's format carries the
		 * producer's last field_rate (usb_webcam's FIELD_RATE, 5), and Renga's FormatProposal() now puts the
		 * camera's actual rate into the proposal, which media_format::Matches() would refuse against a fixed
		 * one. The negotiated format below says what the producer picked. */
		media_format accept = format;
		set_size(accept, HaikuVideoSize());
		accept.u.raw_video.field_rate = 0.0f;
		SetAcceptedFormat(accept);
		result.requested_rate = params.size.IsDefault() ? 0.0f : params.field_rate;
		if (!params.size.IsDefault()) {
			media_format request = accept;
			set_size(request, params.size);
			request.u.raw_video.field_rate = params.field_rate;
			err = BMediaRecorder::Connect(node, &output, &request);
			if (err != B_OK) {
				result.size_refused = true;
				result.requested_rate = 0.0f;
			}
		}
		if (err != B_OK) {
			/* The device's own size, which the output's format has (usb_webcam treats 0 x 0 as 320 x 240,
			 * UVCCamDevice::AcceptVideoFrame()), at its default rate for it (field_rate 0). */
			media_format request = format;
			request.u.raw_video.field_rate = 0.0f;
			err = BMediaRecorder::Connect(node, &output, &request);
		}
	} else {
		SetAcceptedFormat(format);
		err = BMediaRecorder::Connect(node, &output, &format);
	}
	if (err != B_OK) {
		result.error = err;
		result.reason = "connecting to the device failed";
		Close();
		return result;
	}

	/* MediaInput() carries the format that was actually negotiated */
	SetConnectedFormat(MediaInput().format);

	/* Buffers only arrive from here on: the connection is made muted (B_CONNECT_MUTED in
	 * BMediaRecorder::_Connect()) and Start() enables the data. */
	err = Start();
	if (err != B_OK) {
		result.error = err;
		result.reason = "starting the device failed";
		Close();
		return result;
	}

	opened = true;
	return result;
}

void HaikuRecorder::Close()
{
	if (IsRunning())
		Stop(true);
	if (IsConnected())
		Disconnect();

	if (have_node) {
		/* For a loopback this is the only reference: the media add-on server deletes the instance. */
		if (BMediaRoster *roster = BMediaRoster::CurrentRoster())
			roster->ReleaseNode(node);
		have_node = false;
	}
	have_dormant = false;

	opened = false;
}

media_format HaikuRecorder::ConnectedFormat()
{
	std::lock_guard<std::mutex> lock(shared->format_mutex);
	return shared->connected_format;
}

bool HaikuRecorder::NodeAlive() const
{
	/* a node's control port lives as long as the node (BMediaNode deletes it on destruction), and with it
	 * the team that hosts the node */
	port_info info;
	return have_node && get_port_info(node.port, &info) == B_OK;
}

bool HaikuRecorder::DevicePublished() const
{
	if (!have_dormant)
		return true;
	BMediaRoster *roster = BMediaRoster::CurrentRoster();
	if (!roster)
		return true;

	/* The usb_webcam add-on keeps the node of an unplugged camera alive (it just stops sending), but drops
	 * its flavor. A complete list without the flavor is a definite answer; anything else isn't. */
	const int32 max = 256;
	std::vector<dormant_node_info> infos((size_t)max);
	int32 count = max;
	if (roster->GetDormantNodes(infos.data(), &count, NULL, NULL, NULL, B_BUFFER_PRODUCER, 0) != B_OK ||
	    count < 0 || count >= max)
		return true;
	for (int32 i = 0; i < count; i++)
		if (infos[i].addon == dormant.addon && infos[i].flavor_id == dormant.flavor_id)
			return true;
	return false;
}

/* recorder thread; touches nothing but `shared` (see the class comment) */
void HaikuRecorder::BufferReceived(void *buffer, size_t size, const media_header &header)
{
	Shared *const s = shared;
	if (!s->func || !s->delivering)
		return;

	media_format format;
	{
		std::lock_guard<std::mutex> lock(s->format_mutex);
		format = s->connected_format;
	}
	s->func(buffer, size, format, header);
}

/* ------------------------------------------------------------------------------------------------------ */
/* Media Kit notifications                                                                                */

/* One looper for the module receives the notifications that tell us a device may have come or gone, and
 * wakes every HaikuCapture. On Renga they are what device handling relies on (there is no polling):
 *  - B_MEDIA_FLAVORS_CHANGED: an add-on's devices changed. multi_audio's device watcher and usb_webcam call
 *    BMediaAddOn::NotifyFlavorChange() when a sound card or camera is plugged in or removed
 *    (MultiAudioAddOn.cpp, usb_webcam/AddOn.cpp CameraAdded()/CameraRemoved()); the media add-on server
 *    rescans the add-on and sends the notification (MediaAddonServer.cpp, end of _ScanAddOnFlavors()).
 *  - B_MEDIA_NODE_DELETED: a node was unregistered, or the media server cleaned up after a team that died,
 *    such as a crashed media add-on server (NodeManager::CleanupTeam(), which also reports the flavors).
 *  - B_MEDIA_SERVER_QUIT / B_MEDIA_SERVER_STARTED: from this team's own BMediaRoster.
 *  - B_MEDIA_CONNECTION_BROKEN: some connection ended; a busy device may be free. */
namespace {

std::mutex captures_mutex;
std::vector<HaikuCapture *> captures;

/* sent by the media server; registrations are lost when it restarts */
const int32 server_notifications[] = {B_MEDIA_NODE_DELETED, B_MEDIA_CONNECTION_BROKEN, B_MEDIA_FLAVORS_CHANGED};
/* sent by this team's BMediaRoster */
const int32 roster_notifications[] = {B_MEDIA_SERVER_STARTED, B_MEDIA_SERVER_QUIT};

void wake_captures(bool retry, bool server_quit, bool broken = false)
{
	std::lock_guard<std::mutex> lock(captures_mutex);
	for (HaikuCapture *capture : captures)
		capture->Wake(retry, server_quit, broken);
}

void devices_changed()
{
	std::lock_guard<std::mutex> lock(captures_mutex);
	for (HaikuCapture *capture : captures)
		capture->DevicesChanged();
}

/* a broken connection whose consumer is one of our recorders: we disconnected it ourselves */
bool own_connection(BMessage *msg)
{
	const void *data = nullptr;
	ssize_t size = 0;
	if (msg->FindData("destination", B_RAW_TYPE, &data, &size) != B_OK || size != sizeof(media_destination))
		return false;
	media_destination destination;
	memcpy(&destination, data, sizeof(destination));
	return own_port(destination.port);
}

/* posted by register_capture(): StartWatching() waits on the media server (up to its timeouts), which the
 * thread creating a source (often the UI thread) must not */
#define MSG_WATCH 0x686d5761 /* 'hmWa' */

class MediaWatcher : public BLooper {
public:
	MediaWatcher() : BLooper("haiku-media events") {}

	void Watch(bool server_only)
	{
		BMediaRoster *roster = BMediaRoster::Roster();
		if (!roster)
			return;
		const BMessenger self(this);
		for (int32 what : server_notifications)
			roster->StartWatching(self, what);
		if (!server_only)
			for (int32 what : roster_notifications)
				roster->StartWatching(self, what);
	}

	void Unwatch()
	{
		BMediaRoster *roster = BMediaRoster::CurrentRoster();
		if (!roster)
			return;
		const BMessenger self(this);
		for (int32 what : server_notifications)
			roster->StopWatching(self, what);
		for (int32 what : roster_notifications)
			roster->StopWatching(self, what);
	}

	void MessageReceived(BMessage *msg) override
	{
		switch (msg->what) {
		case MSG_WATCH:
			Watch(false);
			return;
		case B_MEDIA_SERVER_STARTED:
			Watch(true); /* the new media server knows nothing of our registrations */
			wake_captures(true, false);
			return;
		case B_MEDIA_SERVER_QUIT:
			wake_captures(false, true);
			return;
		case B_MEDIA_CONNECTION_BROKEN:
			/* another application may have let go of a busy device; our own disconnects (Close()) say
			 * nothing */
			if (!own_connection(msg))
				wake_captures(false, false, true);
			return;
		case B_MEDIA_FLAVORS_CHANGED: /* a device was plugged in or removed */
			wake_captures(true, false);
			devices_changed();
			return;
		case B_MEDIA_NODE_DELETED:
			/* only a reason to check open devices: retrying on it would wake on our own recorders */
			wake_captures(false, false);
			return;
		default:
			BLooper::MessageReceived(msg);
		}
	}
};

MediaWatcher *watcher = nullptr; /* captures_mutex */

void register_capture(HaikuCapture *capture)
{
	std::lock_guard<std::mutex> lock(captures_mutex);
	captures.push_back(capture);
	if (watcher)
		return;
	watcher = new MediaWatcher;
	if (watcher->Run() < 0) {
		/* can't happen short of running out of threads or ports; a looper whose Run() failed half-way
		 * can't be deleted safely, so it is left alone (it is tiny) */
		watcher = nullptr;
		blog(LOG_ERROR, "[haiku-media] cannot start the Media Kit notification looper; devices that are "
				"unplugged or plugged in will not be noticed");
		return;
	}
	watcher->PostMessage(MSG_WATCH);
}

void unregister_capture(HaikuCapture *capture)
{
	MediaWatcher *last = nullptr;
	{
		std::lock_guard<std::mutex> lock(captures_mutex);
		captures.erase(std::remove(captures.begin(), captures.end(), capture), captures.end());
		if (captures.empty()) {
			last = watcher;
			watcher = nullptr;
		}
	}
	/* outside captures_mutex: the looper may be in wake_captures(), waiting for it. Under the looper's lock:
	 * a queued MSG_WATCH either ran already (and is undone here) or never runs. */
	if (last && last->Lock()) {
		last->Unwatch();
		last->Quit();
	}
}

} // namespace

/* ------------------------------------------------------------------------------------------------------ */
/* HaikuCapture                                                                                           */

HaikuCapture::HaikuCapture(obs_source_t *source_, const char *recorder_name_, media_type type_,
			   HaikuCaptureKind kind_, HaikuRecorder::BufferFunc on_buffer_, OpenedFunc on_opened_,
			   ClosedFunc on_closed_)
	: source(source_),
	  recorder_name(recorder_name_),
	  type(type_),
	  kind(kind_),
	  on_buffer(std::move(on_buffer_)),
	  on_opened(std::move(on_opened_)),
	  on_closed(std::move(on_closed_))
{
	register_capture(this);
	/* An exception from here would leave through the source's create callback, a C function, and end in
	 * std::terminate(). Without the thread the source stays silent (or black); ~HaikuCapture() copes. */
	try {
		thread = std::thread([this]() { Run(); });
	} catch (const std::system_error &error) {
		blog(LOG_ERROR, "[haiku-media] '%s': cannot start the capture thread: %s", obs_source_get_name(source),
		     error.what());
	}
}

HaikuCapture::~HaikuCapture()
{
	unregister_capture(this); /* no Wake() from here on */
	{
		std::lock_guard<std::mutex> lock(mutex);
		stop = true;
	}
	cv.notify_all();
	if (thread.joinable())
		thread.join();
}

void HaikuCapture::Configure(const std::string &device_id, const HaikuOpenParams &params, bool active)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (desired.device_id == device_id && desired.params == params && desired.active == active)
			return;
		if (desired.device_id != device_id || !(desired.params == params))
			generation++;
		desired.device_id = device_id;
		desired.params = params;
		desired.active = active;
		changed = true;
	}
	cv.notify_all();
}

void HaikuCapture::Wake(bool retry, bool server_quit, bool broken)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (retry)
			retry_event = true;
		else if (broken)
			broken_event = true;
		else
			check_event = true;
		if (server_quit)
			server_quit_event = true;
	}
	cv.notify_all();
}

/* Watcher looper thread, with the list of captures locked (so this capture is not being destroyed). The
 * signal only tells listeners to reload the properties; the frontend's handlers queue that to the UI thread,
 * as they must for every source that updates its properties from a worker thread. */
void HaikuCapture::DevicesChanged()
{
	if (kind == HaikuCaptureKind::Device)
		obs_source_update_properties(source);
}

void HaikuCapture::Run()
{
	os_set_thread_name(type == B_MEDIA_RAW_VIDEO ? "haiku-video-capture" : "haiku-audio-capture");

	std::unique_lock<std::mutex> lock(mutex);
	while (!stop) {
		const Config want = desired;
		const uint64_t gen = generation;
		const bool retry = retry_event, broken = broken_event, check = check_event, quit = server_quit_event;
		retry_event = broken_event = check_event = server_quit_event = changed = false;
		lock.unlock();

		const bigtime_t wait = Step(want, gen, retry, broken, check, quit);

		lock.lock();
		if (stop)
			break;
		cv.wait_for(lock, std::chrono::microseconds(std::max<bigtime_t>(wait, 1000)), [this]() {
			return stop || changed || retry_event || broken_event || check_event || server_quit_event;
		});
	}
	lock.unlock();

	CloseRecorder();
}

void HaikuCapture::CloseRecorder(bool notify)
{
	const bool was_open = recorder && recorder->IsOpen();
	/* deleting the recorder also unregisters its node: after a media server restart the old registration
	 * is useless, so a reconnect always starts with a fresh one */
	recorder.reset();
	using_fallback = false;
	unpublished_seen = 0;
	if (was_open && notify && on_closed)
		on_closed();
}

/* what is being captured, for the log */
static std::string target_label(HaikuCaptureKind kind, const std::string &device_id)
{
	if (kind == HaikuCaptureKind::Loopback)
		return device_id.empty() ? "the system mixer's loopback" : "the loopback of '" + device_id + "'";
	return "'" + device_id + "'";
}

void HaikuCapture::StartStreak(const char *what, const Config &want, const HaikuOpenResult *result)
{
	if (in_streak)
		return; /* logged once per streak */
	in_streak = true;
	recovery_logged = false;
	broken_pulled = false;
	if (result)
		blog(LOG_WARNING, "[haiku-media] '%s': %s %s: %s (%s); retrying in the background",
		     obs_source_get_name(source), what, target_label(kind, want.device_id).c_str(),
		     result->reason.c_str(), strerror(result->error));
	else
		blog(LOG_WARNING, "[haiku-media] '%s': %s %s; reconnecting when it is back",
		     obs_source_get_name(source), what, target_label(kind, want.device_id).c_str());
}

/* After a successful open: logging and bookkeeping shared by Step() and SwitchFromFallback(). */
void HaikuCapture::Opened(const Config &want, const HaikuOpenResult &result)
{
	if (in_streak && !recovery_logged) {
		blog(LOG_INFO, "[haiku-media] '%s': %s is open again", obs_source_get_name(source),
		     target_label(kind, want.device_id).c_str());
		recovery_logged = true;
	}
	if (result.used_fallback) {
		blog(LOG_WARNING,
		     "[haiku-media] '%s': device '%s' not found, using the default device until it appears",
		     obs_source_get_name(source), want.device_id.c_str());
		switch_backoff = 0;
		switch_next = 0;
		switch_failed_logged = false;
	}
	opened_at = system_time();
	unpublished_seen = 0;
	using_fallback = result.used_fallback;

	if (on_opened)
		on_opened(recorder->ConnectedFormat(), result);
	/* the resolution and frame rate lists come from what the device listed just now; the frontend queues
	 * the reload to the UI thread (see DevicesChanged()) */
	if (result.modes_changed && kind == HaikuCaptureKind::Device)
		obs_source_update_properties(source);
}

/* The named device is listed again while the default device stands in for it. It is opened next to the
 * fallback (which keeps feeding the source meanwhile) and replaces it only once it works; if it can't be
 * opened the fallback stays, and the switch is tried again later with the usual backoff. */
bool HaikuCapture::SwitchFromFallback(const Config &want)
{
	HaikuRecorderPtr candidate(new HaikuRecorder(recorder_name.c_str(), type, kind, on_buffer));
	candidate->SetDelivering(false);
	const HaikuOpenResult result = candidate->Open(want.device_id, want.params, false);
	if (result.error != B_OK) {
		if (!switch_failed_logged) {
			blog(LOG_WARNING, "[haiku-media] '%s': '%s' is available but cannot be opened: %s (%s); "
					  "staying on the default device and retrying in the background",
			     obs_source_get_name(source), want.device_id.c_str(), result.reason.c_str(),
			     strerror(result.error));
			switch_failed_logged = true;
		}
		return false;
	}

	blog(LOG_INFO, "[haiku-media] '%s': '%s' is available now, switched to it", obs_source_get_name(source),
	     want.device_id.c_str());
	CloseRecorder(false); /* the source gets the new device's frames right away; no blank frame between */
	recorder = std::move(candidate);
	recorder->SetDelivering(true);
	Opened(want, result);
	return true;
}

bigtime_t HaikuCapture::Step(const Config &want, uint64_t gen, bool retry, bool broken, bool check,
			     bool server_quit)
{
	const bigtime_t now = system_time();

	if (!want.active) {
		CloseRecorder();
		in_streak = false;
		backoff = 0;
		applied_generation = UINT64_MAX; /* reactivating starts afresh */
		return IDLE_US;                  /* Configure() wakes us */
	}

	if (gen != applied_generation) {
		CloseRecorder();
		applied_generation = gen;
		in_streak = false;
		backoff = 0;
		next_attempt = now;
	}

	if (recorder && recorder->IsOpen()) {
		/* Checked when the Media Kit reports something (see the watcher above), not periodically. */
		bool lost = server_quit || !recorder->NodeAlive();
		if (!lost && (check || retry || broken || unpublished_seen > 0)) {
			/* twice in a row, so a list read while the add-on server rescans can't drop the device */
			if (recorder->DevicePublished())
				unpublished_seen = 0;
			else
				lost = ++unpublished_seen >= 2;
		}
		/* On the default device because the named one was missing: switch when a device arrives, and
		 * again (with backoff) after a switch that failed. */
		if (!lost && using_fallback && (retry || switch_backoff) && now >= switch_next) {
			if (!device_listed(want.device_id, type)) {
				/* not there (any more): nothing to retry until the devices change again */
				switch_backoff = 0;
			} else if (SwitchFromFallback(want)) {
				return in_streak ? STABLE_US : IDLE_US;
			} else {
				switch_backoff = switch_backoff ? std::min<bigtime_t>(switch_backoff * 2, RETRY_MAX_US)
								: RETRY_FIRST_US;
				switch_next = system_time() + switch_backoff;
			}
		}
		if (!lost) {
			bigtime_t wait = IDLE_US;
			if (unpublished_seen > 0)
				wait = RECHECK_US;
			if (in_streak) {
				/* open long enough: a device that fails again starts a new streak (logged, quick
				 * retry) */
				if (now - opened_at >= STABLE_US) {
					in_streak = false;
					backoff = 0;
				} else {
					wait = std::min(wait, opened_at + STABLE_US - now);
				}
			}
			if (using_fallback && switch_backoff)
				wait = std::min(wait, std::max<bigtime_t>(switch_next - system_time(), 0));
			return wait;
		}

		StartStreak("lost", want, nullptr);
		CloseRecorder();
		backoff = backoff ? std::min<bigtime_t>(backoff * 2, RETRY_MAX_US) : RETRY_FIRST_US;
		next_attempt = now + backoff;
		return backoff;
	}

	/* closed: open, or wait for the next attempt */
	/* a device may have become available: try sooner, but not more than once a second. A broken
	 * connection elsewhere only pulls the retry forward once per streak: some applications reconnect
	 * all the time, and each attempt costs a device list query. */
	if ((retry || (broken && !broken_pulled)) && next_attempt > now) {
		if (!retry)
			broken_pulled = true;
		next_attempt = std::max(now, std::min(next_attempt, last_attempt + RETRY_FIRST_US));
	}
	if (now < next_attempt)
		return next_attempt - now;

	last_attempt = now;
	recorder.reset(new HaikuRecorder(recorder_name.c_str(), type, kind, on_buffer));
	const HaikuOpenResult result = recorder->Open(want.device_id, want.params);
	if (result.error != B_OK) {
		StartStreak("cannot open", want, &result);
		recorder.reset();
		/* the modes may have been read before connecting failed */
		if (result.modes_changed && kind == HaikuCaptureKind::Device)
			obs_source_update_properties(source);
		backoff = backoff ? std::min<bigtime_t>(backoff * 2, RETRY_MAX_US) : RETRY_FIRST_US;
		next_attempt = system_time() + backoff;
		return backoff;
	}

	Opened(want, result);
	return in_streak ? STABLE_US : IDLE_US;
}
