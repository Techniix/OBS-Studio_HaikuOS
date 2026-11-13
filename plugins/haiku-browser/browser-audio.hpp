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
 * Built on Renga's system mixer (F-58, OBS requests O-3 and O-18; headers/private/media/AudioStreamControl.h):
 *  - O-3, loopback by team: the source instantiates its own "What you hear (loopback)" flavor of the mixer
 *    add-on and points it at the helper's team with AudioStreamControl::SetLoopbackTeam(). The mixer then feeds
 *    it that team's streams only (MixerCore::_InputFade(), kMixApplication with selection->team > 0).
 *  - O-18, loopback only by team: AudioStreamControl::SetTeamLoopbackOnly(helperTeam, true) puts the team on
 *    the mixer's "renga:loopback_only_teams" list. The mixer marks a stream of a listed team loopback-only while
 *    it connects (AudioMixer::Connected(), before its first buffer) and switches streams already playing at
 *    once (AudioMixer::SetParameterValue(), PARAM_ETC_LOOPBACK_ONLY_TEAMS). Such a stream plays on no output and
 *    is left out of every all-streams loopback, Desktop Audio's included (MixerCore::_InputFade(), kMixAll), but
 *    is still recorded by a loopback that selects its team. Nothing is remembered: the mixer forgets the team
 *    when it quits (AudioMixer::_PruneLoopbackOnlyTeams()), and other helpers and docks are never touched.
 *
 * Order: the team is set loopback-only first, then the loopback is set up. The mixer accepts any running team,
 * also one without streams (it only checks get_team_info(); B_BAD_VALUE is for a team that does not run), so
 * this is done as soon as the helper has been spawned. The client holds back every message to a new helper of
 * a rerouted source, the page's URL included, until `ready` reported the team (HaikuBrowserClient::
 * AudioReady()), so the page cannot play before its team is listed. If that cannot be done (no mixer, an older
 * mixer), the page is released anyway and the failure is logged; the client also gives up waiting after a few
 * seconds (a hung media server must not keep the page blank).
 *
 * Threads: one worker per instance does every Media Kit call (the media server can be slow or restarting, and
 * the callers are the UI thread and a client's I/O thread). The recorder's node thread outputs the audio.
 * Neither the constructor nor the destructor waits for the worker: the destructor only stops the audio output
 * and the ready callback (waiting at most for one call in progress) and tells the worker to clean up and end;
 * haiku_browser_audio_shutdown() joins what is left at module unload.
 */

#pragma once

#include <obs-module.h>

#include <OS.h>

#include <functional>
#include <memory>

class BrowserAudioReroute {
public:
	/* `ready` is called on the worker thread once the helper's team was put on the mixer's loopback-only list
	 * (or that failed): the page may load. Never called after the destructor returned. */
	BrowserAudioReroute(obs_source_t *source, std::function<void(team_id)> ready);
	~BrowserAudioReroute();

	BrowserAudioReroute(const BrowserAudioReroute &) = delete;
	BrowserAudioReroute &operator=(const BrowserAudioReroute &) = delete;

	/* Any thread, never waits: the team of the helper that renders the page (it changes when the helper is
	 * restarted). The loopback and the loopback-only setting follow it. */
	void SetHelperTeam(team_id team);

	/* Any thread, never waits: the helper is being ended together with the source. Its team is then left on
	 * the loopback-only list (the mixer forgets it when the helper has quit), so that the page's last sound
	 * does not reach the speakers. Without this call, the destructor takes the team off the list: the option
	 * was turned off and the page keeps playing as an ordinary application. */
	void HelperClosing();

	struct State;

private:
	std::shared_ptr<State> state;
};

/* Joins the workers of destroyed instances; call from obs_module_unload(). */
void haiku_browser_audio_shutdown(void);
