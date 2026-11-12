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
OBS_MODULE_USE_DEFAULT_LOCALE("haiku-capture", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Screen and window capture through Renga's capture API";
}

extern void haiku_register_screen_capture(void);
extern void haiku_register_window_capture(void);
extern void capture_permission_shutdown(void);

bool obs_module_load(void)
{
	haiku_register_screen_capture();
	haiku_register_window_capture();
	return true;
}

/*
 * Quits and joins the looper that receives the screen recording permission answers (capture-source.cpp,
 * BScreenCapture::RequestPermission(), F-58 / O-7). libobs calls this from obs_shutdown() (free_module()), which
 * the frontend runs in OBSApp's destructor body (applicationShutdown()), that is before ~QApplication and so
 * while be_app, which every BScreenCapture call needs, still exists. Sources that libobs destroys after this
 * (obs_free_data() runs after the modules are freed) don't ask again.
 */
void obs_module_unload(void)
{
	capture_permission_shutdown();
}
