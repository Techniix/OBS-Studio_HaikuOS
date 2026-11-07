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
- **Runs**, **Functionally tested**, **Fully verified**: on real Haiku hardware. Nothing has reached these yet.

| Feature | Implemented | Compiles | Runs | Functionally tested | Fully verified | Known limitations |
| --- | --- | --- | --- | --- | --- | --- |
| Build system (`OS_HAIKU`, `cmake/haiku`) | Yes | clang | – | – | – | Configures in the harness; native configure untested |
| libobs platform layer (paths, system info, threads, UUIDs) | Yes | clang | – | – | – | Sleep inhibition is a no-op (Haiku has no API) |
| OpenGL renderer (Mesa EGL pbuffers + readback) | Yes | clang | – | – | – | One `glReadPixels` per displayed preview frame |
| Qt frontend (preview, projectors, multiview, settings) | Yes | clang | – | – | – | Preview is painted from a CPU copy |
| Global hotkeys | Yes | clang | – | – | – | OBS "Ctrl" = Haiku Command key (Alt by default); punctuation on non-US layouts may bind to another key |
| Recording / streaming (FFmpeg, x264, RTMP, WHIP) | Yes (portable) | clang | – | – | – | Software encoders only; no hardware encoders on Haiku |
| SRT / RIST output (native MPEG-TS) | Yes (bundled libsrt 1.5.4, librist 0.2.12) | clang | – | – | – | Built from source with small Haiku patches |
| obs-websocket 5 | Yes (bundled qrcodegen, Asio, WebSocket++) | clang | – | – | – | – |
| Audio input capture (mic, line-in) | Yes (Media Kit `BMediaRecorder`) | clang | – | – | – | Busy devices stay silent until the source is re-saved |
| Desktop audio capture | Yes (tap node helper, `obs-haiku-audio-tap`) | clang | – | – | – | Captures the whole system mix (no per-application capture); monitored sources are captured too |
| Audio monitoring | Yes (`BSoundPlayer`) | clang | – | – | – | "Default" (system mixer) is the only device; ~50–250 ms latency |
| Webcam / video capture devices | Yes (Media Kit) | clang | – | – | – | Uses the device's current format; no resolution/format picker yet |
| Screen capture | Yes (`BScreen::ReadBitmap`) | clang | – | – | – | Main screen only |
| Window capture | Yes, as screen-region capture of the tracked window | clang | – | – | – | Not isolated: windows covering it are captured too (app_server keeps no per-window buffers) |
| Browser source | Yes (HaikuWebKit helper `obs-haiku-browser`) | clang | – | – | – | No WebGL, no per-source audio (page audio goes to Desktop Audio), transparency depends on WebKit's alpha output |
| Browser docks (custom docks, service integrations) | Yes (QCef interface on HaikuWebKit) | clang | – | – | – | Cookies are kept per helper process in memory, not persisted |
| Automatic scene switcher | Yes (matches application names) | clang | – | – | – | Window titles of other apps aren't used |
| Scripting (Lua, Python) | Yes (portable) | clang | – | – | – | Needs SWIG, LuaJIT, Python packages |
| Third-party plugins | Yes (standard module loading, plugin manager) | clang | – | – | – | Plugins must be built for Haiku |
| AJA, DeckLink, NVENC, QSV, VA-API | No | – | – | – | – | No drivers/SDKs exist for Haiku |

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
| `libobs-opengl/gl-haiku.c` | EGL pbuffer swapchains with readback |
| `frontend/utility/*-haiku.cpp`, `frontend/widgets/OBSQTDisplay.*` | Frontend platform glue, preview painting |
| `plugins/haiku-capture/` | Screen capture, window capture |
| `plugins/haiku-media/` | Audio input, desktop audio (with the `obs-haiku-audio-tap` helper), video capture devices |
| `plugins/haiku-browser/` | Browser source and docks (module `obs-browser`, helper `obs-haiku-browser`) |
| `deps/qrcodegen`, `deps/srt-haiku`, `deps/librist-haiku` | Bundled builds of libraries HaikuPorts doesn't package |
| `build-aux/haiku/` | Dependency installer, build script, log collector, cross-check harness |
