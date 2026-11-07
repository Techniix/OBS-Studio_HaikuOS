# Haiku port architecture

This document explains how each part of OBS Studio was adapted to Haiku, why it was done that way, and where
the code lives. Read it before changing Haiku-specific code.

All Haiku code is either in new files with `haiku` in the name or in `#if defined(__HAIKU__)` /
`OS_HAIKU` / `$<PLATFORM_ID:Haiku>` blocks inside upstream files, so `git log`/`git diff 33.0.1` shows the
whole port and rebasing onto a new upstream release stays manageable.

## Contents

1. [Build system](#1-build-system)
2. [libobs platform layer](#2-libobs-platform-layer)
3. [Hotkeys](#3-hotkeys)
4. [OpenGL renderer](#4-opengl-renderer)
5. [Qt frontend](#5-qt-frontend)
6. [Audio monitoring](#6-audio-monitoring)
7. [haiku-media: audio and video capture](#7-haiku-media-audio-and-video-capture)
8. [haiku-capture: screen capture](#8-haiku-capture-screen-capture)
9. [Other plugins](#9-other-plugins)
10. [Known limitations and ideas](#10-known-limitations-and-ideas)

## 1. Build system

| File | What it does |
| --- | --- |
| `cmake/common/osconfig.cmake` | Sets `OS_HAIKU` when the target (`CMAKE_SYSTEM_NAME`) or the host is Haiku. Checked first so cross-compiling from Linux works. |
| `cmake/haiku/compilerconfig.cmake` | Reuses the Linux GCC/Clang flags. `CMAKE_COMPILE_WARNING_AS_ERROR` defaults to `OFF` because Haiku's headers and GCC 13 produce warnings upstream CI never sees. |
| `cmake/haiku/defaults.cmake` | Install layout (default prefix `/boot/home/config/non-packaged`; CMake's own Haiku default would be `/boot/system`, which is read-only). Sets `ENABLE_PULSEAUDIO`/`ENABLE_WAYLAND` to `OFF` as plain variables, and points FindSIMDe at the bundled `deps/simde` submodule. |
| `cmake/haiku/helpers.cmake` | Reuses the Linux target helpers (rundir layout, RPATH, install rules). Haiku's runtime loader understands ELF RPATH/`$ORIGIN`. |
| `cmake/common/versionconfig.cmake` | Falls back to version `33.0.1-g<hash>` when `git describe` finds no release tag (fork clones often have none). |
| `libobs/cmake/os-haiku.cmake`, `frontend/cmake/os-haiku.cmake` | Platform sources and libraries (`be`, `media`, `locale`, `network`). |

Haiku keeps libc, libm, libdl and pthreads in `libroot` (linked implicitly), sockets in `libnetwork`, and the
C++ kits in `libbe`/`libmedia`/`liblocale`.

## 2. libobs platform layer

`libobs/obs-haiku.c` replaces `obs-nix.c` (which is X11/Wayland specific) and provides:

- `find_libobs_data_file()` — `OBS_DATA_PATH`, then `../share/obs/libobs` relative to the executable, then the
  install prefix.
- `log_system_info()` — CPU brand string via `cpuid`, nominal frequency from `get_cpu_topology_info()`,
  memory from `get_system_info()`, kernel/hrev from `uname()`.
- The hotkey platform hooks (section 3).

`libobs/util/platform-nix.c` is shared with Linux/BSD; the Haiku blocks:

| Function | Haiku implementation |
| --- | --- |
| `os_get_executable_path_ptr()` | First `B_APP_IMAGE` from `get_next_image_info()` (there is no `/proc/self/exe`). |
| `os_get_config_path*()` | `$HOME/config/settings/<name>`, Haiku's `B_USER_SETTINGS_DIRECTORY`, unless `XDG_CONFIG_HOME` is set. OBS settings end up in `~/config/settings/obs-studio`. |
| `os_get_physical_cores()` | Counts `B_TOPOLOGY_CORE` nodes. |
| `os_get_proc_memory_usage()` etc. | Sums `area_info.size`/`ram_size` over the team's areas. |
| `os_get_sys_free_size()`/`os_get_sys_total_size()` | `system_info.free_memory`, `max_pages * B_PAGE_SIZE`. |
| `os_generate_uuid()` | RFC 4122 v4 from `/dev/urandom` (Haiku has no libuuid in the base system). |
| `os_inhibit_sleep_*()` | No-op. Haiku's screen saver has no inhibit interface; the Linux fallback would try to spawn `xdg-screensaver` every 30 s. |

`libobs/util/threading-posix.c` names threads with `rename_thread(find_thread(NULL), name)` so they show up in
ProcessController and the Debugger.

`obs-nix-platform.c` is still built so that third-party plugins calling `obs_get_nix_platform()` link; it
reports `OBS_NIX_PLATFORM_INVALID` on Haiku.

## 3. Hotkeys

Global hotkeys poll the keyboard state from the app_server, the same model as X11:

- `libobs/obs-haiku-be.cpp` wraps `get_key_info()` (128-bit key state bitmap indexed by Haiku raw key code)
  and `get_mouse()` (button mask) for C.
- `libobs/obs-haiku.c` maps `obs_key_t` ↔ Haiku raw key codes. The table was checked against
  `data/system/data/KeyboardLayouts/Generic 105-key International` from the Haiku tree. Left and right
  modifiers both count (`Shift`, `Control`, `Alt` = Command, `Super` = Option/Windows key).
  Polling is cached for 4 ms because the hotkey thread asks for every bound key on each tick.
- "Virtual keys" (`obs_key_from_virtual_key()`) are Haiku raw key codes.

Qt's Haiku platform plugin does not pass raw key codes through `QKeyEvent::nativeVirtualKey()`, so when you
bind a hotkey in Settings the frontend translates the Qt key instead (`HaikuObsKeyFromQtKeyEvent()` in
`frontend/utility/platform-haiku.cpp`). Shifted symbols are folded back onto the physical key on a US layout
(`!` → `1`, `?` → `/`, …) so the binding matches what `get_key_info()` reports. On other layouts some
punctuation keys may bind to a different physical key than expected; letters, digits, F-keys, the numpad and
navigation keys are layout independent.

## 4. OpenGL renderer

`libobs-opengl/gl-haiku.c`.

Haiku's Mesa (HaikuPorts `mesa` + `mesa_eglvnd`, dispatched through `libglvnd`) implements EGL with the
`haiku` platform. An EGL *window* surface on Haiku belongs to a BWindow/BView, and that view is driven by its
window's looper thread. OBS renders every display (preview, projectors, multiview, property previews) from
one graphics thread, and the frontend's widgets are Qt widgets rather than BViews. Rendering straight into
them isn't possible.

Instead:

1. One EGL context (desktop OpenGL 3.3 core) is created on `EGL_DEFAULT_DISPLAY`. A 1×1 pbuffer serves as
   the surface when no swapchain is current; if pbuffers are unavailable, `EGL_KHR_surfaceless_context` is
   used.
2. Each swapchain gets its own pbuffer surface the size of the widget. `device_load_swapchain()` makes it
   current, so OBS's "render to framebuffer 0" lands in that pbuffer. `device_resize()` → `gl_update()`
   reallocates it.
3. `device_present()` reads the pbuffer back with `glReadPixels(GL_BGRA)`, flips it to top-down order and
   calls `gs_window.present(param, pixels, width, height, linesize)`.

`struct gs_window` on Haiku (in `libobs/graphics/graphics.h`) is therefore a callback rather than a native
handle:

```c
struct gs_window {
	void (*present)(void *param, const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t linesize);
	void *param;
};
```

The callback runs on the graphics thread while libobs holds the display lock; it must copy the pixels and
return quickly. Program output (recording/streaming) never goes through this path. The readback costs about
`width × height × 4` bytes per displayed frame, which is negligible for preview sizes.

GL entry points are loaded through `eglGetProcAddress` (falling back to `dlsym` on libGL). OpenGL 3.3 is
required; Mesa's llvmpipe provides 4.5.

## 5. Qt frontend

The frontend is built against HaikuPorts' Qt 6 (`qt6_base`, `qt6_svg`) with its Haiku platform plugin.

**Displays** (`frontend/widgets/OBSQTDisplay.*`). On Haiku every native `QWindow` becomes its own BWindow, so
the preview must not be a native child window. Haiku displays are ordinary (alien) widgets:

- No `WA_PaintOnScreen`/`WA_NativeWindow` (except for top-level projectors, which keep a `QWindow` so
  `windowHandle()` works for the code that tags projector windows).
- `CreateDisplay()` passes `HaikuPresent` + `this` as the `gs_window`.
- `HaikuPresent()` copies the frame into a `QImage` (`Format_RGB32` has the same byte order as BGRA on
  little-endian) under a mutex and schedules one `update()` on the UI thread (coalesced with an atomic flag).
- `paintEvent()` draws the last frame with `QPainter`, honoring the device pixel ratio.
- Displays are created on `showEvent` and destroyed on `hideEvent`, like on Linux.

**Platform helpers** (`frontend/utility/platform-haiku.cpp`):

- Single instance check: `flock()` on `~/config/settings/obs-studio.lock`, held for the process lifetime.
- `GetPreferredLocales()`: `BLocaleRoster::GetPreferredLanguages()`, falling back to `LC_MESSAGES`.
- `GetDefaultVideoSavePath()`: the home folder (`B_USER_DIRECTORY`).
- Hotkey translation (section 3), CPU frequency for system info.

`frontend/utility/system-info-haiku.cpp` fills the capabilities report used by Enhanced Broadcasting.
`CrashHandler_Haiku.cpp` is a stub: Haiku's Debugger writes crash reports to the Desktop on its own.
The plugin manager reuses the BSD loader, which only uses POSIX/libobs APIs.

## 6. Audio monitoring

`libobs/audio-monitoring/haiku/`.

Each monitored source gets a `BSoundPlayer` connected to the system mixer, asking for float32 interleaved
audio at OBS's sample rate and channel count, so the mixer handles conversion to the sound card.

- libobs's audio thread converts the source's planar float audio to interleaved (`audio_resampler`), applies
  the source volume and appends it to a FIFO.
- The player's realtime thread (`haiku-sound-player.cpp`) pulls 10 ms buffers out of it.
- The FIFO waits for 40 ms of audio before starting and after an underrun, and drops the oldest audio
  beyond 200 ms, so monitoring latency stays bounded.

Haiku routes all application audio through the system mixer, so "Default" is the only monitoring device.

## 7. haiku-media: audio and video capture

`plugins/haiku-media/`, built on the Media Kit's `BMediaRecorder`.

`media-device.cpp`:

- Devices are the physical inputs published by media add-ons (`GetDormantNodes()` with
  `B_BUFFER_PRODUCER | B_PHYSICAL_INPUT` and a raw audio/video output). Their names are used as persistent
  IDs. "Default" uses the input chosen in Haiku's Media preferences (`GetAudioInput()`/`GetVideoInput()`).
- `HaikuRecorder` subclasses `BMediaRecorder`. It reuses a running instance of the device node if there is
  one, connects to a free output with the output's own format, records the negotiated format from
  `MediaInput()` (the recorder's `Format()` would only return the wildcard we accepted), starts the
  producer, and forwards every buffer to a callback.
- If the device has no free output (another program is recording from it), the source logs a warning and
  stays silent/black; re-saving the source properties retries.

**Audio Input Capture** (`haiku_audio_input_capture`): float, 32-bit, 16-bit and 8-bit PCM, 1–8
channels, host byte order. Buffers are timestamped on arrival.

**Video Capture Device** (`haiku_video_input_capture`): `B_RGB32`, `B_RGBA32`, `B_RGB24` and `B_YCbCr422`
(as YUY2, partial range). Haiku's `usb_webcam` add-on produces `B_RGB32`. "Deactivate when not showing"
disconnects the device while the source isn't visible.

There is no desktop audio capture: the system mixer has a single output, which is connected to the sound
card.

## 8. haiku-capture: screen capture

`plugins/haiku-capture/screen-capture.cpp`. A thread calls `BScreen::ReadBitmap()` into a `B_RGB32` bitmap
at the configured rate (1–60 fps, default 30) and outputs it as an async BGRX source, so the app_server copy
never blocks rendering. It captures the main screen with the cursor optional. The source only runs while it
is shown.

`BScreen` needs a connection to the app_server, which exists because Qt's Haiku plugin creates a
`BApplication`.

## 9. Other plugins

| Plugin | Haiku changes |
| --- | --- |
| frontend-tools | Automatic Scene Switcher matches running application names (`BRoster`) because window titles of other teams aren't public API. |
| text-freetype2 | Uses fontconfig (`find-font-unix.c`) like Linux. |
| obs-outputs | Links `libnetwork`. |
| obs-scripting | Links libpython directly like on Linux. |
| obs-ffmpeg, obs-x264, obs-filters, image-source, obs-transitions, rtmp-services, obs-webrtc, vlc-video | Portable, unchanged. No VA-API/NVENC/QSV on Haiku. |
| obs-browser, AJA, DeckLink, VST, linux-* | Not built on Haiku. |

## 10. Known limitations and ideas

- **Desktop audio capture**: would need a virtual audio node inserted between the mixer and the sound card,
  or an app_server/media_server hook.
- **Window capture**: needs the private app_server window list (`get_window_info`) plus a way to grab
  individual windows. Screen capture plus cropping works in the meantime.
- **Preview performance**: if `glReadPixels` turns out to be a bottleneck with llvmpipe at high preview
  resolutions, the readback could be made asynchronous with a PBO ring.
- **Hardware encoding**: none on Haiku today (x264/FFmpeg software encoders only).
- **Browser source/docks**: depend on CEF, which hasn't been ported to Haiku.
- **obs-websocket**: needs `qrcodegencpp` packaged for Haiku.
- **SRT/RIST output**: needs `libsrt`/`librist` packaged for Haiku.
