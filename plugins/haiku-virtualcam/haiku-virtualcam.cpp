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

/* OBS's virtual camera on Renga (docs/haiku/ARCHITECTURE.md, "Virtual camera").
 *
 * Renga's roadmap F-58 (OBS request O-12, Renga commit d6f1bc7b) added a Media Kit video source, the "Virtual
 * Camera" flavor of the virtual_camera media add-on, that one program feeds and every other program opens like
 * any camera (CodyCam, the Media preferences, Chromium). It is fed through BPrivate::VirtualCameraFeeder from the
 * header-only private header VirtualCamera.h: Connect(width, height, fieldRate, slotCount) sends a memfd with
 * slotCount B_RGB32 frame slots to the node over the local SOCK_SEQPACKET socket VIRTUAL_CAMERA_SOCKET, and
 * WriteFrame() copies one frame into the next slot and announces it with a small message. The node scales what
 * it gets to whatever size its consumer connected with.
 *
 * This module registers the output type "virtualcam_output", the id the frontend looks for (frontend/utility/
 * VCamConfig.hpp, VIRTUAL_CAM_ID), as plugins/linux-v4l2 and plugins/mac-virtualcam do. The frontend chooses what
 * the camera shows (program, preview, a scene or a source: OBSBasicVCamConfig) by giving the output a video_t
 * with obs_output_set_media(); this output just sends whatever video it is given, at that video's size.
 *
 * Threads: raw_video() runs on libobs' video output thread and calls WriteFrame(..., 0) itself (F-58 batch 2,
 * OBS request O-20, Renga commit 677afa3d). With a timeout of 0 WriteFrame() never waits: it first polls the
 * socket, and if the node has not taken the previous frames (media_addon_server busy, stopped or hung) it returns
 * B_WOULD_BLOCK without copying, and the frame counts as dropped; the feed stays connected. Otherwise it copies
 * the frame once, row by row (so OBS's line size needs no copy of ours), into the shared memfd and sends a
 * 16-byte message with MSG_DONTWAIT. The copy is what the previous design (a writer thread fed through a
 * one-slot mailbox) also did on this thread, into the mailbox: about 8 MB at 1080p, 1-2 ms, which the video
 * thread can afford; the writer's second copy, its thread and the mailbox are gone. Disconnect() never waits for
 * the node either (it shuts the socket down first), so stop() cannot hang.
 * VirtualCameraFeeder is "not thread safe: one thread at a time" (VirtualCamera.h), and libobs' video thread may
 * still call raw_video() for a moment after stop() (obs_output_end_data_capture() stops the callbacks on a
 * thread of its own). So every use of the feeder is under `feed_mutex`; nothing under it waits for anything.
 */

#include <obs-module.h>
#include <media-io/video-io.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <FindDirectory.h>
#include <OS.h>

#include <VirtualCamera.h>

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <new>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("haiku-virtualcam", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Virtual camera for Renga: OBS's output as the Media Kit's \"Virtual Camera\" that other applications "
	       "open like a camera";
}

namespace {

/* The feeder's default (VirtualCamera.h): one slot the node may be copying, one with the newest frame, one being
 * written. More only adds memory; a frame that finds no room is dropped, not queued. */
constexpr uint32 kSlotCount = 3;

/* How often a summary of dropped frames is logged while frames are being dropped (os_gettime_ns()) */
constexpr uint64_t kDropReportInterval = 60000000000ULL;

struct haiku_vcam {
	obs_output_t *output = nullptr;

	/* Guards `feeder` and the counters (see the top of the file). */
	pthread_mutex_t feed_mutex;
	BPrivate::VirtualCameraFeeder feeder;
	uint64_t sent = 0;
	uint64_t dropped = 0;
	uint64_t report_sent = 0; /* the counts at the last summary */
	uint64_t report_dropped = 0;
	uint64_t next_report = 0;

	/* Set when the feed broke; raw_video() then does nothing until the next start(). */
	volatile bool failed = false;
};

const char *vcam_name(void *)
{
	return obs_module_text("VirtualCamera");
}

void set_error(haiku_vcam *vcam, const char *text_id, status_t status)
{
	struct dstr message = {};
	dstr_copy(&message, obs_module_text(text_id));
	if (status != B_OK) {
		dstr_cat(&message, "\n\n");
		dstr_cat(&message, strerror(status));
	}
	obs_output_set_last_error(vcam->output, message.array);
	blog(LOG_WARNING, "[haiku-virtualcam] %s", message.array);
	dstr_free(&message);
}

/* feed_mutex held. Ends the feed and logs what it sent. */
void disconnect_locked(haiku_vcam *vcam, bool log_totals)
{
	vcam->feeder.Disconnect();
	if (log_totals && (vcam->sent || vcam->dropped)) {
		blog(LOG_INFO,
		     "[haiku-virtualcam] %llu frames sent, %llu dropped (the Virtual Camera had not taken the "
		     "previous frames yet)",
		     (unsigned long long)vcam->sent, (unsigned long long)vcam->dropped);
	}
	vcam->sent = vcam->dropped = vcam->report_sent = vcam->report_dropped = 0;
}

void *vcam_create(obs_data_t *, obs_output_t *output)
{
	haiku_vcam *vcam = new (std::nothrow) haiku_vcam;
	if (!vcam) {
		return nullptr;
	}
	vcam->output = output;
	pthread_mutex_init(&vcam->feed_mutex, nullptr);
	return vcam;
}

void vcam_destroy(void *data)
{
	haiku_vcam *vcam = static_cast<haiku_vcam *>(data);
	/* libobs stops an active output and waits for its data capture to end (obs_output_destroy()) before it
	 * calls destroy(), so raw_video() no longer runs. */
	pthread_mutex_lock(&vcam->feed_mutex);
	disconnect_locked(vcam, false);
	pthread_mutex_unlock(&vcam->feed_mutex);
	pthread_mutex_destroy(&vcam->feed_mutex);
	delete vcam;
}

bool vcam_start(void *data)
{
	haiku_vcam *vcam = static_cast<haiku_vcam *>(data);

	video_t *video = obs_output_video(vcam->output);
	if (!video) {
		return false;
	}

	/* The size of the video the frontend gave us, not obs_output_get_width(): that returns the conversion
	 * size of the previous start, which is stale after the canvas or output resolution changed. */
	const uint32_t width = video_output_get_width(video);
	const uint32_t height = video_output_get_height(video);
	const struct video_output_info *info = video_output_get_info(video);
	if (width == 0 || height == 0 || !info || info->fps_den == 0) {
		return false;
	}
	if (width > VIRTUAL_CAMERA_MAX_SIZE || height > VIRTUAL_CAMERA_MAX_SIZE) {
		set_error(vcam, "Error.TooLarge", B_OK);
		return false;
	}
	const float fps = (float)info->fps_num / (float)info->fps_den;

	/* libobs starts an output only after the previous data capture has fully ended (obs_output_actual_start()
	 * waits for stopping_event), so raw_video() does not run here. One request and its reply on the local
	 * socket; the node's feed thread answers at once (B_OK, or B_BUSY from its poll loop while another program
	 * feeds the camera). A feed left over from a failure is replaced (Connect() disconnects first). */
	pthread_mutex_lock(&vcam->feed_mutex);
	disconnect_locked(vcam, false);
	const status_t status = vcam->feeder.Connect(width, height, fps, kSlotCount);
	vcam->next_report = os_gettime_ns() + kDropReportInterval;
	pthread_mutex_unlock(&vcam->feed_mutex);
	if (status != B_OK) {
		if (status == B_BUSY) {
			set_error(vcam, "Error.Busy", B_OK);
		} else if (status == B_NO_INIT) { /* the socket does not exist: no node, media services not running */
			set_error(vcam, "Error.NoMediaServices", B_OK);
		} else {
			set_error(vcam, "Error.ConnectFailed", status);
		}
		return false;
	}
	os_atomic_set_bool(&vcam->failed, false);

	/* B_RGB32 is B,G,R,A bytes in memory on little-endian x86 (GraphicsDefs.h), which is VIDEO_FORMAT_BGRA;
	 * libobs converts to it (and scales, which it never needs to here) on the CPU when the output format
	 * differs. BGRA rather than BGRX so that the alpha byte is 255 for consumers that read it, as the node's
	 * own "no signal" frame has it (0xff303030). Full range: RGB has no limited range. */
	struct video_scale_info conversion = {};
	conversion.format = VIDEO_FORMAT_BGRA;
	conversion.width = width;
	conversion.height = height;
	conversion.range = VIDEO_RANGE_FULL;
	conversion.colorspace = info->colorspace;
	obs_output_set_video_conversion(vcam->output, &conversion);

	if (!obs_output_begin_data_capture(vcam->output, 0)) {
		pthread_mutex_lock(&vcam->feed_mutex);
		disconnect_locked(vcam, false);
		pthread_mutex_unlock(&vcam->feed_mutex);
		return false;
	}

	blog(LOG_INFO, "[haiku-virtualcam] feeding the Virtual Camera with %ux%u at %.2f fps", width, height, fps);
	return true;
}

void vcam_stop(void *data, uint64_t)
{
	haiku_vcam *vcam = static_cast<haiku_vcam *>(data);

	/* Ends the raw video callbacks (on libobs' end-data-capture thread, so raw_video() may run a little longer;
	 * it finds the feeder disconnected). Called even when raw_video() has just signalled a failure: in that
	 * case the capture has already ended and this only emits "stop" and the stopping event again, which
	 * libobs' obs_output_actual_stop() needs to see after it reset the event. Disconnect() does not wait for
	 * the node (O-20). */
	obs_output_end_data_capture(vcam->output);
	pthread_mutex_lock(&vcam->feed_mutex);
	disconnect_locked(vcam, true);
	pthread_mutex_unlock(&vcam->feed_mutex);
	blog(LOG_INFO, "[haiku-virtualcam] stopped");
}

/* WriteFrame() uses send() without MSG_NOSIGNAL: if media_addon_server goes away, send() fails with EPIPE and
 * Haiku's socket layer raises SIGPIPE in the sending thread (src/add-ons/kernel/network/stack/net_socket.cpp).
 * The frontend blocks SIGPIPE in main() before any thread is made (obs-main.cpp), and libobs' threads inherit
 * that mask; blocking it in the video thread as well (once) keeps libobs users without that frontend safe. The
 * failure is then reported by WriteFrame()'s return value. */
void block_sigpipe_once()
{
	static thread_local bool blocked = false;
	if (blocked) {
		return;
	}
	sigset_t pipe_set;
	sigemptyset(&pipe_set);
	sigaddset(&pipe_set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipe_set, nullptr);
	blocked = true;
}

void vcam_raw_video(void *data, struct video_data *frame)
{
	haiku_vcam *vcam = static_cast<haiku_vcam *>(data);
	if (os_atomic_load_bool(&vcam->failed)) {
		return;
	}
	block_sigpipe_once();

	/* OBS timestamps come from os_gettime_ns(), which is clock_gettime(CLOCK_MONOTONIC) (libobs/util/
	 * platform-nix.c), and Renga's libroot implements CLOCK_MONOTONIC as system_time() (src/system/libroot/
	 * posix/time/clock_support.cpp). So the frame time in system_time() microseconds, which
	 * virtual_camera_slot::time is, is the timestamp divided by 1000. */
	const bigtime_t time = (bigtime_t)(frame->timestamp / 1000);

	pthread_mutex_lock(&vcam->feed_mutex);
	if (!vcam->feeder.IsConnected()) {
		/* stopped: a last callback before libobs removed it */
		pthread_mutex_unlock(&vcam->feed_mutex);
		return;
	}
	/* The frame is the size given to Connect(): libobs converts to the size set in start() */
	const status_t status = vcam->feeder.WriteFrame(frame->data[0], frame->linesize[0], time, 0);
	if (status == B_OK) {
		vcam->sent++;
	} else if (status == B_WOULD_BLOCK) {
		vcam->dropped++;
	}
	const uint64_t now = os_gettime_ns();
	if (status == B_OK || status == B_WOULD_BLOCK) {
		if (now >= vcam->next_report) {
			const uint64_t dropped = vcam->dropped - vcam->report_dropped;
			if (dropped) {
				blog(LOG_INFO,
				     "[haiku-virtualcam] %llu of %llu frames dropped in the last minute (the Virtual "
				     "Camera was not keeping up)",
				     (unsigned long long)dropped,
				     (unsigned long long)(dropped + vcam->sent - vcam->report_sent));
			}
			vcam->report_sent = vcam->sent;
			vcam->report_dropped = vcam->dropped;
			vcam->next_report = now + kDropReportInterval;
		}
		pthread_mutex_unlock(&vcam->feed_mutex);
		return;
	}
	/* Any other error: WriteFrame() has already disconnected the feeder (VirtualCamera.h); this logs the
	 * totals. */
	disconnect_locked(vcam, true);
	pthread_mutex_unlock(&vcam->feed_mutex);

	/* End the output the way network outputs do on a lost connection: libobs ends the data capture on a thread
	 * of its own (obs_output_end_data_capture_internal()), so this does not wait here, and emits "stop" with
	 * the code; the frontend shows the last error. Our stop() is not called then; the next start() or
	 * destroy() cleans up. */
	os_atomic_set_bool(&vcam->failed, true);
	set_error(vcam, "Error.Lost", status);
	obs_output_signal_stop(vcam->output, OBS_OUTPUT_ERROR);
}

/* Whether this Renga has the virtual camera add-on (Renga d6f1bc7b and later): it is installed as
 * add-ons/media/virtual_camera.media_addon of the haiku package (build/jam/packages/Haiku,
 * SYSTEM_ADD_ONS_MEDIA). Looked up as a file, not through BMediaRoster, so that the answer does not depend on the
 * media services running at the moment OBS loads its modules; if they are not running when the camera is
 * started, start() says so. */
bool virtual_camera_installed()
{
	char **paths = nullptr;
	size_t count = 0;
	if (find_paths_etc(nullptr, B_FIND_PATH_MEDIA_NODES_DIRECTORY, "virtual_camera.media_addon",
			   B_FIND_PATH_EXISTING_ONLY, &paths, &count) != B_OK) {
		return false;
	}
	free(paths); /* one allocation (find_paths_etc() in src/system/libroot/os/find_paths.cpp) */
	return count > 0;
}

struct obs_output_info make_output_info()
{
	struct obs_output_info info = {};
	info.id = "virtualcam_output";
	info.flags = OBS_OUTPUT_VIDEO;
	info.get_name = vcam_name;
	info.create = vcam_create;
	info.destroy = vcam_destroy;
	info.start = vcam_start;
	info.stop = vcam_stop;
	info.raw_video = vcam_raw_video;
	return info;
}

} // namespace

bool obs_module_load(void)
{
	/* Like linux-v4l2, which registers the output only when v4l2loopback is there: the frontend shows "Start
	 * Virtual Camera" exactly when the type exists (OBSBasic::OBSInit(), obs_get_output_flags()). */
	if (!virtual_camera_installed()) {
		blog(LOG_WARNING, "[haiku-virtualcam] virtual_camera.media_addon not found (Renga older than F-58); "
				  "virtual camera not registered");
		return true;
	}
	static const struct obs_output_info info = make_output_info();
	obs_register_output(&info);
	return true;
}
