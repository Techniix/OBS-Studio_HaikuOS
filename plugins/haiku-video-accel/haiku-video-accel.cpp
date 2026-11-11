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
 * Hardware video encoders on Renga's Media Kit video acceleration interface (roadmap F-31 phase 5,
 * headers/private/media/VideoAcceleration.h; request O-11 in docs/haiku/RENGA-REQUESTS.md).
 *
 * The interface is VA-API shaped and vendor neutral: back ends are add-ons in add-ons/media/video_accelerators,
 * loaded by BPrivate::VideoAcceleratorRoster (libmedia). Encoders are offered only when a runtime check passes:
 * at module load the roster is asked, for every profile of every codec, which back end encodes it
 * (VideoAcceleratorRoster::Find(), which applies the user's choice from ~/config/settings/system/video_acceleration
 * or RENGA_VIDEO_ACCELERATION), and an encoder type is registered only if that back end says IsHardware().
 *
 * Renga also ships a "software" reference back end (FFmpeg behind the same interface) to test the plumbing. Find()
 * never returns it unless it is chosen by name (VideoAcceleration.cpp, Find()), and this module additionally
 * requires OBS_HAIKU_VA_ALLOW_SOFTWARE=1 before offering it, so a stray system setting cannot put a slow
 * "hardware" encoder in front of users. Such encoders are named "... software test back end".
 */

#include <obs-module.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include "va-encoder.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("haiku-video-accel", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Hardware video encoders through Renga's Media Kit video acceleration interface";
}

using namespace BPrivate;

namespace haiku_va {

/*
 * The codecs OBS can carry and VideoAccelProfile can name for encoding. H.264, HEVC and AV1 are what OBS's
 * outputs and services expect. VP9 is included as well: VA-API-class hardware encodes it, OBS records it into
 * Matroska (ffmpeg_muxer accepts any codec FFmpeg knows; the frontend allows it for .mkv), and it is the only one
 * of these the "software" reference back end can encode (its FFmpeg has no H.264/HEVC/AV1 encoder,
 * SoftwareAccelerator.cpp kProfiles), so it is what exercises this module without a GPU back end.
 * MPEG-2/MPEG-4 Part 2/VP8 are left out: no OBS output wants them.
 */
static const CodecInfo kCodecs[] = {
	{"haiku_va_h264",
	 "h264",
	 "Codec.H264",
	 Bitstream::H264,
	 {{"high", "Profile.High", B_VIDEO_ACCEL_H264_HIGH, false},
	  {"main", "Profile.Main", B_VIDEO_ACCEL_H264_MAIN, false},
	  {"baseline", "Profile.ConstrainedBaseline", B_VIDEO_ACCEL_H264_CONSTRAINED_BASELINE, false}}},
	{"haiku_va_hevc",
	 "hevc",
	 "Codec.HEVC",
	 Bitstream::HEVC,
	 {{"main", "Profile.Main", B_VIDEO_ACCEL_HEVC_MAIN, false},
	  {"main10", "Profile.Main10", B_VIDEO_ACCEL_HEVC_MAIN10, true},
	  {nullptr, nullptr, B_VIDEO_ACCEL_PROFILE_NONE, false}}},
	{"haiku_va_av1",
	 "av1",
	 "Codec.AV1",
	 Bitstream::AV1,
	 {{"main", "Profile.Main", B_VIDEO_ACCEL_AV1_MAIN, false},
	  {"main10", "Profile.Main10Bit", B_VIDEO_ACCEL_AV1_MAIN, true},
	  {nullptr, nullptr, B_VIDEO_ACCEL_PROFILE_NONE, false}}},
	{"haiku_va_vp9",
	 "vp9",
	 "Codec.VP9",
	 Bitstream::VP9,
	 {{"profile0", "Profile.VP9Profile0", B_VIDEO_ACCEL_VP9_PROFILE0, false},
	  {"profile2", "Profile.VP9Profile2", B_VIDEO_ACCEL_VP9_PROFILE2, true},
	  {nullptr, nullptr, B_VIDEO_ACCEL_PROFILE_NONE, false}}},
};

static constexpr size_t kCodecCount = sizeof(kCodecs) / sizeof(kCodecs[0]);

static EncoderType *sTypes[kCodecCount];

const EncoderType *find_encoder_type(const char *id)
{
	for (size_t i = 0; i < kCodecCount; i++) {
		if (sTypes[i] != nullptr && strcmp(sTypes[i]->codec->id, id) == 0) {
			return sTypes[i];
		}
	}
	return nullptr;
}

/* free_type_data of the registered types: obs_shutdown() frees encoder types (FREE_REGISTERED_TYPES in obs.c) after
 * every encoder is gone and before modules are unloaded. */
void free_encoder_type(void *data)
{
	EncoderType *type = static_cast<EncoderType *>(data);
	for (size_t i = 0; i < kCodecCount; i++) {
		if (sTypes[i] == type) {
			sTypes[i] = nullptr;
		}
	}
	delete type;
}

const VideoAccelCapability *find_encode_capability(const VideoAccelerator *accelerator, VideoAccelProfile profile)
{
	for (int32 i = 0; i < accelerator->CountCapabilities(); i++) {
		const VideoAccelCapability *capability = accelerator->CapabilityAt(i);
		if (capability != nullptr && capability->profile == profile &&
		    (capability->entrypoints & B_VIDEO_ACCEL_ENCODE) != 0) {
			return capability;
		}
	}
	return nullptr;
}

static std::string replace_placeholder(std::string text, const char *placeholder, const std::string &value)
{
	size_t pos = text.find(placeholder);
	if (pos != std::string::npos) {
		text.replace(pos, strlen(placeholder), value);
	}
	return text;
}

static void probe_codec(size_t index, VideoAcceleratorRoster *roster, bool allowSoftware)
{
	const CodecInfo &codec = kCodecs[index];
	VideoAccelerator *chosen = nullptr;
	bool supported[kMaxProfiles] = {};
	bool any = false;

	for (int i = 0; i < kMaxProfiles && codec.profiles[i].setting != nullptr; i++) {
		const ProfileInfo &profile = codec.profiles[i];
		/* Find() applies the user's choice ("auto" = hardware back ends only, "off", or a name) and checks
		 * Supports(profile, B_VIDEO_ACCEL_ENCODE); the size is checked again when an encoder is created. */
		VideoAccelerator *accelerator = roster->Find(profile.profile, B_VIDEO_ACCEL_ENCODE);
		if (accelerator == nullptr) {
			continue;
		}
		if (!accelerator->IsHardware() && !allowSoftware) {
			HAIKU_VA_LOG(LOG_INFO,
				     "%s %s: back end \"%s\" is not hardware; set OBS_HAIKU_VA_ALLOW_SOFTWARE=1 to "
				     "offer it for testing",
				     codec.id, profile.setting, accelerator->Name());
			continue;
		}
		/* One back end per OBS encoder type: the one found for the most preferred profile. */
		if (chosen != nullptr && accelerator != chosen) {
			continue;
		}
		const VideoAccelCapability *capability = find_encode_capability(accelerator, profile.profile);
		uint32 format = profile.tenBit ? B_VIDEO_ACCEL_P010 : B_VIDEO_ACCEL_NV12;
		if (capability == nullptr || (capability->formats & format) == 0) {
			continue;
		}
		chosen = accelerator;
		supported[i] = true;
		any = true;
	}

	if (!any) {
		HAIKU_VA_LOG(LOG_INFO, "%s: no usable back end encodes %s; encoder not offered", codec.id, codec.codec);
		return;
	}

	EncoderType *type = new (std::nothrow) EncoderType;
	if (type == nullptr) {
		return;
	}
	type->codec = &codec;
	type->accelerator = chosen;
	type->software = !chosen->IsHardware();
	for (int i = 0; i < kMaxProfiles; i++) {
		type->profileSupported[i] = supported[i];
	}

	std::string backend = chosen->Name();
	type->name = replace_placeholder(obs_module_text(type->software ? "Name.Software" : "Name"), "%1",
					 obs_module_text(codec.display));
	type->name = replace_placeholder(type->name, "%2", backend);
	type->backendDescription = backend + " (" + chosen->DeviceName() + ")";

	HAIKU_VA_LOG(LOG_INFO, "offering \"%s\" (%s) on back end %s%s", type->name.c_str(), codec.id,
		     type->backendDescription.c_str(), type->software ? ", SOFTWARE TEST BACK END (not hardware)" : "");
	for (int i = 0; i < kMaxProfiles; i++) {
		if (supported[i]) {
			HAIKU_VA_LOG(LOG_INFO, "  profile %s (%s)", codec.profiles[i].setting,
				     VideoAcceleratorRoster::ProfileName(codec.profiles[i].profile));
		}
	}

	sTypes[index] = type;
	register_encoder(type);
}

} // namespace haiku_va

bool obs_module_load(void)
{
	using namespace haiku_va;

	VideoAcceleratorRoster *roster = VideoAcceleratorRoster::Default();
	if (roster == nullptr) {
		HAIKU_VA_LOG(LOG_WARNING, "no video acceleration roster; no hardware encoders");
		return true;
	}

	/* CountAccelerators() loads the back end add-ons. The roster's lazy loading has no lock
	 * (VideoAcceleration.cpp, _Load()), so it is done here, on the thread that loads modules, before any encoder
	 * thread can touch the roster. */
	int32 count = roster->CountAccelerators();
	const char *allow = getenv("OBS_HAIKU_VA_ALLOW_SOFTWARE");
	bool allowSoftware = allow != nullptr && strcmp(allow, "1") == 0;

	HAIKU_VA_LOG(LOG_INFO, "%d video acceleration back end(s); choice \"%s\"; software test back end %s",
		     (int)count, roster->Choice().String(),
		     allowSoftware ? "ALLOWED (OBS_HAIKU_VA_ALLOW_SOFTWARE=1)" : "not offered");
	for (int32 i = 0; i < count; i++) {
		VideoAccelerator *accelerator = roster->AcceleratorAt(i);
		if (accelerator != nullptr) {
			HAIKU_VA_LOG(LOG_INFO, "  back end \"%s\": %s, %s", accelerator->Name(),
				     accelerator->DeviceName(), accelerator->IsHardware() ? "hardware" : "software");
		}
	}

	for (size_t i = 0; i < kCodecCount; i++) {
		probe_codec(i, roster, allowSoftware);
	}
	return true;
}
