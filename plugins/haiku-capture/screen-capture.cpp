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

/*
 * Screen Capture: the whole main screen, or a rectangle of it, through Renga's capture API
 * (BScreenCapture::StartScreen()/StartRegion(); see capture-source.hpp for how frames travel).
 *
 * With a region the app_server copies and reports changes for that rectangle only, and libobs copies frames of
 * that size: cheaper than capturing the whole screen and cropping the source.
 */

#include "capture-source.hpp"

#include <algorithm>

#include <Screen.h>

#define S_MODE "mode"
#define S_REGION_X "region_x"
#define S_REGION_Y "region_y"
#define S_REGION_WIDTH "region_width"
#define S_REGION_HEIGHT "region_height"

namespace {

enum capture_mode {
	MODE_SCREEN = 0,
	MODE_REGION = 1,
};

struct Region {
	int mode = MODE_SCREEN;
	int x = 0;
	int y = 0;
	int width = 1280;
	int height = 720;

	bool operator==(const Region &other) const
	{
		return mode == other.mode && x == other.x && y == other.y && width == other.width &&
		       height == other.height;
	}
};

class ScreenCapture : public CaptureSource {
public:
	explicit ScreenCapture(obs_source_t *source) : CaptureSource(source, "Screen capture") {}

	void Update(obs_data_t *settings)
	{
		Region wanted;
		wanted.mode = (int)obs_data_get_int(settings, S_MODE);
		wanted.x = (int)obs_data_get_int(settings, S_REGION_X);
		wanted.y = (int)obs_data_get_int(settings, S_REGION_Y);
		wanted.width = std::max(1, (int)obs_data_get_int(settings, S_REGION_WIDTH));
		wanted.height = std::max(1, (int)obs_data_get_int(settings, S_REGION_HEIGHT));

		{
			std::lock_guard<std::mutex> lock(settings_mutex);
			/* A session's source is fixed when it starts */
			if (!(wanted == region)) {
				RequestRestart();
			}
			region = wanted;
		}
		UpdateCommon(settings);
	}

protected:
	status_t StartSession(BScreenCapture &capture) override
	{
		Region current;
		{
			std::lock_guard<std::mutex> lock(settings_mutex);
			current = region;
		}

		if (current.mode != MODE_REGION) {
			return capture.StartScreen(CommonFlags(), 0);
		}

		/* The app_server clips the rectangle to the screen (ScreenCaptureManager::_SourceFrame()) and
		 * answers B_ENTRY_NOT_FOUND if nothing is left; Frame() is the clipped rectangle. */
		const BRect rect(current.x, current.y, current.x + current.width - 1, current.y + current.height - 1);
		return capture.StartRegion(rect, CommonFlags(), 0);
	}

	const char *MissingText() const override { return T_("Status.RegionOffScreen"); }

private:
	std::mutex settings_mutex;
	Region region;
};

const char *screen_capture_get_name(void *)
{
	return T_("ScreenCapture");
}

void screen_capture_update(void *data, obs_data_t *settings)
{
	static_cast<ScreenCapture *>(data)->Update(settings);
}

void *screen_capture_create(obs_data_t *settings, obs_source_t *source)
{
	ScreenCapture *sc = new ScreenCapture(source);
	sc->Update(settings);
	return sc;
}

void screen_capture_destroy(void *data)
{
	ScreenCapture *sc = static_cast<ScreenCapture *>(data);
	sc->Hide();
	delete sc;
}

void screen_capture_show(void *data)
{
	static_cast<ScreenCapture *>(data)->Show();
}

void screen_capture_hide(void *data)
{
	static_cast<ScreenCapture *>(data)->Hide();
}

void screen_capture_defaults(obs_data_t *settings)
{
	const Region defaults;
	obs_data_set_default_int(settings, S_MODE, defaults.mode);
	obs_data_set_default_int(settings, S_REGION_X, defaults.x);
	obs_data_set_default_int(settings, S_REGION_Y, defaults.y);
	obs_data_set_default_int(settings, S_REGION_WIDTH, defaults.width);
	obs_data_set_default_int(settings, S_REGION_HEIGHT, defaults.height);
	CaptureSource::CommonDefaults(settings);
}

bool mode_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool region = obs_data_get_int(settings, S_MODE) == MODE_REGION;
	for (const char *name : {S_REGION_X, S_REGION_Y, S_REGION_WIDTH, S_REGION_HEIGHT}) {
		obs_property_set_visible(obs_properties_get(props, name), region);
	}
	return true;
}

obs_properties_t *screen_capture_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	CaptureSource::AddStatusProperties(props, static_cast<ScreenCapture *>(data));

	obs_property_t *mode =
		obs_properties_add_list(props, S_MODE, T_("Capture"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, T_("Capture.Screen"), MODE_SCREEN);
	obs_property_list_add_int(mode, T_("Capture.Region"), MODE_REGION);
	obs_property_set_modified_callback(mode, mode_changed);

	/* Limit the rectangle to the main screen as it is now (the only screen the capture API has, until
	 * multi-monitor support, F-38). A rectangle that reaches outside after a mode change is clipped by the
	 * app_server. */
	int screen_width = 16384;
	int screen_height = 16384;
	{
		BScreen screen(B_MAIN_SCREEN_ID);
		if (screen.IsValid()) {
			screen_width = screen.Frame().IntegerWidth() + 1;
			screen_height = screen.Frame().IntegerHeight() + 1;
		}
	}
	obs_properties_add_int(props, S_REGION_X, T_("Region.X"), 0, screen_width - 1, 1);
	obs_properties_add_int(props, S_REGION_Y, T_("Region.Y"), 0, screen_height - 1, 1);
	obs_properties_add_int(props, S_REGION_WIDTH, T_("Region.Width"), 1, screen_width, 1);
	obs_properties_add_int(props, S_REGION_HEIGHT, T_("Region.Height"), 1, screen_height, 1);

	CaptureSource::AddCommonProperties(props);

	return props;
}

} // namespace

void haiku_register_screen_capture(void)
{
	struct obs_source_info info = {};
	info.id = "haiku_screen_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = screen_capture_get_name;
	info.create = screen_capture_create;
	info.destroy = screen_capture_destroy;
	info.update = screen_capture_update;
	info.get_defaults = screen_capture_defaults;
	info.get_properties = screen_capture_properties;
	info.show = screen_capture_show;
	info.hide = screen_capture_hide;
	info.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE;

	obs_register_source(&info);
}
