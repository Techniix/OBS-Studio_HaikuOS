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
 * "Control audio via OBS" (obs-browser's reroute_audio) for one browser source: the page's sound becomes the
 * source's audio and is neither played on the speakers nor recorded by Desktop Audio.
 *
 * Built on Renga's system mixer (batch F-58, OBS requests O-2 and O-3; headers/private/media/AudioStreamControl.h):
 *  - O-3, loopback by team: the source instantiates its own "What you hear (loopback)" flavor of the mixer
 *    add-on and points it at the helper's team with AudioStreamControl::SetLoopbackTeam(). The mixer then feeds
 *    it that team's streams only (MixerCore::_InputFade(), kMixApplication with selection->team > 0).
 *  - O-2, loopback-only route: the helper's streams are routed to RENGA_LOOPBACK_ONLY_ROUTE with
 *    AudioStreamControl::SetLoopbackOnly(). Such a stream plays on no output and is left out of every
 *    all-streams loopback, Desktop Audio's included (MixerCore::_InputFade(), kMixAll), but is still recorded by
 *    a loopback that selects its team.
 *
 * Which streams are the helper's: the mixer's parameter web names a stream's application by signature only
 * (AudioStream has no team), and the mixer remembers a route per signature (MixerSettings::
 * SaveConnectionSettings() keys input settings by "signature"). A helper of a rerouted source therefore runs
 * under its own signature, OBS_BROWSER_HELPER_REROUTED_SIGNATURE (browser-ipc.hpp): every stream with it is
 * routed loopback-only, the mixer remembers that, so the next helper's streams start out loopback-only
 * (LoadConnectionSettings() on connection), and the route of the ordinary helpers (other sources, docks) is
 * never touched. The loopback, on the other hand, selects by team, so each source records only its own page.
 *
 * Threads: one worker per instance does every Media Kit call (the media server can be slow or restarting, and
 * the callers are the UI thread and a client's I/O thread). The recorder's node thread outputs the audio.
 * Neither the constructor nor the destructor waits for the worker: the destructor only stops the audio output
 * (waiting at most for one obs_source_output_audio() in progress) and tells the worker to clean up and end;
 * haiku_browser_audio_shutdown() joins what is left at module unload.
 */

#pragma once

#include <obs-module.h>

#include <OS.h>

#include <memory>

class BrowserAudioReroute {
public:
	explicit BrowserAudioReroute(obs_source_t *source);
	~BrowserAudioReroute();

	BrowserAudioReroute(const BrowserAudioReroute &) = delete;
	BrowserAudioReroute &operator=(const BrowserAudioReroute &) = delete;

	/* Any thread, never waits: the team of the helper that renders the page (it changes when the helper is
	 * restarted). The loopback follows it. */
	void SetHelperTeam(team_id team);

	struct State;

private:
	std::shared_ptr<State> state;
};

/* Joins the workers of destroyed instances; call from obs_module_unload(). */
void haiku_browser_audio_shutdown(void);
