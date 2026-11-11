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

#include "obs-haiku.h"

#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <pthread.h>

#include <Application.h>
#include <InterfaceDefs.h>
#include <Point.h>
#include <Roster.h>
#include <Screen.h>
#include <UnicodeChar.h>
#include <WindowScreen.h> /* set_mouse_position() (libgame) */

/* ScreenSaverFilter (input_server) starts the blanker when a B_MOUSE_MOVED lands in its "blank now" corner,
 * a kBlankCornerSize (5 px) square, and arms "never blank" in a 10 px one. The configured corner is private,
 * so no poke is sent anywhere near any screen corner. */
#define SCREEN_CORNER_MARGIN 16.0f

/* SCREEN_BLANKER_SIG from Haiku's private ScreenSaverSettings.h: the app the screen saver filter launches */
#define SCREEN_BLANKER_SIGNATURE "application/x-vnd.Haiku.screenblanker"

static_assert(OBS_HAIKU_SHIFT_KEY == B_SHIFT_KEY && OBS_HAIKU_COMMAND_KEY == B_COMMAND_KEY &&
		      OBS_HAIKU_CONTROL_KEY == B_CONTROL_KEY && OBS_HAIKU_OPTION_KEY == B_OPTION_KEY,
	      "modifier constants out of sync with InterfaceDefs.h");

extern "C" bool obs_haiku_get_key_states(uint8_t states[OBS_HAIKU_KEY_STATE_BYTES], uint32_t *modifiers)
{
	key_info info;

	/* Talks to the input_server over a port; works from any thread with no BApplication.
	 * (Unlike get_mouse(), which needs be_app; see below.) */
	if (get_key_info(&info) != B_OK)
		return false;

	static_assert(sizeof(info.key_states) == OBS_HAIKU_KEY_STATE_BYTES, "unexpected key_info layout");
	memcpy(states, info.key_states, OBS_HAIKU_KEY_STATE_BYTES);
	*modifiers = info.modifiers;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Keymap (key labels and modifier key positions)                             */

/* get_key_map() is an input_server round trip that returns two malloc()ed blocks, and the hotkey settings
 * page asks for the label of every bound key in a row, so what is needed from the keymap is cached. The
 * cache is refreshed after two seconds, which is how a layout switch (Keymap preferences, keymap switcher)
 * is picked up; the same time stamp throttles retries while the input_server cannot be reached. */
#define KEYMAP_CACHE_US 2000000LL

static pthread_mutex_t keymap_mutex = PTHREAD_MUTEX_INITIALIZER;
static bigtime_t keymap_fetched = 0;
static bool keymap_valid = false;
static char keymap_labels[128][OBS_HAIKU_KEY_LABEL_SIZE];
static uint32 keymap_modifier_keys[3][2]; /* [command, control, option][left, right] */

/* Copies the character a keymap table produces for a raw key into `label`, upper-cased ("a" -> "A",
 * "\xc3\xa9" (e acute) -> "\xc3\x89"). The table holds offsets into `chars`, where each entry is a length
 * byte followed by that many UTF-8 bytes (see key_map in InterfaceDefs.h and the Keyboard chapter of the
 * Haiku Book). Returns false for keys that produce nothing printable. */
static bool keymap_char(const int32 *table, const char *chars, int code, char label[OBS_HAIKU_KEY_LABEL_SIZE])
{
	const int32 offset = table[code];
	if (offset < 0)
		return false;

	const uint8 length = (uint8)chars[offset];
	if (length == 0 || length >= OBS_HAIKU_KEY_LABEL_SIZE)
		return false;

	/* zero padded, so decoding a truncated sequence cannot run past the copy */
	char utf8[OBS_HAIKU_KEY_LABEL_SIZE + 8] = {};
	memcpy(utf8, chars + offset + 1, length);

	/* control characters, space and DEL are not usable as a label */
	if ((uint8)utf8[0] <= 0x20 || utf8[0] == 0x7f)
		return false;

	const char *in = utf8;
	const uint32 c = BUnicodeChar::FromUTF8(&in);
	if (c != 0 && (size_t)(in - utf8) == length) {
		/* a single character: show it the way key caps do. Characters whose upper case form needs a
		 * longer UTF-8 sequence than the label can hold are kept as they are. */
		char upper[OBS_HAIKU_KEY_LABEL_SIZE + 8] = {};
		char *out = upper;
		BUnicodeChar::ToUTF8(BUnicodeChar::ToUpper(c), &out);
		if (out > upper && (size_t)(out - upper) < OBS_HAIKU_KEY_LABEL_SIZE)
			memcpy(utf8, upper, sizeof(upper));
	}

	memcpy(label, utf8, OBS_HAIKU_KEY_LABEL_SIZE);
	label[OBS_HAIKU_KEY_LABEL_SIZE - 1] = 0;
	return true;
}

/* keymap_mutex must be held */
static void refresh_keymap(void)
{
	const bigtime_t now = system_time();
	if (keymap_fetched != 0 && now - keymap_fetched < KEYMAP_CACHE_US)
		return;
	keymap_fetched = now;

	key_map *map = NULL;
	char *chars = NULL;
	/* Talks to the input_server over a port like get_key_info(); leaves both pointers NULL on failure. */
	get_key_map(&map, &chars);
	if (map == NULL || chars == NULL) {
		free(map);
		free(chars);
		keymap_valid = false;
		return;
	}

	memset(keymap_labels, 0, sizeof(keymap_labels));
	for (int code = 1; code < 128; code++) {
		char label[OBS_HAIKU_KEY_LABEL_SIZE];

		if (!keymap_char(map->normal_map, chars, code, label))
			continue;

		/* Number row (raw keys 0x12..0x1b): layouts such as French AZERTY type the digit with Shift.
		 * The digit is what is printed on the key cap and what users call the key, so prefer it. */
		if (code >= 0x12 && code <= 0x1b && !(label[0] >= '0' && label[0] <= '9' && label[1] == 0)) {
			char shifted[OBS_HAIKU_KEY_LABEL_SIZE];
			if (keymap_char(map->shift_map, chars, code, shifted) && shifted[0] >= '0' &&
			    shifted[0] <= '9' && shifted[1] == 0)
				memcpy(label, shifted, sizeof(label));
		}

		memcpy(keymap_labels[code], label, sizeof(label));
	}

	keymap_modifier_keys[0][0] = map->left_command_key;
	keymap_modifier_keys[0][1] = map->right_command_key;
	keymap_modifier_keys[1][0] = map->left_control_key;
	keymap_modifier_keys[1][1] = map->right_control_key;
	keymap_modifier_keys[2][0] = map->left_option_key;
	keymap_modifier_keys[2][1] = map->right_option_key;
	keymap_valid = true;

	free(map);
	free(chars);
}

extern "C" bool obs_haiku_get_key_label(uint8_t code, char label[OBS_HAIKU_KEY_LABEL_SIZE])
{
	bool found = false;

	if (code == 0 || code >= 128)
		return false;

	pthread_mutex_lock(&keymap_mutex);
	refresh_keymap();
	if (keymap_valid && keymap_labels[code][0] != 0) {
		memcpy(label, keymap_labels[code], OBS_HAIKU_KEY_LABEL_SIZE);
		found = true;
	}
	pthread_mutex_unlock(&keymap_mutex);

	return found;
}

extern "C" uint32_t obs_haiku_get_modifier_raw_key(uint32_t modifier, bool right)
{
	uint32_t code = 0;
	int index = modifier == OBS_HAIKU_COMMAND_KEY   ? 0
		    : modifier == OBS_HAIKU_CONTROL_KEY ? 1
		    : modifier == OBS_HAIKU_OPTION_KEY  ? 2
							: -1;

	if (index < 0)
		return 0;

	pthread_mutex_lock(&keymap_mutex);
	refresh_keymap();
	if (keymap_valid)
		code = keymap_modifier_keys[index][right ? 1 : 0];
	pthread_mutex_unlock(&keymap_mutex);

	return code < 128 ? code : 0;
}

extern "C" bool obs_haiku_get_mouse_buttons(uint32_t *buttons)
{
	BPoint where;
	uint32 state = 0;

	/* get_mouse() goes through BPrivate::AppServerLink, which calls debugger() when there is
	 * no be_app (e.g. the hotkey thread running before/without Qt's BApplication). Report no
	 * buttons instead. */
	if (be_app == NULL)
		return false;

	if (get_mouse(&where, &state) != B_OK)
		return false;

	*buttons = state;
	return true;
}

extern "C" bool obs_haiku_reset_idle_timer(int64_t min_idle_us)
{
	/* idle_time() and get_mouse() use the app_server link, which needs be_app (see above). */
	if (be_app == NULL)
		return false;

	/* The user is active: real input already keeps the screen saver away, and not injecting anything
	 * then also rules out racing a mouse movement in progress. */
	if (idle_time() < min_idle_us)
		return true;

	/* Never wake a screen saver that is already running (it may be locked). */
	if (be_roster->IsRunning(SCREEN_BLANKER_SIGNATURE))
		return false;

	BPoint where;
	uint32 buttons = 0;
	/* The injected event carries no buttons; sending it during a (stalled) drag would look like a release. */
	if (get_mouse(&where, &buttons) != B_OK || buttons != 0)
		return false;

	/* An injected move with the cursor resting in the blank corner would start the blanker. */
	BRect frame = BScreen(B_MAIN_SCREEN_ID).Frame();
	if (!frame.IsValid())
		return false;
	const bool near_x = where.x <= frame.left + SCREEN_CORNER_MARGIN || where.x >= frame.right - SCREEN_CORNER_MARGIN;
	const bool near_y = where.y <= frame.top + SCREEN_CORNER_MARGIN || where.y >= frame.bottom - SCREEN_CORNER_MARGIN;
	if (near_x && near_y)
		return false;

	/* Enqueued as a B_MOUSE_MOVED device event ("be:set_mouse"): it passes through the input_server
	 * filters -- ScreenSaverFilter::Filter() stamps fLastEventTime for every event -- and the app_server
	 * resets its idle time. The cursor stays where it is. */
	set_mouse_position((int32)lroundf(where.x), (int32)lroundf(where.y));
	return true;
}
