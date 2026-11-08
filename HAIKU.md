# OBS Studio for Haiku

This repository ports [OBS Studio](https://github.com/obsproject/obs-studio) to [Haiku](https://www.haiku-os.org/).
It tracks upstream release **33.0.1** and keeps the Haiku work in separate commits on top, so it can be rebased
onto new releases. Everything is under OBS's license, GPL v2 or later (see `COPYING`).

The goal is full feature parity. No OBS feature is switched off for Haiku; where Haiku lacks what upstream relies on
(CEF, PulseAudio loopback devices, per-window buffers), the port provides a Haiku implementation behind the same
interfaces.

| Document | Contents |
| --- | --- |
| [docs/haiku/BUILDING.md](docs/haiku/BUILDING.md) | Dependencies, build, run, install, troubleshooting |
| [docs/haiku/TESTING.md](docs/haiku/TESTING.md) | Step-by-step checklist for testing on real hardware |
| [docs/haiku/ARCHITECTURE.md](docs/haiku/ARCHITECTURE.md) | How each subsystem was adapted and why |

## Status

**The port has not been compiled with Haiku's GCC or run on Haiku yet.** Every translation unit of the build (about
550, including Qt moc/uic output) passes a syntax-only compile with clang for `x86_64-unknown-haiku` against the real
Haiku system headers and the real dependency headers (`build-aux/haiku/crosscheck/`). GCC-specific errors, link
errors and all runtime behavior are still unverified; the first test on hardware is the next milestone.

### Feature matrix

Column meanings:

- **Implemented**: the code exists for Haiku.
- **Compiles**: "clang" means it passes the cross-check harness (syntax-only, clang, real Haiku headers); "GCC" would
  mean a real build on Haiku.
- **Runs**, **Functionally tested**, **Fully verified**: on real Haiku hardware (the maintainer's x86-64 machine with an
  RTX 5070, hrev60368). "GCC" means the whole tree was built natively on Haiku; changes made after that build (the
  2026-11-08 review fixes and supplements) are syntax-checked with clang until the next native build.

| Feature | Implemented | Compiles | Runs | Functionally tested | Fully verified | Known limitations |
| --- | --- | --- | --- | --- | --- | --- |
| Build system (`OS_HAIKU`, `cmake/haiku`) | Yes | GCC | Yes | Yes (full build on hrev60368) | – | `build.sh` turns optional features on or off from the installed packages each run |
| libobs platform layer (paths, system info, threads, UUIDs, sleep inhibition) | Yes | GCC | Yes | Partly | – | Haiku has no idle system sleep; while recording/streaming the screen saver and display power-off are held off by resetting the input idle timer (public API, no settings changed) |
| OpenGL renderer (EGL or OSMesa, chosen at runtime, offscreen + readback) | Yes | GCC | Yes (OSMesa) | Partly | – | Hardware EGL drivers (e.g. nvidia-haiku) are used only after passing a self-test in a separate process; a driver that hangs or misrenders falls back to OSMesa (CPU) automatically. One `glReadPixels` per displayed preview frame |
| Qt frontend (preview, projectors, multiview, settings) | Yes | GCC | Yes | Partly | – | Preview is painted from a CPU copy |
| Global hotkeys | Yes | GCC | – | – | – | OBS "Ctrl" = Haiku Command key (Alt by default); punctuation on non-US layouts may bind to another key |
| Recording / streaming (FFmpeg, x264, RTMP, WHIP) | Yes (portable) | GCC | Yes | Partly (MP4 recording) | – | Software encoders only (see below) |
| SRT / RIST output (native MPEG-TS) | Yes (bundled libsrt 1.5.4, librist 0.2.12) | GCC | – | – | – | Built from source with small Haiku patches |
| obs-websocket 5 | Yes (bundled qrcodegen, Asio, WebSocket++) | GCC | – | – | – | – |
| Audio input capture (mic, line-in) | Yes (Media Kit `BMediaRecorder`) | GCC | – | – | – | Busy or unplugged devices are retried automatically (1–10 s backoff) and picked up again when they return; 3–5 and 7 channel layouts are remapped to OBS layouts |
| Desktop audio capture | Yes (tap node helper, `obs-haiku-audio-tap`) | GCC | Yes | Partly (tap insertion) | – | Captures the whole system mix (no per-application capture); monitored sources are captured too. Changing the output in Media preferences moves capture and playback to the new card within a few seconds; with OBS closed, a later output change needs "Restart media services" |
| Audio monitoring | Yes (`BSoundPlayer`) | GCC | – | – | – | Default = system mixer; physical outputs the mixer doesn't use (e.g. a USB headset) can be chosen directly (sources on one card share one connection); a missing or busy card falls back to the mixer; no clock-drift correction beyond the buffer |
| Webcam / video capture devices | Yes (Media Kit) | GCC | – | – | – | Resolution picker lists the sizes the device accepted when first opened; frame rate and colour space are fixed by Haiku's webcam driver; unplug/replug is followed automatically |
| Screen capture | Yes (`BScreen::ReadBitmap`) | GCC | Yes | Partly (recording made) | – | Main screen only; reads pause the pointer briefly (*Tear-free*, default) or may show a seam on fast motion (*Smooth pointer*); capture rate drops automatically on slow frame-buffer reads |
| Window capture | Yes, as screen-region capture of the tracked window | GCC | – | – | – | Not isolated: windows covering it are captured too (app_server keeps no per-window buffers) |
| Browser source | Yes (HaikuWebKit helper `obs-haiku-browser`) | GCC | – | – | – | No WebGL, no per-source audio (page audio goes to Desktop Audio), transparency depends on WebKit's alpha output |
| Browser docks (custom docks, service integrations) | Yes (QCef interface on HaikuWebKit) | GCC | – | – | – | Cookies persist per cookie profile (mode 0600) and are shared live by all docks of a profile; persistent logins survive restarts, but session cookies (no expiry) are cleared in every dock whenever a new browser helper starts (HaikuWebKit behaviour) — use a site's "remember me" option; clearing/checking cookies from OBS needs libsqlite3 |
| Automatic scene switcher | Yes (matches application names) | GCC | – | – | – | Window titles of other apps aren't used |
| Scripting (Lua, Python) | Yes (portable) | clang | – | – | – | Needs SWIG, LuaJIT, Python packages |
| Third-party plugins | Yes (standard module loading, plugin manager) | GCC | – | – | – | Plugins must be built for Haiku |
| AJA, DeckLink, NVENC, QSV, VA-API | No | – | – | – | – | No drivers/SDKs exist for Haiku. NVENC needs NVIDIA's closed userspace libraries (Linux/Windows only); a hardware path on Haiku would be Vulkan Video encode through NVK once Mesa supports it |

## Quick start

```sh
git clone https://github.com/Techniix/OBS-Studio_HaikuOS.git
cd OBS-Studio_HaikuOS
build-aux/haiku/install-deps.sh
build-aux/haiku/build.sh
cd build/rundir/RelWithDebInfo/bin && ./obs
```

`build.sh` fetches the submodules it needs. Settings and logs are in `~/config/settings/obs-studio`.

## Where the Haiku code is

| Path | Purpose |
| --- | --- |
| `cmake/haiku/`, `cmake/common/osconfig.cmake` | Platform detection, compiler flags, install layout, finders that fall back to bundled deps |
| `libobs/obs-haiku.c`, `libobs/obs-haiku-be.cpp`, `libobs/util/platform-nix.c` | System info, data paths, hotkeys, platform utilities |
| `libobs/audio-monitoring/haiku/` | Audio monitoring through `BSoundPlayer` |
| `libobs-opengl/gl-haiku.c` | Offscreen swapchains (EGL framebuffer objects or OSMesa) with readback |
| `frontend/utility/*-haiku.cpp`, `frontend/widgets/OBSQTDisplay.*` | Frontend platform glue, preview painting |
| `plugins/haiku-capture/` | Screen capture, window capture |
| `plugins/haiku-media/` | Audio input, desktop audio (with the `obs-haiku-audio-tap` helper), video capture devices |
| `plugins/haiku-browser/` | Browser source and docks (module `obs-browser`, helper `obs-haiku-browser`) |
| `deps/qrcodegen`, `deps/srt-haiku`, `deps/librist-haiku` | Bundled builds of libraries HaikuPorts doesn't package |
| `build-aux/haiku/` | Dependency installer, build script, log collector, cross-check harness |
