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
 * Two measures keep the desktop usable:
 *  - BandedScreenReader reads the area in horizontal bands sized so each ReadBitmap() holds the app_server for
 *    only ~2 ms, and yields between bands. The software cursor is moved by the same app_server lock, so a
 *    full-frame read made the pointer stop for the whole copy, in a regular rhythm.
 *  - CaptureThrottle keeps the total time spent reading to at most kMaxDutyCycle of the wall time, lowering the
 *    effective frame rate when the hardware can't keep up, and logs when it does.
 */

#pragma once

#include <obs-module.h>
#include <util/platform.h>

#include <Bitmap.h>
#include <OS.h>
#include <Screen.h>

#include <algorithm>
#include <memory>
#include <stdint.h>
#include <string.h>
#include <vector>

class CaptureThrottle {
public:
	explicit CaptureThrottle(const char *what) : what(what) {}

	/* Time spent inside ReadBitmap() for one frame of `pixels` pixels. */
	void Record(uint64_t took, uint64_t pixels)
	{
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
	uint64_t avg_read = 0;
	uint64_t last_pixels = 0;
	uint64_t next = os_gettime_ns();
	bool warned = false;
};

class BandedScreenReader {
public:
	/* Reads `rect` (screen coordinates, integral) into `frame` as BGRX rows of `stride` bytes. Returns the
	 * time spent inside the app_server, or 0 on failure. */
	uint64_t Read(BScreen &screen, BRect rect, bool cursor, std::vector<uint8_t> &frame, uint32_t &stride)
	{
		const int32 width = rect.IntegerWidth() + 1;
		const int32 height = rect.IntegerHeight() + 1;
		if (width <= 0 || height <= 0)
			return 0;

		if (!band || band->Bounds().IntegerWidth() + 1 != width) {
			band = std::make_unique<BBitmap>(BRect(0, 0, width - 1, kMaxRows - 1), B_RGB32);
			if (band->InitCheck() != B_OK) {
				band.reset();
				return 0;
			}
		}

		stride = (uint32_t)width * 4;
		frame.resize((size_t)stride * height);

		const uint8_t *bits = (const uint8_t *)band->Bits();
		const size_t band_stride = (size_t)band->BytesPerRow();
		uint64_t total = 0;

		for (int32 y = 0; y < height;) {
			const int32 n = std::min(rows, height - y);
			BRect part(rect.left, rect.top + y, rect.right, rect.top + y + n - 1);

			const uint64_t start = os_gettime_ns();
			const status_t err = screen.ReadBitmap(band.get(), cursor, &part);
			const uint64_t took = os_gettime_ns() - start;
			if (err != B_OK)
				return 0;
			total += took;

			for (int32 r = 0; r < n; r++)
				memcpy(frame.data() + (size_t)(y + r) * stride, bits + (size_t)r * band_stride, stride);
			y += n;

			/* size the next band so one read holds the app_server for about kTargetNs */
			if (took > 0) {
				const double scaled = (double)n * (double)kTargetNs / (double)took;
				rows = std::clamp((int32)scaled, kMinRows, kMaxRows);
			}

			/* let the app_server move the cursor and draw before the next band */
			if (y < height)
				snooze(kGapUs);
		}
		return total;
	}

private:
	static constexpr uint64_t kTargetNs = 2000000; /* 2 ms per band */
	static constexpr bigtime_t kGapUs = 1000;
	static constexpr int32 kMinRows = 8;
	static constexpr int32 kMaxRows = 512;

	std::unique_ptr<BBitmap> band;
	int32 rows = 64;
};
