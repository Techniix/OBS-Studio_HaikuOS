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
 * Pass-through Media Kit filter node. It is inserted between the system mixer's output and the sound card
 * input; every buffer it receives is copied into the shared-memory ring and then forwarded unchanged to the
 * sound card, so what the user hears is untouched. The node structure follows Haiku's MIT-licensed
 * Equalizer add-on (src/add-ons/media/media-add-ons/equalizer).
 */

#pragma once

#include <BufferConsumer.h>
#include <BufferProducer.h>
#include <MediaEventLooper.h>

#include "audio-tap-shm.h"

class TapNode : public BBufferConsumer, public BBufferProducer, public BMediaEventLooper {
public:
	/* `preferred` is the format the mixer and sound card were using before the tap was inserted; both
	 * sides of the tap are negotiated to it so neither end has to convert. */
	TapNode(obs_tap_shm *shm, const media_format &preferred);
	~TapNode() override;

	/* BMediaNode */
	BMediaAddOn *AddOn(int32 *id) const override;
	status_t HandleMessage(int32 message, const void *data, size_t size) override;

	/* false once the mixer has been disconnected from us (e.g. rerouted by Media preferences) */
	bool InputConnected() const { return input.source != media_source::null; }
	/* Stops the control thread now, so nothing in this team still reads the node's time source when its
	 * last reference goes away (Quit() is idempotent; the destructor calls it again) */
	void Shutdown() { Quit(); }

protected:
	void NodeRegistered() override;

	/* BBufferConsumer */
	status_t AcceptFormat(const media_destination &dst, media_format *format) override;
	status_t GetNextInput(int32 *cookie, media_input *input) override;
	void DisposeInputCookie(int32 cookie) override;
	void BufferReceived(BBuffer *buffer) override;
	void ProducerDataStatus(const media_destination &dst, int32 status, bigtime_t when) override;
	status_t GetLatencyFor(const media_destination &dst, bigtime_t *latency,
			       media_node_id *timeSource) override;
	status_t Connected(const media_source &src, const media_destination &dst, const media_format &format,
			   media_input *input) override;
	void Disconnected(const media_source &src, const media_destination &dst) override;
	status_t FormatChanged(const media_source &src, const media_destination &dst, int32 changeTag,
			       const media_format &format) override;

	/* BBufferProducer */
	status_t FormatSuggestionRequested(media_type type, int32 quality, media_format *format) override;
	status_t FormatProposal(const media_source &src, media_format *format) override;
	status_t FormatChangeRequested(const media_source &src, const media_destination &dst,
				       media_format *format, int32 *_deprecated_) override;
	status_t GetNextOutput(int32 *cookie, media_output *output) override;
	status_t DisposeOutputCookie(int32 cookie) override;
	status_t SetBufferGroup(const media_source &src, BBufferGroup *group) override;
	status_t PrepareToConnect(const media_source &src, const media_destination &dst, media_format *format,
				  media_source *outSource, char *outName) override;
	void Connect(status_t status, const media_source &src, const media_destination &dst,
		     const media_format &format, char *name) override;
	void Disconnect(const media_source &src, const media_destination &dst) override;
	void LateNoticeReceived(const media_source &src, bigtime_t late, bigtime_t when) override;
	void EnableOutput(const media_source &src, bool enabled, int32 *_deprecated_) override;
	status_t GetLatency(bigtime_t *latency) override;
	void LatencyChanged(const media_source &src, const media_destination &dst, bigtime_t latency,
			    uint32 flags) override;

	/* BMediaEventLooper */
	void HandleEvent(const media_timed_event *event, bigtime_t late, bool realTimeEvent) override;
	bigtime_t OfflineTime() override;

private:
	status_t NegotiateFormat(media_format &proposed) const;
	void PublishFormat(const media_format &format);
	void CopyToRing(const void *data, size_t size);

	obs_tap_shm *shm;
	media_format preferred;
	media_input input;
	media_output output;
	media_format format; /* wildcard until the first side connects; then both sides share it */
	bigtime_t downstream_latency = 0;
	bool output_enabled = true;
	int64 write_total = 0;   /* bytes copied into the ring; only ever stored to shm->write_pos */
	int32 format_serial = 0; /* seqlock counter, only ever stored to shm->format_serial */
	BBufferGroup *upstream_group = nullptr; /* group passed on to the mixer by SetBufferGroup() */
};
