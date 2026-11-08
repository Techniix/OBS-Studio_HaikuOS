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
 *                                  and left them disconnected, then exit; does nothing while another
 *                                  helper is running (that one owns the routing)
 *
 * Only one helper may own the tap. The app is launched with B_MULTIPLE_LAUNCH (so a helper started while the
 * previous one is still restoring the routing is not turned away by the registrar); instead every instance
 * takes an flock() on OBS_TAP_LOCK_PATH, which the kernel releases when the process dies however it dies.
 * A starting helper waits a few seconds for the lock, so it simply runs once the previous one is done.
 */

#include "audio-tap-shm.h"
#include "tap-node.h"

#include <Application.h>
#include <MediaRoster.h>
#include <MessageRunner.h>
#include <OS.h>
#include <TimeSource.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define TAP_SIGNATURE "application/x-vnd.obs-studio-audio-tap"
#define OBS_TAP_LOCK_PATH "/tmp/obs-haiku-audio-tap.lock"
/* how long a starting helper waits for a previous one to finish restoring the routing */
#define LOCK_WAIT_US 5000000

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

/* Takes the single-instance lock, waiting up to `wait` µs for its holder to exit. Returns the locked fd,
 * -1 if another helper holds it, or -2 if no lock file could be used (then only the area check applies). */
static int acquire_instance_lock(bigtime_t wait)
{
	int fd = open(OBS_TAP_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		fprintf(stderr, "obs-haiku-audio-tap: cannot open %s: %s\n", OBS_TAP_LOCK_PATH, strerror(errno));
		return -2;
	}
	const bigtime_t deadline = system_time() + wait;
	for (;;) {
		if (flock(fd, LOCK_EX | LOCK_NB) == 0)
			return fd;
		if (errno != EWOULDBLOCK && errno != EINTR) {
			fprintf(stderr, "obs-haiku-audio-tap: cannot lock %s: %s\n", OBS_TAP_LOCK_PATH,
				strerror(errno));
			close(fd);
			return -2;
		}
		if (system_time() >= deadline) {
			close(fd);
			return -1;
		}
		snooze(50000);
	}
}

/* How each end of the link source -> destination sees it. A failed or half-done Disconnect() can leave one
 * side connected and the other free. */
static void link_state(BMediaRoster *roster, const media_node &src_node, const media_source &src,
		       const media_node &dst_node, const media_destination &dst, bool *src_side, bool *dst_side)
{
	*src_side = *dst_side = false;

	media_output outputs[8];
	int32 count = 0;
	if (roster->GetConnectedOutputsFor(src_node, outputs, 8, &count) == B_OK)
		for (int32 i = 0; i < count; i++)
			if (outputs[i].source == src && outputs[i].destination == dst)
				*src_side = true;

	media_input inputs[8];
	count = 0;
	if (roster->GetConnectedInputsFor(dst_node, inputs, 8, &count) == B_OK)
		for (int32 i = 0; i < count; i++)
			if (inputs[i].destination == dst && inputs[i].source == src)
				*dst_side = true;
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
			/* Repair() only frees endpoints left connected to dead ports and joins free ones, so it can't
			 * break a live tap's links, but it could race a helper that is between disconnecting mixer ->
			 * card and connecting the tap. A running helper repairs on its own start anyway. */
			lock_fd = acquire_instance_lock(0);
			if (lock_fd == -1 || find_area(OBS_TAP_AREA_NAME) >= 0)
				fprintf(stderr,
					"obs-haiku-audio-tap: a desktop audio tap is running; nothing to repair\n");
			else
				Repair();
			ReleaseLock();
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

		/* Media services restarting invalidates the routing we inserted into; quit (OBS starts a new helper,
		 * which inserts the tap into the new routing). A new default audio output only takes effect when
		 * Media preferences restarts the media services, which B_MEDIA_SERVER_QUIT covers; any other
		 * rerouting of the mixer output disconnects our input, which the watchdog notices
		 * (InputConnected()). B_MEDIA_DEFAULT_CHANGED is accepted by StartWatching() but the Media Kit never
		 * sends it (Notifications.cpp: "TODO: missing: B_MEDIA_DEFAULT_CHANGED"); we watch it only in case a
		 * future Haiku does. */
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
			if (system_time() - __atomic_load_n(&shm->client_heartbeat, __ATOMIC_ACQUIRE) >
			    OBS_TAP_CLIENT_TIMEOUT) {
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
			/* not sent by current Haiku (see ReadyToRun()) */
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

		/* single instance: the flock() is the lock (atomic, released on any exit); the area name check
		 * also turns away a helper from an older build that predates the lock */
		lock_fd = acquire_instance_lock(LOCK_WAIT_US);
		if (lock_fd == -1)
			return B_BUSY;
		if (find_area(OBS_TAP_AREA_NAME) >= 0)
			return B_BUSY;

		void *addr = nullptr;
		area = create_area(OBS_TAP_AREA_NAME, &addr, B_ANY_ADDRESS, obs_tap_area_size(), B_NO_LOCK,
				   B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
		if (area < 0)
			return area;

		shm = (obs_tap_shm *)addr;
		memset(shm, 0, sizeof(*shm));
		shm->version = OBS_TAP_VERSION;
		shm->ring_size = OBS_TAP_RING_SIZE;
		shm->helper_team = (int32_t)getpid();
		/* grace period for the client to attach */
		__atomic_store_n(&shm->client_heartbeat, (int64_t)system_time(), __ATOMIC_RELAXED);
		/* last, with release: a reader that sees the magic sees the rest of the header */
		__atomic_store_n(&shm->magic, OBS_TAP_MAGIC, __ATOMIC_RELEASE);

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

		/* Follow the sound card's clock like the mixer does. SetTimeSourceFor() only posts a message; the
		 * node's control thread then takes its own reference to the shared time source object. Releasing
		 * ours right away could drop the count to zero while that thread looks the object up, leaving it
		 * with a deleted object (libmedia's TimeSourceObjectManager doesn't guard against that; it crashed
		 * in BTimeSource::GetTime()). So keep our reference until the node is gone (ReleaseClock()). */
		clock = roster->MakeTimeSourceFor(mixer);
		if (clock)
			roster->SetTimeSourceFor(tap->Node().node, clock->ID());

		/* From here on the original connection is broken until Remove() or the reconnect below. Disconnect()
		 * returns an error if either end refused, even when the other end did disconnect, so mark the link
		 * as touched first: Remove() then checks what state it is really in and restores it. */
		err = roster->Disconnect(mixer.node, orig_out.source, orig_dest_node, orig_out.destination);
		disconnected = true;
		if (err != B_OK)
			return err;

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

		__atomic_store_n(&shm->helper_alive, 1, __ATOMIC_RELEASE);
		return B_OK;
	}

	void Remove()
	{
		if (shm)
			__atomic_store_n(&shm->helper_alive, 0, __ATOMIC_RELEASE);
		if (!roster) {
			ReleaseLock();
			return;
		}

		if (media_server_gone) {
			/* nothing to restore: the nodes are gone with the server; just drop our references */
			if (tap) {
				tap->Shutdown();
				tap->Release();
				tap = nullptr;
			}
			ReleaseClock();
			disconnected = have_up = have_down = have_mixer = have_card = false;
			if (area >= 0) {
				delete_area(area);
				area = -1;
				shm = nullptr;
			}
			ReleaseLock();
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
			RestoreOriginalLink();
			disconnected = false;
		}

		if (tap) {
			/* we created the node, so we hold the last reference; this unregisters and deletes it. Stop
			 * its control thread first in case someone else still holds a reference. */
			tap->Shutdown();
			tap->Release();
			tap = nullptr;
		}
		ReleaseClock();

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
		/* last: a helper waiting for the lock starts once the routing is back to normal */
		ReleaseLock();
	}

	/* Reconnects mixer -> original destination. Copes with a Disconnect() in Insert() that failed: the link
	 * may still be fully up (nothing to do), half up (clear the stale side first) or down. */
	void RestoreOriginalLink()
	{
		media_node dest;
		bool src_side = false, dst_side = false;
		const bool have_dest = roster->GetNodeFor(orig_dest_node, &dest) == B_OK;
		if (have_dest)
			link_state(roster, mixer, orig_out.source, dest, orig_out.destination, &src_side, &dst_side);

		if (src_side && dst_side) {
			fprintf(stderr, "obs-haiku-audio-tap: original mixer connection is still up\n");
		} else {
			if (src_side || dst_side) {
				fprintf(stderr, "obs-haiku-audio-tap: clearing a half-disconnected mixer connection\n");
				roster->Disconnect(mixer.node, orig_out.source, orig_dest_node, orig_out.destination);
			}
			media_format fmt = orig_format;
			media_output o;
			media_input i;
			status_t err = roster->Connect(orig_out.source, orig_out.destination, &fmt, &o, &i);
			if (err != B_OK) {
				log_status("restoring the original mixer connection failed, retrying", err);
				Repair();
			}
		}
		if (have_dest)
			roster->ReleaseNode(dest);
	}

	void ReleaseLock()
	{
		if (lock_fd >= 0) {
			close(lock_fd); /* drops the flock */
			lock_fd = -1;
		}
	}

	void ReleaseClock()
	{
		if (clock) {
			clock->Release();
			clock = nullptr;
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
		if (r->GetAudioMixer(&m) != B_OK) {
			fprintf(stderr, "obs-haiku-audio-tap: no system mixer\n");
			return;
		}
		if (r->GetAudioOutput(&c) != B_OK) {
			fprintf(stderr, "obs-haiku-audio-tap: no audio output\n");
			r->ReleaseNode(m);
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
	int lock_fd = -1;

	area_id area = -1;
	obs_tap_shm *shm = nullptr;

	media_node mixer, card;
	bool have_mixer = false, have_card = false;

	media_output orig_out;
	media_node_id orig_dest_node = -1;
	media_format orig_format;
	bool disconnected = false;

	TapNode *tap = nullptr;
	BTimeSource *clock = nullptr; /* the mixer's time source, held while the tap exists */
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
