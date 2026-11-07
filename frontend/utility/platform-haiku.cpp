/******************************************************************************
    Copyright (C) 2026 by the OBS Studio Haiku port contributors

    Portions derived from platform-x11.cpp:
    Copyright (C) 2013 by Hugh Bailey <obs.jim@gmail.com>

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

#include "platform.hpp"

#include <OBSApp.hpp>

#include <obs-config.h>
#include <obs-hotkey.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QKeyEvent>
#include <QWidget>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <clocale>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <FindDirectory.h>
#include <OS.h>
#include <LocaleRoster.h>
#include <Message.h>
#include <String.h>

using std::ostringstream;
using std::string;
using std::vector;

/* Single-instance detection: hold an exclusive flock() on a file in the user's settings directory for
 * the lifetime of the process. The lock is released by the kernel when the process exits or crashes. */
void CheckIfAlreadyRunning(bool &already_running)
{
	static int lock_fd = -1;

	already_running = false;

	if (lock_fd != -1)
		return;

	char settings[B_PATH_NAME_LENGTH];
	string lock_path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, true, settings, sizeof(settings)) == B_OK)
		lock_path = string(settings) + "/obs-studio.lock";
	else
		lock_path = "/tmp/obs-studio.lock";

	int fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd == -1) {
		blog(LOG_ERROR, "Failed to check for running instance, open(%s): %d", lock_path.c_str(), errno);
		return;
	}

	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		already_running = errno == EWOULDBLOCK;
		close(fd);
		return;
	}

	lock_fd = fd;
}

static inline bool check_path(const char *data, const char *path, string &output)
{
	ostringstream str;
	str << path << data;
	output = str.str();

	blog(LOG_DEBUG, "Attempted path: %s", output.c_str());

	return (access(output.c_str(), R_OK) == 0);
}

#define INSTALL_DATA_PATH OBS_INSTALL_PREFIX "/" OBS_DATA_PATH "/obs-studio/"

bool GetDataFilePath(const char *data, string &output)
{
	char *data_path = getenv("OBS_DATA_PATH");
	if (data_path != NULL) {
		if (check_path(data, data_path, output))
			return true;
	}

	char *relative_data_path = os_get_executable_path_ptr("../" OBS_DATA_PATH "/obs-studio/");

	if (relative_data_path) {
		bool result = check_path(data, relative_data_path, output);
		bfree(relative_data_path);

		if (result)
			return true;
	}

	if (check_path(data, OBS_DATA_PATH "/obs-studio/", output))
		return true;

	if (check_path(data, INSTALL_DATA_PATH, output))
		return true;

	return false;
}

string GetDefaultVideoSavePath()
{
	char path[B_PATH_NAME_LENGTH];

	/* Haiku has no standard "Videos" folder; recordings go to the home directory like on Linux. */
	if (find_directory(B_USER_DIRECTORY, -1, false, path, sizeof(path)) == B_OK)
		return string(path);

	const char *home = getenv("HOME");
	return string(home ? home : "/boot/home");
}

static vector<string> MatchLocale(string language)
{
	vector<string> matched;

	/* Haiku reports e.g. "en", "de", "pt_BR"; OBS locale names look like "pt-BR". */
	for (char &c : language) {
		if (c == '_')
			c = '-';
	}

	for (auto &locale_pair : GetLocaleNames()) {
		auto &locale = locale_pair.first;
		if (locale == language.substr(0, locale.size()))
			return {locale};

		if (language.size() >= 2 && locale.substr(0, 2) == language.substr(0, 2))
			matched.push_back(locale);
	}

	return matched;
}

vector<string> GetPreferredLocales()
{
	BMessage languages;
	if (BLocaleRoster::Default()->GetPreferredLanguages(&languages) == B_OK) {
		BString language;
		for (int32 i = 0; languages.FindString("language", i, &language) == B_OK; i++) {
			vector<string> matched = MatchLocale(language.String());
			if (!matched.empty())
				return matched;
		}
	}

	const char *messages = setlocale(LC_MESSAGES, NULL);
	if (!messages || !*messages || strcmp(messages, "C") == 0 || strcmp(messages, "POSIX") == 0)
		return {};

	return MatchLocale(messages);
}

bool IsAlwaysOnTop(QWidget *window)
{
	return (window->windowFlags() & Qt::WindowStaysOnTopHint) != 0;
}

void SetAlwaysOnTop(QWidget *window, bool enable)
{
	Qt::WindowFlags flags = window->windowFlags();

	if (enable)
		flags |= Qt::WindowStaysOnTopHint;
	else
		flags &= ~Qt::WindowStaysOnTopHint;

	window->setWindowFlags(flags);
	window->show();
}

bool SetDisplayAffinitySupported(void)
{
	return false;
}

void TaskbarOverlayInit() {}
void TaskbarOverlaySetStatus(TaskbarOverlayStatus) {}

bool HighContrastEnabled()
{
	return false;
}

uint32_t HaikuGetCpuFrequencyMHz()
{
	uint32 count = 0;
	if (get_cpu_topology_info(nullptr, &count) != B_OK || count == 0)
		return 0;

	vector<cpu_topology_node_info> nodes(count);
	if (get_cpu_topology_info(nodes.data(), &count) != B_OK)
		return 0;

	for (uint32 i = 0; i < count; i++) {
		if (nodes[i].type == B_TOPOLOGY_CORE)
			return (uint32_t)(nodes[i].data.core.default_frequency / 1000000);
	}

	return 0;
}

/* Qt's Haiku platform plugin does not report Haiku raw key codes as native virtual keys, so hotkeys are
 * mapped from Qt::Key instead. Shifted symbols are folded back onto the physical key that produced them
 * (US layout) so that the result can be polled with get_key_info() in libobs. */
obs_key_t HaikuObsKeyFromQtKeyEvent(const QKeyEvent *event)
{
	const int key = event->key();
	const bool keypad = (event->modifiers() & Qt::KeypadModifier) != 0;

	if (key >= Qt::Key_A && key <= Qt::Key_Z)
		return (obs_key_t)(OBS_KEY_A + (key - Qt::Key_A));

	if (key >= Qt::Key_0 && key <= Qt::Key_9)
		return (obs_key_t)((keypad ? OBS_KEY_NUM0 : OBS_KEY_0) + (key - Qt::Key_0));

	if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
		return (obs_key_t)(OBS_KEY_F1 + (key - Qt::Key_F1));

	switch (key) {
	case Qt::Key_Escape:
		return OBS_KEY_ESCAPE;
	case Qt::Key_Tab:
	case Qt::Key_Backtab:
		return OBS_KEY_TAB;
	case Qt::Key_Backspace:
		return OBS_KEY_BACKSPACE;
	case Qt::Key_Return:
		return OBS_KEY_RETURN;
	case Qt::Key_Enter:
		return keypad ? OBS_KEY_ENTER : OBS_KEY_RETURN;
	case Qt::Key_Insert:
		return OBS_KEY_INSERT;
	case Qt::Key_Delete:
		return OBS_KEY_DELETE;
	case Qt::Key_Pause:
		return OBS_KEY_PAUSE;
	case Qt::Key_Print:
	case Qt::Key_SysReq:
		return OBS_KEY_PRINT;
	case Qt::Key_Home:
		return OBS_KEY_HOME;
	case Qt::Key_End:
		return OBS_KEY_END;
	case Qt::Key_Left:
		return OBS_KEY_LEFT;
	case Qt::Key_Up:
		return OBS_KEY_UP;
	case Qt::Key_Right:
		return OBS_KEY_RIGHT;
	case Qt::Key_Down:
		return OBS_KEY_DOWN;
	case Qt::Key_PageUp:
		return OBS_KEY_PAGEUP;
	case Qt::Key_PageDown:
		return OBS_KEY_PAGEDOWN;
	case Qt::Key_CapsLock:
		return OBS_KEY_CAPSLOCK;
	case Qt::Key_NumLock:
		return OBS_KEY_NUMLOCK;
	case Qt::Key_ScrollLock:
		return OBS_KEY_SCROLLLOCK;
	case Qt::Key_Menu:
		return OBS_KEY_MENU;
	case Qt::Key_Space:
		return OBS_KEY_SPACE;

	case Qt::Key_Asterisk:
		return keypad ? OBS_KEY_NUMASTERISK : OBS_KEY_8;
	case Qt::Key_Plus:
		return keypad ? OBS_KEY_NUMPLUS : OBS_KEY_EQUAL;
	case Qt::Key_Minus:
		return keypad ? OBS_KEY_NUMMINUS : OBS_KEY_MINUS;
	case Qt::Key_Slash:
		return keypad ? OBS_KEY_NUMSLASH : OBS_KEY_SLASH;
	case Qt::Key_Period:
		return keypad ? OBS_KEY_NUMPERIOD : OBS_KEY_PERIOD;
	case Qt::Key_Comma:
		return keypad ? OBS_KEY_NUMPERIOD : OBS_KEY_COMMA;

	case Qt::Key_Exclam:
		return OBS_KEY_1;
	case Qt::Key_At:
		return OBS_KEY_2;
	case Qt::Key_NumberSign:
		return OBS_KEY_3;
	case Qt::Key_Dollar:
		return OBS_KEY_4;
	case Qt::Key_Percent:
		return OBS_KEY_5;
	case Qt::Key_AsciiCircum:
		return OBS_KEY_6;
	case Qt::Key_Ampersand:
		return OBS_KEY_7;
	case Qt::Key_ParenLeft:
		return OBS_KEY_9;
	case Qt::Key_ParenRight:
		return OBS_KEY_0;
	case Qt::Key_Underscore:
		return OBS_KEY_MINUS;
	case Qt::Key_Equal:
		return OBS_KEY_EQUAL;
	case Qt::Key_BracketLeft:
	case Qt::Key_BraceLeft:
		return OBS_KEY_BRACKETLEFT;
	case Qt::Key_BracketRight:
	case Qt::Key_BraceRight:
		return OBS_KEY_BRACKETRIGHT;
	case Qt::Key_Backslash:
	case Qt::Key_Bar:
		return OBS_KEY_BACKSLASH;
	case Qt::Key_Semicolon:
	case Qt::Key_Colon:
		return OBS_KEY_SEMICOLON;
	case Qt::Key_Apostrophe:
	case Qt::Key_QuoteDbl:
		return OBS_KEY_APOSTROPHE;
	case Qt::Key_QuoteLeft:
	case Qt::Key_AsciiTilde:
		return OBS_KEY_QUOTELEFT;
	case Qt::Key_Less:
		/* Shift+comma on US layouts, otherwise the ISO "<>" key */
		return (event->modifiers() & Qt::ShiftModifier) ? OBS_KEY_COMMA : OBS_KEY_LESS;
	case Qt::Key_Greater:
		return OBS_KEY_PERIOD;
	case Qt::Key_Question:
		return OBS_KEY_SLASH;
	default:
		break;
	}

	/* Last resort: the platform plugin may still report a Haiku raw key code. */
	return obs_key_from_virtual_key((int)event->nativeVirtualKey());
}
