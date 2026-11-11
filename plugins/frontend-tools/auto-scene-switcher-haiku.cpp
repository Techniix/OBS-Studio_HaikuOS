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

/* Haiku's public API exposes running applications rather than individual window titles, so the
 * automatic scene switcher matches on application names (e.g. "WebPositive", "Terminal"). */

#include "auto-scene-switcher.hpp"

#include <algorithm>

#include <List.h>
#include <Roster.h>

using namespace std;

static bool GetAppName(team_id team, string &name)
{
	app_info info;
	if (be_roster->GetRunningAppInfo(team, &info) != B_OK)
		return false;

	/* Skip background-only servers and add-ons */
	if ((info.flags & B_BACKGROUND_APP) != 0)
		return false;

	/* ref.name is NULL for an app registered without an entry_ref */
	if (info.ref.name == NULL)
		return false;

	name = info.ref.name;
	return !name.empty();
}

void GetWindowList(vector<string> &windows)
{
	windows.clear();

	BList teams;
	be_roster->GetAppList(&teams);

	for (int32 i = 0; i < teams.CountItems(); i++) {
		team_id team = (team_id)(addr_t)teams.ItemAt(i);
		string name;
		if (GetAppName(team, name) && find(windows.begin(), windows.end(), name) == windows.end())
			windows.emplace_back(std::move(name));
	}
}

void GetCurrentWindowTitle(string &title)
{
	title.clear();

	app_info info;
	if (be_roster->GetActiveAppInfo(&info) == B_OK && info.ref.name)
		title = info.ref.name;
}

void CleanupSceneSwitcher() {}
