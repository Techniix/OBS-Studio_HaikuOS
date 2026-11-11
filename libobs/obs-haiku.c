/******************************************************************************
    Copyright (C) 2026 by the OBS Studio Haiku port contributors

    Portions derived from obs-nix.c:
    Copyright (C) 2013 by Hugh Bailey <obs.jim@gmail.com>
    Copyright (C) 2014 by Zachary Lund <admin@computerquip.com>

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

/* Haiku platform glue for libobs: data file lookup, system information logging and global hotkeys. */

#include "obs-internal.h"
#include "obs-haiku.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/utsname.h>

#include <OS.h>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

const char *get_module_extension(void)
{
	return ".so";
}

/*
 *   <prefix>/share/obs/libobs (or the HaikuPorts data directory)
 *   ../share/obs/libobs relative to the executable (rundir / relocatable builds)
 */
char *find_libobs_data_file(const char *file)
{
	struct dstr output;
	dstr_init(&output);

	if (check_path(file, OBS_DATA_PATH "/libobs/", &output))
		return output.array;

	char *relative_data_path = os_get_executable_path_ptr("../" OBS_DATA_PATH "/libobs/");
	if (relative_data_path) {
		bool found = check_path(file, relative_data_path, &output);
		bfree(relative_data_path);

		if (found)
			return output.array;
	}

	if (OBS_INSTALL_PREFIX[0] != 0) {
		if (check_path(file, OBS_INSTALL_DATA_PATH "/libobs/", &output))
			return output.array;
	}

	dstr_free(&output);
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* System information                                                        */

static void log_processor_info(void)
{
#if defined(__i386__) || defined(__x86_64__)
	unsigned int regs[12] = {0};
	unsigned int max_ext = __get_cpuid_max(0x80000000, NULL);

	if (max_ext >= 0x80000004) {
		__get_cpuid(0x80000002, &regs[0], &regs[1], &regs[2], &regs[3]);
		__get_cpuid(0x80000003, &regs[4], &regs[5], &regs[6], &regs[7]);
		__get_cpuid(0x80000004, &regs[8], &regs[9], &regs[10], &regs[11]);

		char name[sizeof(regs) + 1];
		memcpy(name, regs, sizeof(regs));
		name[sizeof(regs)] = 0;

		struct dstr proc_name;
		dstr_init_copy(&proc_name, name);
		dstr_depad(&proc_name);
		blog(LOG_INFO, "CPU Name: %s", proc_name.array);
		dstr_free(&proc_name);
	}
#endif

	uint32 count = 0;
	if (get_cpu_topology_info(NULL, &count) == B_OK && count > 0) {
		cpu_topology_node_info *nodes = bzalloc(sizeof(*nodes) * count);
		if (get_cpu_topology_info(nodes, &count) == B_OK) {
			for (uint32 i = 0; i < count; i++) {
				if (nodes[i].type == B_TOPOLOGY_CORE) {
					blog(LOG_INFO, "CPU Speed: %" PRIu64 "MHz",
					     nodes[i].data.core.default_frequency / 1000000);
					break;
				}
			}
		}
		bfree(nodes);
	}
}

static void log_processor_cores(void)
{
	blog(LOG_INFO, "Physical Cores: %d, Logical Cores: %d", os_get_physical_cores(), os_get_logical_cores());
}

static void log_memory_info(void)
{
	system_info info;
	if (get_system_info(&info) != B_OK)
		return;

	blog(LOG_INFO, "Physical Memory: %" PRIu64 "MB Total, %" PRIu64 "MB Free",
	     (uint64_t)info.max_pages * B_PAGE_SIZE / 1024 / 1024, (uint64_t)info.free_memory / 1024 / 1024);
}

static void log_kernel_version(void)
{
	struct utsname info;
	if (uname(&info) < 0)
		return;

	/* e.g. "Haiku 1 hrev58000 Oct 1 2026" */
	blog(LOG_INFO, "Kernel Version: %s %s %s", info.sysname, info.release, info.version);
}

void log_system_info(void)
{
	log_processor_info();
	log_processor_cores();
	log_memory_info();
	log_kernel_version();
}

/* ------------------------------------------------------------------------- */
/* Hotkeys                                                                   */

/* Haiku raw key codes for a standard PC keyboard (see the "Keyboard" chapter of the Haiku Book). */
enum haiku_raw_key {
	HK_ESCAPE = 0x01,
	HK_F1 = 0x02, /* F1..F12 = 0x02..0x0d */
	HK_PRINT = 0x0e,
	HK_SCROLLLOCK = 0x0f,
	HK_PAUSE = 0x10,
	HK_GRAVE = 0x11,
	HK_1 = 0x12, /* 1..9 = 0x12..0x1a */
	HK_0 = 0x1b,
	HK_MINUS = 0x1c,
	HK_EQUAL = 0x1d,
	HK_BACKSPACE = 0x1e,
	HK_INSERT = 0x1f,
	HK_HOME = 0x20,
	HK_PAGEUP = 0x21,
	HK_NUMLOCK = 0x22,
	HK_NUMSLASH = 0x23,
	HK_NUMASTERISK = 0x24,
	HK_NUMMINUS = 0x25,
	HK_TAB = 0x26,
	HK_BRACKETLEFT = 0x31,
	HK_BRACKETRIGHT = 0x32,
	HK_BACKSLASH = 0x33,
	HK_DELETE = 0x34,
	HK_END = 0x35,
	HK_PAGEDOWN = 0x36,
	HK_NUM7 = 0x37,
	HK_NUM8 = 0x38,
	HK_NUM9 = 0x39,
	HK_NUMPLUS = 0x3a,
	HK_CAPSLOCK = 0x3b,
	HK_SEMICOLON = 0x45,
	HK_APOSTROPHE = 0x46,
	HK_RETURN = 0x47,
	HK_NUM4 = 0x48,
	HK_NUM5 = 0x49,
	HK_NUM6 = 0x4a,
	HK_LSHIFT = 0x4b,
	HK_COMMA = 0x53,
	HK_PERIOD = 0x54,
	HK_SLASH = 0x55,
	HK_RSHIFT = 0x56,
	HK_UP = 0x57,
	HK_NUM1 = 0x58,
	HK_NUM2 = 0x59,
	HK_NUM3 = 0x5a,
	HK_ENTER = 0x5b,
	/* 0x5c, 0x5d, 0x5f, 0x60, 0x66 and 0x67 are the Ctrl, Alt, AltGr, right Ctrl and Windows keys; which
	 * modifier each one is depends on the keymap, see modifier_raw_key(). */
	HK_LCONTROL = 0x5c,
	HK_LALT = 0x5d,
	HK_SPACE = 0x5e,
	HK_RALT = 0x5f,
	HK_RCONTROL = 0x60,
	HK_LEFT = 0x61,
	HK_DOWN = 0x62,
	HK_RIGHT = 0x63,
	HK_NUM0 = 0x64,
	HK_NUMPERIOD = 0x65,
	HK_LWINDOWS = 0x66,
	HK_RWINDOWS = 0x67,
	HK_MENU = 0x68,
	HK_LESS = 0x69, /* ISO "<>" key */
};

struct haiku_key_map {
	obs_key_t key;
	uint8_t code;
};

/* Raw key positions of the non-modifier keys (the US key caps name the OBS keys; see obs_key_to_str() for the
 * labels of other layouts). Modifier keys are not in this table: their positions come from the keymap. */
static const struct haiku_key_map key_map[] = {
	{OBS_KEY_ESCAPE, HK_ESCAPE},
	{OBS_KEY_F1, HK_F1 + 0},
	{OBS_KEY_F2, HK_F1 + 1},
	{OBS_KEY_F3, HK_F1 + 2},
	{OBS_KEY_F4, HK_F1 + 3},
	{OBS_KEY_F5, HK_F1 + 4},
	{OBS_KEY_F6, HK_F1 + 5},
	{OBS_KEY_F7, HK_F1 + 6},
	{OBS_KEY_F8, HK_F1 + 7},
	{OBS_KEY_F9, HK_F1 + 8},
	{OBS_KEY_F10, HK_F1 + 9},
	{OBS_KEY_F11, HK_F1 + 10},
	{OBS_KEY_F12, HK_F1 + 11},
	{OBS_KEY_PRINT, HK_PRINT},
	{OBS_KEY_SCROLLLOCK, HK_SCROLLLOCK},
	{OBS_KEY_PAUSE, HK_PAUSE},
	{OBS_KEY_QUOTELEFT, HK_GRAVE},
	{OBS_KEY_1, HK_1 + 0},
	{OBS_KEY_2, HK_1 + 1},
	{OBS_KEY_3, HK_1 + 2},
	{OBS_KEY_4, HK_1 + 3},
	{OBS_KEY_5, HK_1 + 4},
	{OBS_KEY_6, HK_1 + 5},
	{OBS_KEY_7, HK_1 + 6},
	{OBS_KEY_8, HK_1 + 7},
	{OBS_KEY_9, HK_1 + 8},
	{OBS_KEY_0, HK_0},
	{OBS_KEY_MINUS, HK_MINUS},
	{OBS_KEY_EQUAL, HK_EQUAL},
	{OBS_KEY_BACKSPACE, HK_BACKSPACE},
	{OBS_KEY_INSERT, HK_INSERT},
	{OBS_KEY_HOME, HK_HOME},
	{OBS_KEY_PAGEUP, HK_PAGEUP},
	{OBS_KEY_NUMLOCK, HK_NUMLOCK},
	{OBS_KEY_NUMSLASH, HK_NUMSLASH},
	{OBS_KEY_NUMASTERISK, HK_NUMASTERISK},
	{OBS_KEY_NUMMINUS, HK_NUMMINUS},
	{OBS_KEY_TAB, HK_TAB},
	{OBS_KEY_Q, 0x27},
	{OBS_KEY_W, 0x28},
	{OBS_KEY_E, 0x29},
	{OBS_KEY_R, 0x2a},
	{OBS_KEY_T, 0x2b},
	{OBS_KEY_Y, 0x2c},
	{OBS_KEY_U, 0x2d},
	{OBS_KEY_I, 0x2e},
	{OBS_KEY_O, 0x2f},
	{OBS_KEY_P, 0x30},
	{OBS_KEY_BRACKETLEFT, HK_BRACKETLEFT},
	{OBS_KEY_BRACKETRIGHT, HK_BRACKETRIGHT},
	{OBS_KEY_BACKSLASH, HK_BACKSLASH},
	{OBS_KEY_DELETE, HK_DELETE},
	{OBS_KEY_END, HK_END},
	{OBS_KEY_PAGEDOWN, HK_PAGEDOWN},
	{OBS_KEY_NUM7, HK_NUM7},
	{OBS_KEY_NUM8, HK_NUM8},
	{OBS_KEY_NUM9, HK_NUM9},
	{OBS_KEY_NUMPLUS, HK_NUMPLUS},
	{OBS_KEY_CAPSLOCK, HK_CAPSLOCK},
	{OBS_KEY_A, 0x3c},
	{OBS_KEY_S, 0x3d},
	{OBS_KEY_D, 0x3e},
	{OBS_KEY_F, 0x3f},
	{OBS_KEY_G, 0x40},
	{OBS_KEY_H, 0x41},
	{OBS_KEY_J, 0x42},
	{OBS_KEY_K, 0x43},
	{OBS_KEY_L, 0x44},
	{OBS_KEY_SEMICOLON, HK_SEMICOLON},
	{OBS_KEY_APOSTROPHE, HK_APOSTROPHE},
	{OBS_KEY_RETURN, HK_RETURN},
	{OBS_KEY_NUM4, HK_NUM4},
	{OBS_KEY_NUM5, HK_NUM5},
	{OBS_KEY_NUM6, HK_NUM6},
	{OBS_KEY_Z, 0x4c},
	{OBS_KEY_X, 0x4d},
	{OBS_KEY_C, 0x4e},
	{OBS_KEY_V, 0x4f},
	{OBS_KEY_B, 0x50},
	{OBS_KEY_N, 0x51},
	{OBS_KEY_M, 0x52},
	{OBS_KEY_COMMA, HK_COMMA},
	{OBS_KEY_PERIOD, HK_PERIOD},
	{OBS_KEY_SLASH, HK_SLASH},
	{OBS_KEY_UP, HK_UP},
	{OBS_KEY_NUM1, HK_NUM1},
	{OBS_KEY_NUM2, HK_NUM2},
	{OBS_KEY_NUM3, HK_NUM3},
	{OBS_KEY_ENTER, HK_ENTER},
	{OBS_KEY_SPACE, HK_SPACE},
	{OBS_KEY_LEFT, HK_LEFT},
	{OBS_KEY_DOWN, HK_DOWN},
	{OBS_KEY_RIGHT, HK_RIGHT},
	{OBS_KEY_NUM0, HK_NUM0},
	{OBS_KEY_NUMPERIOD, HK_NUMPERIOD},
	{OBS_KEY_MENU, HK_MENU},
	{OBS_KEY_LESS, HK_LESS},
};

#define KEY_MAP_COUNT (sizeof(key_map) / sizeof(key_map[0]))

/* Modifiers follow the convention of Qt's Haiku platform plugin (QHaikuSurfaceView::hostToQtModifiers()),
 * which reports B_COMMAND_KEY as Qt::ControlModifier and B_CONTROL_KEY as Qt::AltModifier, and never reports
 * B_OPTION_KEY. So OBS_KEY_CONTROL is the Haiku Command modifier, OBS_KEY_ALT the Haiku Control modifier and
 * OBS_KEY_META the Haiku Option modifier, and bindings recorded in the Qt frontend round-trip. Which physical
 * keys produce those modifiers is up to the keymap (stock Haiku: Command = Alt key, Renga and the Keymap
 * preferences' "Windows/Linux" mode: Command = Ctrl key), so their positions are read from it. */
static uint32_t obs_modifier_to_haiku(obs_key_t key)
{
	switch (key) {
	case OBS_KEY_CONTROL:
		return OBS_HAIKU_COMMAND_KEY;
	case OBS_KEY_ALT:
		return OBS_HAIKU_CONTROL_KEY;
	case OBS_KEY_META:
		return OBS_HAIKU_OPTION_KEY;
	default:
		return 0;
	}
}

/* Raw key of a modifier; falls back to the stock Haiku keymap if the keymap cannot be read. */
static int modifier_raw_key(obs_key_t key, bool right)
{
	if (key == OBS_KEY_SHIFT)
		return right ? HK_RSHIFT : HK_LSHIFT;

	uint32_t modifier = obs_modifier_to_haiku(key);
	if (!modifier)
		return 0;

	uint32_t code = obs_haiku_get_modifier_raw_key(modifier, right);
	if (code)
		return (int)code;

	switch (key) {
	case OBS_KEY_CONTROL:
		return right ? HK_RWINDOWS : HK_LALT;
	case OBS_KEY_ALT:
		return right ? HK_RCONTROL : HK_LCONTROL;
	default:
		return right ? HK_RALT : HK_LWINDOWS;
	}
}

static const obs_key_t modifier_keys[] = {OBS_KEY_SHIFT, OBS_KEY_CONTROL, OBS_KEY_ALT, OBS_KEY_META};
#define MODIFIER_KEY_COUNT (sizeof(modifier_keys) / sizeof(modifier_keys[0]))

/* Key state is an input_server round trip and mouse state an app_server round trip, so cache each
 * briefly: the hotkey thread queries the modifiers and every bound key once per tick. The two caches
 * are independent so the mouse is only polled when a mouse button binding is actually queried. */
#define KEY_STATE_CACHE_NS 4000000ULL

struct obs_hotkeys_platform {
	pthread_mutex_t mutex;
	uint64_t keys_polled;
	uint64_t mouse_polled;
	uint8_t key_states[OBS_HAIKU_KEY_STATE_BYTES];
	uint32_t modifiers;
	uint32_t mouse_buttons;
	bool keys_valid;
	bool mouse_valid;
	bool have_keys;
	bool have_mouse;
};

static int haiku_code_for_key(obs_key_t key)
{
	if (key == OBS_KEY_SHIFT || obs_modifier_to_haiku(key))
		return modifier_raw_key(key, false);

	for (size_t i = 0; i < KEY_MAP_COUNT; i++) {
		if (key_map[i].key == key)
			return key_map[i].code;
	}
	return 0;
}

static obs_key_t haiku_key_for_code(int code)
{
	if (code <= 0 || code > 0xff)
		return OBS_KEY_NONE;

	for (size_t i = 0; i < KEY_MAP_COUNT; i++) {
		if (key_map[i].code == code)
			return key_map[i].key;
	}
	for (size_t i = 0; i < MODIFIER_KEY_COUNT; i++) {
		if (modifier_raw_key(modifier_keys[i], false) == code || modifier_raw_key(modifier_keys[i], true) == code)
			return modifier_keys[i];
	}
	return OBS_KEY_NONE;
}

static inline bool key_state_bit(const uint8_t *states, uint8_t code)
{
	return (states[code >> 3] & (1 << (7 - (code & 7)))) != 0;
}

static inline bool cache_fresh(bool valid, uint64_t polled, uint64_t now)
{
	return valid && now - polled < KEY_STATE_CACHE_NS;
}

static void poll_keys(obs_hotkeys_platform_t *plat)
{
	uint64_t now = os_gettime_ns();
	if (cache_fresh(plat->keys_valid, plat->keys_polled, now))
		return;

	plat->have_keys = obs_haiku_get_key_states(plat->key_states, &plat->modifiers);
	plat->keys_polled = now;
	plat->keys_valid = true;
}

static void poll_mouse(obs_hotkeys_platform_t *plat)
{
	uint64_t now = os_gettime_ns();
	if (cache_fresh(plat->mouse_valid, plat->mouse_polled, now))
		return;

	plat->have_mouse = obs_haiku_get_mouse_buttons(&plat->mouse_buttons);
	plat->mouse_polled = now;
	plat->mouse_valid = true;
}

bool obs_hotkeys_platform_init(struct obs_core_hotkeys *hotkeys)
{
	obs_hotkeys_platform_t *plat = bzalloc(sizeof(*plat));
	if (pthread_mutex_init(&plat->mutex, NULL) != 0) {
		bfree(plat);
		return false;
	}

	hotkeys->platform_context = plat;
	return true;
}

void obs_hotkeys_platform_free(struct obs_core_hotkeys *hotkeys)
{
	obs_hotkeys_platform_t *plat = hotkeys->platform_context;
	if (!plat)
		return;

	pthread_mutex_destroy(&plat->mutex);
	bfree(plat);
	hotkeys->platform_context = NULL;
}

bool obs_hotkeys_platform_is_pressed(obs_hotkeys_platform_t *plat, obs_key_t key)
{
	bool pressed = false;

	if (!plat)
		return false;

	pthread_mutex_lock(&plat->mutex);

	if (key >= OBS_KEY_MOUSE1 && key <= OBS_KEY_MOUSE29) {
		poll_mouse(plat);
		if (plat->have_mouse) {
			uint32_t bit = 1u << (uint32_t)(key - OBS_KEY_MOUSE1);
			pressed = (plat->mouse_buttons & bit) != 0;
		}
	} else {
		poll_keys(plat);
		if (!plat->have_keys) {
			/* no key state available */
		} else if (key == OBS_KEY_SHIFT || key == OBS_KEY_CONTROL || key == OBS_KEY_ALT ||
			   key == OBS_KEY_META) {
			/* Use the modifier mask rather than physical keys: it follows the keymap (left and right
			 * keys, swapped Command/Control) exactly as the modifiers of Qt's key events do. */
			uint32_t bit = key == OBS_KEY_SHIFT ? OBS_HAIKU_SHIFT_KEY : obs_modifier_to_haiku(key);
			pressed = (plat->modifiers & bit) != 0;
		} else {
			int code = haiku_code_for_key(key);
			if (code > 0 && code < 0x80)
				pressed = key_state_bit(plat->key_states, (uint8_t)code);
		}
	}

	pthread_mutex_unlock(&plat->mutex);
	return pressed;
}

static const char *printable_key_name(obs_key_t key)
{
	switch (key) {
	case OBS_KEY_SPACE:
		return NULL; /* translated below */
	case OBS_KEY_QUOTELEFT:
		return "`";
	case OBS_KEY_MINUS:
		return "-";
	case OBS_KEY_EQUAL:
		return "=";
	case OBS_KEY_BRACKETLEFT:
		return "[";
	case OBS_KEY_BRACKETRIGHT:
		return "]";
	case OBS_KEY_BACKSLASH:
		return "\\";
	case OBS_KEY_SEMICOLON:
		return ";";
	case OBS_KEY_APOSTROPHE:
		return "'";
	case OBS_KEY_COMMA:
		return ",";
	case OBS_KEY_PERIOD:
		return ".";
	case OBS_KEY_SLASH:
		return "/";
	case OBS_KEY_LESS:
		return "<";
	default:
		return NULL;
	}
}

/* Which of the translated names "Control", "Alt" and "Super" fits the physical key the keymap assigns to an
 * OBS modifier, so that labels name the key cap: with the stock keymap, OBS "Control" (Haiku Command) is the
 * Alt key and is shown as "Alt". */
static obs_key_t modifier_label_key(obs_key_t key)
{
	switch (modifier_raw_key(key, false)) {
	case HK_LCONTROL:
	case HK_RCONTROL:
		return OBS_KEY_CONTROL;
	case HK_LALT:
	case HK_RALT:
		return OBS_KEY_ALT;
	case HK_LWINDOWS:
	case HK_RWINDOWS:
		return OBS_KEY_META;
	default:
		return key;
	}
}

/* Keys whose label is the character the user's keymap puts on them (letters, digits, punctuation). */
static bool key_has_layout_label(obs_key_t key)
{
	switch (key) {
	case OBS_KEY_QUOTELEFT:
	case OBS_KEY_MINUS:
	case OBS_KEY_EQUAL:
	case OBS_KEY_BRACKETLEFT:
	case OBS_KEY_BRACKETRIGHT:
	case OBS_KEY_BACKSLASH:
	case OBS_KEY_SEMICOLON:
	case OBS_KEY_APOSTROPHE:
	case OBS_KEY_COMMA:
	case OBS_KEY_PERIOD:
	case OBS_KEY_SLASH:
	case OBS_KEY_LESS:
		return true;
	default:
		return (key >= OBS_KEY_A && key <= OBS_KEY_Z) || (key >= OBS_KEY_0 && key <= OBS_KEY_9);
	}
}

void obs_key_to_str(obs_key_t key, struct dstr *dstr)
{
	/* OBS keys name raw key positions (US layout); show what the user's keyboard has printed there,
	 * e.g. "Q" rather than "A" for the key left of "S" on a French keyboard. */
	if (key_has_layout_label(key)) {
		int code = haiku_code_for_key(key);
		char label[OBS_HAIKU_KEY_LABEL_SIZE];
		if (code > 0 && code < 0x80 && obs_haiku_get_key_label((uint8_t)code, label)) {
			dstr_copy(dstr, label);
			return;
		}
	}

	if (key >= OBS_KEY_MOUSE1 && key <= OBS_KEY_MOUSE29) {
		if (obs->hotkeys.translations[key])
			dstr_copy(dstr, obs->hotkeys.translations[key]);
		else
			dstr_printf(dstr, "Mouse %d", (int)(key - OBS_KEY_MOUSE1 + 1));
		return;
	}

	if (key >= OBS_KEY_NUM0 && key <= OBS_KEY_NUM9) {
		if (obs->hotkeys.translations[key])
			dstr_copy(dstr, obs->hotkeys.translations[key]);
		else
			dstr_printf(dstr, "Numpad %d", (int)(key - OBS_KEY_NUM0));
		return;
	}

	if (key >= OBS_KEY_F1 && key <= OBS_KEY_F35) {
		dstr_printf(dstr, "F%d", (int)(key - OBS_KEY_F1 + 1));
		return;
	}

	if (key >= OBS_KEY_A && key <= OBS_KEY_Z) {
		dstr_printf(dstr, "%c", 'A' + (int)(key - OBS_KEY_A));
		return;
	}

	if (key >= OBS_KEY_0 && key <= OBS_KEY_9) {
		dstr_printf(dstr, "%c", '0' + (int)(key - OBS_KEY_0));
		return;
	}

	const char *printable = printable_key_name(key);
	if (printable) {
		dstr_copy(dstr, printable);
		return;
	}

#define translate_key(key, def) dstr_copy(dstr, obs_get_hotkey_translation(key, def))

	switch (key) {
	case OBS_KEY_INSERT:
		return translate_key(key, "Insert");
	case OBS_KEY_DELETE:
		return translate_key(key, "Delete");
	case OBS_KEY_HOME:
		return translate_key(key, "Home");
	case OBS_KEY_END:
		return translate_key(key, "End");
	case OBS_KEY_PAGEUP:
		return translate_key(key, "Page Up");
	case OBS_KEY_PAGEDOWN:
		return translate_key(key, "Page Down");
	case OBS_KEY_NUMLOCK:
		return translate_key(key, "Num Lock");
	case OBS_KEY_SCROLLLOCK:
		return translate_key(key, "Scroll Lock");
	case OBS_KEY_CAPSLOCK:
		return translate_key(key, "Caps Lock");
	case OBS_KEY_BACKSPACE:
		return translate_key(key, "Backspace");
	case OBS_KEY_TAB:
		return translate_key(key, "Tab");
	case OBS_KEY_PRINT:
		return translate_key(key, "Print");
	case OBS_KEY_PAUSE:
		return translate_key(key, "Pause");
	case OBS_KEY_LEFT:
		return translate_key(key, "Left");
	case OBS_KEY_RIGHT:
		return translate_key(key, "Right");
	case OBS_KEY_UP:
		return translate_key(key, "Up");
	case OBS_KEY_DOWN:
		return translate_key(key, "Down");
	case OBS_KEY_SHIFT:
		return translate_key(key, "Shift");
	case OBS_KEY_ALT:
	case OBS_KEY_CONTROL:
	case OBS_KEY_META:
		switch (modifier_label_key(key)) {
		case OBS_KEY_CONTROL:
			return translate_key(OBS_KEY_CONTROL, "Control");
		case OBS_KEY_ALT:
			return translate_key(OBS_KEY_ALT, "Alt");
		default:
			return translate_key(OBS_KEY_META, "Super");
		}
	case OBS_KEY_MENU:
		return translate_key(key, "Menu");
	case OBS_KEY_SPACE:
		return translate_key(key, "Space");
	case OBS_KEY_ESCAPE:
		return translate_key(key, "Escape");
	case OBS_KEY_RETURN:
		return translate_key(key, "Return");
	case OBS_KEY_ENTER:
		return translate_key(key, "Enter");
	case OBS_KEY_NUMASTERISK:
		return translate_key(key, "Numpad *");
	case OBS_KEY_NUMPLUS:
		return translate_key(key, "Numpad +");
	case OBS_KEY_NUMMINUS:
		return translate_key(key, "Numpad -");
	case OBS_KEY_NUMPERIOD:
		return translate_key(key, "Numpad .");
	case OBS_KEY_NUMSLASH:
		return translate_key(key, "Numpad /");
	default:
		break;
	}

#undef translate_key

	/* Fall back to the symbolic name without the OBS_KEY_ prefix. */
	const char *name = obs_key_to_name(key);
	if (name && strncmp(name, "OBS_KEY_", 8) == 0)
		name += 8;
	dstr_copy(dstr, name ? name : "");
}

/* On Haiku, "virtual keys" are Haiku raw key codes (as reported in B_KEY_DOWN's "key" field). */
obs_key_t obs_key_from_virtual_key(int code)
{
	return haiku_key_for_code(code);
}

int obs_key_to_virtual_key(obs_key_t key)
{
	return haiku_code_for_key(key);
}

static inline void add_combo_key(obs_key_t key, struct dstr *str)
{
	struct dstr key_str = {0};

	obs_key_to_str(key, &key_str);

	if (!dstr_is_empty(&key_str)) {
		if (!dstr_is_empty(str))
			dstr_cat(str, " + ");
		dstr_cat_dstr(str, &key_str);
	}

	dstr_free(&key_str);
}

void obs_key_combination_to_str(obs_key_combination_t combination, struct dstr *str)
{
	if ((combination.modifiers & INTERACT_CONTROL_KEY) != 0)
		add_combo_key(OBS_KEY_CONTROL, str);
	if ((combination.modifiers & INTERACT_COMMAND_KEY) != 0)
		add_combo_key(OBS_KEY_META, str);
	if ((combination.modifiers & INTERACT_ALT_KEY) != 0)
		add_combo_key(OBS_KEY_ALT, str);
	if ((combination.modifiers & INTERACT_SHIFT_KEY) != 0)
		add_combo_key(OBS_KEY_SHIFT, str);
	if (combination.key != OBS_KEY_NONE)
		add_combo_key(combination.key, str);
}
