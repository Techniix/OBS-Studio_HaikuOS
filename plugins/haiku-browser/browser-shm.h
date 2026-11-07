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
 * and browser docks on Haiku (there is no CEF for Haiku).
 *
 * Frames: OBS creates a cloneable area per browser instance, passes its id on the helper's command line (and
 * later in OBS_BROWSER_MSG_ATTACH_AREA when it needs a bigger one) and the helper clones it. The helper copies
 * the rendered page into pixels[] under a sequence lock: `seq` is odd while a frame is being written.
 *
 * Control: OBS sends BMessages to the helper application (BMessenger by signature + team): configuration,
 * input events, JavaScript, reload. Nothing flows back except through the area.
 *
 * Both sides watch the other's heartbeat. A helper whose client stopped beating quits; a client whose helper
 * died starts a new one, so a crashing page never takes OBS down.
 */

#pragma once

#include <stdint.h>

#define OBS_BROWSER_HELPER_NAME "obs-haiku-browser"
#define OBS_BROWSER_HELPER_SIGNATURE "application/x-vnd.obs-studio-browser-helper"

#define OBS_BROWSER_MAGIC 0x4f425342u /* 'OBSB' */
#define OBS_BROWSER_VERSION 1u

/* pixels[] starts one page into the area */
#define OBS_BROWSER_HEADER_SIZE 4096u

#define OBS_BROWSER_CLIENT_TIMEOUT 5000000LL /* µs without a client heartbeat before the helper quits */

#define OBS_BROWSER_TITLE_LEN 256
#define OBS_BROWSER_URL_LEN 2048

enum obs_browser_helper_state {
	OBS_BROWSER_STATE_STARTING = 0,
	OBS_BROWSER_STATE_READY = 1,
};

struct obs_browser_shm {
	uint32_t magic;
	uint32_t version;
	uint32_t capacity; /* bytes available in pixels[] */

	volatile int32_t seq;    /* sequence lock, odd while the helper writes a frame */
	volatile int32_t width;  /* of the frame in pixels[] */
	volatile int32_t height;
	volatile int32_t stride;
	volatile int32_t has_alpha; /* 0: treat as BGRX */
	volatile int64_t frame_count;

	volatile int64_t client_heartbeat; /* system_time() written by OBS */
	volatile int64_t helper_heartbeat; /* system_time() written by the helper */
	volatile int32_t helper_state;     /* obs_browser_helper_state */
	volatile int32_t helper_team;

	/* page information for docks (titleChanged/urlChanged); info_serial changes after an update */
	volatile int32_t info_serial;
	char title[OBS_BROWSER_TITLE_LEN];
	char url[OBS_BROWSER_URL_LEN];
};

static inline uint8_t *obs_browser_pixels(struct obs_browser_shm *shm)
{
	return (uint8_t *)shm + OBS_BROWSER_HEADER_SIZE;
}

static inline uint32_t obs_browser_area_size(uint32_t width, uint32_t height)
{
	const uint32_t total = OBS_BROWSER_HEADER_SIZE + width * height * 4u;
	return (total + 4095u) & ~4095u;
}

/* BMessage codes, OBS -> helper */
enum {
	/* "url" string, "width"/"height"/"fps" int32, "css" string, "transparent" bool, "reroute_audio" bool */
	OBS_BROWSER_MSG_CONFIG = 'obCF',
	/* "area" int32: switch to a new (bigger) area */
	OBS_BROWSER_MSG_ATTACH_AREA = 'obAA',
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
	/* "x"/"y" int32, "buttons" int32, "modifiers" int32, "leave" bool */
	OBS_BROWSER_MSG_MOUSE_MOVE = 'obMM',
	/* "dx"/"dy" float (wheel steps, positive = down/right) */
	OBS_BROWSER_MSG_MOUSE_WHEEL = 'obMW',
	/* "bytes" string (UTF-8 of the key, or a B_*_KEY control byte), "key" int32 raw key, "modifiers" int32 */
	OBS_BROWSER_MSG_KEY_DOWN = 'obKD',
	OBS_BROWSER_MSG_KEY_UP = 'obKU',
	/* "focus" bool */
	OBS_BROWSER_MSG_FOCUS = 'obFO',
	/* "direction" int32: -1 out, 0 reset, 1 in */
	OBS_BROWSER_MSG_ZOOM = 'obZM',
};
