# Requests from OBS Studio to Renga and Quasar

What OBS Studio for Renga (branch `renga` of `Techniix/OBS-Studio_HaikuOS`) needs from Renga
(`Techniix/haiku`, `ce/main`) and from Quasar (`Techniix/nvidia-haiku`, `renga`). This file is read by the
Renga and Quasar work to plan their side; the OBS branch never changes those repositories. Each request
states what is needed, a proposed interface and the reason. A request is a proposal until the other side
accepts it.

States: **Proposed**, **Accepted**, **Landed** (with the commit), **Withdrawn**.

Priority is OBS's view of the order to take them in: **1** fixes a limitation users meet in everyday use or
removes a fragile workaround; **2** enables a visible new feature; **3** is an improvement or test coverage;
**Roadmap** comes with work already planned in Renga or Quasar and needs nothing extra.

Quasar records its side in `docs/APP-INTEGRATION.md` on its `renga` branch; the Quasar section below and
that file are kept consistent.

## Renga

| ID | Request | Area | Priority | State | Roadmap |
| --- | --- | --- | --- | --- | --- |
| O-1 | Exclude applications from a loopback | Audio | 1 | Landed (`d97f5a34`) | F-31 |
| O-2 | "Loopback only" stream route | Audio, browser source | 2 | Landed (`34526255`) | F-31 |
| O-3 | Loopback selection by team | Browser source | 2 | Landed (`34526255`) | F-31 |
| O-4 | Stream route to a device by name, kept while it is absent | Audio monitoring | 1 | Landed (`805d2929`) | F-31, F-32 |
| O-5 | Camera mode lists and frame rates | Webcam | 2 | Landed (`ab6f163f`) | F-31 phase 2 |
| O-6 | Screen capture permission without blocking the app_server link | Capture | 1 | Landed (`b92ace62`) | F-31 phase 3 |
| O-7 | Distinguish a permission timeout from a denial; asynchronous permission request | Capture | 1 | Landed (`b92ace62`) | F-31 phase 3 |
| O-8 | Frame notification instead of polling | Capture | 3 | Landed (`23d36b26`) | F-31 phase 3 |
| O-9 | Window visibility in capture frames | Capture | 3 | Landed (`23d36b26`) | F-31 phase 3, F-33 |
| O-10 | GPU buffers from capture (scanout, windows) | Capture | Roadmap | Accepted (GPU stage, with R-2 and F-47) | F-47 (with Quasar R-2) |
| O-11 | Hardware video encode behind the Media Kit (`VideoAcceleration.h`) | Encoding | 2 | Interface landed (`446221a6`); GPU back end waits on Q-3 | F-31 phase 5 |
| O-12 | A virtual camera producer that applications can feed | Virtual camera | 2 | Landed (`d6f1bc7b`) | F-31 |
| O-13 | SOCK_SEQPACKET coverage with SCM_RIGHTS in the socket tests | Platform | 3 | Landed (`fcac2cd7`) | F-15 |
| O-14 | `VideoAccelEncoder` details: bitstream formats, dts/B-frames, rate control, colour, surface release, roster locking | Encoding | 2 | Proposed | F-31 phase 5 |
| O-15 | H.264 (and HEVC/AV1) in the software reference back end | Encoding | 3 | Proposed | F-31 phase 5 |
| O-16 | A leftover permission request must not answer the next request | Capture | 3 | Landed (`a1378d54`) | F-58 follow-up |
| O-17 | Team of each mixer stream | Audio, browser source | 2 | Landed (`cbd2a2c8`) | F-58 follow-up |
| O-18 | Loopback-only routing by team, not remembered | Browser source | 2 | Landed (`cbd2a2c8`) | F-58 follow-up |
| O-19 | Refuse oversize UNIX datagram/seqpacket messages with EMSGSIZE | Platform | 3 | Landed (`a220f582`) | F-58 follow-up |
| O-20 | A timeout for VirtualCameraFeeder::WriteFrame() | Virtual camera | 3 | Landed (`677afa3d`) | F-58 follow-up |
| O-21 | Recording size once per team for the recording indicator | Capture | 3 | Landed (`b734d8f4`) | F-59 follow-up |

**Schedule (Renga, 2026-11-11).** O-1 to O-9, O-12 and O-13 are accepted as roadmap batch F-58, after the
Media Kit is finished, in this order: O-1; O-7 with O-6 (including an asynchronous
`BScreenCapture::RequestPermission(BMessenger, uint32)`); O-4; O-5 and O-12; O-2 and O-3; O-8 and O-9; O-13.
Items land one commit at a time; each is marked Landed here with its commit, and any interface difference from
the proposal is noted. O-10 is part of the GPU stage with Quasar's R-2 and F-47.

**Landed (Renga, 2026-11-12, batch F-58).** All of O-1 to O-9, O-12 and O-13 are on `ce/main`; Renga's notes
for OBS are in `docs/partners/OBS.md` there. Interface differences from the proposals:
- O-1: the exclusion parameter is id 4 (look it up by kind `renga:loopback_exclude`); helper
  `AudioStreamControl::SetLoopbackExclusions()`.
- O-4: no per-stream parameter; one hidden mixer text parameter `renga:routes` (`<input id> <device>` lines),
  helper `AudioStreamControl::SetRoute()`.
- O-2/O-3: route `renga:loopback-only` (`SetLoopbackOnly()`), loopback kind `renga:loopback_team` id 5
  (`SetLoopbackTeam()`).
- O-5: hidden read-only `renga:video_modes` text parameter on the camera node; rate through `field_rate`.
- O-7: `BScreenCapture::RequestPermission(BMessenger, uint32)` answers with `SCREEN_CAPTURE_PERMISSION_ANSWER`.
- O-8: a semaphore (`SetFrameNotification()`) or `WaitForFrame(timeout)`, not a message.
- O-9: `NextFrame(..., &flags)` with `SCREEN_CAPTURE_FRAME_*` flags.
- O-12: `BPrivate::VirtualCameraFeeder` (`headers/private/media/VirtualCamera.h`).
- O-13: three kernel fixes came with the tests (empty messages, descriptors with empty messages, MSG_CTRUNC).
OBS has adapted to each (OBS commits ab7b54e capture, 13ad317 audio and webcam, 73d1ed8 browser audio, 1237ec5
virtual camera); the request text below is kept as the original proposal. O-16 to O-20 came up while adapting.

**Landed (Renga, 2026-11-13, F-58 batch 2).** O-16 `a1378d54` (an answer belongs only to its own request;
"Try again" calls `RequestPermission()` once), O-17 and O-18 `cbd2a2c8` (`AudioStream::team`,
`FindStream(team_id)`, `SetTeamLoopbackOnly(team, on)`, set before the helper's stream starts, nothing
remembered), O-19 `a220f582` (`EMSGSIZE` for UNIX datagram/seqpacket messages larger than the receiver's buffer),
O-20 `677afa3d` (`WriteFrame(..., timeout)`; 0 returns `B_WOULD_BLOCK` instead of waiting; `Disconnect()` never
waits). Also from Renga (F-59, `a22d9aba`): screen capture reads app_server's back buffer in main memory, so the
pointer no longer stutters during capture; no change needed in OBS.

Suggested order: O-1 (the most noticeable everyday problem, a small change in the mixer's loopback), then
O-7 with O-6 (removes a fragile workaround in OBS and a UI freeze), then O-4, then O-5 and O-12 (the
largest visible gains). Q-1 is independent of these and blocks hardware rendering in OBS on NVIDIA cards.

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
  call in OBS waits meanwhile, including the UI's, so OBS's interface can stall for up to the full prompt
  timeout (2 minutes by default) while the prompt is open. OBS asks in 1-second slices from a background
  thread to bound this, which is part of the workaround O-7 would remove.
- **Proposed interface.** Take the link per poll attempt, or (better, O-7) an asynchronous request.

### O-7 Permission timeout vs. denial; asynchronous request

- **Need.** A timeout returns `B_NOT_ALLOWED`, like a "no"; OBS cannot tell the user which happened. OBS needs
  a helper thread and a 1-pixel probe session to ask without blocking.
- **Also:** a timeout and a non-remembered "Don't allow" both return `B_NOT_ALLOWED`, so OBS, which asks in
  1-second slices so it can stop at quit, checks whether the `ScreenRecording --prompt` process is still running
  to tell them apart. That check can mistake another application's prompt for OBS's.
- **Proposed interface.** `B_TIMED_OUT` for an unanswered prompt (or `B_WOULD_BLOCK` while still pending); `BScreenCapture::RequestPermission(
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

- **State.** The Media Kit's phase-5 interface has landed (`446221a6`): `headers/private/media/VideoAcceleration.h`,
  shaped like VA-API but inside the Media Kit (no libva; FFmpeg's `*_vaapi` encoders are not available).
  `VideoAcceleratorRoster` finds back-end add-ons; `VideoAccelerator::IsHardware()` tells hardware from the
  software reference back end; `VideoAccelEncoder` takes NV12/P010 surfaces and returns packets with a key-frame
  flag and the codec's extradata. No GPU back end exists yet; NVIDIA's is Quasar's Q-3.
- **OBS plan.** An OBS encoder plug-in directly on `VideoAccelEncoder` (H.264, HEVC, AV1 as the back end
  supports them), offered only when the roster finds a hardware back end with an encode entry point for that
  codec. This fits OBS better than `BMediaEncoder`/`BMediaFile` (OBS needs raw packets, key frames and
  extradata for its own muxers and RTMP/SRT outputs), so **no libva request**.
- **Done in OBS:** `plugins/haiku-video-accel` (H.264, HEVC, AV1, and VP9 for testing with the software back
  end). Refinements requested in O-14 and O-15.
- **Remaining need.** GPU-texture input without a CPU copy (surfaces importable from a GPU buffer), with O-10.

### O-12 Virtual camera

- **Need.** OBS's "Start Virtual Camera" makes OBS's output a camera other applications can open.
- **Proposed interface.** A Media Kit producer add-on (global flavor, listed among cameras by
  `GetDormantNodes()`) fed by one client process through shared memory (memfd) with timestamps, so CodyCam,
  Chromium and others see it as an ordinary camera.

### O-14 `VideoAccelEncoder` details

OBS's encoder plug-in (`plugins/haiku-video-accel`) is written against `VideoAcceleration.h` as it is. These
gaps make it guess or limit features:

1. **Bitstream format.** Specify that packets are Annex B (H.264/HEVC) or low-overhead OBUs (AV1), and the
   extradata format (avcC/hvcC/av1C or Annex B). OBS converts both ways today.
2. **Decode timestamps and B-frames.** A `bFrames` field in the encoder setup and a decode timestamp from
   `ReceivePacket()` (for example a `VideoAccelPacket` struct). Without dts, OBS cannot use B-frames.
3. **Rate control.** Mode (CBR/VBR/CQP), maximum bit rate, buffer size, a latency/preset hint, and
   `SetBitRate()` for OBS's dynamic bitrate while streaming.
4. **Surfaces.** `ReleaseSurface()` to return an acquired but unsent surface; document who owns a surface when
   `SendSurface()` returns `B_WOULD_BLOCK`.
5. **Colour.** Primaries, matrix, transfer and range in the setup, so the stream's VUI is right for BT.709,
   full range and HDR.
6. **Threading.** `VideoAcceleratorRoster::_Load()` has no lock; document the threading rules (OBS loads the
   roster once at module load to avoid it).

### O-15 Software H.264 encoder for testing

The software reference back end encodes VP8/VP9 only, so the H.264/HEVC/AV1 paths of hardware encoder
clients cannot be tested without a GPU back end. An H.264 encoder in it (for example openh264), and HEVC/AV1
if practical, would let OBS's main codecs be tested end to end. OBS tests with VP9 meanwhile.

### O-16 Leftover permission requests

- **Need.** After a "Don't allow" that was not remembered, or a prompt closed without an answer, the app_server
  keeps the request (`ScreenCaptureManager` `_AnswerRequest`/`_Decide`), and the next `RequestPermission()`
  is answered "no" from it without a prompt. OBS's "Try again" therefore asks twice when the first answer is
  an immediate "no" with request -1.
- **Proposed interface.** None new: a request answered or cancelled is consumed and never answers a later one.

### O-17 Team of each mixer stream

- **Need.** OBS's browser sources run in helper processes that share one executable. The mixer's stream list
  (`AudioStreamControl::Update()`, the parameter web) names streams by signature only, so OBS cannot find the
  streams of one helper to route them.
- **Proposed interface.** A hidden per-stream parameter `renga:app_team` and an `AudioStream::team` field.

### O-18 Loopback-only routing by team

- **Need.** Routes are remembered by signature, so routing one helper's streams "loopback only" would apply to
  every later helper and to browser docks. OBS works around it by starting helpers of sources with "Control
  audio via OBS" under a second signature.
- **Proposed interface.** A route by team that the mixer does not remember, or automatic "loopback only" for
  a team's streams while a loopback selecting that team (and asking for it, e.g. a `renga:loopback_exclusive`
  flag) exists. With O-17 and O-18 the second signature goes away, and no page sound reaches the speakers
  before OBS has set the route (up to about 1 s today).

### O-19 Oversize UNIX messages

- **Need.** A `SOCK_SEQPACKET`/`SOCK_DGRAM` UNIX message larger than the receiver's buffer (64 KiB default,
  128 KiB maximum) is not refused: the send waits forever (`net_socket.cpp`, `UnixFifo.cpp`), because the
  module does not mark the protocol as atomic. The browser helper IPC stays on `SOCK_STREAM` because custom CSS
  and scripts can be megabytes.
- **Proposed interface.** `EMSGSIZE` for messages that can never fit, as on Linux, and the limit documented.

### O-20 WriteFrame() timeout

- **Need.** `VirtualCameraFeeder::WriteFrame()` ends in a blocking `send()`. If media_addon_server stops
  reading without closing the socket, OBS's writer thread blocks, and stopping the virtual camera hangs.
- **Proposed interface.** A timeout argument (or `SetTimeout()`), or `MSG_NOSIGNAL` with a non-blocking mode
  returning `B_WOULD_BLOCK`.

### O-21 Recording size per team

- **Need.** `BScreenCapture::SetOutputSize()` (`00fae122`) would let OBS show its recording's size in the
  recording indicator, but it sets the value on every session of the team and the indicator adds the sessions
  up (`ScreenRecording.cpp`), so OBS with two capture sources would show the size doubled.
- **Proposed interface.** Store the output size once per team and have the indicator count each team once.
- **Landed (2026-11-13, `b734d8f4`).** app_server keeps one output size per team; `GetSessions()` adds
  `total_team`/`total_bytes`. OBS reports the bytes written by its recording outputs once a second while a
  capture source is capturing (`plugins/haiku-capture/capture-source.cpp`, `report_recording_size()`).

### Planned Renga work OBS will use as it lands

No request needed; OBS adapts when these arrive:

- **Multi-monitor (F-38).** Screen capture is main-screen only today. Once `BScreenCapture` can name a screen,
  OBS adds a screen choice to Screen Capture; nothing extra is needed beyond F-38 itself.
- **Compositor (F-33).** Window capture of covered windows; O-9 becomes unnecessary.
- **Zero-copy presentation (F-47).** OBS's preview, projectors and multiview presented from the GPU instead
  of a `glReadPixels` copy painted by Qt.
- **futex (F-49).** No OBS change; Mesa and libstdc++ waits benefit once rebuilt.
- **Crash logs (F-51).** Already used: the browser helper's crash report path is logged. OBS's own crashes
  are covered by the system without changes.
- **Hardware video (F-31 phase 5).** See O-11.
- **Chromium port.** A Chromium-based browser source to replace the HaikuWebKit helper (WebGL, modern sites),
  with shared GPU textures once Q-2 exists.

### O-13 SOCK_SEQPACKET tests

- **Need.** The browser helper IPC would use `SOCK_SEQPACKET` (message boundaries for free) but the socket
  tests only cover `SOCK_STREAM` with `SCM_RIGHTS`, so OBS uses a stream socket with its own framing.
- **Proposed interface.** None new: a test case for `SOCK_SEQPACKET` socketpairs passing descriptors.

## Quasar

| ID | Request | Priority | State | Quasar reference |
| --- | --- | --- | --- | --- |
| Q-1 | Fix the timeline-fence hang with OBS's Screen Capture load | 1 | Passed to Quasar | `docs/event-sync.md`, Open issues |
| Q-2 | Shared GPU textures between processes | Roadmap | Passed to Quasar | external memory (`RENGA-STATUS.md`) |
| Q-3 | Hardware video encode back end | 2 | Passed to Quasar | Video, Media Kit back end |
| Q-4 | Quiet driver logging by default | 3 | Passed to Quasar | Power and system |

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
