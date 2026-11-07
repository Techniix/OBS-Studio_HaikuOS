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
	HK_LCONTROL = 0x5c,
	HK_LCOMMAND = 0x5d, /* left Alt on PC keyboards */
	HK_SPACE = 0x5e,
	HK_RCOMMAND = 0x5f, /* right Alt on PC keyboards */
	HK_RCONTROL = 0x60,
	HK_LEFT = 0x61,
	HK_DOWN = 0x62,
	HK_RIGHT = 0x63,
	HK_NUM0 = 0x64,
	HK_NUMPERIOD = 0x65,
	HK_LOPTION = 0x66, /* left Windows key */
	HK_ROPTION = 0x67, /* right Windows key */
	HK_MENU = 0x68,
	HK_LESS = 0x69, /* ISO "<>" key */
};

struct haiku_key_map {
	obs_key_t key;
	uint8_t code;
};

/* Primary mapping; for keys with a left and right variant, the second code is in key_alt_codes. */
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
	{OBS_KEY_SHIFT, HK_LSHIFT},
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
	{OBS_KEY_CONTROL, HK_LCONTROL},
	{OBS_KEY_ALT, HK_LCOMMAND},
	{OBS_KEY_SPACE, HK_SPACE},
	{OBS_KEY_LEFT, HK_LEFT},
	{OBS_KEY_DOWN, HK_DOWN},
	{OBS_KEY_RIGHT, HK_RIGHT},
	{OBS_KEY_NUM0, HK_NUM0},
	{OBS_KEY_NUMPERIOD, HK_NUMPERIOD},
	{OBS_KEY_META, HK_LOPTION},
	{OBS_KEY_MENU, HK_MENU},
	{OBS_KEY_LESS, HK_LESS},
};

/* Right-hand modifier keys that should also count as the generic modifier being held. */
static const struct haiku_key_map key_alt_codes[] = {
	{OBS_KEY_SHIFT, HK_RSHIFT},
	{OBS_KEY_CONTROL, HK_RCONTROL},
	{OBS_KEY_ALT, HK_RCOMMAND},
	{OBS_KEY_META, HK_ROPTION},
};

#define KEY_MAP_COUNT (sizeof(key_map) / sizeof(key_map[0]))
#define KEY_ALT_COUNT (sizeof(key_alt_codes) / sizeof(key_alt_codes[0]))

/* Polling the app_server is a round trip, so cache the key/mouse state briefly: the hotkey thread
 * queries every registered key once per tick. */
#define KEY_STATE_CACHE_NS 4000000ULL

struct obs_hotkeys_platform {
	pthread_mutex_t mutex;
	uint64_t last_poll;
	uint8_t key_states[OBS_HAIKU_KEY_STATE_BYTES];
	uint32_t mouse_buttons;
	bool have_keys;
	bool have_mouse;
};

static int haiku_code_for_key(obs_key_t key)
{
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
	for (size_t i = 0; i < KEY_ALT_COUNT; i++) {
		if (key_alt_codes[i].code == code)
			return key_alt_codes[i].key;
	}
	return OBS_KEY_NONE;
}

static inline bool key_state_bit(const uint8_t *states, uint8_t code)
{
	return (states[code >> 3] & (1 << (7 - (code & 7)))) != 0;
}

static void poll_state(obs_hotkeys_platform_t *plat)
{
	uint64_t now = os_gettime_ns();
	if (plat->last_poll && now - plat->last_poll < KEY_STATE_CACHE_NS)
		return;

	plat->have_keys = obs_haiku_get_key_states(plat->key_states);
	plat->have_mouse = obs_haiku_get_mouse_buttons(&plat->mouse_buttons);
	plat->last_poll = now;
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
	poll_state(plat);

	if (key >= OBS_KEY_MOUSE1 && key <= OBS_KEY_MOUSE29) {
		if (plat->have_mouse) {
			uint32_t bit = 1u << (uint32_t)(key - OBS_KEY_MOUSE1);
			pressed = (plat->mouse_buttons & bit) != 0;
		}
	} else if (plat->have_keys) {
		int code = haiku_code_for_key(key);
		if (code)
			pressed = key_state_bit(plat->key_states, (uint8_t)code);

		for (size_t i = 0; !pressed && i < KEY_ALT_COUNT; i++) {
			if (key_alt_codes[i].key == key)
				pressed = key_state_bit(plat->key_states, key_alt_codes[i].code);
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

void obs_key_to_str(obs_key_t key, struct dstr *dstr)
{
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
		return translate_key(key, "Alt");
	case OBS_KEY_CONTROL:
		return translate_key(key, "Control");
	case OBS_KEY_META:
		return translate_key(key, "Super");
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
