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

#include <Application.h>
#include <InterfaceDefs.h>
#include <Point.h>

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
