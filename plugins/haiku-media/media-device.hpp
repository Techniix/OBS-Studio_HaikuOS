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

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <MediaDefs.h>
#include <MediaNode.h>
#include <MediaRecorder.h>

#define HAIKU_MEDIA_DEFAULT_DEVICE "default"

/* A physical capture device as published by a Media Kit add-on (e.g. "HDA audio input", "USB Webcam"). */
struct HaikuMediaDevice {
	std::string id;   /* persistent id: the add-on's node name */
	std::string name; /* display name */
};

/* Lists physical input devices that produce raw buffers of the given type. */
std::vector<HaikuMediaDevice> HaikuEnumerateCaptureDevices(media_type type);

/* Fills an OBS list property with "Default" plus the available devices. */
void HaikuFillDeviceList(obs_property_t *list, media_type type);

/* BMediaRecorder that forwards buffers together with the format negotiated for the connection. */
class HaikuRecorder : public BMediaRecorder {
public:
	using BufferFunc = std::function<void(const void *data, size_t size, const media_format &format)>;

	HaikuRecorder(const char *name, media_type type, BufferFunc func);
	~HaikuRecorder() override;

	/* Acquires the device's node (or the system default for `type`), connects to a free output and
	 * starts the producer. Safe to call again to switch devices. */
	status_t Open(const std::string &device_id, media_type type);
	void Close();

	bool IsOpen() const { return opened; }
	const media_format &ConnectedFormat() const { return connected_format; }

protected:
	void BufferReceived(void *buffer, size_t size, const media_header &header) override;

private:
	BufferFunc func;
	media_node node;
	bool have_node = false;
	bool opened = false;
	media_format connected_format;
	std::mutex format_mutex;
};
