# Requests from OBS Studio to Renga and Quasar

What OBS Studio for Renga (branch `renga` of `Techniix/OBS-Studio_HaikuOS`) needs from Renga
(`Techniix/haiku`, `ce/main`) and from Quasar (`Techniix/nvidia-haiku`, `renga`). This file is read by the
Renga and Quasar work to plan their side; the OBS branch never changes those repositories. Each request
states what is needed, a proposed interface and the reason. A request is a proposal until the other side
accepts it.

States: **Proposed**, **Accepted**, **Landed** (with the commit), **Withdrawn**.

Quasar records its side in `docs/APP-INTEGRATION.md` on its `renga` branch; the Quasar section below and
that file are kept consistent.

## Renga

| ID | Request | Area | State | Roadmap |
| --- | --- | --- | --- | --- |
| O-1 | Exclude applications from a loopback | Audio | Proposed | F-31 |
| O-2 | "Loopback only" stream route | Audio, browser source | Proposed | F-31 |
| O-3 | Loopback selection by team | Browser source | Proposed | F-31 |
| O-4 | Stream route to a device by name, kept while it is absent | Audio monitoring | Proposed | F-31, F-32 |
| O-5 | Camera mode lists and frame rates | Webcam | Proposed | F-31 phase 2 |
| O-6 | Screen capture permission without blocking the app_server link | Capture | Proposed | F-31 phase 3 |
| O-7 | Distinguish a permission timeout from a denial; asynchronous permission request | Capture | Proposed | F-31 phase 3 |
| O-8 | Frame notification instead of polling | Capture | Proposed | F-31 phase 3 |
| O-9 | Window visibility in capture frames | Capture | Proposed | F-31 phase 3, F-33 |
| O-10 | GPU buffers from capture (scanout, windows) | Capture | Proposed | F-47 (with Quasar R-2) |
| O-11 | Hardware video encode behind the Media Kit | Encoding | Proposed | F-31 phase 5 |
| O-12 | A virtual camera producer that applications can feed | Virtual camera | Proposed | F-31 |
| O-13 | SOCK_SEQPACKET coverage with SCM_RIGHTS in the socket tests | Platform | Proposed | F-15 |

### O-1 Exclude applications from a loopback

- **Need.** Desktop Audio records "What you hear", which includes OBS's own monitoring playback. A source set
  to "Monitor and Output" is then recorded twice (once directly, once through the loopback), with an echo.
- **Proposed interface.** A hidden text parameter of the loopback flavor, id 3, kind
  `renga:loopback_exclude`: a newline-separated list of signatures whose streams are left out. Empty: none.
- **Today in OBS.** Desktop Audio's properties say so; sources flagged `DO_NOT_SELF_MONITOR` are never
  monitored.

### O-2 "Loopback only" stream route

- **Need.** Per-source audio for browser sources ("Control audio via OBS"): the page's sound must reach OBS
  but not the speakers. `B_MUTE` silences the stream in the loopback too (`MixerInput.h`, the gain is
  `fEnabled ? gain : 0` even with `evenIfMuted`), and the "Output" mux only offers the default output or a
  device (`AudioMixer.cpp`).
- **Proposed interface.** A further "Output" choice, "None (loopback only)": the stream is left out of every
  output and out of all-streams loopbacks, and kept in loopbacks that select its application (or team, O-3).

### O-3 Loopback selection by team

- **Need.** Every browser source and dock is a helper process of the same executable, so they share one
  signature; per-source capture needs to pick one process. Per-instance signatures would work but fill the
  mixer's 50-entry per-signature settings store (`MixerSettings.h`).
- **Proposed interface.** A hidden text parameter of the loopback flavor, kind `renga:loopback_team`: a team
  id; streams of that team only. Empty or 0: by signature as today.

### O-4 Stream route to a device by name

- **Need.** OBS's monitoring device is chosen by name and stored in its settings. The "Output" parameter only
  accepts devices that exist when it is set, and libobs has no hook to re-apply the route when a device
  appears, so a headset plugged in after monitoring starts is not used until monitoring is reset.
- **Proposed interface.** A per-stream hidden text parameter `renga:route` taking a device name; the mixer
  routes the stream there whenever the device exists (falling back to the default output meanwhile, as
  routes stored per signature already do).

### O-5 Camera mode lists and frame rates

- **Need.** OBS lists resolutions and frame rates for a camera. Renga's UVC producer has no list: it accepts
  exact descriptor sizes only (`UVCCamDevice.cpp`), uses a fixed 30 fps interval and reports `field_rate`
  5 (`Producer.cpp`). OBS finds sizes by connecting with each candidate size (without starting), which costs
  buffer allocations and can leave a UVC output busy.
- **Proposed interface.** Either a parameter-web entry listing the camera's modes (width x height x frame
  interval x encoding), or `FormatSuggestionRequested(B_MEDIA_RAW_VIDEO, quality, ...)` returning each mode
  in turn; and `field_rate` in a connection's format honoured.

### O-6 Screen capture permission without blocking the app_server link

- **Need.** `BScreenCapture::_Start()` waits for the user's answer (up to 2 minutes) while holding the
  process-wide `AppServerLink` lock between its 100 ms polls (`ScreenCapture.cpp`). Every other app_server
  call in OBS waits meanwhile, including the UI's.
- **Proposed interface.** Take the link per poll attempt, or (better, O-7) an asynchronous request.

### O-7 Permission timeout vs. denial; asynchronous request

- **Need.** A timeout returns `B_NOT_ALLOWED`, like a "no"; OBS cannot tell the user which happened. OBS needs
  a helper thread and a 1-pixel probe session to ask without blocking.
- **Proposed interface.** `B_TIMED_OUT` for an unanswered prompt; `BScreenCapture::RequestPermission(
  BMessenger target, uint32 sources)` that returns at once and sends the answer as a message.

### O-8 Frame notification

- **Need.** OBS polls `NextFrame()` at the source's frame rate; an idle screen still costs one round trip per
  frame, and a change is seen up to one frame late.
- **Proposed interface.** `BScreenCapture::WaitForFrame(bigtime_t timeout)`, or a semaphore/port signalled on
  damage for the session (`SetFrameNotification(sem_id)`).

### O-9 Window visibility in capture frames

- **Need.** Window capture copies the window's area of the screen without a visibility check; OBS calls
  `get_window_info()` on changed frames to pause on minimised, hidden or other-workspace windows. Renga's own
  test notes app_server hangs when polling `get_window_info()` during window removal
  (`screen_capture_test.cpp`).
- **Proposed interface.** A `visible` flag from `NextFrame()` (or `B_WINDOW_NOT_VISIBLE` status), until the
  compositor (F-33) makes windows capturable while covered.

### O-10 GPU buffers from capture

- **Need.** Zero-copy capture: the scanout or a window's buffer as a GPU handle imported as a texture, instead
  of an app_server copy plus a libobs upload per frame.
- **Proposed interface.** As Quasar's R-2 and F-47 define it; OBS needs a handle it can import with EGL
  (`EGL_EXT_image_dma_buf_import`-style) or Vulkan external memory, plus the frame's damage and timestamp.

### O-11 Hardware video encode

- **Need.** OBS encodes in software today (x264). The Media Kit phase-5 hardware video API does not exist yet.
- **Proposed interface.** What phase 5 defines; for OBS the best fit is VA-API (libva) as the roadmap plans,
  because FFmpeg's `h264_vaapi`/`hevc_vaapi`/`av1_vaapi` encoders then work unchanged, with GPU-texture input
  through dma-buf import (O-10). OBS would offer the encoders only after a runtime check.

### O-12 Virtual camera

- **Need.** OBS's "Start Virtual Camera" makes OBS's output a camera other applications can open.
- **Proposed interface.** A Media Kit producer add-on (global flavor, listed among cameras by
  `GetDormantNodes()`) fed by one client process through shared memory (memfd) with timestamps, so CodyCam,
  Chromium and others see it as an ordinary camera.

### O-13 SOCK_SEQPACKET tests

- **Need.** The browser helper IPC would use `SOCK_SEQPACKET` (message boundaries for free) but the socket
  tests only cover `SOCK_STREAM` with `SCM_RIGHTS`, so OBS uses a stream socket with its own framing.
- **Proposed interface.** None new: a test case for `SOCK_SEQPACKET` socketpairs passing descriptors.

## Quasar

| ID | Request | State | Quasar reference |
| --- | --- | --- | --- |
| Q-1 | Fix the timeline-fence hang with OBS's Screen Capture load | Proposed | `docs/event-sync.md`, Open issues |
| Q-2 | Shared GPU textures between processes | Proposed | external memory (`RENGA-STATUS.md`) |
| Q-3 | Hardware video encode back end | Proposed | Video, Media Kit back end |
| Q-4 | Quiet driver logging by default | Proposed | Power and system |

### Q-1 Screen Capture hang

`obs-haiku-gl-probe` now follows libobs' per-frame call order (see `docs/haiku/ARCHITECTURE.md`, renderer
section). Reproduce with `obs-haiku-gl-probe --verbose`; isolate with `--no-stage`, `--no-stream`,
`--no-second-display`, `--no-handoff`, `--fences`, `--fps 0`. After `--stall` seconds without progress the
probe prints `STALL:` lines naming the stuck GL call. Exit 0 pass, 5 a fence or step never completed. The
test run (`build-aux/haiku/test.sh`) records which NVK/Mesa libraries OBS loads.

### Q-2 Shared GPU textures

Browser sources (later the Chromium port) and capture could hand frames to OBS as GPU memory instead of
shared CPU memory. Needs Vulkan/EGL external memory and semaphores between processes.

### Q-3 Hardware encode

The NVIDIA back end of O-11 (NVENC through the driver, or Vulkan Video encode in NVK).

### Q-4 Quiet logging

The driver prints `VideoStreamsWsi:` and RM version lines on every context creation; OBS's log and the test
archive fill with them.
