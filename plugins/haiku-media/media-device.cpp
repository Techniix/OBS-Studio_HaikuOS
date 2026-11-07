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

#include <algorithm>
#include <cstring>

#include <MediaAddOn.h>
#include <MediaRoster.h>

#define T_(text) obs_module_text(text)

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

	int32 count = 64;
	nodes.resize((size_t)count);
	err = roster->GetDormantNodes(nodes.data(), &count, NULL, &format, NULL, B_BUFFER_PRODUCER | B_PHYSICAL_INPUT);
	if (err != B_OK || count < 0)
		count = 0;

	nodes.resize((size_t)std::min<int32>(count, 64));
	return nodes;
}

std::vector<HaikuMediaDevice> HaikuEnumerateCaptureDevices(media_type type)
{
	std::vector<HaikuMediaDevice> devices;

	for (const dormant_node_info &info : dormant_capture_nodes(type))
		devices.push_back({info.name, info.name});

	return devices;
}

void HaikuFillDeviceList(obs_property_t *list, media_type type)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, T_("Default"), HAIKU_MEDIA_DEFAULT_DEVICE);

	for (const HaikuMediaDevice &device : HaikuEnumerateCaptureDevices(type))
		obs_property_list_add_string(list, device.name.c_str(), device.id.c_str());
}

/* Gets a reference to the node for a device; the caller must ReleaseNode() it. */
static status_t acquire_node(const std::string &device_id, media_type type, media_node *node)
{
	status_t err;
	BMediaRoster *roster = BMediaRoster::Roster(&err);
	if (!roster)
		return err;

	if (device_id.empty() || device_id == HAIKU_MEDIA_DEFAULT_DEVICE) {
		/* The devices selected in Haiku's Media preferences */
		return type == B_MEDIA_RAW_AUDIO ? roster->GetAudioInput(node) : roster->GetVideoInput(node);
	}

	for (const dormant_node_info &info : dormant_capture_nodes(type)) {
		if (device_id != info.name)
			continue;

		/* Reuse a running instance (e.g. the system's audio input) rather than creating a second one */
		media_node_id id;
		int32 count = 1;
		if (roster->GetInstancesFor(info.addon, info.flavor_id, &id, &count) == B_OK && count > 0)
			return roster->GetNodeFor(id, node);

		return roster->InstantiateDormantNode(info, node, B_FLAVOR_IS_GLOBAL);
	}

	blog(LOG_WARNING, "[haiku-media] Device '%s' not found, using the default device", device_id.c_str());
	return acquire_node(HAIKU_MEDIA_DEFAULT_DEVICE, type, node);
}

HaikuRecorder::HaikuRecorder(const char *name, media_type type, BufferFunc func_)
	: BMediaRecorder(name, type),
	  func(std::move(func_))
{
}

HaikuRecorder::~HaikuRecorder()
{
	Close();
}

status_t HaikuRecorder::Open(const std::string &device_id, media_type type)
{
	Close();

	status_t err = InitCheck();
	if (err != B_OK) {
		blog(LOG_ERROR, "[haiku-media] Media recorder init failed: %s (is the media server running?)",
		     strerror(err));
		return err;
	}

	BMediaRoster *roster = BMediaRoster::Roster(&err);
	if (!roster)
		return err;

	err = acquire_node(device_id, type, &node);
	if (err != B_OK) {
		blog(LOG_WARNING, "[haiku-media] No %s capture device available: %s",
		     type == B_MEDIA_RAW_AUDIO ? "audio" : "video", strerror(err));
		return err;
	}
	have_node = true;

	media_output output;
	int32 count = 0;
	err = roster->GetFreeOutputsFor(node, &output, 1, &count, type);
	if (err != B_OK || count < 1) {
		blog(LOG_WARNING,
		     "[haiku-media] The device has no free output; it may already be in use by another "
		     "application");
		Close();
		return err != B_OK ? err : B_BUSY;
	}

	media_format format = raw_format(type);
	if (type == B_MEDIA_RAW_AUDIO)
		format.u.raw_audio = output.format.u.raw_audio;
	else
		format.u.raw_video = output.format.u.raw_video;

	SetAcceptedFormat(format);

	err = Connect(node, &output, &format);
	if (err != B_OK) {
		blog(LOG_WARNING, "[haiku-media] Connecting to the device failed: %s", strerror(err));
		Close();
		return err;
	}

	{
		std::lock_guard<std::mutex> lock(format_mutex);
		/* MediaInput() carries the format that was actually negotiated */
		connected_format = MediaInput().format;
	}

	err = Start();
	if (err != B_OK) {
		blog(LOG_WARNING, "[haiku-media] Starting the device failed: %s", strerror(err));
		Close();
		return err;
	}

	opened = true;
	return B_OK;
}

void HaikuRecorder::Close()
{
	if (IsRunning())
		Stop(true);
	if (IsConnected())
		Disconnect();

	if (have_node) {
		BMediaRoster::Roster()->ReleaseNode(node);
		have_node = false;
	}

	opened = false;
}

void HaikuRecorder::BufferReceived(void *buffer, size_t size, const media_header &)
{
	media_format format;
	{
		std::lock_guard<std::mutex> lock(format_mutex);
		format = connected_format;
	}

	if (func)
		func(buffer, size, format);
}
