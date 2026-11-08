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

#include "haiku-sound-player.h"

#include <string.h>

#include <MediaDefs.h>
#include <SoundPlayer.h>

struct obs_haiku_sound_player {
	BSoundPlayer *player;
	obs_haiku_fill_cb cb;
	void *param;
	uint32_t channels;
};

static void play_buffer(void *cookie, void *buffer, size_t size, const media_raw_audio_format &format)
{
	obs_haiku_sound_player *p = static_cast<obs_haiku_sound_player *>(cookie);

	/* The mixer accepts the format we asked for; anything else would be a negotiation bug. */
	if (format.format != media_raw_audio_format::B_AUDIO_FLOAT || format.channel_count != p->channels) {
		memset(buffer, 0, size);
		return;
	}

	const size_t frames = size / (sizeof(float) * p->channels);
	p->cb(p->param, static_cast<float *>(buffer), frames, p->channels);
}

extern "C" obs_haiku_sound_player *obs_haiku_sound_player_create(const char *name, uint32_t sample_rate,
								 uint32_t channels, size_t buffer_frames,
								 obs_haiku_fill_cb cb, void *param)
{
	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.frame_rate = (float)sample_rate;
	format.channel_count = channels;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.byte_order = B_MEDIA_HOST_ENDIAN;
	format.buffer_size = buffer_frames * channels * sizeof(float);

	obs_haiku_sound_player *p = new obs_haiku_sound_player;
	p->cb = cb;
	p->param = param;
	p->channels = channels;
	p->player = new BSoundPlayer(&format, name, play_buffer, NULL, p);

	if (p->player->InitCheck() != B_OK || p->player->Start() != B_OK) {
		delete p->player;
		delete p;
		return NULL;
	}

	p->player->SetHasData(true);
	return p;
}

extern "C" void obs_haiku_sound_player_destroy(obs_haiku_sound_player *p)
{
	if (!p)
		return;

	/* Stop without blocking: Stop(true, ...) polls IsPlaying() for up to 500 ms and then snoozes
	 * Latency() + 2 ms so queued buffers finish playing, which is only about letting the tail play
	 * out (and ~BSoundPlayer would do the same if the player were still started, hence the explicit
	 * non-blocking Stop first). Callback safety comes from the destructor: it releases the
	 * SoundPlayNode, whose DeleteHook (BMediaEventLooper) calls Quit(), which closes the control port
	 * and wait_for_thread()s the control thread -- the only thread that calls PlayBuffer -- before the
	 * node is deleted. So play_buffer cannot run once delete returns. */
	p->player->Stop(false, false);
	delete p->player;
	delete p;
}
