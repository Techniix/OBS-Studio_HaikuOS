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

#include "tap-node.h"

#include <Buffer.h>
#include <ByteOrder.h>
#include <TimeSource.h>

#include <string.h>

enum {
	TAP_INPUT_ID = 0,
	TAP_OUTPUT_ID = 1,
};

TapNode::TapNode(obs_tap_shm *shm_, const media_format &preferred_)
	: BMediaNode("OBS Desktop Audio Tap"),
	  BBufferConsumer(B_MEDIA_RAW_AUDIO),
	  BBufferProducer(B_MEDIA_RAW_AUDIO),
	  BMediaEventLooper(),
	  shm(shm_),
	  preferred(preferred_)
{
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = media_raw_audio_format::wildcard;
}

TapNode::~TapNode()
{
	Quit();
}

BMediaAddOn *TapNode::AddOn(int32 *id) const
{
	if (id)
		*id = 0;
	return nullptr; /* application-owned node */
}

status_t TapNode::HandleMessage(int32 message, const void *data, size_t size)
{
	if (BBufferConsumer::HandleMessage(message, data, size) == B_OK ||
	    BBufferProducer::HandleMessage(message, data, size) == B_OK ||
	    BMediaNode::HandleMessage(message, data, size) == B_OK)
		return B_OK;

	HandleBadMessage(message, data, size);
	return B_ERROR;
}

void TapNode::NodeRegistered()
{
	input.destination.port = ControlPort();
	input.destination.id = TAP_INPUT_ID;
	input.node = Node();
	input.source = media_source::null;
	input.format = format;
	strlcpy(input.name, "Tap input", B_MEDIA_NAME_LENGTH);

	output.source.port = ControlPort();
	output.source.id = TAP_OUTPUT_ID;
	output.node = Node();
	output.destination = media_destination::null;
	output.format = format;
	strlcpy(output.name, "Tap output", B_MEDIA_NAME_LENGTH);

	SetPriority(B_REAL_TIME_PRIORITY);
	Run();
}

/* Fills the wildcards of `proposed` from the current (or preferred) format and reports whether the
 * non-wildcard fields conflict with it. */
status_t TapNode::NegotiateFormat(media_format &proposed) const
{
	if (proposed.type != B_MEDIA_RAW_AUDIO) {
		proposed = preferred;
		return B_MEDIA_BAD_FORMAT;
	}

	const media_format &ref = (format.u.raw_audio.format != media_raw_audio_format::wildcard.format)
					  ? format
					  : preferred;
	const media_raw_audio_format &w = media_raw_audio_format::wildcard;
	const media_raw_audio_format &r = ref.u.raw_audio;
	media_raw_audio_format &f = proposed.u.raw_audio;
	status_t result = B_OK;

#define TAP_FIELD(field)                                \
	if (r.field != w.field && f.field != r.field) { \
		if (f.field != w.field)                 \
			result = B_MEDIA_BAD_FORMAT;    \
		f.field = r.field;                      \
	}
	TAP_FIELD(frame_rate)
	TAP_FIELD(channel_count)
	TAP_FIELD(format)
	TAP_FIELD(byte_order)
	TAP_FIELD(buffer_size)
#undef TAP_FIELD

	/* still-open wildcards: pick sane defaults */
	if (f.frame_rate == w.frame_rate)
		f.frame_rate = 48000.0f;
	if (f.channel_count == w.channel_count)
		f.channel_count = 2;
	if (f.format == w.format)
		f.format = media_raw_audio_format::B_AUDIO_FLOAT;
	if (f.byte_order == w.byte_order)
		f.byte_order = B_HOST_IS_BENDIAN ? B_MEDIA_BIG_ENDIAN : B_MEDIA_LITTLE_ENDIAN;

	return result;
}

void TapNode::PublishFormat(const media_format &fmt)
{
	const media_raw_audio_format &raw = fmt.u.raw_audio;
	shm->frame_rate = raw.frame_rate;
	shm->channels = raw.channel_count;
	shm->format = raw.format;
	shm->byte_order = raw.byte_order;
	__sync_synchronize();
	atomic_add((int32 *)&shm->format_serial, 1);
}

void TapNode::CopyToRing(const void *data, size_t size)
{
	if (!size)
		return;
	if (size > shm->ring_size)
		size = shm->ring_size;

	const int64 pos = shm->write_pos;
	const size_t offset = (size_t)(pos % shm->ring_size);
	const size_t first = (size < shm->ring_size - offset) ? size : shm->ring_size - offset;

	memcpy(shm->ring + offset, data, first);
	if (first < size)
		memcpy(shm->ring, (const uint8 *)data + first, size - first);

	/* the data must be visible before readers see the new position */
	__sync_synchronize();
	shm->write_pos = pos + (int64)size;
}

/* -------------------------------------------------------------------------------------------------- */
/* BBufferConsumer                                                                                     */

status_t TapNode::AcceptFormat(const media_destination &dst, media_format *fmt)
{
	if (dst != input.destination)
		return B_MEDIA_BAD_DESTINATION;
	if (fmt->type != B_MEDIA_RAW_AUDIO)
		return B_MEDIA_BAD_FORMAT;
	NegotiateFormat(*fmt);
	return B_OK;
}

status_t TapNode::GetNextInput(int32 *cookie, media_input *out)
{
	if (*cookie)
		return B_BAD_INDEX;
	++*cookie;
	*out = input;
	return B_OK;
}

void TapNode::DisposeInputCookie(int32) {}

void TapNode::BufferReceived(BBuffer *buffer)
{
	if (buffer->Header()->destination != input.destination.id) {
		buffer->Recycle();
		return;
	}

	/* the copy is a plain memcpy into a 1 MiB ring; it never blocks the real-time path */
	CopyToRing(buffer->Data(), buffer->SizeUsed());

	if (output.destination == media_destination::null || !output_enabled) {
		buffer->Recycle();
		return;
	}

	if (SendBuffer(buffer, output.source, output.destination) != B_OK)
		buffer->Recycle();
}

void TapNode::ProducerDataStatus(const media_destination &, int32 status, bigtime_t when)
{
	if (output.destination != media_destination::null)
		SendDataStatus(status, output.destination, when);
}

status_t TapNode::GetLatencyFor(const media_destination &dst, bigtime_t *latency, media_node_id *timeSource)
{
	if (dst != input.destination)
		return B_MEDIA_BAD_DESTINATION;
	*latency = downstream_latency + SchedulingLatency();
	*timeSource = TimeSource()->ID();
	return B_OK;
}

status_t TapNode::Connected(const media_source &src, const media_destination &dst, const media_format &fmt,
			    media_input *out)
{
	if (dst != input.destination)
		return B_MEDIA_BAD_DESTINATION;
	if (input.source != media_source::null)
		return B_MEDIA_ALREADY_CONNECTED;

	input.source = src;
	input.format = fmt;
	format = fmt;
	PublishFormat(fmt);
	*out = input;
	return B_OK;
}

void TapNode::Disconnected(const media_source &src, const media_destination &dst)
{
	if (src != input.source || dst != input.destination)
		return;
	input.source = media_source::null;
	if (output.destination == media_destination::null)
		format.u.raw_audio = media_raw_audio_format::wildcard;
	input.format = format;
}

status_t TapNode::FormatChanged(const media_source &, const media_destination &, int32, const media_format &)
{
	/* the mixer renegotiates by reconnecting; in-place changes are refused like other filter nodes do */
	return B_MEDIA_BAD_FORMAT;
}

/* -------------------------------------------------------------------------------------------------- */
/* BBufferProducer                                                                                     */

status_t TapNode::FormatSuggestionRequested(media_type type, int32, media_format *fmt)
{
	if (type != B_MEDIA_RAW_AUDIO)
		return B_MEDIA_BAD_FORMAT;
	*fmt = (format.u.raw_audio.format != media_raw_audio_format::wildcard.format) ? format : preferred;
	return B_OK;
}

status_t TapNode::FormatProposal(const media_source &src, media_format *fmt)
{
	if (src != output.source)
		return B_MEDIA_BAD_SOURCE;
	if (fmt->type != B_MEDIA_RAW_AUDIO)
		return B_MEDIA_BAD_FORMAT;
	NegotiateFormat(*fmt);
	return B_OK;
}

status_t TapNode::FormatChangeRequested(const media_source &, const media_destination &, media_format *,
					int32 *)
{
	return B_MEDIA_BAD_FORMAT;
}

status_t TapNode::GetNextOutput(int32 *cookie, media_output *out)
{
	if (*cookie)
		return B_BAD_INDEX;
	++*cookie;
	*out = output;
	return B_OK;
}

status_t TapNode::DisposeOutputCookie(int32)
{
	return B_OK;
}

status_t TapNode::SetBufferGroup(const media_source &src, BBufferGroup *group)
{
	if (src != output.source)
		return B_MEDIA_BAD_SOURCE;
	if (input.source == media_source::null)
		return B_ERROR;

	/* let the mixer write straight into the sound card's buffers: zero-copy forwarding */
	int32 change_tag = 0;
	return SetOutputBuffersFor(input.source, input.destination, group, nullptr, &change_tag);
}

status_t TapNode::PrepareToConnect(const media_source &src, const media_destination &dst, media_format *fmt,
				   media_source *outSource, char *outName)
{
	if (src != output.source)
		return B_MEDIA_BAD_SOURCE;
	if (fmt->type != B_MEDIA_RAW_AUDIO)
		return B_MEDIA_BAD_FORMAT;
	if (output.destination != media_destination::null)
		return B_MEDIA_ALREADY_CONNECTED;

	status_t err = NegotiateFormat(*fmt);
	if (err != B_OK)
		return err;

	if (fmt->u.raw_audio.buffer_size == media_raw_audio_format::wildcard.buffer_size)
		fmt->u.raw_audio.buffer_size = input.source != media_source::null
						       ? input.format.u.raw_audio.buffer_size
						       : 1024 * (fmt->u.raw_audio.format & 0xf) * fmt->u.raw_audio.channel_count;

	output.destination = dst;
	output.format = *fmt;
	*outSource = output.source;
	strlcpy(outName, output.name, B_MEDIA_NAME_LENGTH);
	return B_OK;
}

void TapNode::Connect(status_t status, const media_source &, const media_destination &dst,
		      const media_format &fmt, char *name)
{
	if (status != B_OK) {
		output.destination = media_destination::null;
		return;
	}

	strlcpy(name, output.name, B_MEDIA_NAME_LENGTH);
	output.destination = dst;
	output.format = fmt;
	format = fmt;
	PublishFormat(fmt);

	media_node_id time_source;
	if (FindLatencyFor(output.destination, &downstream_latency, &time_source) != B_OK)
		downstream_latency = 0;
	SetEventLatency(downstream_latency);

	if (input.source != media_source::null)
		SendLatencyChange(input.source, input.destination, EventLatency() + SchedulingLatency());

	const media_raw_audio_format &raw = fmt.u.raw_audio;
	const size_t frame_size = (raw.format & 0xf) * raw.channel_count;
	if (raw.buffer_size > 0 && raw.frame_rate > 0 && frame_size > 0)
		SetBufferDuration((bigtime_t)((raw.buffer_size / frame_size) * 1000000.0 / raw.frame_rate));
}

void TapNode::Disconnect(const media_source &src, const media_destination &dst)
{
	if (src != output.source || dst != output.destination)
		return;
	output.destination = media_destination::null;
	if (input.source == media_source::null)
		format.u.raw_audio = media_raw_audio_format::wildcard;
	output.format = format;
}

void TapNode::LateNoticeReceived(const media_source &src, bigtime_t late, bigtime_t when)
{
	if (src != output.source || input.source == media_source::null)
		return;
	NotifyLateProducer(input.source, late, when);
}

void TapNode::EnableOutput(const media_source &src, bool enabled, int32 *)
{
	if (src == output.source)
		output_enabled = enabled;
}

status_t TapNode::GetLatency(bigtime_t *latency)
{
	*latency = EventLatency() + SchedulingLatency();
	return B_OK;
}

void TapNode::LatencyChanged(const media_source &src, const media_destination &dst, bigtime_t latency, uint32)
{
	if (src != output.source || dst != output.destination)
		return;
	downstream_latency = latency;
	SetEventLatency(downstream_latency);
	if (input.source != media_source::null)
		SendLatencyChange(input.source, input.destination, EventLatency() + SchedulingLatency());
}

/* -------------------------------------------------------------------------------------------------- */
/* BMediaEventLooper                                                                                   */

void TapNode::HandleEvent(const media_timed_event *, bigtime_t, bool) {}

bigtime_t TapNode::OfflineTime()
{
	return 0;
}
