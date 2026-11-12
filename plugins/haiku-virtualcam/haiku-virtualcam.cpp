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
 * WriteFrame(bits, bytesPerRow, time) copies one frame into the next slot and announces it with a small message.
 * The node scales what it gets to whatever size its consumer connected with.
 *
 * This module registers the output type "virtualcam_output", the id the frontend looks for (frontend/utility/
 * VCamConfig.hpp, VIRTUAL_CAM_ID), as plugins/linux-v4l2 and plugins/mac-virtualcam do. The frontend chooses what
 * the camera shows (program, preview, a scene or a source: OBSBasicVCamConfig) by giving the output a video_t
 * with obs_output_set_media(); this output just sends whatever video it is given, at that video's size.
 *
 * Threads:
 *  - start()/stop()/destroy() run on the thread that starts or stops the output (the UI thread, or e.g.
 *    obs-websocket's); libobs does not call them concurrently for one output.
 *  - raw_video() runs on libobs' video output thread. It only copies the frame into a buffer it owns and puts it
 *    into a one-slot mailbox, dropping the frame that was there if the writer has not taken it yet.
 *  - The writer thread takes frames from the mailbox and calls WriteFrame(). WriteFrame() ends in a blocking
 *    send() on the socket, which waits whenever the node's socket buffer is full (the node not reading, e.g. a
 *    busy or stuck media_addon_server), so it must not run on the video thread, which renders for every other
 *    output too.
 * VirtualCameraFeeder is "not thread safe: one thread at a time" (VirtualCamera.h). Here it is used by start()
 * before the writer exists, then only by the writer, and by stop()/start()/destroy() again after the writer has
 * been joined; pthread_create() and pthread_join() order the accesses.
 */

#include <obs-module.h>
#include <media-io/video-io.h>
#include <util/bmem.h>
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
#include <utility>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("haiku-virtualcam", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Virtual camera for Renga: OBS's output as the Media Kit's \"Virtual Camera\" that other applications "
	       "open like a camera";
}

namespace {

/* The feeder's default (VirtualCamera.h): one slot the node may be copying, one with the newest frame, one being
 * written. More only adds memory: frames are dropped in our mailbox, not queued in the slots. */
constexpr uint32 kSlotCount = 3;

struct haiku_vcam {
	obs_output_t *output = nullptr;
	BPrivate::VirtualCameraFeeder feeder;

	uint32_t width = 0;
	uint32_t height = 0;
	size_t buffer_size = 0;

	/* Three frame buffers that change hands, so that nobody copies a frame while holding the mutex:
	 * `fill` belongs to raw_video(), `ready` is the mailbox (guarded by `mutex`), `write` belongs to the writer.
	 * raw_video() and the writer swap their buffer with `ready` under the mutex. */
	uint8_t *fill = nullptr;
	uint8_t *ready = nullptr;
	uint8_t *write = nullptr;
	bigtime_t fill_time = 0;
	bigtime_t ready_time = 0;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool ready_full = false; /* guarded by mutex */
	bool quit = false;       /* guarded by mutex */

	pthread_t writer;
	bool writer_running = false; /* touched only by start()/stop()/destroy() */

	/* Set by the writer when the feed broke; raw_video() then stops copying frames nobody will send. */
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

void *writer_thread(void *data)
{
	haiku_vcam *vcam = static_cast<haiku_vcam *>(data);

	/* WriteFrame() uses send() without MSG_NOSIGNAL: if media_addon_server goes away, send() fails with EPIPE
	 * and Haiku's socket layer raises SIGPIPE in the sending thread (src/add-ons/kernel/network/stack/
	 * net_socket.cpp). The frontend blocks SIGPIPE in main() before any thread is made (obs-main.cpp), and
	 * threads inherit that mask; blocking it here as well keeps libobs users without that frontend safe. The
	 * failure is then reported by WriteFrame()'s return value. */
	sigset_t pipe_set;
	sigemptyset(&pipe_set);
	sigaddset(&pipe_set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipe_set, nullptr);

	os_set_thread_name("haiku-virtualcam: writer");

	const uint32 bytes_per_row = vcam->width * 4;
	status_t status = B_OK;
	bool stopping = false;

	pthread_mutex_lock(&vcam->mutex);
	for (;;) {
		while (!vcam->ready_full && !vcam->quit) {
			pthread_cond_wait(&vcam->cond, &vcam->mutex);
		}
		if (vcam->quit) {
			stopping = true;
			break;
		}
		std::swap(vcam->write, vcam->ready);
		bigtime_t time = vcam->ready_time;
		vcam->ready_full = false;
		pthread_mutex_unlock(&vcam->mutex);

		status = vcam->feeder.WriteFrame(vcam->write, bytes_per_row, time);

		pthread_mutex_lock(&vcam->mutex);
		if (status != B_OK) {
			stopping = vcam->quit;
			break;
		}
	}
	pthread_mutex_unlock(&vcam->mutex);

	if (status != B_OK) {
		/* WriteFrame() has already disconnected the feeder (VirtualCamera.h). When stop() is not under way,
		 * end the output the way network outputs do on a lost connection: libobs ends the data capture and
		 * emits "stop" with the code, and the frontend shows the last error. Our stop() is not called then;
		 * the next start() or destroy() joins this thread. */
		os_atomic_set_bool(&vcam->failed, true);
		if (!stopping) {
			set_error(vcam, "Error.Lost", status);
			obs_output_signal_stop(vcam->output, OBS_OUTPUT_ERROR);
		}
	}
	return nullptr;
}

/* Joins the writer, if one runs or ended on its own after a failure. */
void stop_writer(haiku_vcam *vcam)
{
	if (!vcam->writer_running) {
		return;
	}
	pthread_mutex_lock(&vcam->mutex);
	vcam->quit = true;
	pthread_cond_signal(&vcam->cond);
	pthread_mutex_unlock(&vcam->mutex);
	/* Waits at most for one WriteFrame(): a frame copy and a send() that the node normally takes at once (its
	 * feed thread only receives these messages). It could wait longer only if media_addon_server stopped
	 * reading without closing the socket. */
	pthread_join(vcam->writer, nullptr);
	vcam->writer_running = false;
}

void free_buffers(haiku_vcam *vcam)
{
	bfree(vcam->fill);
	bfree(vcam->ready);
	bfree(vcam->write);
	vcam->fill = vcam->ready = vcam->write = nullptr;
	vcam->buffer_size = 0;
}

void *vcam_create(obs_data_t *, obs_output_t *output)
{
	haiku_vcam *vcam = new (std::nothrow) haiku_vcam;
	if (!vcam) {
		return nullptr;
	}
	vcam->output = output;
	pthread_mutex_init(&vcam->mutex, nullptr);
	pthread_cond_init(&vcam->cond, nullptr);
	return vcam;
}

void vcam_destroy(void *data)
{
	haiku_vcam *vcam = static_cast<haiku_vcam *>(data);
	/* libobs stops an active output and waits for its data capture to end (obs_output_destroy()) before it
	 * calls destroy(), so raw_video() no longer runs. */
	stop_writer(vcam);
	vcam->feeder.Disconnect();
	free_buffers(vcam);
	pthread_cond_destroy(&vcam->cond);
	pthread_mutex_destroy(&vcam->mutex);
	delete vcam;
}

bool vcam_start(void *data)
{
	haiku_vcam *vcam = static_cast<haiku_vcam *>(data);

	/* A writer that ended on its own after a failure (libobs ended the capture without calling stop()). */
	stop_writer(vcam);
	vcam->feeder.Disconnect();

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

	/* One request and its reply on the local socket; the node's feed thread answers at once (B_OK, or B_BUSY
	 * from its poll loop while another program feeds the camera). */
	status_t status = vcam->feeder.Connect(width, height, fps, kSlotCount);
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

	const size_t size = (size_t)width * 4 * height;
	if (size != vcam->buffer_size) {
		free_buffers(vcam);
		vcam->fill = (uint8_t *)bmalloc(size);
		vcam->ready = (uint8_t *)bmalloc(size);
		vcam->write = (uint8_t *)bmalloc(size);
		vcam->buffer_size = size;
	}
	vcam->width = width;
	vcam->height = height;
	vcam->ready_full = false;
	vcam->quit = false;
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

	if (pthread_create(&vcam->writer, nullptr, writer_thread, vcam) != 0) {
		vcam->feeder.Disconnect();
		set_error(vcam, "Error.ConnectFailed", B_NO_MEMORY);
		return false;
	}
	vcam->writer_running = true;

	if (!obs_output_begin_data_capture(vcam->output, 0)) {
		stop_writer(vcam);
		vcam->feeder.Disconnect();
		return false;
	}

	blog(LOG_INFO, "[haiku-virtualcam] feeding the Virtual Camera with %ux%u at %.2f fps", width, height, fps);
	return true;
}

void vcam_stop(void *data, uint64_t)
{
	haiku_vcam *vcam = static_cast<haiku_vcam *>(data);

	/* Ends the raw video callbacks (on libobs' end-data-capture thread, so raw_video() may run a little longer;
	 * it only fills the mailbox, and the buffers live until the next start() or destroy()). Called even when
	 * the writer has just signalled a failure: in that case the capture has already ended and this only emits
	 * "stop" and the stopping event again, which libobs' obs_output_actual_stop() needs to see after it reset
	 * the event. */
	obs_output_end_data_capture(vcam->output);
	stop_writer(vcam);
	vcam->feeder.Disconnect();
	blog(LOG_INFO, "[haiku-virtualcam] stopped");
}

void vcam_raw_video(void *data, struct video_data *frame)
{
	haiku_vcam *vcam = static_cast<haiku_vcam *>(data);
	if (os_atomic_load_bool(&vcam->failed) || !vcam->fill) {
		return;
	}

	const size_t row = (size_t)vcam->width * 4;
	const uint8_t *source = frame->data[0];
	if (frame->linesize[0] == row) {
		memcpy(vcam->fill, source, row * vcam->height);
	} else {
		for (uint32_t y = 0; y < vcam->height; y++) {
			memcpy(vcam->fill + y * row, source + (size_t)y * frame->linesize[0], row);
		}
	}

	/* OBS timestamps come from os_gettime_ns(), which is clock_gettime(CLOCK_MONOTONIC) (libobs/util/
	 * platform-nix.c), and Renga's libroot implements CLOCK_MONOTONIC as system_time() (src/system/libroot/
	 * posix/time/clock_support.cpp). So the frame time in system_time() microseconds, which
	 * virtual_camera_slot::time is, is the timestamp divided by 1000. */
	vcam->fill_time = (bigtime_t)(frame->timestamp / 1000);

	pthread_mutex_lock(&vcam->mutex);
	std::swap(vcam->fill, vcam->ready);
	vcam->ready_time = vcam->fill_time;
	vcam->ready_full = true; /* replaces a frame the writer has not taken yet */
	pthread_cond_signal(&vcam->cond);
	pthread_mutex_unlock(&vcam->mutex);
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
