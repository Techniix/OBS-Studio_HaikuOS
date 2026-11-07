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
8. [Desktop audio capture](#8-desktop-audio-capture)
9. [haiku-capture: screen and window capture](#9-haiku-capture-screen-and-window-capture)
10. [Browser source and docks](#10-browser-source-and-docks)
11. [Bundled dependencies: obs-websocket, SRT, RIST](#11-bundled-dependencies-obs-websocket-srt-rist)
12. [Other plugins](#12-other-plugins)
13. [Known limitations and ideas](#13-known-limitations-and-ideas)
14. [Design decisions](#14-design-decisions)

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
work"), so EGL is tried first and OSMesa is the fallback. HaikuPorts builds that Mesa with `-Dosmesa=true` on the
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
   the single-buffered OSMesa framebuffer), flips it to top-down order and calls
   `gs_window.present(param, pixels, width, height, linesize)`.

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

The frontend uses `haiku_audio_input_capture` for the global Mic/Aux devices in *Settings → Audio* and
`haiku_desktop_audio_capture` (section 8) for the Desktop Audio devices (`frontend/OBSApp.cpp`).

## 8. Desktop audio capture

Haiku has no loopback device: the system mixer has one output, connected to the sound card's input. Capturing what
is played therefore means getting into that connection.

```
before:  system mixer ──────────────────────────────► sound card
after:   system mixer ──► TapNode ──► sound card
                            │ copy
                            ▼
                 area "obs-studio desktop audio tap" ──► Desktop Audio source(s) in OBS
```

**Helper process** `obs-haiku-audio-tap` (`plugins/haiku-media/audio-tap/`). It is a separate program so that a fault
in the real-time media path cannot take OBS down, and so the original routing is restored even when OBS crashes:

1. Creates the cloneable area (`audio-tap-shm.h`: header + 1 MiB ring). The area name doubles as a single-instance
   lock.
2. Saves the mixer's connected output (`GetConnectedOutputsFor`), registers `TapNode`, slaves it to the mixer's
   time source, disconnects mixer → card, connects tap → card (with the format the mixer and card were already
   using), then mixer → tap, and starts the node. On any failure the original connection is restored.
3. `TapNode` (`tap-node.cpp`, structured after Haiku's MIT-licensed Equalizer add-on) is a `BBufferConsumer` +
   `BBufferProducer` + `BMediaEventLooper`. Each buffer is `memcpy`'d into the ring and forwarded unchanged with
   `SendBuffer`; `SetBufferGroup` passes the card's buffers upstream so the mixer writes straight into them. What
   you hear is not modified.
4. A watchdog quits when no reader has written a heartbeat for 10 s. Quitting, `SIGINT`/`SIGTERM`/`SIGHUP` and the
   last OBS source going away all restore mixer → card. If the helper was killed with `SIGKILL`, the next start (or
   `obs-haiku-audio-tap --repair`) reconnects the mixer's free output to the card's free input.

**OBS source** "Desktop Audio (Media Kit)" (`plugins/haiku-media/desktop-audio.cpp`). A reader thread finds the area
(starting the helper with `posix_spawn` if needed), clones it, and every 10 ms converts new ring data to
`obs_source_audio` in the mixer's format (float/int/short/8-bit, 1–8 channels). It detaches and reconnects if the
helper exits or dies, skips ahead if it falls more than half a ring behind, and stops the helper when the last
desktop audio source is destroyed. The source has `OBS_SOURCE_DO_NOT_SELF_MONITOR`: monitoring plays into the very
mixer the tap captures, so monitoring Desktop Audio would feed back into itself.

Helpers are marked as background apps (`B_BACKGROUND_APP` via `BAppFileInfo`, set on first use) so they don't show
in the Deskbar.

## 9. haiku-capture: screen and window capture

**Screen capture** (`screen-capture.cpp`). A thread calls `BScreen::ReadBitmap()` into a `B_RGB32` bitmap at the
configured rate (1–60 fps, default 30) and outputs it as an async BGRX source, so the app_server copy never blocks
rendering. It captures the main screen with the cursor optional. The source only runs while it is shown.

**Reading the screen without freezing the desktop** (`capture-throttle.hpp`, used by both sources).
`BScreen::ReadBitmap()` runs in the app_server under its exclusive drawing lock. The software cursor is moved
under the same lock, and reading a frame buffer in video memory is slow (~25–30 ms for 2560×1080 on the
nvidia-haiku driver), so capturing stops the pointer for the length of each read. Two things limit the impact:

- A duty-cycle throttle keeps reads to at most 25% of wall time and lowers the capture rate (logging it) when
  the hardware can't keep up.
- A per-source **Capture method** setting. *Tear-free* (default) reads each frame in one piece: every frame is a
  consistent snapshot, and the pointer pauses briefly during each read. *Smooth pointer* reads in horizontal
  bands that hold the lock for ~2 ms each with a 1 ms yield in between: the pointer stays smooth, but something
  that moves during a read can show a seam in that frame.

Retrace synchronization (`BScreen::WaitForRetrace()`) doesn't help here: the seams come from the app_server
drawing between bands, not from the monitor's refresh. A complete fix would be in the app_server. It keeps a
copy of the screen in system memory (it is double-buffered with accelerant drivers), and serving `ReadBitmap()`
from that copy instead of from video memory would make a full-frame read take a few milliseconds. Patches that do
this are in `docs/haiku/upstream/` (not yet in Haiku).

With accelerant drivers that use the **software** cursor, the app_server composites it into the frame buffer, so it
can appear in the capture even with *Capture Cursor* off. With a **hardware** cursor (for example the nvidia-haiku
driver's `rtx5070-hw-cursor` branch), it isn't in the frame buffer, and `ReadBitmap(drawCursor)` adds it only when
asked, so the toggle works. The app_server patches in `docs/haiku/upstream/` make the toggle work with software
cursors too, except where the cursor is over a direct window or a window that is being redrawn (those areas are
read from the frame buffer).

**Window capture** (`window-capture.cpp`). The app_server composes windows straight into the frame buffer and keeps
no per-window backing store another team could read, so isolated window capture (like PipeWire or
`CGWindowListCreateImage`) is not possible on Haiku. The source is therefore a clearly labelled screen-region
capture that follows one window:

- Windows are listed with the private window-list API Deskbar and the Switcher use (`get_token_list()`,
  `get_window_info()`, `BPrivate::get_window_order()`; declarations copied in `haiku-window-info.h`). OBS's own
  windows are skipped.
- The selected window is remembered as "application name + title" and found again after restarts.
- Every frame, the window's current frame (optionally with border and tab) is read from the screen. Moving and
  resizing are followed; the source pauses while the window is hidden, minimized or on another workspace, and logs
  when other windows cover it (their pixels are captured too).

`BScreen` needs a connection to the app_server, which exists because Qt's Haiku plugin creates a
`BApplication`.

## 10. Browser source and docks

CEF (Chromium) has not been ported to Haiku, and obs-browser is built on it. The Haiku port replaces it with
`plugins/haiku-browser/`, using HaikuWebKit, the WebKit port behind WebPositive.

```
OBS process                                               obs-haiku-browser (one per page)
┌──────────────────────────────┐   BMessage: config,     ┌────────────────────────────────────┐
│ browser_source / dock widget │   input, JS, reload ──► │ BApplication (WebCore main thread) │
│   HaikuBrowserClient         │                         │ BWebWindow parked off-screen       │
│   area "obs browser <pid>.n" │ ◄── frames (seqlock) ── │   BWebView → offscreen BBitmap     │
└──────────────────────────────┘                         │ pump thread copies at source fps   │
                                                         └────────────────────────────────────┘
```

- **Module identity.** The module is built under the name `obs-browser` and registers `browser_source` with
  obs-browser's settings keys (`url`, `is_local_file`, `local_file`, `width`, `height`, `fps_custom`, `fps`, `css`,
  `shutdown`, `restart_when_active`, `reroute_audio`). Scene collections move between platforms unchanged, and the
  frontend finds the dock implementation through `obs_get_module("obs-browser")` → `obs_browser_create_qcef()`
  exactly as with CEF.
- **One helper per page** (`helper/browser-helper.cpp`): a hung or crashing page only affects its own process.
  `HaikuBrowserClient` restarts it with backoff, and restarts it if its heartbeat stops for 10 s. The helper quits
  when OBS's heartbeat stops for 5 s.
- **Rendering.** HaikuWebKit renders every page into the `BWebView`'s offscreen bitmap (it is built without
  accelerated compositing, so that bitmap is the complete page). The helper reads it with
  `WebCore::DumpRenderTreeClient::getOffscreen()`, the exported accessor HaikuWebKit's test runner uses, and copies it
  into the shared area under a sequence lock. The window must stay *shown*: hiding a `BWebView` marks the page
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
- **Docks** (`browser-panel.cpp`) implement `QCef`/`QCefWidget` from obs-browser's `panel/browser-panel.hpp` (the
  only file used from that submodule). The widget paints the latest frame and forwards Qt input; title and URL
  changes come back through the shared area and are emitted as `titleChanged`/`urlChanged`. Cookie managers map to a
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

- **Window capture isolation**: windows in front of the captured one are captured too. True per-window capture
  would need an app_server extension (e.g. letting a privileged client read a window's drawing), which is outside
  OBS.
- **Per-application audio capture**: the tap sees only the final mix. Per-application capture would need a tap per
  mixer input; the same node could be inserted there, but the mixer's input side has no stable public routing API.
- **Browser**: no WebGL (HaikuWebKit is built without it), page audio is not routed to the source ("Control audio via
  OBS" is shown disabled; the sound reaches recordings through Desktop Audio), cookies aren't persisted between
  helper runs. JavaScript calls that need the frontend (`obsstudio.getCurrentScene`, `setCurrentScene`, …) are not
  implemented yet; `pluginVersion`, `getStatus`, `getControlLevel` and the visibility/activity events are.
- **Webcam formats**: the device's current format is used; a resolution/format picker could use the node's
  `BParameterWeb` or format negotiation.
- **Preview performance**: if `glReadPixels` is a bottleneck with llvmpipe at high preview resolutions, the readback
  could be made asynchronous with a PBO ring.
- **Hardware encoding**: none on Haiku today (x264/FFmpeg software encoders only).

## 14. Design decisions

Decisions taken during the port, with the reasons, so they can be revisited:

| Decision | Reason |
| --- | --- |
| Render every display into its own offscreen buffer (EGL pbuffer or OSMesa) and read it back | Haiku EGL window surfaces belong to BViews driven by their window thread; OBS renders all displays from one graphics thread into Qt widgets. |
| Desktop audio and browser rendering in helper processes | Crash isolation (WebKit, real-time media path) and guaranteed restoration of the audio routing. Shared memory keeps the per-frame cost at one copy. |
| Helpers started on demand by the plugins, not by the frontend | No frontend changes; works for any OBS profile and for headless use of libobs. |
| HaikuWebKit instead of porting CEF | CEF/Chromium is not available for Haiku and porting it is a project of its own; HaikuWebKit ships with every Haiku install. |
| Keep obs-browser's module name, source id and settings | Scene collections and the frontend's dock code work unchanged. |
| Window capture as a labelled screen-region capture | No per-window buffers exist in the app_server; this is the closest faithful behavior. |
| Bundle libraries missing from HaikuPorts, patching copies at configure time | No feature has to be disabled, the submodules stay pristine, and system packages win as soon as they exist. |
| Haiku modifier convention (Ctrl = Command) | Matches Haiku's Qt plugin, so hotkeys captured in the UI are the ones polled. |
| EGL first, OSMesa fallback, OSMesa loaded at runtime | The Mesa HaikuPorts ships today (22.0.5) has EGL disabled; Mesa 25 has working EGL but no OSMesa. Supporting both means OBS works now and keeps working after the Mesa update. |
| Warnings not treated as errors | Haiku headers and GCC produce warnings upstream CI never sees. |
