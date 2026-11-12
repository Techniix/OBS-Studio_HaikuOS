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

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-browser", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Browser source and browser docks for Renga (HaikuWebKit)";
}

extern void haiku_register_browser_source(void);
extern void haiku_browser_shutdown(void);
extern void haiku_browser_audio_shutdown(void);

bool obs_module_load(void)
{
	haiku_register_browser_source();
	return true;
}

/* Sources and docks are gone by now; helpers they told to quit may still be flushing WebKit's storage, and the
 * audio threads of rerouted sources may still be releasing their loopbacks. Those threads have to finish
 * before this module's code is unloaded. */
void obs_module_unload(void)
{
	haiku_browser_audio_shutdown();
	haiku_browser_shutdown();
}
