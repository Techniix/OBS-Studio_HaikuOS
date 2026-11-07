# OBS Studio for Haiku

This repository is a port of [OBS Studio](https://github.com/obsproject/obs-studio) to
[Haiku](https://www.haiku-os.org/). It tracks upstream releases (currently **33.0.1**) and keeps the
Haiku-specific changes as separate commits on top, so they can be rebased onto new releases.

OBS Studio is licensed under the GNU General Public License v2 or later (see `COPYING`); all changes in
this fork are under the same license.

## Status

Early bring-up. The code has been checked against the Haiku system headers but has **not yet been built
or run on Haiku** — expect build fixes on the first real compile.

| Area | State |
| --- | --- |
| Build system (`OS_HAIKU`, `cmake/haiku`) | Done |
| libobs platform layer (paths, system info, threads, UUIDs) | Done |
| Global hotkeys (`get_key_info()` / `get_mouse()`) | Done |
| OpenGL renderer (Mesa EGL, offscreen pbuffers + readback) | Done |
| Qt frontend (preview, projectors, settings, hotkey capture) | Done |
| Screen capture (`haiku-capture`, `BScreen::ReadBitmap`) | Done |
| Automatic scene switcher (matches running app names) | Done |
| Encoding/streaming/recording (FFmpeg, x264, RTMP, WebRTC) | Portable code, enabled |
| Audio input/output capture (Media Kit) | Not started |
| Audio monitoring (Media Kit / `BSoundPlayer`) | Not started (null backend) |
| Webcam / video capture devices (Media Kit) | Not started |
| Window capture | Not started |
| Browser source / docks (CEF) | Not available on Haiku |

### How rendering works

Haiku has no way to let another thread render straight into a Qt widget's window with EGL, so
`libobs-opengl/gl-haiku.c` renders every display (preview, projector, multiview) into its own EGL pbuffer.
On present, the frame is read back and passed to the owner through a callback in `struct gs_window`.
The Qt frontend (`OBSQTDisplay`) copies it into a `QImage` and paints it on the UI thread. This costs
one `glReadPixels` per displayed frame, which is fine at preview sizes. It does not affect what gets
recorded or streamed.

## Building

### Dependencies

Install with `pkgman install` (secondary-architecture builds aren't supported):

```sh
pkgman install cmake ninja pkgconfig \
    ffmpeg8_devel x264_devel jansson_devel curl_devel mbedtls3_devel zlib_devel uthash \
    qt6_base_devel qt6_svg_devel qt6_tools \
    mesa_devel libglvnd_devel freetype_devel fontconfig_devel \
    nlohmann_json libdatachannel_devel \
    luajit_devel swig python3.10_devel \
    speexdsp_devel rnnoise_devel
```

Optional: `vlc_devel` (VLC video source), `fdk_aac_devel` (libfdk AAC encoder).

SIMDe isn't packaged in HaikuPorts; it is bundled as the `deps/simde` submodule and found automatically.

### Configure and build

```sh
git clone --recursive https://github.com/Techniix/OBS-Studio_HaikuOS.git
cd OBS-Studio_HaikuOS

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_WEBSOCKET=OFF \
    -DENABLE_NEW_MPEGTS_OUTPUT=OFF
cmake --build build
```

- `ENABLE_WEBSOCKET=OFF`: obs-websocket needs `qrcodegencpp`, which isn't in HaikuPorts yet.
- `ENABLE_NEW_MPEGTS_OUTPUT=OFF`: the native SRT/RIST output needs `libsrt` and `librist`, which aren't
  in HaikuPorts yet.
- Browser source, AJA, DeckLink and VST plugins are skipped automatically on Haiku.
- Warnings are not treated as errors on Haiku. Pass `-DCMAKE_COMPILE_WARNING_AS_ERROR=ON` to opt in.

### Running from the build tree

```sh
cd build/rundir/RelWithDebInfo/bin
./obs
```

Settings and logs are stored in `~/config/settings/obs-studio`.

### Installing

The default install prefix is `/boot/home/config/non-packaged`:

```sh
cmake --install build
```

## Haiku-specific files

| Path | Purpose |
| --- | --- |
| `cmake/haiku/` | Compiler flags, install layout, helpers |
| `libobs/cmake/os-haiku.cmake` | libobs sources/libraries for Haiku |
| `libobs/obs-haiku.c`, `libobs/obs-haiku-be.cpp` | System info, data paths, hotkeys |
| `libobs/util/platform-nix.c` (`__HAIKU__` blocks) | Executable path, config dir, memory/CPU queries, UUIDs |
| `libobs-opengl/gl-haiku.c` | EGL pbuffer swapchains with readback |
| `frontend/cmake/os-haiku.cmake`, `frontend/utility/*-haiku.cpp` | Frontend platform glue |
| `frontend/widgets/OBSQTDisplay.*` (`__HAIKU__` blocks) | Paints frames handed back by libobs |
| `plugins/haiku-capture/` | Screen capture source |
| `plugins/frontend-tools/auto-scene-switcher-haiku.cpp` | Scene switcher backend |
