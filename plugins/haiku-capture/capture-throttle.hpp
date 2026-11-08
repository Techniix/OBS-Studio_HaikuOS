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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <string.h>
#include <vector>

/*
 * libobs calls show/hide (and so the sources' Stop(), which joins the capture thread) on the graphics thread.
 * Every wait in a capture thread goes through this so that Stop() wakes it at once instead of rendering stalling
 * until a sleep runs out.
 */
class StopSignal {
public:
	void Reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopped = false;
	}

	void Stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopped = true;
		}
		cond.notify_all();
	}

	bool Stopped() const { return stopped; }

	/* Waits until os_gettime_ns() reaches `deadline`. Returns false if Stop() was called. */
	bool WaitUntil(uint64_t deadline)
	{
		std::unique_lock<std::mutex> lock(mutex);
		while (!stopped) {
			const uint64_t now = os_gettime_ns();
			if (now >= deadline)
				return true;
			cond.wait_for(lock, std::chrono::nanoseconds((int64_t)(deadline - now)));
		}
		return false;
	}

	bool WaitFor(uint64_t ns) { return WaitUntil(os_gettime_ns() + ns); }

private:
	std::mutex mutex;
	std::condition_variable cond;
	std::atomic_bool stopped = false;
};

/* Connects to the main screen, retrying once a second until that works or `stop` is signalled (returns null). */
static inline std::unique_ptr<BScreen> open_main_screen(StopSignal &stop, const char *what)
{
	bool warned = false;
	while (!stop.Stopped()) {
		std::unique_ptr<BScreen> screen = std::make_unique<BScreen>(B_MAIN_SCREEN_ID);
		if (screen->IsValid())
			return screen;
		if (!warned) {
			blog(LOG_ERROR, "[haiku-capture] %s: unable to connect to the main screen, retrying", what);
			warned = true;
		}
		screen.reset();
		stop.WaitFor(1000000000ULL);
	}
	return nullptr;
}

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

	/* Sleeps until the next frame is due given the requested `fps`. Returns false if `stop` was signalled. */
	bool Wait(int fps, StopSignal &stop)
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
		if (next < now) {
			next = now; /* fell behind: don't try to catch up */
			return !stop.Stopped();
		}
		return stop.WaitUntil(next);
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

/* One captured frame, BGRX rows of `linesize` bytes; valid until the next BandedScreenReader::Read(). */
struct CapturedFrame {
	const uint8_t *data = nullptr;
	uint32_t linesize = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t took = 0; /* time spent inside the app_server */
};

class BandedScreenReader {
public:
	/* Reads `rect` (screen coordinates, integral) in one piece (tear-free) or in short bands (smooth pointer).
	 * Tear-free frames point straight into the bitmap the app_server wrote to (libobs copies the frame during
	 * obs_source_output_video(), before the next read); banded frames are assembled in a buffer. Returns false
	 * on failure, or if `stop` was signalled before all bands were read. */
	bool Read(BScreen &screen, BRect rect, bool cursor, bool banded, StopSignal &stop, CapturedFrame &out)
	{
		const int32 width = rect.IntegerWidth() + 1;
		const int32 height = rect.IntegerHeight() + 1;
		if (width <= 0 || height <= 0)
			return false;

		/* The app_server copies each read to the bitmap's top-left corner, at most the bitmap's size, and rows
		 * are addressed through BytesPerRow(), so a larger bitmap works as well: only grow it, rounded up, so
		 * that resizing a window doesn't reallocate it every frame. */
		const int32 rows_needed = banded ? std::min(kMaxRows, height) : height;
		const int32 have_width = band ? band->Bounds().IntegerWidth() + 1 : 0;
		const int32 have_rows = band ? band->Bounds().IntegerHeight() + 1 : 0;
		if (have_width < width || have_rows < rows_needed) {
			const int32 alloc_width = RoundUp(std::max(width, have_width));
			const int32 alloc_rows = RoundUp(std::max(rows_needed, have_rows));
			band = std::make_unique<BBitmap>(BRect(0, 0, alloc_width - 1, alloc_rows - 1), B_RGB32);
			if (band->InitCheck() != B_OK) {
				band.reset();
				return false;
			}
		}

		const uint8_t *bits = (const uint8_t *)band->Bits();
		const size_t band_stride = (size_t)band->BytesPerRow();

		if (!banded) {
			const uint64_t start = os_gettime_ns();
			const status_t err = screen.ReadBitmap(band.get(), cursor, &rect);
			out.took = os_gettime_ns() - start;
			if (err != B_OK)
				return false;

			out.data = bits;
			out.linesize = (uint32_t)band_stride;
			out.width = (uint32_t)width;
			out.height = (uint32_t)height;
			return true;
		}

		const size_t stride = (size_t)width * 4;
		frame.resize(stride * height);
		uint64_t total = 0;

		for (int32 y = 0; y < height;) {
			const int32 n = std::min(rows, height - y);
			BRect part(rect.left, rect.top + y, rect.right, rect.top + y + n - 1);

			const uint64_t start = os_gettime_ns();
			const status_t err = screen.ReadBitmap(band.get(), cursor, &part);
			const uint64_t took = os_gettime_ns() - start;
			if (err != B_OK)
				return false;
			total += took;

			for (int32 r = 0; r < n; r++)
				memcpy(frame.data() + (size_t)(y + r) * stride, bits + (size_t)r * band_stride, stride);
			y += n;

			/* size the next band so one read holds the app_server for about kTargetNs */
			if (took > 0) {
				const double scaled = (double)n * (double)kTargetNs / (double)took;
				rows = std::clamp((int32)scaled, kMinRows, kMaxRows);
			}

			/* let the app_server move the cursor and draw before the next band; Stop() abandons the frame */
			if (y < height && !stop.WaitFor(kGapNs))
				return false;
		}

		out.data = frame.data();
		out.linesize = (uint32_t)stride;
		out.width = (uint32_t)width;
		out.height = (uint32_t)height;
		out.took = total;
		return true;
	}

private:
	static constexpr uint64_t kTargetNs = 2000000; /* 2 ms per band */
	static constexpr uint64_t kGapNs = 1000000;
	static constexpr int32 kMinRows = 8;
	static constexpr int32 kMaxRows = 512;
	static constexpr int32 kAlign = 64;

	static int32 RoundUp(int32 v) { return (v + kAlign - 1) / kAlign * kAlign; }

	std::unique_ptr<BBitmap> band;
	std::vector<uint8_t> frame;
	int32 rows = 64;
};
