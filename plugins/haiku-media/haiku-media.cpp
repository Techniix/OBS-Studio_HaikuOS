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
OBS_MODULE_USE_DEFAULT_LOCALE("haiku-media", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Media Kit audio and video capture for Renga: sound card inputs, cameras, desktop audio and "
	       "per-application audio through the system mixer's loopback";
}

extern void haiku_register_audio_input(void);
extern void haiku_register_video_input(void);
extern void haiku_register_desktop_audio(void);
extern void haiku_register_application_audio(void);

bool obs_module_load(void)
{
	haiku_register_audio_input();
	haiku_register_video_input();
	haiku_register_desktop_audio();
	haiku_register_application_audio();
	return true;
}
