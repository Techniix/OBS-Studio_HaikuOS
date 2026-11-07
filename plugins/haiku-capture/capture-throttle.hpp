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
 * BScreen::ReadBitmap() runs inside the app_server while it holds the screen lock: no window on the whole
 * desktop is drawn until the copy finishes. Reading the frame buffer back is also slow on many drivers (the
 * frame buffer is write-combined video memory; CPU reads from it can be 10-100x slower than normal memory), so a
 * full-screen read can take tens of milliseconds. Capturing at the requested rate regardless would keep the
 * app_server busy almost all the time and freeze the system.
 *
 * CaptureThrottle measures every read and schedules the next one so that reads take at most kMaxDutyCycle of
 * the wall time, lowering the effective frame rate when the hardware can't keep up, and logs when it does.
 */

#pragma once

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <stdint.h>

class CaptureThrottle {
public:
	explicit CaptureThrottle(const char *what) : what(what) {}

	/* Call right before ReadBitmap(). */
	void BeginRead() { read_start = os_gettime_ns(); }

	/* Call right after ReadBitmap(); `pixels` is the area read. */
	void EndRead(uint64_t pixels)
	{
		const uint64_t took = os_gettime_ns() - read_start;
		/* exponential average, robust against single slow reads */
		avg_read = avg_read ? (avg_read * 7 + took) / 8 : took;
		last_pixels = pixels;
	}

	/* Sleeps until the next frame is due given the requested `fps`. */
	void Wait(int fps)
	{
		const uint64_t interval = 1000000000ULL / (uint64_t)(fps > 0 ? fps : 30);
		/* a read of duration d must be followed by at least d * (1 - duty) / duty of idle time */
		const uint64_t min_gap = (uint64_t)((double)avg_read * (1.0 - kMaxDutyCycle) / kMaxDutyCycle);
		const uint64_t step = std::max(interval, avg_read + min_gap);

		const bool throttled = step > interval + interval / 10;
		if (throttled && !warned) {
			blog(LOG_WARNING,
			     "[haiku-capture] %s: reading %llu pixels from the screen takes %.1f ms; capturing at "
			     "%.1f FPS instead of %d so the desktop stays responsive",
			     what, (unsigned long long)last_pixels, avg_read / 1e6, 1e9 / (double)step, fps);
			warned = true;
		} else if (!throttled && warned) {
			blog(LOG_INFO, "[haiku-capture] %s: back to %d FPS", what, fps);
			warned = false;
		}

		next += step;
		const uint64_t now = os_gettime_ns();
		if (next < now)
			next = now; /* fell behind: don't try to catch up */
		else
			os_sleepto_ns(next);
	}

	void Reset() { next = os_gettime_ns(); }

private:
	static constexpr double kMaxDutyCycle = 0.25;

	const char *what;
	uint64_t read_start = 0;
	uint64_t avg_read = 0;
	uint64_t last_pixels = 0;
	uint64_t next = os_gettime_ns();
	bool warned = false;
};
