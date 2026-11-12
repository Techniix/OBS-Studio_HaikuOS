# Haiku port architecture

This document explains how each part of OBS Studio was adapted to Renga (the Haiku-based OS in `Techniix/haiku`,
branch `ce/main`), why it was done that way, and where the code lives. This branch (`renga`) relies on Renga
features that stock Haiku lacks; stock Haiku is supported on `main`. Read this before changing Haiku-specific code.
What OBS still needs from Renga is tracked in `RENGA-REQUESTS.md` (request ids O-n, Q-n are used below).

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
8. [Desktop and application audio capture](#8-desktop-and-application-audio-capture)
9. [haiku-capture: screen and window capture](#9-haiku-capture-screen-and-window-capture)
10. [Browser source and docks](#10-browser-source-and-docks)
11. [Bundled dependencies: obs-websocket, SRT, RIST](#11-bundled-dependencies-obs-websocket-srt-rist)
12. [Other plugins](#12-other-plugins)
13. [Known limitations and ideas](#13-known-limitations-and-ideas)
14. [Design decisions](#14-design-decisions)
15. [haiku-video-accel: hardware encoding](#15-haiku-video-accel-hardware-encoding)
16. [haiku-virtualcam: virtual camera](#16-haiku-virtualcam-virtual-camera)

## 1. Build system

| File | What it does |
| --- | --- |
| `cmake/common/osconfig.cmake` | Sets `OS_HAIKU` when the target (`CMAKE_SYSTEM_NAME`) or the host is Haiku. Checked first so cross-compiling from Linux works. |
| `cmake/haiku/compilerconfig.cmake` | Reuses the Linux GCC/Clang flags. `CMAKE_COMPILE_WARNING_AS_ERROR` defaults to `OFF` because Haiku's headers and GCC 13 produce warnings upstream CI never sees. |
| `cmake/haiku/defaults.cmake` | Install layout (default prefix `/boot/home/config/non-packaged`; CMake's own Haiku default would be `/boot/system`, which is read-only). Sets `ENABLE_PULSEAUDIO`/`ENABLE_WAYLAND` to `OFF` as plain variables, and points FindSIMDe at the bundled `deps/simde` submodule. |
| `cmake/haiku/helpers.cmake` | Reuses the Linux target helpers (rundir layout, RPATH, install rules). Haiku's runtime loader understands ELF RPATH/`$ORIGIN`. |
| `cmake/haiku/finders/` | Prepended to the module path. `Find{qrcodegencpp,Libsrt,Librist}.cmake` run the upstream finder first and, if nothing is installed, build the bundled copy (section 11) and provide the same imported target name, so upstream CMake code is untouched. |
| `cmake/haiku/haiku-patch.cmake` | `haiku_copy_tree()`/`haiku_patch_file()`: copy a submodule's sources into the build tree and apply literal replacements, failing loudly if an expected snippet is missing. Submodules stay pristine. |
| `cmake/common/versionconfig.cmake` | Falls back to version `33.0.1-g<hash>` when `git describe` finds no release tag (fork clones often have none). |
| `libobs/cmake/os-haiku.cmake`, `frontend/cmake/os-haiku.cmake` | Platform sources and libraries (`be`, `media`, `locale`, `network`). |

C++ code is compiled with `_DEFAULT_SOURCE`. Haiku's `bsd/features.h` enables BSD/GNU extensions such as
`NI_MAXHOST` only when `__STRICT_ANSI__` is undefined, and `-std=c++17` (not `gnu++17`) defines it; Asio needs those
declarations.

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
  `data/system/data/KeyboardLayouts/Generic 105-key International` from the Haiku tree.
  Polling is cached for 4 ms because the hotkey thread asks for every bound key on each tick.
- **Modifier convention.** Haiku's Qt plugin reports Haiku's *Command* modifier as Qt's Control and Haiku's
  *Control* modifier as Qt's Alt (Command is what Haiku shortcuts use, like Ctrl elsewhere). libobs follows the same
  convention so hotkeys captured in the UI match what is polled: `OBS_KEY_CONTROL` = Command (raw keys 0x5d/0x5f),
  `OBS_KEY_ALT` = Control (0x5c/0x60), `OBS_KEY_META` = Option (0x66/0x67). Modifier state comes from the
  `key_info.modifiers` bits, so remapped modifier keys (Keymap preferences) behave as the user configured them. With
  Haiku's default settings the physical **Alt** key is Command, so an OBS hotkey shown as "Ctrl + R" is pressed with
  Alt+R.
- "Virtual keys" (`obs_key_from_virtual_key()`) are Haiku raw key codes.

Qt's Haiku platform plugin does not pass raw key codes through `QKeyEvent::nativeVirtualKey()`, so when you
bind a hotkey in Settings the frontend translates the Qt key instead (`HaikuObsKeyFromQtKeyEvent()` in
`frontend/utility/platform-haiku.cpp`). Shifted symbols are folded back onto the physical key on a US layout
(`!` → `1`, `?` → `/`, …) so the binding matches what `get_key_info()` reports. On other layouts some
punctuation keys may bind to a different physical key than expected; letters, digits, F-keys, the numpad and
navigation keys are layout independent.

## 4. OpenGL renderer

`libobs-opengl/gl-haiku.c`.

An OpenGL *window* surface on Haiku belongs to a BWindow/BView, and that view is driven by its window's
looper thread. OBS renders every display (preview, projectors, multiview, property previews) from
one graphics thread, and the frontend's widgets are Qt widgets rather than BViews. Rendering straight into
them isn't possible.

Instead every swapchain renders offscreen and the result is handed to the widget. Two Mesa backends provide
the offscreen buffers, chosen at startup (`OBS_HAIKU_GL=egl|osmesa` forces one):

| Backend | Mesa version | How |
| --- | --- | --- |
| EGL, surfaceless | Mesa 25+ (`mesa_eglvnd` + `libglvnd`, not yet published by HaikuPorts), or the Zink-on-NVK stack of the nvidia-haiku driver (`quasar` package) | One surfaceless EGL context; each swapchain is a framebuffer object with a color renderbuffer |
| OSMesa | Mesa 22.0.5 (current HaikuPorts `mesa`) | One OSMesa context; each swapchain is a memory buffer bound with `OSMesaMakeCurrent()` |

Mesa 22.0.5 ships `libEGL`, but HaikuPorts patches its Haiku EGL driver to fail `eglInitialize()` ("Does not
work"), so EGL is tried first and OSMesa is the fallback.

**Driver self-test.** A hardware driver can initialise fine and then hang or misrender once real work starts
(the nvidia-haiku Zink/NVK stack hung on a GPU fence in OBS's first frames, and later when a Screen Capture source
was added). Unless `OBS_HAIKU_GL` forces a backend, `gl-haiku.c` first runs `obs-haiku-gl-probe`
(`libobs-opengl/haiku-gl-probe/`, installed next to `obs`) as a separate process, with a timeout (8 s,
`OBS_HAIKU_GL_PROBE_TIMEOUT=<seconds>`). Pass: EGL is used. Hang, crash, wrong pixels: the probe is killed, the
reason is logged and OBS uses OSMesa, so a young or broken driver costs speed, not a frozen OBS. No EGL at all
(HaikuPorts' Mesa 22): the probe exits at once with status 2 and the normal EGL attempt fails fast into OSMesa.
Missing probe: EGL untested, as before. It runs at every launch, so a driver update is always re-tested;
`OBS_HAIKU_GL_PROBE=0` skips it. The probe also reports the driver's identity (below).

The probe does, call for call, what libobs-opengl does with a Screen Capture source in the scene and a recording
in progress, following `obs_graphics_thread_loop()` (`libobs/obs-video.c`); the header comment of `gl-probe.c`
lists every step with the libobs function it stands for. Per frame, paced like OBS's video thread (60 fps), the
render thread takes the context three times: `gs_begin_frame()`; `output_frame()`, which uploads a new 2560x1080
BGRX capture frame every other frame exactly like `gs_texture_set_image()` (the texture's own
`GL_PIXEL_UNPACK_BUFFER`, never orphaned, `glMapBuffer(GL_WRITE_ONLY)` while the previous upload may still be in
flight, `glTexImage2D` from the buffer), draws it into the canvas render target (`GL_SRGB8_ALPHA8` texture),
converts that into NV12 planes, reads them into pixel pack buffers with `glGetTexImage` and maps the previous
frame's buffers (`download_frame()`; they stay mapped outside the context until the next frame), then `glFlush`;
and `render_displays()`, which draws the canvas into the main preview's framebuffer object and the capture into
a properties dialog preview's (resized every 8 frames), each read back with `glReadPixels`. The capture texture is
recreated once mid-run while the GPU may still use the old one. A second thread takes the context now and then
to create and delete a texture and to open, close and reopen the properties preview's swapchain. No fence is
waited for until the very end, because libobs never waits on one: its synchronisation is implicit in
`glMapBuffer` and `glReadPixels`, which is where a driver with broken sync objects hangs. Every readback is
checked block by block, so stale uploads are caught too. A healthy hardware driver passes in about a second plus
EGL initialisation (60 frames at 60 fps).

Run by hand, the probe is a reproduction tool for driver work:

```
obs-haiku-gl-probe [--frames N] [--fps N] [--size WxH] [--canvas WxH] [--no-stream] [--no-stage]
                   [--no-second-display] [--no-handoff] [--fences] [--stall SECONDS] [--timeout SECONDS]
                   [--verbose]
```

| Option | Effect |
| --- | --- |
| `--frames N` | frames to render (default 60, at least 16) |
| `--fps N` | real-time pacing with a sleep between frames, like OBS's video thread (default 60); `0` renders as fast as possible |
| `--size WxH` | size of the captured screen (default 2560x1080) |
| `--canvas WxH` | OBS's base resolution, the canvas and NV12 planes (default 1920x1080) |
| `--no-stream` | upload the capture only once instead of every other frame (a static source) |
| `--no-stage` | no NV12 conversion and stage surface readback (OBS with no recording, stream or virtual camera active) |
| `--no-second-display` | no properties dialog preview |
| `--no-handoff` | no second thread taking the context; the properties preview is opened by the render thread |
| `--fences` | wait for a fence after the output frame and after the displays (OBS never does; for comparison) |
| `--stall SECONDS` | report a step that has not returned after this long and exit 5 (default 5; `0` never) |
| `--timeout SECONDS` | `alarm()` after this long plus 5 s (default 30; `0` never) |
| `--verbose` | print every step before it runs (`frame 12 [render]: download_frame(): glMapBuffer(...)`) |

Exit status: 0 pass, 1 usage, 2 no EGL display, 3 no usable OpenGL 3.3 core context, 4 wrong pixels or a GL
error, 5 a fence or a driver call did not return. A stuck step ends the run with `STALL:` lines on stderr, one
per thread; the last one is the step stuck longest that holds the context, normally the call inside the driver
(a thread only waiting for the context is listed first). OBS passes `--stall` at half its timeout so that report
ends up in the OBS log before the probe is killed. To isolate a hang, start from the failing default run and
switch parts off one at a time, for example `--no-stage`, then `--no-handoff --no-second-display`, then
`--no-stream`; `--fps 0` versus the default shows whether real-time pacing matters, `--fences` whether explicit
fence waits hide the problem, `--frames 600` gives a 10 s sustained run.

**Driver hang watchdog.** A driver can still hang on a path the probe does not reach. With EGL, every
`device_enter_context()` (called once per outermost `gs_enter_context()`, under libobs's graphics mutex) records
the thread, the time and a hold sequence number, and `device_leave_context()` clears the time; a watchdog thread
samples them once a second. A hold is declared a driver hang only if, for 15 s (`OBS_HAIKU_GL_WATCHDOG=<seconds>`,
`0` disables it, minimum 3):

- it is the same hold the whole time (the graphics thread sleeps between frames *outside* the context, so an
  idle or minimised OBS never holds it), counted in the watchdog's own ticks: a tick longer than 3 s (the team
  stopped by a debugger or `SIGSTOP`, the machine stalled) restarts the observation;
- the holding thread was blocked (waiting or sleeping, `get_thread_info()`) in at least 9 of 10 samples and used
  less than a quarter of the time in CPU. A thread that computes (shader compilation, a slow plugin) is logged once
  as busy and not treated as a hang; a suspended thread restarts the observation;
- no debugger is attached to the team (`team_info.debugger_nub_thread`; this includes the debug_server's crash
  dialog, so a thread that crashed with the context is not blamed on the driver);
- the context was entered at least 300 times before (startup effect compilation is one long hold).

What is not detected: a driver that spins instead of blocking (OBS freezes as without a watchdog), and an
OBS-internal deadlock while a thread holds the context is reported as a driver hang (OBS is frozen either way).

On a hang it logs `Haiku GL watchdog: the graphics driver stopped responding: ...` with the holding thread,
`GL_RENDERER` and both driver identities, writes the hang record `~/config/settings/obs-studio/haiku-gl-hang` at
once (a GPU hang may need a reset) and shows a native alert ("The graphics driver stopped responding. ...").
The alert is modal for OBS's windows only and never acts by itself; its default button (Enter, Escape) is
**Keep waiting**, the other is **Restart with CPU renderer**, or **Quit OBS** without libOSMesa. Keep waiting asks
again after a minute at the earliest. If the driver recovers, the alert closes, the record is removed again (unless
it existed before this session) and nothing else happens. Restart starts `/bin/sh` with `load_image()` (a new
team, not a fork of the hung one); it waits for the hung team to end (at most 20 s, so OBS's single-instance lock
is free) and runs the same executable with the arguments libobs saved from `main()` (`obs_get_cmdline_args()`,
each passed as its own word) and `OBS_HAIKU_GL=osmesa`, so the new instance does not touch the hung driver at all.
OBS then ends its own team (`kill_team()`), which also ends the thread stuck in the driver. The new instance
shows OBS's unclean-shutdown dialog. Without a `BApplication` no alert is shown and OBS keeps waiting (the log
still has the details).

**Driver identity and the hang record.** The identity (`haiku-gl-probe/gl-identity.h`, shared by the probe and
`gl-haiku.c`) is `GL_RENDERER`, `GL_VERSION`, path, size and modification time of every image in the team that
belongs to the GL/Vulkan stack (path contains `/vulkan`, `libvulkan`, `libEGL`, `libGL`, `/dri/`, `mesa`,
`gallium`, `zink`, `nouveau`, `nvk`, `nvidia` or `glvnd`), the Vulkan ICD manifests in `VK_DRIVER_FILES` /
`VK_ICD_FILENAMES`, and the kernel graphics drivers (`add-ons/kernel/drivers/dev/graphics`), sorted; its id is the
64-bit FNV-1a hash. The same stack looks different from the probe and from OBS (OBS has more images loaded), so
the record holds both: `identity` from the probe and `process-identity` computed in OBS once EGL is up (always).
At startup, with nothing forced, the record is compared like with like: the probe's identity with `identity` if
the probe passed and the record has one, otherwise OBS's own identity with `process-identity`. A match makes OBS
use OSMesa and logs, at every such start, `Haiku GL: the hardware driver hung in a previous session; using the CPU
renderer. Update the driver or delete <path> to use it again (OBS_HAIKU_GL=egl uses it for one session).`; a
mismatch means the driver stack changed: the record is removed and EGL is used. A record with only a probe
identity while the probe did not run is honoured (logged). `OBS_HAIKU_GL=egl` ignores the record and keeps it.
The record is written to a temporary file, synced and renamed into place. Without OSMesa (Mesa 25 alone) the
driver is used anyway, with a warning.

**OSMesa.** HaikuPorts builds Mesa 22 with `-Dosmesa=true` on the
llvmpipe (`swrast` + LLVM) driver, which provides OpenGL 3.3 core and beyond. `libOSMesa` is loaded with
`dlopen()` (Mesa 25 no longer has it), so one binary works with either Mesa.

EGL pbuffers are deliberately not used: Mesa's Haiku EGL driver reads the size of every surface it makes current from
the surface's window (`BitmapHook`), and a pbuffer has none. Instead the context is made current without a surface
and `libobs-opengl/gl-subsystem.c` asks `gl_platform_default_framebuffer()` (a Haiku-only hook) for the framebuffer
object that stands for "framebuffer 0" of the current swapchain.

Nothing is decided at build time: the same build uses EGL on a machine with a working Mesa EGL (e.g. NVIDIA GPUs with
nvidia-haiku) and OSMesa everywhere else.

1. One context (desktop OpenGL 3.3 core). A 1×1 surface/buffer is current when no swapchain is.
2. Each swapchain gets its own framebuffer object or OSMesa buffer the size of the widget. `device_load_swapchain()` makes it
   current, so OBS's "render to framebuffer 0" lands in it. `device_resize()` → `gl_update()` reallocates it.
3. `device_present()` reads it back with `glReadPixels(GL_BGRA)` (from the swapchain's framebuffer object, or `GL_FRONT` of
   the single-buffered OSMesa framebuffer) and calls `gs_window.present(param, top_row, width, height, stride)` with
   the rows as GL returns them (bottom-up): `top_row` points at the last row in memory and `stride` is negative, so
   the consumer's row copy (`OBSQTDisplay::HaikuPresent()`) flips the image.

`struct gs_window` on Haiku (in `libobs/graphics/graphics.h`) is therefore a callback rather than a native
handle:

```c
struct gs_window {
	void (*present)(void *param, const uint8_t *top_row, uint32_t width, uint32_t height, int32_t stride);
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

Each monitored source gets one `BSoundPlayer` connected to the system mixer, asking for float32 interleaved
audio at OBS's sample rate and channel count, with a channel mask matching OBS's speaker layout. The mixer
converts to the sound card, applies the master volume and mixes OBS's players with every other application.

- libobs's audio thread converts the source's planar float audio to interleaved (`audio_resampler`), applies
  the source volume and appends it to a FIFO.
- The player's realtime thread (`haiku-sound-player.cpp`) pulls 10 ms buffers out of it.
- **Monitoring device.** The device list is the mixer's output names, which are stable across restarts. OBS's
  streams are routed by device name through the mixer's `renga:routes` parameter
  (`AudioStreamControl::SetRoute(inputId, name)`, Renga F-58 / O-4); "Default" is the empty route. The mixer takes
  the name of a device that is not plugged in: the stream plays on the default output meanwhile and moves to the
  device when it appears, with nothing for OBS to do. The route is a property of each mixer input, saved per
  application signature, and a new input of OBS inherits it. Setting it on one stream does not move streams that
  are already connected, so after each player starts a worker thread (`obs_haiku_route_streams_async()`) sets it
  on every OBS stream; that also brings back a route changed elsewhere (Media preferences, Deskbar).
- OBS needs an application signature for the mixer to identify its streams by; if none is found the player stays
  on the default output and a warning is logged.
- Desktop Audio leaves OBS's streams out (section 8), so monitoring never comes back through it.
  `devices_match()` therefore answers "no" for every pair: libobs neither skips monitoring Desktop Audio itself
  nor silences monitored sources in the output mix (its deduplication for an audio output capture that records
  the monitoring device). Only if the registrar knows no signature for OBS is Desktop Audio not monitored, since
  the loopback could then not tell OBS's streams apart.

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
- **Hot-plug.** The input and camera lists reload on `B_MEDIA_FLAVORS_CHANGED`, which Renga's media add-on server,
  `usb_webcam` and `multi_audio` send when devices come and go, so there is no health polling. A saved device that
  is unplugged stays listed as "(not connected)" and is taken up again when it returns (the recorder supervisor
  retries with backoff). Cameras list every raw-video producer, including Demo Video Producer, except Renga's
  "Virtual Camera" (`VIRTUAL_CAMERA_FLAVOR_NAME`), which shows OBS's own output: a saved source that names it is
  listed as not available and not opened, and a default video input that is the Virtual Camera (by flavor name,
  or by its `renga:virtual_camera` status parameter) is refused.

**Audio Input Capture** (`haiku_audio_input_capture`): float, 32-bit, 16-bit and 8-bit PCM, 1–8
channels, host byte order. Channels are mapped to an OBS speaker layout from the stream's `channel_mask`
(see Desktop Audio below). Buffers are timestamped on arrival.

**Video Capture Device** (`haiku_video_input_capture`): `B_RGB32`, `B_RGBA32`, `B_RGB24` and `B_YCbCr422`
(as YUY2, partial range). Haiku's `usb_webcam` add-on produces `B_RGB32`. "Deactivate when not showing"
disconnects the device while the source isn't visible.

- **Resolution and frame rate** (Renga F-58 / O-5). When the capture thread opens a camera it reads the node's
  hidden read-only text parameter `renga:video_modes` (`<w>x<h> <encoding> <fps>,<fps>,...` per line, fastest
  first) into a cache. The properties, built on the UI thread, read only that cache: *Resolution* lists the
  sizes, largest first, and *Frame rate* the rates of the chosen size (a modified callback refills it), both
  with "Device default". A camera not opened yet in the session offers "Device default" only; the properties
  reload when the camera's list is read. For a size in both YUY2 and MJPEG, the YUY2 rates are listed, because
  `usb_webcam` uses YUY2 for such a size. A producer without the parameter gets "Device default" only, labelled
  with the size its `FormatSuggestionRequested()` suggests (`GetFormatFor()` of its output).
- The connection asks for the size and the rate as `field_rate`; the camera uses its nearest rate and the
  connected format's `field_rate` is logged. The accepted format leaves `field_rate` open (the output's format
  still carries the producer's old 5 fps, which a fixed value would refuse against the rate the camera
  proposes); "Device default" asks for `field_rate` 0, the camera's default.

The frontend uses `haiku_audio_input_capture` for the global Mic/Aux devices in *Settings → Audio* and
`haiku_desktop_audio_capture` (section 8) for the Desktop Audio devices (`frontend/OBSApp.cpp`).

## 8. Desktop and application audio capture

Renga's Media Kit keeps every application's stream apart in the system mixer and offers loopbacks as ordinary
Media Kit sources: a loopback flavor of the mixer add-on produces "what you hear", or the streams of selected
applications. OBS records one like any other input, so no node is inserted into the playback path and nothing
has to be restored if OBS dies.

```
application streams ──► system mixer ──► output(s) ──► sound card
                              │
                              └─► loopback flavor ──► BMediaRecorder ──► Desktop Audio / Application Audio source
```

**Desktop Audio** (`haiku_desktop_audio_capture`, `plugins/haiku-media/loopback-audio.cpp`; the source id is
unchanged, so scene collections and the frontend's desktop channels keep working). Each source instantiates its
own loopback flavor of the mixer add-on and records it through the plugin's recorder and supervisor (the same
ones Audio Input Capture uses, including reconnecting with backoff). It asks for float stereo at the mixer's own
rate, so the loopback is a plain copy (`MixerTap`) and libobs resamples.

- **Timestamps.** A buffer's `start_time` is converted from the mixer's time source to real time and then to
  `os_gettime_ns()` (`MixerClock`). If there is no usable time source, buffers are stamped on arrival.
- **OBS's own audio is left out.** Before the loopback starts, its `renga:loopback_exclude` parameter (Renga
  F-58 / O-1) is set to `application/x-vnd.obs-studio` (the signature in the executable's resources) and to the
  signature the registrar has for OBS if that differs; the source does not open if the parameter cannot be set.
  OBS's streams are its audio monitoring, so a source set to "Monitor and Output" is recorded once, and Desktop
  Audio can itself be monitored without feedback. The browser helper's streams are not left out: page audio
  reaches recordings through Desktop Audio. The source keeps `OBS_SOURCE_DO_NOT_SELF_MONITOR`, which marks it
  for libobs as an audio output capture (section 6).

**Application Audio Capture** (`haiku_application_audio_capture`, same file). The source selects one
application:

- The list holds applications playing audio (read from the mixer's parameter web through Renga's
  `AudioStreamControl.h`) and running GUI applications. OBS's own signature is excluded, and a *Refresh* button
  rebuilds the list. A saved application that is not running stays selected as "(not running)".
- The selection is written to the loopback's `renga:loopback_signature` parameter. The mixer compares
  signatures every cycle, so the capture follows the application across restarts.
- The recorded sound is taken before the master volume, so it does not change with the speaker volume.
- Per-source audio of a browser source ("Control audio via OBS") uses its own loopback of the helper's team (section 10); its helper runs under a second signature until Renga reports stream teams (O-17, O-18).

## 9. haiku-capture: screen and window capture

Renga's app_server has a capture API (roadmap F-31 phase 3, `BPrivate::BScreenCapture`): sessions for the screen,
a region or a window, with a per-application permission, a recording indicator, and damage tracking so a frame
copies only what changed. Both sources are built on it (`capture-source.{hpp,cpp}`, shared by
`screen-capture.cpp` and `window-capture.cpp`). Renga behaviour relied on is cited in the code comments.

**Frame path.** One capture thread per source sleeps on one semaphore of its own, which it hands to the session
with `SetFrameNotification()` (F-58, O-8): the app_server releases it once when the source's area changes after
a frame was taken, and when the session is stopped. The same semaphore is released by `Hide()` (so the graphics
thread never waits for a sleep to run out), by the permission answer and by the *Try again* / *Start capture*
buttons. On each wakeup the thread calls `NextFrame()`, no faster than the source's frame rate (1–60 fps,
default 30; changes in between are collected by the app_server for the next frame), with a single grow-only
`B_RGB32` bitmap. The app_server copies into the bitmap only what changed since it was last filled (it keeps a
per-bitmap missing region), so one bitmap is enough for an asynchronous source. A frame reported as unchanged is
not output, and an idle screen costs nothing but one `NextFrame()` a second (the idle check, which notices a
window closed while it was not on screen). The next frame also waits at least as long as the last copy took, so
the app_server's drawing lock is never held back to back. The thread hands changed frames to libobs with
`obs_source_output_video()`; libobs copies the whole frame (the damage region is not used because libobs has no
partial update for asynchronous sources). Timestamps come from the frame's `system_time()`. The source is
unbuffered (`obs_source_set_async_unbuffered`): libobs' buffered pacing would stall later frames by the length of
any gap, and an idle screen produces long gaps. A `LOG_DEBUG` line every 10 s reports wakeups, frames read,
changed and output frames, frames not shown because the window was not visible, and the average and maximum
`NextFrame()` time.

**Permission.** The permission is per application signature (`application/x-vnd.obs-studio`,
`frontend/cmake/haiku/obs-studio.rdef.in`), so it is asked once for the whole module, with
`BScreenCapture::RequestPermission()` (F-58, O-7). That call returns at once; the answer comes as a
`SCREEN_CAPTURE_PERMISSION_ANSWER` message to a small `BLooper` of the module, created on first need and quit and
joined in `obs_module_unload()`. Only after `B_OK` do the sources start their sessions, with a permission timeout
of 0, so hide, show and destroy on the graphics thread never wait on the prompt, and OBS's other app_server calls
are not held up while it is open (O-6).

- `B_NOT_ALLOWED` (a "no", now or remembered) shows *Not allowed* with *Try again*; `B_CANCELED` (the prompt was
  closed without an answer) shows its own text with *Try again*. A session start that returns `B_TIMED_OUT` (the
  permission was set back to "ask" since) asks again and shows *Waiting for your permission*, not a denial.
- After a "no" that was not remembered, or a prompt closed unanswered, the app_server keeps the answered (or
  orphaned) request and answers the next request from it, without a prompt. *Try again* therefore asks twice when
  the first answer is an immediate no (`capture-source.cpp`, `permission_answer()`; a Renga request).
- A stop from the recording indicator (`B_CANCELED` from `NextFrame()`) shows *Start capture*. A user stop is not
  undone automatically.
- Permission can be set outside OBS with the `ScreenRecording` preferences or
  `ScreenRecording --set application/x-vnd.obs-studio allow|ask`.

**Screen Capture** (`screen-capture.cpp`). The whole main screen or a region (`StartRegion`), with an optional
cursor. Changing either restarts the session. The capture API has no screen parameter until multi-monitor
support (F-38) exists. The source only runs while it is shown.

**Window Capture** (`window-capture.cpp`). `StartWindow(token)` captures the area of the screen at one window's
frame, with `SCREEN_CAPTURE_WINDOW_BORDER` for the border and tab option.

- Windows are listed with Renga's `WindowInfo.h` (`get_token_list()`, `get_window_info()`); OBS's own windows are
  skipped. The selected window is remembered as "application name + title" and found again after restarts.
- The session follows the window when it moves or is resized.
- `NextFrame()` reports whether the window shows (F-58, O-9). While it is minimised, hidden or on another
  workspace (`SCREEN_CAPTURE_FRAME_HIDDEN`, `_OTHER_WORKSPACE`) the source pauses and keeps the last picture.
  Partly or fully covered (`_COVERED`, `_FULLY_COVERED`), the copy shows the covering windows; *When the window is
  covered* chooses between capturing anyway (the default), pausing while it is fully covered and pausing while any
  part of it is covered.
- When the window is gone (`B_ENTRY_NOT_FOUND`) the picture is cleared and the window is looked up again until
  it reappears.
- Still not isolated: without a compositor (F-33), windows in front of the captured one are captured too.

**Cost.** The copy happens inside the app_server per changed frame and libobs uploads it again; GPU buffers
from capture would remove both (O-10, Quasar Q-2, F-47). Quasar's Q-1 (a fence hang with Screen Capture running)
is described in `RENGA-REQUESTS.md`.

`BScreenCapture` needs a connection to the app_server, which exists because Qt's Haiku plugin creates a
`BApplication`.

## 10. Browser source and docks

CEF (Chromium) has not been ported to Haiku, and obs-browser is built on it. The Haiku port replaces it with
`plugins/haiku-browser/`, using HaikuWebKit, the WebKit port behind WebPositive.

```
OBS process                                                obs-haiku-browser (one per page)
┌──────────────────────────────┐  AF_UNIX socketpair,     ┌────────────────────────────────────┐
│ browser_source / dock widget │  length-framed:          │ BApplication (WebCore main thread) │
│   HaikuBrowserClient         │  BMessages ────────────► │ BWebWindow parked off-screen       │
│   I/O thread (epoll)         │ ◄── event records        │   BWebView → offscreen BBitmap     │
│                              │ ◄── sealed memfd (frames,│ pump thread copies at source fps   │
└──────────────────────────────┘     SCM_RIGHTS) + eventfd└────────────────────────────────────┘
```

- **Module identity.** The module is built under the name `obs-browser` and registers `browser_source` with
  obs-browser's settings keys (`url`, `is_local_file`, `local_file`, `width`, `height`, `fps_custom`, `fps`, `css`,
  `shutdown`, `restart_when_active`, `reroute_audio`). Scene collections move between platforms unchanged, and the
  frontend finds the dock implementation through `obs_get_module("obs-browser")` → `obs_browser_create_qcef()`
  exactly as with CEF.
- **One helper per page** (`helper/browser-helper.cpp`): a hung or crashing page only affects its own process.
  `HaikuBrowserClient` restarts it with backoff.
- **IPC** (`browser-ipc.hpp`), built on the Linux primitives Renga provides (`memfd_create` with sealing,
  `eventfd`, `epoll`, `SCM_RIGHTS` over `AF_UNIX`):
  - *Channel.* One `AF_UNIX` `SOCK_STREAM` socketpair per helper, length-framed. Renga's `SOCK_SEQPACKET` (with
    `SCM_RIGHTS` since F-58, O-13) would keep message boundaries, but a message must fit in the receiver's socket
    buffer (64 KiB by default, 128 KiB at most), and a bigger one is not refused with `EMSGSIZE`: the send waits
    for room forever. OBS-to-helper messages carry custom CSS and scripts (up to 16 MiB), so the stream stays.
    OBS to helper: flattened `BMessage`s handled by the existing handlers; messages sent before the helper's looper
    runs wait in the socket. Helper to OBS: a fixed binary event record, so OBS never unflattens data a page could
    shape. A control message with more descriptors than expected (`MSG_CTRUNC`) ends the helper.
  - *Spawn.* `posix_spawn` without file actions (with file actions Haiku's `posix_spawn` forks OBS instead of using
    `load_image`); the helper's descriptors are made inheritable only for the call, under a lock.
  - *Frames.* For each buffer size the helper creates a memfd, seals it (shrink, grow, future write, seal) and
    sends it with `SCM_RIGHTS`. OBS checks the seals, maps it read-only and validates every header field. The
    hand-off uses a sequence lock; a torn copy is dropped because the next frame wakes the reader again. An
    `eventfd` wakes OBS for each frame. A page that grows simply gets a new memfd; there is no area switching.
  - *OBS side.* One I/O thread per client waits in `epoll_wait` on the socket, the frame eventfd and a stop
    eventfd. Sources output frames directly; docks receive them through a one-slot mailbox and a queued
    `invokeMethod` (no polling timer, no `QSocketNotifier`).
  - *Liveness.* Helper death is end-of-file on its socket and is seen at once. OBS death is end-of-file in the
    helper, which quits. Stalls are found with a ping every 5 s that must be answered on WebCore's main thread
    within 30 s. One module-wide reaper joins finished helpers and is joined in `obs_module_unload`.
- **Crash reports** (Renga F-51). When a helper dies or stops answering, OBS looks in `/boot/system/var/log/crash`
  for an `app-obs-haiku-browser-*.txt` newer than the helper's start whose header names its team, and logs one
  line with the report path (shown in System logs). A crashed helper stays stopped in `debug_server` with its
  socket open, so the check also runs on each missed ping, and the helper is restarted within about 5-10 s.
- **Rendering.** HaikuWebKit renders every page into the `BWebView`'s offscreen bitmap (it is built without
  accelerated compositing, so that bitmap is the complete page). The helper reads it with
  `WebCore::DumpRenderTreeClient::getOffscreen()`, the exported accessor HaikuWebKit's test runner uses, and copies it
  into the current memfd under a sequence lock. The window must stay *shown*: hiding a `BWebView` marks the page
  invisible and WebCore stops its timers. It is borderless, avoids focus and sits at (-30000, -30000).
- **Header constraints.** `haikuwebkit_devel` installs `WebFrame.h`, but that header includes WebCore internals that
  aren't shipped, so it can't be used outside WebKit (WebPositive doesn't use it either). `helper/webkit-abi.h`
  declares the three exported functions the helper needs (`BWebFrame::SetTransparent`, `BWebFrame::GlobalContext`,
  `DumpRenderTreeClient::getOffscreen`) with identical signatures, plus the public JavaScriptCore C API.
- **Transparency.** The frame is made transparent (`BWebFrame::SetTransparent`) and the default CSS sets a
  transparent background, as with obs-browser. The offscreen bitmap is `B_RGB32`; if a transparent page comes back
  with alpha 0 everywhere the alpha channel was not written and the frame is shown opaque instead of invisible.
- **Scripts and CSS** run on the application thread (WebCore's main thread) through `JSEvaluateScript`. Custom CSS is
  injected as a `<style>` element on load commit and load finish. `window.obsstudio.pluginVersion` and the
  `obsSourceVisibleChanged`/`obsSourceActiveChanged` events are provided.
- **Interaction.** Mouse, wheel and keyboard events are posted directly to the `BWebPage` handler (its
  `MessageReceived` builds WebCore events from `be:view_where`, `buttons`, `bytes`, `key`, …). `BWebView`'s own
  hooks can't be used: they read the window's current message and, for the wheel, the real pointer position.
  `BWebPage` inherits `BHandler` privately; the C-style cast in `PageHandler()` is the conversion C++ allows to an
  inaccessible base. The Haiku frontend passes Haiku raw key codes in `obs_key_event.native_vkey`
  (`OBSBasicInteraction.cpp`) so navigation keys can be translated to Haiku's control bytes.
- **Audio** (`browser-audio.{hpp,cpp}`). Without "Control audio via OBS" (`reroute_audio`) the page plays through the
  system mixer like any application and Desktop Audio records it; the source outputs no audio and is hidden from
  the audio mixer (`obs_source_set_audio_active()`), as with obs-browser. With it, the source records its page's
  sound and nothing else hears it, through Renga F-58:
  - *Recording (O-3).* The source instantiates its own "What you hear (loopback)" flavor and sets
    `renga:loopback_team` to the helper's team (`AudioStreamControl::SetLoopbackTeam()`) before connecting a
    `BMediaRecorder` (float stereo at the mixer's rate). Time stamps are converted from the mixer's time source as
    in Desktop Audio. When the helper is restarted, the loopback is pointed at the new team.
  - *Off the speakers and out of Desktop Audio (O-2).* The helper's streams are routed "None (loopback only)"
    (`SetLoopbackOnly()`). The mixer's parameter web names a stream's application by signature only and remembers
    routes per signature, so a rerouted source's helper registers as
    `application/x-vnd.obs-studio-browser-helper-rerouted` (`--audio-rerouted`); other helpers and docks keep the
    ordinary signature and their routes are never touched. Because the mixer remembers the route, a restarted
    helper's streams start loopback-only. A worker thread looks at the mixer's inputs once a second and reads the
    parameter web when they change (or every 10 s), as a safety net for the first use and for routes changed by
    hand. No Media Kit call is made on the UI or graphics thread, and destroying the source does not wait for the
    media server.
  - Changing the option restarts the helper (obs-browser recreates its browser too). Docks have no audio
    handling.
- **Docks** (`browser-panel.cpp`) implement `QCef`/`QCefWidget` from obs-browser's `panel/browser-panel.hpp` (the
  only file used from that submodule). The widget paints the latest frame and forwards Qt input; title and URL
  changes come back as helper events and are emitted as `titleChanged`/`urlChanged`. Cookie managers map to a
  per-panel WebKit storage directory.

## 11. Bundled dependencies: obs-websocket, SRT, RIST

HaikuPorts doesn't package everything OBS needs. Rather than disable features, the missing libraries are built from
source when no system copy is found (`cmake/haiku/finders/`):

| Library | Source | Haiku changes |
| --- | --- | --- |
| qrcodegen (C++) | `deps/qrcodegen` (Nayuki 1.8.0, MIT) | None |
| Asio 1.30.2, WebSocket++ 0.8.2 | submodules `deps/asio`, `deps/websocketpp` | None; HaikuPorts' Asio 1.38 dropped `io_service`, which WebSocket++ 0.8.2 uses |
| libsrt 1.5.4 | submodule `deps/srt`, built by `deps/srt-haiku` | `utilities.h`: Haiku uses the glibc-style `<endian.h>` branch. mbedTLS crypto. |
| librist 0.2.12 | submodule `deps/librist`, built by `deps/librist-haiku` | `endian-shim.h` as above; multicast joins use `ip_mreq` + `IP_MULTICAST_IF` resolved through `getifaddrs()` instead of Linux's `ip_mreqn` |

Patches are applied to copies in the build tree by `haiku_patch_file()`, which fails the configure step if an expected
snippet changed, so updating a submodule can't silently drop a fix.

## 12. Other plugins

| Plugin | Haiku changes |
| --- | --- |
| frontend-tools | Automatic Scene Switcher matches running application names (`BRoster`) because window titles of other teams aren't public API. |
| text-freetype2 | Uses fontconfig (`find-font-unix.c`) like Linux. |
| obs-outputs | Links `libnetwork`. |
| obs-scripting | Links libpython directly like on Linux. |
| obs-ffmpeg | The VA-API include block is skipped on Haiku. Otherwise portable. |
| obs-x264, obs-filters, image-source, obs-transitions, rtmp-services, obs-webrtc, vlc-video, obs-websocket | Portable, unchanged. |
| obs-browser | Replaced by `plugins/haiku-browser` (section 10). |
| AJA, DeckLink, NVENC, QSV, VA-API, linux-* | No Haiku drivers/SDKs; not built. |

## 13. Known limitations and ideas

- **Window capture isolation**: windows in front of the captured one are captured too until Renga has a compositor
  (F-33); the source can pause while the window is covered instead.
- **Capture permission**: *Try again* after a "no" that was not remembered, or after the prompt was closed
  unanswered, sends two requests, because the app_server answers the first from the leftover request.
- **Browser**: no WebGL (HaikuWebKit is built without it). With "Control audio via OBS", the very first sound of a
  rerouted helper can reach the speakers for up to a second before its stream is routed loopback-only (later
  helpers start loopback-only because the mixer remembers the route). JavaScript calls that need the frontend (`obsstudio.getCurrentScene`, `setCurrentScene`, …) are not
  implemented yet; `pluginVersion`, `getStatus`, `getControlLevel` and the visibility/activity events are.
- **Webcam modes**: the UVC side of `renga:video_modes` and of the rate choice is untested on a real camera
  (Renga tested it with synthetic descriptors and the demo producer). A camera's list is known only after the
  source has opened it once in the session.
- **Preview performance**: if `glReadPixels` is a bottleneck with llvmpipe at high preview resolutions, the readback
  could be made asynchronous with a PBO ring.
- **Hardware encoding**: none today (x264/FFmpeg software encoders only); needs the Media Kit hardware video API
  (O-11) and Quasar's back end (Q-3).
- **Virtual camera**: B_RGB32 only, so frames are converted to BGRA on the CPU and copied twice; one feeder at a
  time system-wide (section 16).
- **Zero-copy preview and capture**: needs GPU buffers from capture and shared GPU textures (O-10, Q-2, F-47).

## 14. Design decisions

Decisions taken during the port, with the reasons, so they can be revisited:

| Decision | Reason |
| --- | --- |
| Render every display into its own offscreen buffer (EGL pbuffer or OSMesa) and read it back | Haiku EGL window surfaces belong to BViews driven by their window thread; OBS renders all displays from one graphics thread into Qt widgets. |
| Browser rendering in a helper process | Crash isolation (WebKit). A sealed memfd per buffer size keeps the per-frame cost at one copy and lets OBS trust nothing a page could shape. |
| Desktop audio as a mixer loopback, not a node in the playback path | Renga's mixer offers loopbacks as Media Kit sources; nothing has to be restored if OBS dies. |
| Monitoring through the system mixer with its routes by device name | The mixer already mixes, converts, routes and follows hot-plug; OBS only chooses the output. |
| Capture through `BScreenCapture` sessions | Permission, indicator and damage tracking are in the app_server; no throttling of reads is needed. |
| Helpers started on demand by the plugins, not by the frontend | No frontend changes; works for any OBS profile and for headless use of libobs. |
| HaikuWebKit instead of porting CEF | CEF/Chromium is not available for Haiku and porting it is a project of its own; HaikuWebKit ships with every Haiku install. |
| Keep obs-browser's module name, source id and settings | Scene collections and the frontend's dock code work unchanged. |
| Window capture as a labelled screen-region capture | No per-window buffers exist in the app_server until the compositor (F-33); this is the closest faithful behavior. |
| Bundle libraries missing from HaikuPorts, patching copies at configure time | No feature has to be disabled, the submodules stay pristine, and system packages win as soon as they exist. |
| Haiku modifier convention (Ctrl = Command) | Matches Haiku's Qt plugin, so hotkeys captured in the UI are the ones polled. |
| EGL first, OSMesa fallback, OSMesa loaded at runtime | The Mesa HaikuPorts ships today (22.0.5) has EGL disabled; Mesa 25 has working EGL but no OSMesa. Supporting both means OBS works now and keeps working after the Mesa update. |
| Warnings not treated as errors | Haiku headers and GCC produce warnings upstream CI never sees. |

## 15. haiku-video-accel: hardware encoding

`plugins/haiku-video-accel` adds OBS video encoders on Renga's Media Kit video acceleration interface (roadmap
F-31 phase 5, `headers/private/media/VideoAcceleration.h`; request O-11). The interface is vendor neutral and
shaped like VA-API: back ends are add-ons in `add-ons/media/video_accelerators`, loaded by
`BPrivate::VideoAcceleratorRoster` (libmedia), and each one lists profiles with decode/encode entry points, the
largest picture and the surface formats (NV12, P010). The plug-in never talks to a driver, so it works with any
back end that appears (Quasar's NVIDIA back end, Q-3, or Mesa's video through LinuxKPI DRM later).

**Encoders.** `haiku_va_h264` (codec `h264`), `haiku_va_hevc` (`hevc`), `haiku_va_av1` (`av1`) and
`haiku_va_vp9` (`vp9`). VP9 is there because VA-API-class hardware encodes it, OBS records it into Matroska, and
it is the one codec of the four that Renga's software reference back end encodes; that makes it the only way to
exercise the plug-in without a GPU back end. Names show the back end: "Hardware H.264 (<back end name>)".

**Runtime check.** At module load (`haiku-video-accel.cpp`) the roster is asked, for each profile of each
codec, which back end encodes it (`VideoAcceleratorRoster::Find(profile, B_VIDEO_ACCEL_ENCODE)`). `Find()`
applies the user's choice: `~/config/settings/system/video_acceleration` (`backend <name>` or `backend off`),
overridden by `RENGA_VIDEO_ACCELERATION`; with neither, only back ends with `IsHardware()` are used. An encoder
type is registered only if that back end is hardware and offers the needed surface format (P010 for the 10-bit
profiles). The software back end is chosen only by name and is offered only if `OBS_HAIKU_VA_ALLOW_SOFTWARE=1`
is also set; such encoders are named "... (software test back end)" and their properties show a warning. The
log lists the back ends, the choice in effect and what was offered. A changed choice needs an OBS restart.
`CountAccelerators()` is called once at load because the roster's lazy loading takes no lock.

**Frame path.** `get_video_info()` makes libobs deliver NV12 (P010 for 10-bit profiles). Per frame:
`AcquireSurface()` (if every surface is in flight, packets are taken out and it is retried for up to about
200 ms), the two planes are copied row by row with the surface's own `bytesPerRow`, and `SendSurface()` hands the
surface back, forcing a key frame on the first frame and at every keyframe interval. `ReceivePacket()` is then
called until `B_WOULD_BLOCK`. A buffer that is too small (`B_NO_MEMORY`) is grown to the size the back end
reports. Packets are queued and returned one per `encode()` call. OBS pts (units of 1/fps_num s) are converted
to the interface's microsecond timestamps and mapped back through a table of recent frames. This is one CPU copy
per frame; GPU-texture input waits on surfaces importable from GPU buffers (O-10, O-11).

**Bitstream format.** OBS wants Annex B packets and an Annex B SPS/PPS(/VPS) header from `get_extra_data()`
(its flv/mp4 outputs convert it themselves); for AV1 the header is the sequence header OBU. The interface does not
define the format (`GetExtradata()` says "avcC, ...", and nothing about packets), so `va-bitstream.cpp`
accepts both: Annex B extradata is used as it is, an avcC/hvcC record is unpacked and its NAL length size is used
to turn length-prefixed packets into Annex B, and an av1C record yields its configOBUs. With no extradata, the
headers (and SEI) are taken out of the first packet with `obs_extract_avc_headers()` / `_hevc_` / `_av1_`, as
`obs-ffmpeg-vaapi.c` does. Key frames come from the bitstream for H.264/HEVC/AV1 (`obs_*_keyframe()`) and from
the back end's flag for VP9.

**Settings.** Rate control "Target bitrate" (kbps; `VideoAccelEncoderSetup::bitRate`) or "Constant quality"
(1-100, mapped to `quality` 0..1 with `bitRate` 0), keyframe interval in seconds (0 lets the back end choose)
and the profiles the back end encodes. The interface has no CBR/VBR choice, maximum bitrate, buffer size,
preset, B-frame count, colour signalling or reconfiguration, so none is offered; changes take effect at the next
start (`update()` only logs). There are no B-frames: the interface returns one timestamp per packet and no decode
timestamp, so `dts = pts`, and a back end that returns packets out of order is reported in the log.

**Failure and teardown.** `create()` returns NULL, with a log line, when the size is above the back end's
maximum or `CreateEncoder()` fails (busy device, unsupported setup). OBS then reports that the encoder could not
start, as it does for other encoders. `destroy()` flushes with `SendSurface(NULL)`, takes out the remaining packets
(bounded), and deletes the `VideoAccelEncoder`, which owns its surfaces. The accelerators belong to the roster
and are never freed by OBS. Each OBS encoder is used by one thread at a time (libobs), and nothing is shared
between encoders except the read-only type data.

## 16. haiku-virtualcam: virtual camera

`plugins/haiku-virtualcam` registers the output type `virtualcam_output`, the id the frontend looks for
(`frontend/utility/VCamConfig.hpp`), as `linux-v4l2` and `mac-virtualcam` do. It feeds Renga's "Virtual Camera"
(roadmap F-58, request O-12, Renga `d6f1bc7b`): a global flavor of the `virtual_camera` media add-on
(`B_BUFFER_PRODUCER | B_PHYSICAL_INPUT`, raw video in `B_RGB32`) that every other application opens like a camera
(CodyCam, the Media preferences, Chromium). Up to eight consumers show the same feed, each at the size and
`field_rate` it connected with; the node scales (nearest neighbour). Without a feeder it sends dark grey.

**Interface.** `BPrivate::VirtualCameraFeeder` from the header-only `headers/private/media/VirtualCamera.h`
(link `gnu` for `memfd_create()` and `network`): `Connect(width, height, fieldRate, slotCount)` creates a memfd
with a header and `slotCount` frame slots, connects to the local `SOCK_SEQPACKET` socket
`/boot/system/var/run/media/virtual_camera`, sends the memfd (`SCM_RIGHTS`) with the configuration and waits for
the node's reply: `B_OK`, `B_BUSY` when another program feeds the camera, `B_NO_INIT` when the socket does not
exist (no node: media services not running). `WriteFrame(bits, bytesPerRow, time)` copies a frame into the next
slot (a per-slot sequence number is odd while it is written; the node retries a copy that raced) and announces it
with a small message; on a send error it disconnects and returns the error. `Disconnect()` closes everything; the
node then goes back to "no signal". The feeder is not thread safe.

**Registration.** At module load the output is registered only if `virtual_camera.media_addon` exists in a media
add-ons directory (`find_paths_etc(B_FIND_PATH_MEDIA_NODES_DIRECTORY, ...)`). The frontend shows *Start Virtual
Camera* exactly when the type exists (`obs_get_output_flags()`), so it needs no Haiku code for that; this mirrors
the v4l2loopback check on Linux. A file check rather than a `BMediaRoster` query, so that the answer does not
depend on the media services running while OBS starts; if they are not running at *Start*, the start fails with
a message.

**Start.** The size is that of the `video_t` the frontend gives the output (`obs_output_set_media()`; program,
preview, scene or source per the virtual camera settings), and the field rate is its FPS. Sizes above 8192 are
refused with a message. `Connect()` runs on the starting thread (one local request and reply). The output asks
libobs for `VIDEO_FORMAT_BGRA` at that size, full range: `B_RGB32` is B, G, R, A in memory on little-endian x86;
BGRA rather than BGRX so the alpha byte is 255, as in the node's own frames. Errors go through
`obs_output_set_last_error()`, which the frontend shows in its "Failed to start virtual camera" box.

**Frames and threads.** `raw_video()` runs on libobs' video output thread, which renders for every output, and
must not wait. `WriteFrame()` can: its `send()` blocks while the node's socket buffer is full. So `raw_video()`
copies the frame into a buffer it owns and swaps it into a one-slot mailbox (an unsent frame there is
replaced), and a writer thread swaps the mailbox out and calls `WriteFrame()`. Three buffers change hands under a
mutex; no copy happens with the mutex held. The feeder is used by `start()` before the writer exists, then only
by the writer, then again after the writer is joined. OBS timestamps are `os_gettime_ns()`, which is
`CLOCK_MONOTONIC`, which Renga's libroot implements as `system_time()`; the slot time is the timestamp / 1000.

**Failure and stop.** If `WriteFrame()` fails mid-stream (media services restarted or crashed), the writer sets
the last error and calls `obs_output_signal_stop(OBS_OUTPUT_ERROR)`. libobs then ends the data capture without
calling the output's `stop()`, so the writer is joined by the next `start()` or by `destroy()`. The frontend's
`OBSBasic::OnVirtualCamStop()` (in a `__HAIKU__` block, though nothing in it is Haiku specific) shows the last
error when the stop code is not `OBS_OUTPUT_SUCCESS`; upstream shows nothing because its outputs never stop
themselves. `send()` is called without `MSG_NOSIGNAL`, so a broken socket raises SIGPIPE in the sending thread:
the frontend blocks SIGPIPE for all threads in `main()`, and the writer blocks it again for itself. `stop()` ends
the data capture, joins the writer (at most one `WriteFrame()` away) and disconnects. The frame buffers live until
the next `start()` or `destroy()`, because libobs ends the raw video callbacks on another thread after `stop()`
returns.

**Limits.** One CPU colour conversion (when OBS's output format is not BGRA) and two frame copies per frame
(into the mailbox, into the shared memory), plus the node's copy or scale per consumer. No audio: the camera is a
video source. HDR output is converted without tone mapping. One feeder system-wide. The Virtual Camera is also
listed by OBS's own *Video Capture Device* source; capturing it in the OBS that feeds it makes a feedback loop.
