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

#include <string.h>

#include <math.h>

#include <Application.h>
#include <InterfaceDefs.h>
#include <Point.h>
#include <Roster.h>
#include <Screen.h>
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
