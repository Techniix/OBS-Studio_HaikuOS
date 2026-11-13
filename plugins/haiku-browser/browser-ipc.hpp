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
 * Protocol between the OBS process and obs-haiku-browser, the HaikuWebKit helper that renders browser sources
 * and browser docks (there is no CEF for Haiku or Renga yet).
 *
 * It is built on Renga's Linux-style descriptors (3rdparty/ce/PORTING.md in the Renga tree) rather than on
 * areas, heartbeats in shared memory and BMessenger, because each of them removes a failure mode:
 *
 * Channel. OBS makes an AF_UNIX SOCK_STREAM socketpair and the helper inherits one end ("--ipc <fd>").
 *   Either side dying closes its end, which the other one sees at once as end-of-file: no heartbeat, no
 *   timeout, and a helper can never outlive OBS. Messages are framed by hand: a uint32 byte count, then the
 *   payload. Everything is ordered on this one channel, in both directions; data written before the helper's
 *   BApplication exists simply waits in the socket.
 *   Why not SOCK_SEQPACKET, which Renga has with SCM_RIGHTS (F-58, O-13; tests/posix_compat/seqpacket_test.cpp)
 *   and which would keep message boundaries: a message has to fit whole in the receiver's buffer, 64 KiB by
 *   default and 128 KiB at most with SO_RCVBUF (UnixFifo.h: UNIX_FIFO_MAXIMAL_CAPACITY). Since F-58 batch 2
 *   (O-19) a bigger one fails at once with EMSGSIZE (UnixFifo::_MessageTooLarge()) instead of waiting forever,
 *   but it still cannot be sent. OBS -> helper messages carry custom CSS and scripts (OBS_BROWSER_MAX_MESSAGE),
 *   and CSS with embedded data: fonts or images is easily bigger than 128 KiB. They would need a second path
 *   (a sealed memfd per large message, sent with SCM_RIGHTS, or splitting and reassembly), which is more code
 *   and more states than the uint32 length prefix it would remove. So the stream stays; on it EMSGSIZE does not
 *   occur (streams are written in parts), and every send error other than EINTR/EAGAIN means the peer is gone:
 *   OBS treats it as the helper's death (HaikuBrowserClient::Flush()), the helper as OBS's (Channel::Send(),
 *   the reader then sees end-of-file).
 *   OBS -> helper payloads are flattened BMessages (the helper trusts OBS, and its handlers are BMessage
 *   handlers anyway). Helper -> OBS payloads are a fixed binary record, obs_browser_event: a renderer can be
 *   taken over by the page it shows, and unflattening a BMessage from it would let it hand OBS arbitrary field
 *   tables (or an area id to clone).
 *
 * Frames. The helper owns the pixels: for every buffer size it needs it creates a memfd (memfd_create), sizes
 *   it, seals it against resizing (F_SEAL_SHRINK | F_SEAL_GROW, so no mapping can fault after a truncate, and
 *   F_SEAL_FUTURE_WRITE, so nobody else can map it writable) and passes it with OBS_BROWSER_MSG_BUFFER
 *   (SCM_RIGHTS). OBS maps it read-only and drops the previous one. A resize is just the next buffer message.
 *   The file starts with obs_browser_frame_header; the helper writes frames under a sequence lock (`seq` is
 *   odd while it writes). A reader that loses the race drops that copy: it was caused by a newer frame, whose
 *   wake-up is already on its way. (Flipping between two buffers would not be tear-free without a reply from
 *   the reader, and the reader has, on purpose, no write access.)
 *
 * Wake-up. After each frame the helper adds 1 to an eventfd it inherited from OBS ("--wake <fd>"). The counter
 *   coalesces, so a slow reader gets one wake-up for many frames and the helper never blocks on it.
 *
 * Liveness. Death is the socket's end-of-file. A helper that is alive but stuck (WebCore's main thread in an
 *   endless script, or stopped in the debugger after a crash) is found with OBS_BROWSER_MSG_PING, answered on
 *   the helper's application thread.
 */

#pragma once

#include <Message.h>

#include <stdint.h>
#include <string.h>

#include <vector>

#define OBS_BROWSER_HELPER_NAME "obs-haiku-browser"
#define OBS_BROWSER_HELPER_SIGNATURE "application/x-vnd.obs-studio-browser-helper"

/* Sent in OBS_BROWSER_MSG_HELLO; the helper is installed next to obs, but an older copy can be left over in
 * non-packaged. Still 3: dropping the "--audio-rerouted" argument that came with 3 changed no message. */
#define OBS_BROWSER_PROTOCOL 3

/* Upper bound of one OBS -> helper message (scripts travel in them) */
#define OBS_BROWSER_MAX_MESSAGE (16u << 20)

/* Page size limits, the same on both sides */
#define OBS_BROWSER_MAX_DIMENSION 8192

/* HaikuWebKit's cookie database (WebCore's curl CookieJarDB, SQLite), one per storage directory; every helper
 * using the same directory shares it. The same name WebPositive uses. */
#define OBS_BROWSER_COOKIE_DB "cookie.jar.db"

#define OBS_BROWSER_FRAME_MAGIC 0x4f425346u /* 'OBSF' */

/* pixels start one page into the memfd */
#define OBS_BROWSER_FRAME_HEADER_SIZE 4096u

/* At offset 0 of a frame memfd. Written by the helper only; OBS has a read-only mapping and treats every field
 * as untrusted (a page can take over its renderer): it checks them against the size of its own mapping. */
struct obs_browser_frame_header {
	uint32_t magic;
	uint32_t seq;       /* sequence lock, odd while the helper writes a frame */
	uint32_t width;     /* of the frame in the pixels, BGRA */
	uint32_t height;
	uint32_t stride;    /* bytes per row */
	uint32_t has_alpha; /* 0: treat as BGRX */
	uint64_t frame_count;
};

/* Message codes, OBS -> helper */
enum {
	/* "url" string, "width"/"height"/"fps" int32, "css" string, "transparent" bool */
	OBS_BROWSER_MSG_CONFIG = 'obCF',
	/* "nocache" bool */
	OBS_BROWSER_MSG_RELOAD = 'obRL',
	/* "script" string */
	OBS_BROWSER_MSG_EXECUTE_JS = 'obJS',
	/* "startup" string: script run on every page load */
	OBS_BROWSER_MSG_STARTUP_SCRIPT = 'obSS',
	/* "visible"/"active" bool: fires obsSourceVisibleChanged/obsSourceActiveChanged */
	OBS_BROWSER_MSG_VISIBILITY = 'obVS',
	/* "x"/"y" int32, "buttons" int32 (B_PRIMARY_MOUSE_BUTTON...), "clicks" int32, "modifiers" int32 */
	OBS_BROWSER_MSG_MOUSE_DOWN = 'obMD',
	OBS_BROWSER_MSG_MOUSE_UP = 'obMU',
	/* "x"/"y" int32, "modifiers" int32, "leave" bool */
	OBS_BROWSER_MSG_MOUSE_MOVE = 'obMM',
	/* "dx"/"dy" float (wheel steps, positive = down/right), "modifiers" int32 */
	OBS_BROWSER_MSG_MOUSE_WHEEL = 'obMW',
	/* "bytes" string (UTF-8 of the key, or a B_*_KEY control byte), "key" int32 raw key, "modifiers" int32 */
	OBS_BROWSER_MSG_KEY_DOWN = 'obKD',
	OBS_BROWSER_MSG_KEY_UP = 'obKU',
	/* "focus" bool */
	OBS_BROWSER_MSG_FOCUS = 'obFO',
	/* "direction" int32: -1 out, 0 reset, 1 in */
	OBS_BROWSER_MSG_ZOOM = 'obZM',
	/* "serial" int32; answered with OBS_BROWSER_MSG_PONG from the application (WebCore main) thread */
	OBS_BROWSER_MSG_PING = 'obPI',
};

/* Event codes, helper -> OBS (obs_browser_event::what) */
enum {
	/* value: OBS_BROWSER_PROTOCOL; the first event, sent once WebKit and the page's window exist */
	OBS_BROWSER_EVENT_HELLO = 'obHI',
	/* a new frame memfd, passed with SCM_RIGHTS on the first byte of this event; replaces the previous one */
	OBS_BROWSER_EVENT_BUFFER = 'obBF',
	/* followed by two NUL-terminated strings, the page's title and URL (docks: titleChanged/urlChanged) */
	OBS_BROWSER_EVENT_INFO = 'obIN',
	/* value: the serial of the OBS_BROWSER_MSG_PING it answers */
	OBS_BROWSER_EVENT_PONG = 'obPO',
};

/* The payload of a helper -> OBS message; text (if any) follows */
struct obs_browser_event {
	uint32_t what;
	int32_t value;
};

/* An event with its text can never be bigger than this; OBS drops a helper that claims otherwise */
#define OBS_BROWSER_MAX_EVENT 16384u

/* One framed OBS -> helper message: a uint32 byte count (host order, both ends are the same machine) and the
 * flattened BMessage. Empty if the message cannot be flattened or is too big. */
static inline std::vector<uint8_t> obs_browser_frame_message(const BMessage &message)
{
	std::vector<uint8_t> out;
	const ssize_t size = message.FlattenedSize();
	if (size <= 0 || (size_t)size > OBS_BROWSER_MAX_MESSAGE)
		return out;
	out.resize(sizeof(uint32_t) + (size_t)size);
	const uint32_t length = (uint32_t)size;
	memcpy(out.data(), &length, sizeof(length));
	if (message.Flatten((char *)out.data() + sizeof(length), size) != B_OK)
		out.clear();
	return out;
}
