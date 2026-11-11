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

/*
 * Bitstream format helpers for the hardware encoder.
 *
 * Renga's VideoAccelEncoder (headers/private/media/VideoAcceleration.h) does not say in which form a back end
 * returns H.264/HEVC data: GetExtradata() is documented as "codec configuration for the container (avcC, ...)",
 * and nothing is said about the packets. OBS, on the other hand, wants Annex B everywhere: its packets are
 * Annex B (obs_avc_keyframe() and the flv/mp4/mpegts outputs parse start codes) and get_extra_data() must be the
 * Annex B SPS/PPS(/VPS) that obs_extract_avc_headers() would produce, because the outputs convert it themselves
 * (obs_parse_avc_header() / obs_parse_hevc_header()).
 *
 * So the encoder accepts both forms and normalises to Annex B: an avcC/hvcC record is unpacked into start-code
 * prefixed parameter sets, and its NAL length size is used to rewrite length-prefixed packets. If Renga fixes the
 * format in the interface (requested in RENGA-REQUESTS.md), only the unused branch becomes dead.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace haiku_va {

enum class Bitstream { H264, HEVC, AV1, VP9 };

/* True if data starts with a 3- or 4-byte Annex B start code. */
bool is_annexb(const uint8_t *data, size_t size);

/*
 * Turns a back end's extradata into OBS's header format.
 * H.264/HEVC: Annex B is copied as it is; an avcC/hvcC record (first byte 1) is unpacked, and *nal_length_size is
 * set to its lengthSizeMinusOne + 1 (otherwise 0). AV1: an av1C record (first byte 0x81) yields its configOBUs,
 * anything else is taken as OBUs already. VP9: nothing (OBS needs no header).
 * Returns false if the data could not be understood; header is then left empty.
 */
bool config_to_obs_header(Bitstream kind, const uint8_t *data, size_t size, std::vector<uint8_t> &header,
			  int *nal_length_size);

/*
 * Rewrites a packet of length-prefixed NAL units (nal_length_size 1..4) as Annex B into out. Returns false (out
 * untouched) unless the length prefixes cover the packet exactly, so an Annex B packet is never mangled.
 */
bool length_prefixed_to_annexb(const uint8_t *data, size_t size, int nal_length_size, std::vector<uint8_t> &out);

} // namespace haiku_va
