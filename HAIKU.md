# OBS Studio for Renga

This branch (`renga`) targets **Renga** only, the Haiku-based OS in `Techniix/haiku` (branch `ce/main`). Stock Haiku
support is on the `main` branch; this branch drops the workarounds that exist there for what stock Haiku lacks
(a desktop audio helper, throttled screen reads, shared-memory browser IPC) and uses Renga's Media Kit, screen
capture API and Linux IPC primitives instead. What OBS still needs from Renga and Quasar is listed in
[docs/haiku/RENGA-REQUESTS.md](docs/haiku/RENGA-REQUESTS.md).

This repository ports [OBS Studio](https://github.com/obsproject/obs-studio) to [Haiku](https://www.haiku-os.org/).
It tracks upstream release **33.0.1** and keeps the Haiku work in separate commits on top, so it can be rebased
onto new releases. Everything is under OBS's license, GPL v2 or later (see `COPYING`).

The goal is full feature parity. No OBS feature is switched off for Haiku; where Haiku lacks what upstream relies on
(CEF, per-window buffers), the port provides a Haiku implementation behind the same
interfaces.

| Document | Contents |
| --- | --- |
| [docs/haiku/BUILDING.md](docs/haiku/BUILDING.md) | Dependencies, build, run, install, troubleshooting |
| [docs/haiku/TESTING.md](docs/haiku/TESTING.md) | Step-by-step checklist for testing on real hardware |
| [docs/haiku/ARCHITECTURE.md](docs/haiku/ARCHITECTURE.md) | How each subsystem was adapted and why |
| [docs/haiku/RENGA-REQUESTS.md](docs/haiku/RENGA-REQUESTS.md) | What OBS needs from Renga and Quasar (O-1..O-13, Q-1..Q-4) |

## Status

**The port has not been compiled with Haiku's GCC or run on Haiku yet.** Every translation unit of the build (about
550, including Qt moc/uic output) passes a syntax-only compile with clang for `x86_64-unknown-haiku` against the real
Haiku system headers and the real dependency headers (`build-aux/haiku/crosscheck/`). GCC-specific errors, link
errors and all runtime behavior are still unverified; the first test on hardware is the next milestone.

**The `renga` branch as a whole has not been built on Renga yet.** The rows below that were not touched by the Renga
work keep the values from the last native build on stock Haiku (hrev60368). The rows rewritten for Renga (screen and
window capture, desktop and application audio, audio input, audio monitoring, webcam, browser source and docks, crash
reports) have only passed the clang cross-check against Renga's headers: nothing in them has been built with GCC or
run on Renga, so their Runs and tested columns are empty. Behaviour they rely on was read from Renga's source, not
observed.

### Feature matrix

Column meanings:

- **Implemented**: the code exists for Haiku.
- **Compiles**: "clang" means it passes the cross-check harness (syntax-only, clang, real Haiku headers); "clang
  (Renga headers)" is the same against Renga's headers; "GCC" would mean a real build on Haiku or Renga.
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
| Audio input capture (mic, line-in) | Yes (Media Kit `BMediaRecorder`; list reloads on `B_MEDIA_FLAVORS_CHANGED`) | clang (Renga headers) | – | – | – | A saved device that is unplugged stays listed as "(not connected)" and is picked up again when it returns (no polling; relies on Renga's flavor notifications); 3–5 and 7 channel layouts are remapped to OBS layouts. Unverified: the notifications arrive for USB and multi_audio devices |
| Desktop audio capture | Yes (loopback flavor of the system mixer add-on, `MixerTap`; float stereo at the mixer's rate; OBS left out with `renga:loopback_exclude`, O-1) | clang (Renga headers) | – | – | – | Records "what you hear" on every output except OBS's own streams (its monitoring), so monitored sources are recorded once and Desktop Audio can itself be monitored; it does not open if the exclusion cannot be set. Browser page audio is still recorded. Timestamps come from the mixer's time source (arrival time if none is usable). Source id unchanged (`haiku_desktop_audio_capture`) |
| Application audio capture (`haiku_application_audio_capture`) | Yes (loopback selected by application signature through the `renga:loopback_signature` parameter) | clang (Renga headers) | – | – | – | Lists applications playing audio and running GUI applications; a saved application that is not running stays selected as "(not running)"; OBS itself is excluded. Follows the application across restarts. Unverified: that OBS's Qt application registers a signature |
| Audio monitoring | Yes (one `BSoundPlayer` per source into the system mixer; routed by device name with the mixer's `renga:routes`, O-4) | clang (Renga headers) | – | – | – | Devices are the mixer's output names. A chosen device that is unplugged plays on the default output and is returned to when it appears (the mixer holds the route). Desktop Audio can be monitored (no feedback: it leaves OBS out). Unverified: that OBS registers an application signature, which routing and the exclusion are keyed by |
| Webcam / video capture devices | Yes (Media Kit; list reloads on `B_MEDIA_FLAVORS_CHANGED`; modes from `renga:video_modes`, rate as `field_rate`, O-5) | clang (Renga headers) | – | – | – | Lists every raw-video producer (including Demo Video Producer) except Renga's Virtual Camera (OBS's own output; shown as not available if saved, never opened); a saved camera that is unplugged stays listed as "(not connected)". Resolution and frame rate lists come from the camera's mode list, read when it is opened (until then "Device default" only); the granted rate is logged. Unverified: the UVC side on a real camera (Renga tested it with synthetic descriptors) |
| Screen capture | Yes (`BScreenCapture`: whole screen or region, damage-tracked frames, frame notification instead of polling, capped at the source's frame rate) | clang (Renga headers) | – | – | – | Permission asked asynchronously (`RequestPermission()`, O-7); OBS stays responsive while the prompt is open (O-6). A "no", an unanswered prompt and a start that wants to ask again are shown distinctly, with *Try again*. Needs OBS's application signature for the remembered permission. CPU copy per frame (O-10) |
| Window capture | Yes (`BScreenCapture` window session; follows moves/resizes, pauses when minimised, hidden or on another workspace from `NextFrame()`'s visibility flags (O-9), re-finds a closed window) | clang (Renga headers) | – | – | – | Not isolated: without a compositor (F-33) covering windows are captured too; *When the window is covered* can pause instead (fully or partly covered). Same permission and frame path as Screen capture |
| Browser source (helper `obs-haiku-browser`) | Yes (HaikuWebKit; helper IPC on a socketpair, sealed memfd frames, eventfd/epoll) | clang (Renga headers) | – | – | – | No WebGL; transparency depends on WebKit's alpha output. "Control audio via OBS" records the page through a loopback of the helper's team and routes its streams loopback-only (Renga F-58, O-2/O-3; unverified on hardware); the first sound of the first rerouted helper can reach the speakers for up to a second. The IPC stays a framed `SOCK_STREAM` (`SOCK_SEQPACKET` messages are limited to the socket buffer, see ARCHITECTURE.md section 10) |
| Browser docks (custom docks, service integrations) | Yes (QCef interface on HaikuWebKit; frames via a one-slot mailbox) | clang (Renga headers) | – | – | – | Cookies persist per cookie profile (mode 0600) and are shared live by all docks of a profile; session cookies (no expiry) are cleared in every dock whenever a new browser helper starts (HaikuWebKit behaviour); clearing/checking cookies from OBS needs libsqlite3. The IPC layer is new on this branch and unverified |
| Crash reports (browser helper) | Yes (log line naming the `debug_server` report in `/boot/system/var/log/crash`) | clang (Renga headers) | – | – | – | Renga F-51. A crashed helper stays stopped in `debug_server`, so it is detected on a missed ping and restarted within about 5–10 s. Unverified: report file naming and header format |
| Hardware encoding (`haiku-video-accel`: H.264, HEVC, AV1, VP9 on the Media Kit's `VideoAccelEncoder`) | Yes (offered only when a hardware back end encodes the codec) | clang (Renga headers) | – | – | – | No hardware back end exists yet (Quasar Q-3), so no encoder is offered by default. The plumbing is testable with Renga's software back end (`RENGA_VIDEO_ACCELERATION=software OBS_HAIKU_VA_ALLOW_SOFTWARE=1`), which encodes VP9 only. One CPU copy per frame; no B-frames, no CBR/VBR choice and no live bitrate changes (interface limits, O-11) |
| Virtual camera (`haiku-virtualcam`, output `virtualcam_output`) | Yes (feeds Renga's "Virtual Camera" Media Kit source through `BPrivate::VirtualCameraFeeder`, O-12/F-58) | clang (Renga headers) | – | – | – | Registered only when `virtual_camera.media_addon` is installed. One feeder at a time: if another application feeds the camera, starting fails with a message. B_RGB32 only, so OBS converts to BGRA on the CPU and copies each frame twice (into a one-slot mailbox, then into the shared memory); the camera scales to each consumer's size. No audio (the Media Kit camera is video only). HDR output is not tone-mapped. Do not capture the Virtual Camera in the OBS that feeds it |
| Zero-copy preview / GPU capture | No | – | – | – | – | Blocked: needs GPU buffers from capture (O-10), shared GPU textures in Quasar (Q-2) and F-47. The preview and capture paths copy through the CPU |
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
| `libobs/audio-monitoring/haiku/` | Audio monitoring through `BSoundPlayer` and the mixer's per-stream routing |
| `libobs-opengl/gl-haiku.c` | Offscreen swapchains (EGL framebuffer objects or OSMesa) with readback |
| `frontend/utility/*-haiku.cpp`, `frontend/widgets/OBSQTDisplay.*` | Frontend platform glue, preview painting |
| `plugins/haiku-capture/` | Screen capture, window capture (`BScreenCapture`) |
| `plugins/haiku-media/` | Audio input, desktop audio and application audio (mixer loopback), video capture devices |
| `plugins/haiku-browser/` | Browser source and docks (module `obs-browser`, helper `obs-haiku-browser`, IPC in `browser-ipc.hpp`) |
| `deps/qrcodegen`, `deps/srt-haiku`, `deps/librist-haiku` | Bundled builds of libraries HaikuPorts doesn't package |
| `build-aux/haiku/` | Dependency installer, build script, log collector, cross-check harness |
