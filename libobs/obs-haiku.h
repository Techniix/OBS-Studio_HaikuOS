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

#pragma once

/* Internal helpers implemented with the Haiku C++ API (obs-haiku-be.cpp) for use by obs-haiku.c. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Haiku raw key codes are 8 bits wide; key_info.key_states is a 128-bit bitmap. */
#define OBS_HAIKU_KEY_STATE_BYTES 16

/* Global keyboard state from the app_server, indexed by Haiku raw key code. */
bool obs_haiku_get_key_states(uint8_t states[OBS_HAIKU_KEY_STATE_BYTES]);

/* Global mouse button bitmask (B_PRIMARY_MOUSE_BUTTON = 1, B_SECONDARY = 2, B_TERTIARY = 4, ...). */
bool obs_haiku_get_mouse_buttons(uint32_t *buttons);

#ifdef __cplusplus
}
#endif
