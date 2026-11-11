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

#include <string>

#include <VideoAcceleration.h>

#include "va-bitstream.hpp"

#define HAIKU_VA_LOG(level, format, ...) blog(level, "[haiku-video-accel] " format, ##__VA_ARGS__)

namespace haiku_va {

constexpr int kMaxProfiles = 3;

/* One selectable profile of a codec: the value stored in the encoder's "profile" setting, its locale key, the
 * Renga profile it maps to and whether it needs 10-bit (P010) surfaces. */
struct ProfileInfo {
	const char *setting;
	const char *text;
	BPrivate::VideoAccelProfile profile;
	bool tenBit;
};

/* One OBS encoder type. */
struct CodecInfo {
	const char *id;      // OBS encoder id
	const char *codec;   // OBS codec name
	const char *display; // locale key of the codec's name
	Bitstream bitstream;
	ProfileInfo profiles[kMaxProfiles]; // in order of preference; setting == nullptr ends the list
};

/*
 * What module load found for a codec (haiku-video-accel.cpp): the back end to use, which of the codec's profiles
 * it encodes, and the encoder's display name. Lives as the encoder type's type_data for the whole run; the
 * accelerator belongs to Renga's VideoAcceleratorRoster, which never unloads its back ends.
 */
struct EncoderType {
	const CodecInfo *codec = nullptr;
	BPrivate::VideoAccelerator *accelerator = nullptr;
	bool software = false;
	bool profileSupported[kMaxProfiles] = {};
	std::string name;
	std::string backendDescription;
};

/* Registers the obs_encoder_info for a probed codec (va-encoder.cpp). */
void register_encoder(EncoderType *type);

/* The type registered under an OBS encoder id, for create(), which gets no type_data (haiku-video-accel.cpp). */
const EncoderType *find_encoder_type(const char *id);

/* Deletes an EncoderType (obs_encoder_info.free_type_data). */
void free_encoder_type(void *data);

/* The capability record of a profile with the encode entry point, or nullptr. */
const BPrivate::VideoAccelCapability *find_encode_capability(const BPrivate::VideoAccelerator *accelerator,
							     BPrivate::VideoAccelProfile profile);

} // namespace haiku_va
