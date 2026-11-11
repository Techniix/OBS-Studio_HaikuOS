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

/* Haiku modifier bits (subset of InterfaceDefs.h, so C code doesn't need the C++ headers) */
#define OBS_HAIKU_SHIFT_KEY 0x00000001   /* B_SHIFT_KEY */
#define OBS_HAIKU_COMMAND_KEY 0x00000002 /* B_COMMAND_KEY: the Alt key on PC keyboards by default */
#define OBS_HAIKU_CONTROL_KEY 0x00000004 /* B_CONTROL_KEY */
#define OBS_HAIKU_OPTION_KEY 0x00000040  /* B_OPTION_KEY: the Windows key by default */

/* Global keyboard state from the input_server: key bitmap indexed by Haiku raw key code, and the current
 * modifier mask (which reflects the user's Keymap preferences, e.g. swapped Ctrl/Alt). */
bool obs_haiku_get_key_states(uint8_t states[OBS_HAIKU_KEY_STATE_BYTES], uint32_t *modifiers);

/* Size of a key label buffer: one character of the keymap in UTF-8 (the keymap allows a few bytes more for
 * keys that type several characters; those keep their built-in US name), plus the terminating NUL. */
#define OBS_HAIKU_KEY_LABEL_SIZE 8

/* The character the key with the given Haiku raw key code types in the user's current keymap without
 * modifiers, upper-cased, as UTF-8 -- i.e. what is printed on that key of the user's keyboard ("Q" for raw
 * key 0x3c with a French layout, where a US keyboard has "A"). Number row keys report their digit even on
 * layouts that need Shift for it. Returns false if the key types nothing printable or the keymap cannot be
 * read; callers then fall back to the US name. Safe to call from any thread, no BApplication needed. */
bool obs_haiku_get_key_label(uint8_t code, char label[OBS_HAIKU_KEY_LABEL_SIZE]);

/* Raw key code of the (left) key the user's keymap assigns to a modifier; `modifier` is one of
 * OBS_HAIKU_COMMAND_KEY, OBS_HAIKU_CONTROL_KEY or OBS_HAIKU_OPTION_KEY. Stock Haiku has Command on the Alt
 * key (0x5d) and Control on the Ctrl key (0x5c); the Keymap preferences ("Windows/Linux" shortcut mode) and
 * Renga's default swap the two. Returns 0 if the keymap cannot be read. */
uint32_t obs_haiku_get_modifier_raw_key(uint32_t modifier);

/* Global mouse button bitmask (B_PRIMARY_MOUSE_BUTTON = 1, B_SECONDARY = 2, B_TERTIARY = 4, ...) from the
 * app_server. Returns false without a BApplication (the app_server link would call debugger()). */
bool obs_haiku_get_mouse_buttons(uint32_t *buttons);

/* Keeps Haiku's screen saver (and with it the DPMS power-down, which only the screen blanker triggers)
 * from starting while OBS records or streams. If there was no user input for at least min_idle_us
 * microseconds, re-sends a mouse-moved event at the cursor's current position through the input_server
 * (public set_mouse_position()), which resets the screen saver filter's idle timer just like real input.
 * Nothing persistent is changed, so there is nothing to restore after a crash. Does nothing (returns
 * false) without a BApplication, while a mouse button is held, while the cursor rests near a screen corner
 * (the screen saver's "blank now" corner) or while the screen blanker is already running (so a running or
 * locked screen saver is never dismissed). */
bool obs_haiku_reset_idle_timer(int64_t min_idle_us);

#ifdef __cplusplus
}
#endif
