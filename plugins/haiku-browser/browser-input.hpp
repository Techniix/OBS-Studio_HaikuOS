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

/* Translation of OBS / Qt input to what HaikuWebKit expects in B_KEY_DOWN / B_MOUSE_* messages. */

#pragma once

#include <InterfaceDefs.h>
#include <View.h> /* B_PRIMARY_MOUSE_BUTTON... */

#include <stdint.h>
#include <string>

/* Haiku raw key codes of the keys whose "bytes" are control codes rather than text */
enum HaikuRawKey : int32_t {
	HAIKU_RAW_ESCAPE = 0x01,
	HAIKU_RAW_F1 = 0x02, /* .. F12 = 0x0d */
	HAIKU_RAW_F12 = 0x0d,
	HAIKU_RAW_BACKSPACE = 0x1e,
	HAIKU_RAW_INSERT = 0x1f,
	HAIKU_RAW_HOME = 0x20,
	HAIKU_RAW_PAGE_UP = 0x21,
	HAIKU_RAW_TAB = 0x26,
	HAIKU_RAW_DELETE = 0x34,
	HAIKU_RAW_END = 0x35,
	HAIKU_RAW_PAGE_DOWN = 0x36,
	HAIKU_RAW_ENTER = 0x47,
	HAIKU_RAW_UP = 0x57,
	HAIKU_RAW_KP_ENTER = 0x5b,
	HAIKU_RAW_LEFT = 0x61,
	HAIKU_RAW_DOWN = 0x62,
	HAIKU_RAW_RIGHT = 0x63,
};

/* The "bytes" Haiku puts in a key message: the control byte for navigation/editing keys, else the text. */
static inline std::string haiku_key_bytes(int32_t raw_key, const char *text)
{
	char control = 0;
	switch (raw_key) {
	case HAIKU_RAW_ESCAPE:
		control = B_ESCAPE;
		break;
	case HAIKU_RAW_BACKSPACE:
		control = B_BACKSPACE;
		break;
	case HAIKU_RAW_INSERT:
		control = B_INSERT;
		break;
	case HAIKU_RAW_HOME:
		control = B_HOME;
		break;
	case HAIKU_RAW_PAGE_UP:
		control = B_PAGE_UP;
		break;
	case HAIKU_RAW_TAB:
		control = B_TAB;
		break;
	case HAIKU_RAW_DELETE:
		control = B_DELETE;
		break;
	case HAIKU_RAW_END:
		control = B_END;
		break;
	case HAIKU_RAW_PAGE_DOWN:
		control = B_PAGE_DOWN;
		break;
	case HAIKU_RAW_ENTER:
	case HAIKU_RAW_KP_ENTER:
		control = B_ENTER;
		break;
	case HAIKU_RAW_UP:
		control = B_UP_ARROW;
		break;
	case HAIKU_RAW_LEFT:
		control = B_LEFT_ARROW;
		break;
	case HAIKU_RAW_DOWN:
		control = B_DOWN_ARROW;
		break;
	case HAIKU_RAW_RIGHT:
		control = B_RIGHT_ARROW;
		break;
	default:
		if (raw_key >= HAIKU_RAW_F1 && raw_key <= HAIKU_RAW_F12)
			control = B_FUNCTION_KEY;
		break;
	}
	if (control)
		return std::string(1, control);

	if (!text || !*text)
		return std::string();

	/* Qt reports Return as "\r"; Haiku uses "\n" (B_ENTER) */
	if (text[0] == '\r' && !text[1])
		return std::string(1, (char)B_ENTER);
	return std::string(text);
}

/* OBS interaction modifiers -> Haiku modifier bits. On Haiku the key Qt calls Control is B_COMMAND_KEY and
 * the one Qt calls Alt is B_CONTROL_KEY (see the Haiku QPA and libobs/obs-haiku.c); WebCore maps them back the
 * same way. */
static inline int32_t haiku_modifiers_from_interact(uint32_t mods)
{
	int32_t out = 0;
	if (mods & (1u << 1)) /* INTERACT_SHIFT_KEY */
		out |= B_SHIFT_KEY;
	if (mods & (1u << 2)) /* INTERACT_CONTROL_KEY */
		out |= B_COMMAND_KEY;
	if (mods & (1u << 3)) /* INTERACT_ALT_KEY */
		out |= B_CONTROL_KEY;
	if (mods & (1u << 7)) /* INTERACT_COMMAND_KEY */
		out |= B_OPTION_KEY;
	if (mods & 1u) /* INTERACT_CAPS_KEY */
		out |= B_CAPS_LOCK;
	return out;
}
