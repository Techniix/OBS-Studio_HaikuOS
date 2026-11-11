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
 * The OBS encoder on top of BPrivate::VideoAccelEncoder (headers/private/media/VideoAcceleration.h).
 *
 * Per frame: AcquireSurface() hands out a surface of the back end's pool, mapped for writing (NV12, or P010 for
 * 10-bit, with the back end's own bytesPerRow per plane); the frame's planes are copied into it row by row and it
 * is returned with SendSurface(), which takes it back whatever the outcome. ReceivePacket() is then called until
 * it says B_WOULD_BLOCK; packets that came out are queued and given to OBS one per encode() call, which is all
 * obs_encoder_info.encode() allows. The flow follows the FFmpeg plug-in's use of the interface
 * (src/add-ons/media/plugins/ffmpeg/AVCodecEncoder.cpp, _EncodeAcceleratedVideo() and _WriteAcceleratedPackets())
 * and Renga's test (3rdparty/ce/tests/media/media_hwvideo_test.cpp, test_interface()).
 *
 * What the interface does not have, and how this file copes (each is a request in RENGA-REQUESTS.md):
 * - no decode timestamp and no B-frame setting: packets are taken to come out in presentation order (the software
 *   back end sets max_b_frames = 0), dts = pts, and a back end that reorders is reported in the log;
 * - no defined packet/extradata format: see va-bitstream.hpp; everything is normalised to Annex B;
 * - no CBR/VBR choice, maximum bitrate, preset or reconfiguration: the settings offer a target bitrate or a
 *   quality, a keyframe interval and the profile; changes apply at the next start;
 * - no way to give a surface back without encoding it: a surface that cannot be filled fails the encode, and
 *   deleting the encoder reclaims it.
 */

#include <obs-module.h>
#include <obs-avc.h>
#include <obs-av1.h>
#ifdef ENABLE_HEVC
#include <obs-hevc.h>
#endif
#include <media-io/video-io.h>
#include <util/platform.h>

#include <cstring>
#include <deque>
#include <new>
#include <vector>

#include "va-encoder.hpp"

using namespace BPrivate;

namespace haiku_va {

/* AcquireSurface() returning B_WOULD_BLOCK means every surface is in flight; packets are taken out between tries
 * and the wait is bounded (1 ms steps), so a stuck back end fails the encode instead of hanging OBS's encoder
 * thread. */
static const int kSurfaceAttempts = 200;
/* Packets larger than this are treated as an error rather than grown into. */
static const size_t kMaxPacketSize = 64 * 1024 * 1024;
/* Bound on the packets taken out when flushing at destroy time. */
static const int kMaxFlushPackets = 1024;
static const size_t kTimestampSlots = 64;

struct QueuedPacket {
	std::vector<uint8_t> data;
	bigtime_t timestamp;
	bool keyFrame;
};

struct TimestampSlot {
	bigtime_t microseconds;
	int64_t pts;
	bool used;
};

struct Encoder {
	obs_encoder_t *encoder = nullptr;
	const EncoderType *type = nullptr;
	VideoAccelEncoder *va = nullptr;
	const ProfileInfo *profile = nullptr;

	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fpsNum = 0;
	uint32_t fpsDen = 0; // including the encoder's frame rate divisor
	int64_t keyIntervalFrames = 0;
	int64_t framesSent = 0;

	std::vector<uint8_t> receiveBuffer;
	std::deque<QueuedPacket> queue;
	std::vector<uint8_t> output; // the packet handed to OBS; valid until the next encode() call
	std::vector<uint8_t> converted;

	std::vector<uint8_t> header;
	std::vector<uint8_t> sei;
	int nalLengthSize = 0;
	bool firstPacket = true;

	TimestampSlot timestamps[kTimestampSlots] = {};
	size_t nextTimestampSlot = 0;
	int64_t lastPts = 0;
	bool havePts = false;
	bool warnedReorder = false;
	bool warnedFormat = false;
};

#define ENC_LOG(level, enc, format, ...) \
	HAIKU_VA_LOG(level, "[%s] " format, obs_encoder_get_name((enc)->encoder), ##__VA_ARGS__)

static const ProfileInfo *first_supported_profile(const EncoderType *type)
{
	for (int i = 0; i < kMaxProfiles && type->codec->profiles[i].setting != nullptr; i++) {
		if (type->profileSupported[i]) {
			return &type->codec->profiles[i];
		}
	}
	return nullptr;
}

static const ProfileInfo *profile_from_settings(const EncoderType *type, obs_data_t *settings)
{
	const char *wanted = obs_data_get_string(settings, "profile");
	for (int i = 0; i < kMaxProfiles && type->codec->profiles[i].setting != nullptr; i++) {
		if (type->profileSupported[i] && strcmp(type->codec->profiles[i].setting, wanted) == 0) {
			return &type->codec->profiles[i];
		}
	}
	return first_supported_profile(type);
}

/* OBS pts are in units of 1/fps_num seconds (obs-encoder.c advances cur_pts by fps_den * divisor per frame, and
 * the outputs read pts / timebase_den, obs-ffmpeg-mux.c); the interface's timestamps are bigtime_t microseconds. */
static bigtime_t pts_to_microseconds(const Encoder *enc, int64_t pts)
{
	return (bigtime_t)(pts * 1000000 / (int64_t)enc->fpsNum);
}

static void remember_timestamp(Encoder *enc, bigtime_t microseconds, int64_t pts)
{
	TimestampSlot &slot = enc->timestamps[enc->nextTimestampSlot];
	slot.microseconds = microseconds;
	slot.pts = pts;
	slot.used = true;
	enc->nextTimestampSlot = (enc->nextTimestampSlot + 1) % kTimestampSlots;
}

/* The OBS pts a packet's timestamp came from: the exact one if it is still remembered (it is unless the back end
 * holds more than kTimestampSlots frames), otherwise the nearest frame by arithmetic. */
static int64_t microseconds_to_pts(const Encoder *enc, bigtime_t microseconds)
{
	for (size_t i = 0; i < kTimestampSlots; i++) {
		if (enc->timestamps[i].used && enc->timestamps[i].microseconds == microseconds) {
			return enc->timestamps[i].pts;
		}
	}
	int64_t frameUnits = (int64_t)enc->fpsDen;
	int64_t pts = (microseconds * (int64_t)enc->fpsNum + 500000) / 1000000;
	return (pts + frameUnits / 2) / frameUnits * frameUnits;
}

/* ---------------------------------------------------------------------- */
/* packets */

/* Takes out every packet the back end has ready. false on an error from the back end. */
static bool drain_packets(Encoder *enc)
{
	for (;;) {
		size_t size = enc->receiveBuffer.size();
		bigtime_t timestamp = 0;
		bool keyFrame = false;
		status_t status = enc->va->ReceivePacket(enc->receiveBuffer.data(), &size, &timestamp, &keyFrame);
		if (status == B_NO_MEMORY) {
			/* The packet stays queued in the back end and *_size is what it needs (VideoAcceleration.h,
			 * ReceivePacket(); SoftwareAccelerator.cpp keeps fPacket until it fits). */
			if (size <= enc->receiveBuffer.size() || size > kMaxPacketSize) {
				ENC_LOG(LOG_ERROR, enc, "back end wants a %zu byte packet buffer", size);
				return false;
			}
			enc->receiveBuffer.resize(size);
			continue;
		}
		if (status == B_WOULD_BLOCK || status == B_LAST_BUFFER_ERROR) {
			return true;
		}
		if (status != B_OK) {
			ENC_LOG(LOG_ERROR, enc, "ReceivePacket() failed: %s", strerror(status));
			return false;
		}
		if (size == 0) {
			continue;
		}
		QueuedPacket packet;
		packet.data.assign(enc->receiveBuffer.begin(), enc->receiveBuffer.begin() + size);
		packet.timestamp = timestamp;
		packet.keyFrame = keyFrame;
		enc->queue.push_back(std::move(packet));
	}
}

static void take_extradata(Encoder *enc)
{
	const void *data = nullptr;
	size_t size = 0;
	if (enc->va->GetExtradata(&data, &size) != B_OK || data == nullptr || size == 0) {
		return;
	}
	std::vector<uint8_t> header;
	int nalLengthSize = 0;
	if (config_to_obs_header(enc->type->codec->bitstream, (const uint8_t *)data, size, header, &nalLengthSize)) {
		enc->header.swap(header);
		enc->nalLengthSize = nalLengthSize;
		ENC_LOG(LOG_INFO, enc, "codec configuration from the back end: %zu bytes%s", size,
			nalLengthSize > 0 ? " (ISO BMFF record, packets are length-prefixed)" : "");
	} else if (enc->type->codec->bitstream != Bitstream::VP9) {
		ENC_LOG(LOG_WARNING, enc,
			"codec configuration from the back end (%zu bytes) is in no known format; taking the headers "
			"from the first packet instead",
			size);
	}
}

/* Normalises one packet from the back end into enc->output in OBS's format and fills in the OBS packet. */
static void output_packet(Encoder *enc, QueuedPacket &queued, struct encoder_packet *packet)
{
	Bitstream bitstream = enc->type->codec->bitstream;
	const uint8_t *data = queued.data.data();
	size_t size = queued.data.size();

	/* "after the first packet or at once" (VideoAcceleration.h, GetExtradata()) */
	if (enc->firstPacket && enc->header.empty()) {
		take_extradata(enc);
	}

	if ((bitstream == Bitstream::H264 || bitstream == Bitstream::HEVC) && !is_annexb(data, size)) {
		int lengthSize = enc->nalLengthSize > 0 ? enc->nalLengthSize : 4;
		if (length_prefixed_to_annexb(data, size, lengthSize, enc->converted)) {
			if (enc->nalLengthSize == 0 && !enc->warnedFormat) {
				ENC_LOG(LOG_WARNING, enc,
					"packets are length-prefixed but the back end gave no avcC/hvcC; assuming 4-byte "
					"lengths");
				enc->warnedFormat = true;
			}
			data = enc->converted.data();
			size = enc->converted.size();
		} else if (!enc->warnedFormat) {
			ENC_LOG(LOG_WARNING, enc, "packet is neither Annex B nor length-prefixed; passed on as it is");
			enc->warnedFormat = true;
		}
	}

	if (enc->firstPacket && enc->header.empty() && bitstream != Bitstream::VP9) {
		/* No usable extradata: the parameter sets are in the bitstream (as with FFmpeg's VA-API encoders without
		 * a global header). Take them out of the first packet, as obs-ffmpeg-vaapi.c does. */
		uint8_t *newPacket = nullptr;
		size_t newSize = 0;
		uint8_t *header = nullptr;
		size_t headerSize = 0;
		uint8_t *sei = nullptr;
		size_t seiSize = 0;
		switch (bitstream) {
		case Bitstream::H264:
			obs_extract_avc_headers(data, size, &newPacket, &newSize, &header, &headerSize, &sei, &seiSize);
			break;
		case Bitstream::HEVC:
#ifdef ENABLE_HEVC
			obs_extract_hevc_headers(data, size, &newPacket, &newSize, &header, &headerSize, &sei,
						 &seiSize);
#endif
			break;
		case Bitstream::AV1:
			obs_extract_av1_headers(data, size, &newPacket, &newSize, &header, &headerSize);
			break;
		case Bitstream::VP9:
			break;
		}
		if (header != nullptr && headerSize > 0) {
			enc->header.assign(header, header + headerSize);
		}
		if (sei != nullptr && seiSize > 0) {
			enc->sei.assign(sei, sei + seiSize);
		}
		if (newPacket != nullptr) {
			enc->output.assign(newPacket, newPacket + newSize);
		} else {
			enc->output.assign(data, data + size);
		}
		bfree(newPacket);
		bfree(header);
		bfree(sei);
		if (enc->header.empty()) {
			ENC_LOG(LOG_WARNING, enc, "the first packet carries no codec headers");
		}
	} else {
		enc->output.assign(data, data + size);
	}
	enc->firstPacket = false;

	int64_t pts = microseconds_to_pts(enc, queued.timestamp);
	if (enc->havePts && pts <= enc->lastPts && !enc->warnedReorder) {
		/* The interface has no decode timestamp, so B-frames cannot be described to OBS (request in
		 * RENGA-REQUESTS.md). The file will have non-monotonic timestamps. */
		ENC_LOG(LOG_WARNING, enc,
			"the back end returned packets out of presentation order (pts %lld after %lld); the interface "
			"has no decode timestamps, so the output will be wrong",
			(long long)pts, (long long)enc->lastPts);
		enc->warnedReorder = true;
	}
	enc->lastPts = pts;
	enc->havePts = true;

	packet->data = enc->output.data();
	packet->size = enc->output.size();
	packet->pts = pts;
	packet->dts = pts;
	packet->type = OBS_ENCODER_VIDEO;
	switch (bitstream) {
	case Bitstream::H264:
		packet->keyframe = obs_avc_keyframe(packet->data, packet->size);
		break;
	case Bitstream::HEVC:
#ifdef ENABLE_HEVC
		packet->keyframe = obs_hevc_keyframe(packet->data, packet->size);
#else
		packet->keyframe = queued.keyFrame;
#endif
		break;
	case Bitstream::AV1:
		packet->keyframe = obs_av1_keyframe(packet->data, packet->size);
		break;
	case Bitstream::VP9:
		/* No OBS parser for VP9: the back end's flag. */
		packet->keyframe = queued.keyFrame;
		break;
	}
}

/* ---------------------------------------------------------------------- */
/* frames */

static bool acquire_surface(Encoder *enc, VideoAccelSurface *surface)
{
	for (int attempt = 0; attempt < kSurfaceAttempts; attempt++) {
		status_t status = enc->va->AcquireSurface(surface);
		if (status == B_OK) {
			return true;
		}
		if (status != B_WOULD_BLOCK) {
			ENC_LOG(LOG_ERROR, enc, "AcquireSurface() failed: %s", strerror(status));
			return false;
		}
		/* every surface is in flight: take packets out first (AVCodecEncoder.cpp does the same) */
		if (!drain_packets(enc)) {
			return false;
		}
		if (attempt > 0) {
			os_sleep_ms(1);
		}
	}
	ENC_LOG(LOG_ERROR, enc, "no free surface from the back end after %d tries", kSurfaceAttempts);
	return false;
}

/* Copies an NV12 or P010 frame from OBS into the surface, row by row because the strides differ. The interface's
 * P010 is "10-bit 4:2:0 in 16-bit words, MSB-aligned" (VideoAcceleration.h, VideoAccelFormat), which is OBS's
 * VIDEO_FORMAT_P010 layout, so it is a plain copy too. */
static bool fill_surface(Encoder *enc, const VideoAccelSurface &surface, const struct encoder_frame *frame)
{
	VideoAccelFormat expected = enc->profile->tenBit ? B_VIDEO_ACCEL_P010 : B_VIDEO_ACCEL_NV12;
	size_t bytesPerSample = enc->profile->tenBit ? 2 : 1;
	size_t lumaBytes = (size_t)enc->width * bytesPerSample;
	size_t chromaBytes = (size_t)((enc->width + 1) / 2) * 2 * bytesPerSample;
	uint32_t chromaRows = (enc->height + 1) / 2;

	if (surface.format != expected || surface.width < (int32)enc->width || surface.height < (int32)enc->height ||
	    surface.planes[0] == nullptr || surface.planes[1] == nullptr || surface.bytesPerRow[0] < (int32)lumaBytes ||
	    surface.bytesPerRow[1] < (int32)chromaBytes) {
		ENC_LOG(LOG_ERROR, enc, "back end surface does not fit: %dx%d format %d, rows %d/%d bytes",
			(int)surface.width, (int)surface.height, (int)surface.format, (int)surface.bytesPerRow[0],
			(int)surface.bytesPerRow[1]);
		return false;
	}
	if (frame->data[0] == nullptr || frame->data[1] == nullptr) {
		return false;
	}

	for (uint32_t row = 0; row < enc->height; row++) {
		memcpy(surface.planes[0] + (size_t)row * surface.bytesPerRow[0],
		       frame->data[0] + (size_t)row * frame->linesize[0], lumaBytes);
	}
	for (uint32_t row = 0; row < chromaRows; row++) {
		memcpy(surface.planes[1] + (size_t)row * surface.bytesPerRow[1],
		       frame->data[1] + (size_t)row * frame->linesize[1], chromaBytes);
	}
	return true;
}

static bool encode_frame(Encoder *enc, struct encoder_frame *frame, struct encoder_packet *packet,
			 bool *received_packet)
{
	VideoAccelSurface surface;
	if (!acquire_surface(enc, &surface)) {
		return false;
	}
	if (!fill_surface(enc, surface, frame)) {
		return false;
	}

	surface.timestamp = pts_to_microseconds(enc, frame->pts);
	remember_timestamp(enc, surface.timestamp, frame->pts);

	/* Key frames are also forced at the interval, so OBS's "keyint_sec" (which services cap) holds even if a back
	 * end treats VideoAccelEncoderSetup::keyFrameInterval as a hint. */
	bool forceKeyFrame = enc->framesSent == 0 ||
			     (enc->keyIntervalFrames > 0 && enc->framesSent % enc->keyIntervalFrames == 0);

	status_t status = enc->va->SendSurface(&surface, forceKeyFrame);
	/* B_WOULD_BLOCK is not documented for SendSurface(); the software back end returns it when FFmpeg wants
	 * packets taken out first (it has already taken the surface back then). Packets are drained after every
	 * frame, so it should not happen; if it does the frame is dropped rather than the surface sent twice. */
	if (status == B_WOULD_BLOCK) {
		ENC_LOG(LOG_WARNING, enc, "back end refused a frame (would block); frame dropped");
		status = B_OK;
	} else {
		enc->framesSent++;
	}
	if (status != B_OK) {
		ENC_LOG(LOG_ERROR, enc, "SendSurface() failed: %s", strerror(status));
		return false;
	}

	if (!drain_packets(enc)) {
		return false;
	}

	*received_packet = false;
	if (!enc->queue.empty()) {
		output_packet(enc, enc->queue.front(), packet);
		enc->queue.pop_front();
		*received_packet = true;
	}
	return true;
}

/* ---------------------------------------------------------------------- */
/* obs_encoder_info callbacks */

static const char *va_get_name(void *type_data)
{
	return static_cast<EncoderType *>(type_data)->name.c_str();
}

static void va_destroy(void *data)
{
	Encoder *enc = static_cast<Encoder *>(data);
	if (enc == nullptr) {
		return;
	}
	if (enc->va != nullptr) {
		/* Flush ("NULL flushes", VideoAcceleration.h) and take out what is left, so a back end that keeps
		 * device state per frame in flight is idle before it is deleted. OBS has no use for these packets
		 * (encoders are destroyed after their outputs stopped). */
		if (enc->va->SendSurface(nullptr, false) == B_OK) {
			for (int i = 0; i < kMaxFlushPackets; i++) {
				size_t size = enc->receiveBuffer.size();
				bigtime_t timestamp;
				bool keyFrame;
				status_t status =
					enc->va->ReceivePacket(enc->receiveBuffer.data(), &size, &timestamp, &keyFrame);
				if (status == B_NO_MEMORY && size > enc->receiveBuffer.size() &&
				    size <= kMaxPacketSize) {
					try {
						enc->receiveBuffer.resize(size);
					} catch (...) {
						break;
					}
					continue;
				}
				if (status != B_OK) {
					break;
				}
			}
		}
		/* The client deletes the encoder (AVCodecEncoder.cpp ~AVCodecEncoder(): delete fAccelEncoder); that
		 * frees its surfaces. */
		delete enc->va;
		enc->va = nullptr;
	}
	delete enc;
}

static void *va_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	const EncoderType *type = find_encoder_type(obs_encoder_get_id(encoder));
	if (type == nullptr) {
		return nullptr;
	}

	Encoder *enc = new (std::nothrow) Encoder;
	if (enc == nullptr) {
		return nullptr;
	}
	enc->encoder = encoder;
	enc->type = type;

	try {
		enc->profile = profile_from_settings(type, settings);
		video_t *video = obs_encoder_video(encoder);
		const struct video_output_info *voi = video != nullptr ? video_output_get_info(video) : nullptr;
		if (enc->profile == nullptr || voi == nullptr || voi->fps_num == 0 || voi->fps_den == 0) {
			ENC_LOG(LOG_ERROR, enc, "no profile or video output");
			va_destroy(enc);
			return nullptr;
		}
		uint32_t divisor = obs_encoder_get_frame_rate_divisor(encoder);
		if (divisor == 0) {
			divisor = 1;
		}
		enc->width = obs_encoder_get_width(encoder);
		enc->height = obs_encoder_get_height(encoder);
		enc->fpsNum = voi->fps_num;
		enc->fpsDen = voi->fps_den * divisor;
		float frameRate = (float)enc->fpsNum / (float)enc->fpsDen;

		VideoAccelerator *accelerator = type->accelerator;
		if (!accelerator->Supports(enc->profile->profile, B_VIDEO_ACCEL_ENCODE, (int32)enc->width,
					   (int32)enc->height)) {
			const VideoAccelCapability *capability =
				find_encode_capability(accelerator, enc->profile->profile);
			ENC_LOG(LOG_ERROR, enc, "%ux%u is larger than the %s back end encodes %s (%dx%d)", enc->width,
				enc->height, accelerator->Name(),
				VideoAcceleratorRoster::ProfileName(enc->profile->profile),
				capability != nullptr ? (int)capability->maxWidth : 0,
				capability != nullptr ? (int)capability->maxHeight : 0);
			va_destroy(enc);
			return nullptr;
		}

		int64_t keyintSec = obs_data_get_int(settings, "keyint_sec");
		enc->keyIntervalFrames = keyintSec > 0 ? (int64_t)(keyintSec * frameRate + 0.5f) : 0;
		bool byQuality = strcmp(obs_data_get_string(settings, "rate_control"), "quality") == 0;
		int64_t bitrate = obs_data_get_int(settings, "bitrate");
		int64_t quality = obs_data_get_int(settings, "quality");

		VideoAccelEncoderSetup setup = {};
		setup.profile = enc->profile->profile;
		setup.width = (int32)enc->width;
		setup.height = (int32)enc->height;
		setup.frameRate = frameRate;
		/* bitRate 0 means "by quality" (VideoAcceleration.h, VideoAccelEncoderSetup) */
		setup.bitRate = byQuality ? 0 : (int32)(bitrate * 1000);
		setup.quality = (float)(quality < 1 ? 1 : (quality > 100 ? 100 : quality)) / 100.0f;
		setup.keyFrameInterval = (int32)enc->keyIntervalFrames;
		setup.format = enc->profile->tenBit ? B_VIDEO_ACCEL_P010 : B_VIDEO_ACCEL_NV12;

		status_t status = accelerator->CreateEncoder(setup, &enc->va);
		if (status != B_OK || enc->va == nullptr) {
			/* B_BUSY (a device with no free encoder session), B_NOT_SUPPORTED, ...: OBS reports that the
			 * encoder failed to start, and the user picks another one. */
			enc->va = nullptr;
			ENC_LOG(LOG_ERROR, enc, "the %s back end cannot encode %s at %ux%u: %s", accelerator->Name(),
				VideoAcceleratorRoster::ProfileName(enc->profile->profile), enc->width, enc->height,
				strerror(status));
			va_destroy(enc);
			return nullptr;
		}

		size_t frameSize = (size_t)enc->width * enc->height * 3 / 2 * (enc->profile->tenBit ? 2 : 1);
		enc->receiveBuffer.resize(frameSize + 65536);
		take_extradata(enc);

		ENC_LOG(LOG_INFO, enc,
			"%s %s on %s (%s): %ux%u at %.3f fps, %s %lld%s, keyframe every %lld frames%s, %s surfaces",
			type->codec->codec, enc->profile->setting, accelerator->Name(), accelerator->DeviceName(),
			enc->width, enc->height, frameRate, byQuality ? "quality" : "bitrate",
			(long long)(byQuality ? quality : bitrate), byQuality ? "/100" : " kbps",
			(long long)enc->keyIntervalFrames, enc->keyIntervalFrames == 0 ? " (back end's choice)" : "",
			enc->profile->tenBit ? "P010" : "NV12");
	} catch (...) {
		ENC_LOG(LOG_ERROR, enc, "out of memory");
		va_destroy(enc);
		return nullptr;
	}
	return enc;
}

static bool va_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received_packet)
{
	Encoder *enc = static_cast<Encoder *>(data);
	*received_packet = false;
	try {
		return encode_frame(enc, frame, packet, received_packet);
	} catch (...) {
		ENC_LOG(LOG_ERROR, enc, "out of memory");
		return false;
	}
}

static bool va_update(void *data, obs_data_t *settings)
{
	Encoder *enc = static_cast<Encoder *>(data);
	(void)settings;
	/* VideoAccelEncoder has no call to change the bitrate or anything else of a running encoder. */
	ENC_LOG(LOG_INFO, enc, "settings changes take effect when the encoder is started again");
	return false;
}

static bool va_get_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	Encoder *enc = static_cast<Encoder *>(data);
	if (enc->header.empty()) {
		return false;
	}
	*extra_data = enc->header.data();
	*size = enc->header.size();
	return true;
}

static bool va_get_sei_data(void *data, uint8_t **sei_data, size_t *size)
{
	Encoder *enc = static_cast<Encoder *>(data);
	if (enc->sei.empty()) {
		return false;
	}
	*sei_data = enc->sei.data();
	*size = enc->sei.size();
	return true;
}

static void va_get_video_info(void *data, struct video_scale_info *info)
{
	Encoder *enc = static_cast<Encoder *>(data);
	/* The surfaces take NV12 or P010 only (VideoAccelFormat); libobs converts to it. */
	info->format = enc->profile->tenBit ? VIDEO_FORMAT_P010 : VIDEO_FORMAT_NV12;
}

static void va_get_defaults2(obs_data_t *settings, void *type_data)
{
	const EncoderType *type = static_cast<EncoderType *>(type_data);
	const ProfileInfo *profile = first_supported_profile(type);
	obs_data_set_default_string(settings, "rate_control", "bitrate");
	obs_data_set_default_int(settings, "bitrate", 6000);
	obs_data_set_default_int(settings, "quality", 70);
	obs_data_set_default_int(settings, "keyint_sec", 2);
	obs_data_set_default_string(settings, "profile", profile != nullptr ? profile->setting : "");
}

static bool rate_control_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool byQuality = strcmp(obs_data_get_string(settings, "rate_control"), "quality") == 0;
	obs_property_set_visible(obs_properties_get(props, "bitrate"), !byQuality);
	obs_property_set_visible(obs_properties_get(props, "quality"), byQuality);
	return true;
}

static obs_properties_t *va_get_properties2(void *, void *type_data)
{
	const EncoderType *type = static_cast<EncoderType *>(type_data);
	obs_properties_t *props = obs_properties_create();

	std::string backend = obs_module_text("Backend");
	size_t pos = backend.find("%1");
	if (pos != std::string::npos) {
		backend.replace(pos, 2, type->backendDescription);
	}
	obs_properties_add_text(props, "backend_info", backend.c_str(), OBS_TEXT_INFO);
	if (type->software) {
		obs_property_t *warning = obs_properties_add_text(props, "software_warning",
								  obs_module_text("SoftwareWarning"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(warning, OBS_TEXT_INFO_WARNING);
	}

	obs_property_t *p = obs_properties_add_list(props, "rate_control", obs_module_text("RateControl"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("RateControl.Bitrate"), "bitrate");
	obs_property_list_add_string(p, obs_module_text("RateControl.Quality"), "quality");
	obs_property_set_long_description(p, obs_module_text("RateControl.Note"));
	obs_property_set_modified_callback(p, rate_control_modified);

	p = obs_properties_add_int(props, "bitrate", obs_module_text("Bitrate"), 50, 300000, 50);
	obs_property_int_set_suffix(p, " Kbps");
	obs_properties_add_int_slider(props, "quality", obs_module_text("Quality"), 1, 100, 1);

	p = obs_properties_add_int(props, "keyint_sec", obs_module_text("KeyframeInterval"), 0, 20, 1);
	obs_property_int_set_suffix(p, " s");

	p = obs_properties_add_list(props, "profile", obs_module_text("Profile"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	for (int i = 0; i < kMaxProfiles && type->codec->profiles[i].setting != nullptr; i++) {
		if (type->profileSupported[i]) {
			obs_property_list_add_string(p, obs_module_text(type->codec->profiles[i].text),
						     type->codec->profiles[i].setting);
		}
	}
	return props;
}

void register_encoder(EncoderType *type)
{
	struct obs_encoder_info info = {};
	info.id = type->codec->id;
	info.type = OBS_ENCODER_VIDEO;
	info.codec = type->codec->codec;
	info.get_name = va_get_name;
	info.create = va_create;
	info.destroy = va_destroy;
	info.encode = va_encode;
	info.update = va_update;
	info.get_extra_data = va_get_extra_data;
	info.get_sei_data = va_get_sei_data;
	info.get_video_info = va_get_video_info;
	info.get_defaults2 = va_get_defaults2;
	info.get_properties2 = va_get_properties2;
	info.type_data = type;
	info.free_type_data = free_encoder_type;
	obs_register_encoder(&info);
}

} // namespace haiku_va
