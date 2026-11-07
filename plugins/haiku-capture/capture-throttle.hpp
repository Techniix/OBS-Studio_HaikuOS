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
 * The software cursor is moved under the same app_server lock, so every read also stops the pointer. There is
 * no way to get a consistent snapshot without holding the lock for the whole read, so the user chooses:
 *
 *  - CAPTURE_TEAR_FREE (default): one ReadBitmap() per frame. Every frame is a consistent snapshot of the
 *    screen; the pointer pauses for the duration of each read (a few ms to ~30 ms, depending on the driver).
 *  - CAPTURE_SMOOTH_POINTER: horizontal bands sized to hold the lock for ~2 ms each, with a short yield between
 *    them. The pointer stays smooth, but something that moves while a frame is being read can show a seam
 *    between bands in that one frame.
 *
 * In both modes CaptureThrottle keeps the time spent reading to at most kMaxDutyCycle of the wall time, lowering
 * the effective frame rate when the hardware can't keep up, and logs when it does.
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

enum capture_method {
	CAPTURE_TEAR_FREE = 0,
	CAPTURE_SMOOTH_POINTER = 1,
};

#define S_CAPTURE_METHOD "capture_method"

/* Adds the "Capture method" list (shared by screen and window capture). */
static inline void add_capture_method_property(obs_properties_t *props)
{
	obs_property_t *p = obs_properties_add_list(props, S_CAPTURE_METHOD, obs_module_text("CaptureMethod"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("CaptureMethod.TearFree"), CAPTURE_TEAR_FREE);
	obs_property_list_add_int(p, obs_module_text("CaptureMethod.SmoothPointer"), CAPTURE_SMOOTH_POINTER);
	obs_property_set_long_description(p, obs_module_text("CaptureMethod.Description"));
}

class BandedScreenReader {
public:
	/* Reads `rect` (screen coordinates, integral) into `frame` as BGRX rows of `stride` bytes, in one piece
	 * (tear-free) or in short bands (smooth pointer). Returns the time spent inside the app_server, or 0 on
	 * failure. */
	uint64_t Read(BScreen &screen, BRect rect, bool cursor, bool banded, std::vector<uint8_t> &frame,
		      uint32_t &stride)
	{
		const int32 width = rect.IntegerWidth() + 1;
		const int32 height = rect.IntegerHeight() + 1;
		if (width <= 0 || height <= 0)
			return 0;

		/* the app_server copies each read to the bitmap's top-left corner */
		const int32 capacity = banded ? kMaxRows : height;
		if (!band || band->Bounds().IntegerWidth() + 1 != width || band->Bounds().IntegerHeight() + 1 < capacity) {
			band = std::make_unique<BBitmap>(BRect(0, 0, width - 1, capacity - 1), B_RGB32);
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
			const int32 n = banded ? std::min(rows, height - y) : height;
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
			if (banded && took > 0) {
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
