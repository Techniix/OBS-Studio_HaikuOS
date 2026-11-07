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
 * obs-haiku-audio-tap: desktop audio helper for OBS Studio on Haiku.
 *
 * Haiku's system mixer has exactly one output, connected to the sound card, and the Media Kit offers no
 * loopback or monitor device. This helper inserts a pass-through node (TapNode) into that connection:
 *
 *     system mixer --> [TapNode: copy to shared memory] --> sound card
 *
 * Playback is unchanged. OBS reads the copy from a cloneable area. The helper is a separate process so a
 * fault in the real-time media path cannot take OBS down, and so the original connection is always
 * restored when it exits: on quit, on SIGINT/SIGTERM/SIGHUP, or after OBS has stopped reading for
 * OBS_TAP_CLIENT_TIMEOUT.
 *
 *   obs-haiku-audio-tap            insert the tap and serve OBS until no client is left
 *   obs-haiku-audio-tap --repair   reconnect mixer -> sound card if a previous run was killed (-9)
 *                                  and left them disconnected, then exit
 */

#include "audio-tap-shm.h"
#include "tap-node.h"

#include <Application.h>
#include <MediaRoster.h>
#include <MessageRunner.h>
#include <OS.h>
#include <TimeSource.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TAP_SIGNATURE "application/x-vnd.obs-studio-audio-tap"

enum {
	MSG_WATCHDOG = 'wdog',
};

static volatile sig_atomic_t quit_signalled = 0;

/* only sets a flag (async-signal-safe); the watchdog tick turns it into B_QUIT_REQUESTED */
static void on_signal(int)
{
	quit_signalled = 1;
}

static void log_status(const char *what, status_t err)
{
	fprintf(stderr, "obs-haiku-audio-tap: %s: %s\n", what, strerror(err));
}

/* Connects the mixer's free output to the sound card's free input. Used to undo the tap and to recover from
 * a previous helper that was killed before it could restore the chain. */
static status_t connect_mixer_to_output(BMediaRoster *roster, const media_node &mixer, const media_node &out,
					const media_format *hint)
{
	media_output mixer_out;
	media_input card_in;
	int32 count = 0;

	status_t err = roster->GetFreeOutputsFor(mixer, &mixer_out, 1, &count, B_MEDIA_RAW_AUDIO);
	if (err != B_OK || count < 1)
		return err != B_OK ? err : B_MEDIA_ALREADY_CONNECTED;

	count = 0;
	err = roster->GetFreeInputsFor(out, &card_in, 1, &count, B_MEDIA_RAW_AUDIO);
	if (err != B_OK || count < 1)
		return err != B_OK ? err : B_MEDIA_ALREADY_CONNECTED;

	media_format fmt = hint ? *hint : mixer_out.format;
	err = roster->Connect(mixer_out.source, card_in.destination, &fmt, &mixer_out, &card_in);
	if (err != B_OK && hint) {
		/* retry with whatever the two nodes agree on */
		fmt = media_format();
		fmt.type = B_MEDIA_RAW_AUDIO;
		fmt.u.raw_audio = media_raw_audio_format::wildcard;
		err = roster->Connect(mixer_out.source, card_in.destination, &fmt, &mixer_out, &card_in);
	}
	return err;
}

static bool port_is_dead(port_id port)
{
	port_info info;
	return port < 0 || get_port_info(port, &info) != B_OK;
}

/* A helper killed with SIGKILL leaves the mixer's output and the card's input "connected" to its dead ports:
 * the media roster does not track connections centrally (MediaRoster.cpp: "TODO: unregister connection with
 * server"), and nodes only learn about a disconnect when told. Tell each live side, then they are free again.
 * Disconnect() only needs valid node ids to proceed; the messages go to the ports of the source/destination. */
static void break_dead_links(BMediaRoster *roster, const media_node &mixer, const media_node &out)
{
	media_output outputs[8];
	int32 count = 0;
	if (roster->GetConnectedOutputsFor(mixer, outputs, 8, &count) == B_OK) {
		for (int32 i = 0; i < count; i++) {
			if (port_is_dead(outputs[i].destination.port)) {
				fprintf(stderr, "obs-haiku-audio-tap: freeing mixer output left connected to a dead node\n");
				roster->Disconnect(mixer.node, outputs[i].source, out.node, outputs[i].destination);
			}
		}
	}

	media_input inputs[8];
	count = 0;
	if (roster->GetConnectedInputsFor(out, inputs, 8, &count) == B_OK) {
		for (int32 i = 0; i < count; i++) {
			if (port_is_dead(inputs[i].source.port)) {
				fprintf(stderr, "obs-haiku-audio-tap: freeing audio output input left connected to a dead node\n");
				roster->Disconnect(mixer.node, inputs[i].source, out.node, inputs[i].destination);
			}
		}
	}
}

class TapApp : public BApplication {
public:
	explicit TapApp(bool repair_only) : BApplication(TAP_SIGNATURE), repair_only(repair_only) {}

	void ReadyToRun() override
	{
		if (repair_only) {
			Repair();
			exit_code = 0;
			PostMessage(B_QUIT_REQUESTED);
			return;
		}

		status_t err = Insert();
		if (err != B_OK) {
			log_status("could not insert the desktop audio tap", err);
			Remove();
			exit_code = 1;
			PostMessage(B_QUIT_REQUESTED);
			return;
		}

		/* media services restarting or the default output changing invalidate the routing we inserted
		 * into; quit (OBS starts a new helper, which inserts the tap into the new routing) */
		roster->StartWatching(BMessenger(this), B_MEDIA_SERVER_QUIT);
		roster->StartWatching(BMessenger(this), B_MEDIA_DEFAULT_CHANGED);

		BMessage tick(MSG_WATCHDOG);
		watchdog = new BMessageRunner(BMessenger(this), &tick, 500000);
		fprintf(stderr, "obs-haiku-audio-tap: tap inserted (%.0f Hz, %u channels, format 0x%x)\n",
			shm->frame_rate, shm->channels, shm->format);
	}

	void MessageReceived(BMessage *msg) override
	{
		if (msg->what == MSG_WATCHDOG) {
			if (quit_signalled) {
				PostMessage(B_QUIT_REQUESTED);
				return;
			}
			if (system_time() - shm->client_heartbeat > OBS_TAP_CLIENT_TIMEOUT) {
				fprintf(stderr, "obs-haiku-audio-tap: no client, exiting\n");
				PostMessage(B_QUIT_REQUESTED);
			} else if (tap && !tap->InputConnected()) {
				/* someone (Media preferences, Cortex) rerouted the mixer away from us */
				fprintf(stderr, "obs-haiku-audio-tap: mixer was disconnected from the tap, exiting\n");
				PostMessage(B_QUIT_REQUESTED);
			}
			return;
		}
		if (msg->what == B_MEDIA_SERVER_QUIT) {
			fprintf(stderr, "obs-haiku-audio-tap: media services quit, exiting\n");
			media_server_gone = true;
			PostMessage(B_QUIT_REQUESTED);
			return;
		}
		if (msg->what == B_MEDIA_DEFAULT_CHANGED) {
			fprintf(stderr, "obs-haiku-audio-tap: default audio output changed, exiting\n");
			PostMessage(B_QUIT_REQUESTED);
			return;
		}
		BApplication::MessageReceived(msg);
	}

	bool QuitRequested() override
	{
		delete watchdog;
		watchdog = nullptr;
		Remove();
		return true;
	}

	int ExitCode() const { return exit_code; }

private:
	status_t Insert()
	{
		status_t err;
		roster = BMediaRoster::Roster(&err);
		if (!roster)
			return err;

		/* single instance: the area name is the lock */
		if (find_area(OBS_TAP_AREA_NAME) >= 0)
			return B_BUSY;

		void *addr = nullptr;
		area = create_area(OBS_TAP_AREA_NAME, &addr, B_ANY_ADDRESS, obs_tap_area_size(), B_NO_LOCK,
				   B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
		if (area < 0)
			return area;

		shm = (obs_tap_shm *)addr;
		memset(shm, 0, sizeof(*shm));
		shm->magic = OBS_TAP_MAGIC;
		shm->version = OBS_TAP_VERSION;
		shm->ring_size = OBS_TAP_RING_SIZE;
		shm->helper_team = (int32_t)getpid();
		shm->client_heartbeat = system_time(); /* grace period for the client to attach */

		if ((err = roster->GetAudioMixer(&mixer)) != B_OK)
			return err;
		have_mixer = true;
		if ((err = roster->GetAudioOutput(&card)) != B_OK)
			return err;
		have_card = true;

		/* undo whatever a previous helper killed with SIGKILL left behind, then find the mixer -> sound
		 * card connection */
		Repair();
		int32 count = 0;
		err = roster->GetConnectedOutputsFor(mixer, &orig_out, 1, &count);
		if (err != B_OK || count < 1)
			return err != B_OK ? err : B_MEDIA_NOT_CONNECTED;
		if (roster->NodeIDFor(orig_out.destination.port) != card.node) {
			/* someone else is already between the mixer and the card (or the output was rerouted);
			 * insert after the mixer anyway, the original destination is what we reconnect to */
			fprintf(stderr, "obs-haiku-audio-tap: mixer output goes to node %d, not the default output %d\n",
				(int)roster->NodeIDFor(orig_out.destination.port), (int)card.node);
		}
		orig_dest_node = roster->NodeIDFor(orig_out.destination.port);
		orig_format = orig_out.format;

		tap = new TapNode(shm, orig_format);
		if ((err = roster->RegisterNode(tap)) != B_OK) {
			tap->Release();
			tap = nullptr;
			return err;
		}

		/* follow the sound card's clock like the mixer does */
		BTimeSource *mixer_ts = roster->MakeTimeSourceFor(mixer);
		if (mixer_ts) {
			roster->SetTimeSourceFor(tap->Node().node, mixer_ts->ID());
			mixer_ts->Release();
		}

		/* from here on the original connection is broken until Remove() or the reconnect below */
		if ((err = roster->Disconnect(mixer.node, orig_out.source, orig_dest_node, orig_out.destination)) !=
		    B_OK)
			return err;
		disconnected = true;

		media_input tap_in;
		count = 0;
		if ((err = roster->GetFreeInputsFor(tap->Node(), &tap_in, 1, &count, B_MEDIA_RAW_AUDIO)) != B_OK ||
		    count < 1)
			return err != B_OK ? err : B_ERROR;

		media_output tap_out;
		count = 0;
		if ((err = roster->GetFreeOutputsFor(tap->Node(), &tap_out, 1, &count, B_MEDIA_RAW_AUDIO)) !=
			    B_OK ||
		    count < 1)
			return err != B_OK ? err : B_ERROR;

		/* downstream first, so the tap knows the card's latency and buffer group when the mixer connects */
		media_format fmt = orig_format;
		if ((err = roster->Connect(tap_out.source, orig_out.destination, &fmt, &down_out, &down_in)) != B_OK)
			return err;
		have_down = true;

		fmt = down_out.format;
		if ((err = roster->Connect(orig_out.source, tap_in.destination, &fmt, &up_out, &up_in)) != B_OK)
			return err;
		have_up = true;

		BTimeSource *ts = roster->MakeTimeSourceFor(tap->Node());
		if (!ts)
			return B_ERROR;
		err = roster->StartNode(tap->Node(), ts->Now());
		ts->Release();
		if (err != B_OK)
			return err;

		shm->helper_alive = 1;
		return B_OK;
	}

	void Remove()
	{
		if (shm)
			shm->helper_alive = 0;
		if (!roster)
			return;

		if (media_server_gone) {
			/* nothing to restore: the nodes are gone with the server; just drop our references */
			if (tap) {
				tap->Release();
				tap = nullptr;
			}
			disconnected = have_up = have_down = have_mixer = have_card = false;
			if (area >= 0) {
				delete_area(area);
				area = -1;
				shm = nullptr;
			}
			return;
		}

		if (tap) {
			roster->StopNode(tap->Node(), 0, true);
			if (have_up)
				roster->Disconnect(up_out.node.node, up_out.source, up_in.node.node, up_in.destination);
			if (have_down)
				roster->Disconnect(down_out.node.node, down_out.source, down_in.node.node,
						   down_in.destination);
			have_up = have_down = false;
		}

		if (disconnected) {
			media_format fmt = orig_format;
			media_output o;
			media_input i;
			status_t err = roster->Connect(orig_out.source, orig_out.destination, &fmt, &o, &i);
			if (err != B_OK) {
				log_status("restoring the original mixer connection failed, retrying", err);
				Repair();
			}
			disconnected = false;
		}

		if (tap) {
			/* we created the node, so we hold the last reference; this unregisters and deletes it */
			tap->Release();
			tap = nullptr;
		}

		if (have_mixer)
			roster->ReleaseNode(mixer);
		if (have_card)
			roster->ReleaseNode(card);
		have_mixer = have_card = false;

		if (area >= 0) {
			delete_area(area);
			area = -1;
			shm = nullptr;
		}
	}

	void Repair()
	{
		status_t err;
		BMediaRoster *r = BMediaRoster::Roster(&err);
		if (!r) {
			log_status("media roster", err);
			return;
		}
		media_node m, c;
		if (r->GetAudioMixer(&m) != B_OK || r->GetAudioOutput(&c) != B_OK) {
			fprintf(stderr, "obs-haiku-audio-tap: no system mixer or audio output\n");
			return;
		}
		break_dead_links(r, m, c);
		err = connect_mixer_to_output(r, m, c, nullptr);
		if (err == B_OK)
			fprintf(stderr, "obs-haiku-audio-tap: reconnected system mixer to the audio output\n");
		else if (err != B_MEDIA_ALREADY_CONNECTED)
			log_status("repair", err);
		r->ReleaseNode(m);
		r->ReleaseNode(c);
	}

	const bool repair_only;
	bool media_server_gone = false;
	int exit_code = 0;
	BMediaRoster *roster = nullptr;
	BMessageRunner *watchdog = nullptr;

	area_id area = -1;
	obs_tap_shm *shm = nullptr;

	media_node mixer, card;
	bool have_mixer = false, have_card = false;

	media_output orig_out;
	media_node_id orig_dest_node = -1;
	media_format orig_format;
	bool disconnected = false;

	TapNode *tap = nullptr;
	media_output up_out, down_out;
	media_input up_in, down_in;
	bool have_up = false, have_down = false;
};


int main(int argc, char **argv)
{
	bool repair = argc > 1 && strcmp(argv[1], "--repair") == 0;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGHUP, &sa, nullptr);
	signal(SIGPIPE, SIG_IGN);

	TapApp app(repair);
	app.Run();
	return app.ExitCode();
}
