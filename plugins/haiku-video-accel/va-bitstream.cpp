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

#include "va-bitstream.hpp"

namespace haiku_va {

static const uint8_t kStartCode[4] = {0, 0, 0, 1};

bool is_annexb(const uint8_t *data, size_t size)
{
	if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
		return true;
	}
	return size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

static void append_nal(std::vector<uint8_t> &out, const uint8_t *nal, size_t size)
{
	out.insert(out.end(), kStartCode, kStartCode + 4);
	out.insert(out.end(), nal, nal + size);
}

/*
 * Reads count entries of (16-bit big-endian size, NAL unit) at *pos, appending them as Annex B. Used for the
 * parameter set lists of both avcC (ISO/IEC 14496-15 5.3.3.1) and hvcC (8.3.3.1).
 */
static bool read_nal_list(const uint8_t *data, size_t size, size_t *pos, unsigned count, std::vector<uint8_t> &out)
{
	for (unsigned i = 0; i < count; i++) {
		if (*pos + 2 > size) {
			return false;
		}
		size_t length = ((size_t)data[*pos] << 8) | data[*pos + 1];
		*pos += 2;
		if (*pos + length > size) {
			return false;
		}
		append_nal(out, data + *pos, length);
		*pos += length;
	}
	return true;
}

static bool avcc_to_annexb(const uint8_t *data, size_t size, std::vector<uint8_t> &out, int *nal_length_size)
{
	/* version(1) profile(1) compatibility(1) level(1) 6 bits reserved + lengthSizeMinusOne(2)
	 * 3 bits reserved + numOfSequenceParameterSets(5), SPS list, numOfPictureParameterSets(8), PPS list.
	 * A High profile record may continue with chroma/bit depth fields; OBS does not need them. */
	if (size < 7) {
		return false;
	}
	*nal_length_size = (data[4] & 3) + 1;
	size_t pos = 6;
	if (!read_nal_list(data, size, &pos, data[5] & 0x1f, out)) {
		return false;
	}
	if (pos >= size) {
		return false;
	}
	unsigned pps = data[pos++];
	return read_nal_list(data, size, &pos, pps, out);
}

static bool hvcc_to_annexb(const uint8_t *data, size_t size, std::vector<uint8_t> &out, int *nal_length_size)
{
	/* 22 bytes of fixed fields (lengthSizeMinusOne in the low 2 bits of byte 21), numOfArrays(8), then per
	 * array: completeness(1) reserved(1) NAL_unit_type(6), numNalus(16), the NAL list. VPS, SPS, PPS and any
	 * SEI arrays all go into the header; OBS's obs_parse_hevc_header() picks out what it needs. */
	if (size < 23) {
		return false;
	}
	*nal_length_size = (data[21] & 3) + 1;
	unsigned arrays = data[22];
	size_t pos = 23;
	for (unsigned i = 0; i < arrays; i++) {
		if (pos + 3 > size) {
			return false;
		}
		unsigned count = ((unsigned)data[pos + 1] << 8) | data[pos + 2];
		pos += 3;
		if (!read_nal_list(data, size, &pos, count, out)) {
			return false;
		}
	}
	return true;
}

bool config_to_obs_header(Bitstream kind, const uint8_t *data, size_t size, std::vector<uint8_t> &header,
			  int *nal_length_size)
{
	header.clear();
	*nal_length_size = 0;
	if (data == nullptr || size == 0) {
		return false;
	}

	switch (kind) {
	case Bitstream::H264:
	case Bitstream::HEVC: {
		if (is_annexb(data, size)) {
			header.assign(data, data + size);
			return true;
		}
		if (data[0] != 1) {
			return false;
		}
		std::vector<uint8_t> converted;
		bool ok = kind == Bitstream::H264 ? avcc_to_annexb(data, size, converted, nal_length_size)
						  : hvcc_to_annexb(data, size, converted, nal_length_size);
		if (!ok || converted.empty()) {
			*nal_length_size = 0;
			return false;
		}
		header.swap(converted);
		return true;
	}
	case Bitstream::AV1:
		/* av1C (AV1 ISOBMFF binding 2.3): marker(1) version(7) = 0x81, then 3 bytes of profile/level/flags,
		 * then configOBUs (normally the sequence header OBU), which is what OBS's AV1 header is. */
		if (data[0] == 0x81) {
			if (size <= 4) {
				return false;
			}
			header.assign(data + 4, data + size);
		} else {
			header.assign(data, data + size);
		}
		return true;
	case Bitstream::VP9:
		return false;
	}
	return false;
}

bool length_prefixed_to_annexb(const uint8_t *data, size_t size, int nal_length_size, std::vector<uint8_t> &out)
{
	if (nal_length_size < 1 || nal_length_size > 4) {
		return false;
	}

	/* Validate first, so a packet that is really Annex B (or damaged) is passed on untouched. */
	size_t pos = 0;
	size_t nals = 0;
	while (pos < size) {
		if (pos + (size_t)nal_length_size > size) {
			return false;
		}
		size_t length = 0;
		for (int i = 0; i < nal_length_size; i++) {
			length = (length << 8) | data[pos + i];
		}
		pos += nal_length_size;
		if (length == 0 || length > size - pos) {
			return false;
		}
		pos += length;
		nals++;
	}
	if (nals == 0) {
		return false;
	}

	std::vector<uint8_t> converted;
	converted.reserve(size + nals * 4);
	pos = 0;
	while (pos < size) {
		size_t length = 0;
		for (int i = 0; i < nal_length_size; i++) {
			length = (length << 8) | data[pos + i];
		}
		pos += nal_length_size;
		append_nal(converted, data + pos, length);
		pos += length;
	}
	out.swap(converted);
	return true;
}

} // namespace haiku_va
