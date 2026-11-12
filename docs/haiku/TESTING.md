# Testing the Haiku port

## Automated run

If you only have a few minutes, run this from a Terminal on the Haiku desktop (not over SSH), then leave the
machine alone until it finishes:

```sh
cd ~/OBS-Studio_HaikuOS
build-aux/haiku/test.sh            # pull, build, test (the build takes a while)
build-aux/haiku/test.sh --no-build # test the existing build only
build-aux/haiku/test.sh --quick    # shorter recordings, skips the forced-GPU run
build-aux/haiku/test.sh --no-gpu   # never touch the GPU (CPU renderer only)
build-aux/haiku/test.sh --gpu-only # only the GPU steps
```

It first prints a one-line warning that the GPU steps can freeze the machine on an experimental graphics driver
and waits 5 s, so you can press Ctrl-C and use `--no-gpu` instead (`--yes` skips the wait). Then it pulls
(`--ff-only`, skipped if the tree has local changes) and builds, collects system facts (packages, accelerants,
Vulkan ICDs, environment, syslog), and runs the tests from safest to riskiest:

1. OBS with `OBS_HAIKU_GL=osmesa OBS_HAIKU_GL_PROBE=0` (CPU renderer, the GPU is not used at all);
2. `obs-haiku-gl-probe` on the GPU, cheapest pattern first (`--frames 16` with every optional feature off:
   `--no-stream --no-stage --no-second-display --no-handoff`), then one feature at a time, then the full
   default, with `--verbose` whenever the probe offers the option;
3. OBS with the default renderer;
4. OBS with `OBS_HAIKU_GL=egl OBS_HAIKU_GL_WATCHDOG=0` (forced GPU, no hang watchdog) last.

Each OBS run records a known scene (Screen Capture, a colour source, text, desktop audio) for 20 s. OBS windows
open and close by themselves. Every step has a time limit; hangs are recorded with a thread listing and
`listimage`, then killed. `--no-gpu` skips the self-test and the forced-EGL run and gives the default run
`OBS_HAIKU_GL=osmesa`; `--gpu-only` skips the CPU-only run.

Each OBS run uses a throwaway configuration (`XDG_CONFIG_HOME`), so your own OBS settings, profiles and scenes
are not touched. See `build-aux/haiku/test/make-config.sh` for what it contains.

The last line printed is the archive on the Desktop (`obs-test-<branch>-<commit>-<time>.zip`). Send that file;
`SUMMARY.txt` inside it has one PASS/FAIL/HANG/SKIPPED line per step. The manual checklist below is still
needed for things the script cannot judge (preview, dialogs, docks, audio monitoring).

**If the machine freezes.** A graphics driver hang can freeze the whole system, so the run is written to
`~/obs-test-runs/<time>/` (not `/tmp`; `OBS_TEST_WORK` overrides), with a `sync` after every log line.
`progress.txt` there gets the step and the exact command line before each launch, so after a hard reset its
last `LAUNCH` line is what was running (for the probe, also the end of `04-probe/<pattern>.log`). Just run the
script again after rebooting: it lists the unfinished run and packs it into the new archive under
`00-previous-incomplete/`, together with `/var/log/syslog*` and the part of it from the previous boot. The
directory is removed once a run has written its archive (kept with `--keep`).

The syslog may lack the last seconds before a freeze. If your boot loader offers it, turn on *Enable debug
syslog* in the boot menu's debug options before a risky run: after a warm reset (the reset button, not power
off), choose *Save syslog from previous session during boot* and `/var/log/previous_syslog` is packed as well.

A step-by-step checklist for the first runs on real hardware. Work through it in order; each step says what
should happen and what to collect if it doesn't. Tick boxes as you go and note anything odd. Exact error
text and log lines are the most useful thing to send back.

> **Before you start:** run `pkgman update` and reboot if it updated the system, so Haiku, Mesa and Qt are
> current. Testing with headphones avoids feedback in the audio monitoring tests.

## 0. What was verified before this test

**Status (2026-11-07):** on the maintainer's machine (x86-64, RTX 5070) the port builds, starts and records an MP4
with Screen Capture, using the OSMesa renderer (`OBS_HAIKU_GL=osmesa`; before the driver self-test was added, the nvidia-haiku Zink path hung on a fence
wait, a driver issue being fixed there). Desktop audio tap insertion works; the rest of this list is still open.

Before that first build, it was checked with a harness
(`build-aux/haiku/crosscheck/`), which now also covers the desktop audio helper, the browser helper and the browser
dock code (HaikuWebKit's installed API headers are part of the harness sysroot):

- CMake configures the whole project as `CMAKE_SYSTEM_NAME=Haiku` against Haiku-layout dependencies.
- Every C/C++ file in the build, including Qt moc/uic output, compiles (syntax-only) with clang for
  `x86_64-unknown-haiku` against the real Haiku system headers and real dependency headers.

What that can't catch: GCC-specific errors, link errors (missing libraries or symbols), and runtime
behavior. Those are what this test is for.

## 1. Build

```sh
cd ~/OBS-Studio_HaikuOS            # wherever you cloned it
git pull
build-aux/haiku/install-deps.sh    # now also installs haikuwebkit_devel
build-aux/haiku/build.sh           # fetches the submodules it needs
```

- [ ] `install-deps.sh` finishes without "!!" lines for the required packages.
- [ ] CMake configure succeeds. The summary at the end lists enabled/disabled features; note which
      plugins are disabled. Expected enabled on Haiku: obs-websocket, the new MPEG-TS output, `obs-browser`
      ("Browser docks (HaikuWebKit)"), `haiku-capture`, `haiku-media`.
- [ ] The build finishes.
- [ ] `build/rundir/RelWithDebInfo/bin/` contains `obs`, `obs-haiku-audio-tap` and `obs-haiku-browser`.

If configure or the build fails, send `build/haiku-build.log` (or just everything from the first
`error:`/`CMake Error` onward). The build script keeps going after errors (`-k 0`) so one log shows all
failures at once.

## 2. First launch

```sh
cd build/rundir/RelWithDebInfo/bin
./obs --verbose
```

Keep the Terminal open; libobs logs to it and to `~/config/settings/obs-studio/logs/`.

- [ ] The OBS window opens. The Auto-Configuration Wizard may appear; cancel it for now.
- [ ] The log contains lines like these (exact text varies):
  ```
  Platform: Haiku (haiku)
  With the nvidia-haiku driver (Zink), when its self-test passes:
    Haiku GL probe: OK: EGL 1.5, GL_VENDOR=..., GL_RENDERER=zink ..., GL_VERSION=... (60 frames 60 fps, 2560x1080 capture: 30 uploads, 2 recreations; 59 stage readbacks, 7 swapchain resizes, ... context hand-offs; init ... ms, total ... ms)
    Haiku GL: driver self-test passed (... ms)
    Haiku EGL: version 1.5, vendor: Mesa Project
    Haiku GL: rendering through EGL (surfaceless)
    Haiku GL: zink Vulkan 1.x (NVIDIA GeForce RTX 5070 ...)
    Haiku GL: driver identity 0123456789abcdef (self-test), fedcba9876543210 (process)
    Haiku GL: driver hang watchdog: 15 s (OBS_HAIKU_GL_WATCHDOG=<seconds>, 0 disables it)
  Without it (HaikuPorts Mesa 22):
    Haiku EGL: eglInitialize failed: ...
    Haiku GL: rendering through OSMesa
    Haiku GL: llvmpipe (LLVM ..., 256 bits)
  ---------------------------------
  OBS 33.0.1... (haiku)
  CPU Name: ...
  Physical Memory: ...MB Total
  Kernel Version: Haiku 1 hrev5xxxx ...
  ...
    Loaded Modules:
      ...
      haiku-capture.so
      haiku-media.so
      obs-browser.so
      obs-websocket.so
      ...
  ```
  If a module is missing from that list, search the log for its name; libobs logs why it failed to load
  (usually a missing shared library).
- [ ] The preview area in the middle shows the canvas (black when there are no sources) and **not** a
      frozen or garbage image.
- [ ] Resizing the main window resizes the preview without flicker or stretching artifacts.

- [ ] **Driver self-test.** On a driver that hangs (like nvidia-haiku before its fence fix), OBS still starts:
      after up to 8 s the log shows `Haiku GL: the OpenGL driver failed its self-test: ...` and
      `using the CPU renderer (OSMesa)`, and the preview works. If the probe got stuck in a call, the lines
      before it include `Haiku GL probe: STALL: frame ... [render]: <step> has not returned after 4.0 s`; the last
      STALL line names the call. `ps | grep gl-probe` shows no leftover probe. With `OBS_HAIKU_GL=egl` the test is
      skipped (and the driver hang reproduces). On a healthy driver `driver self-test passed (... ms)` is about
      1000-1500 ms (60 frames at 60 fps plus EGL initialisation).
- [ ] **The probe by hand** (in `bin/`): `./obs-haiku-gl-probe; echo $?` prints `OK: ...` and 0.
      `./obs-haiku-gl-probe --frames 600` (10 s at 60 fps) and `--fps 30 --frames 150` also pass. If one
      hangs, run it again with `--verbose 2>&1 | tail -5` and send the last lines; then try `--no-stage`,
      `--no-handoff --no-second-display` and `--no-stream` to narrow it down (each option is described in
      ARCHITECTURE.md, section 4).
- [ ] **Driver hang watchdog.** On a driver that passes the self-test but hangs later (e.g. when a Screen
      Capture source is added), about 15 s after the freeze the log shows
      `Haiku GL watchdog: the graphics driver stopped responding: thread ... has been blocked with the OpenGL
      context for 15 s`, the driver and its identities, and `recorded the hang in .../obs-studio/haiku-gl-hang`,
      and an alert "The graphics driver stopped responding." appears. Enter or Escape picks **Keep waiting**.
  - [ ] **Keep waiting**: OBS stays frozen; the alert comes back after a minute.
  - [ ] **Restart with CPU renderer**: OBS closes and starts again within a few seconds (it shows the
        unclean-shutdown dialog); the new log shows `rendering through OSMesa`. The scene collection is
        intact; a recording that was running may be incomplete.
  - [ ] Starting OBS normally afterwards keeps the CPU renderer and logs `Haiku GL: the hardware driver hung in
        a previous session; using the CPU renderer. Update the driver or delete .../haiku-gl-hang ...`.
        `OBS_HAIKU_GL=egl ./obs` uses the driver (and the hang can be reproduced). Deleting the file, or a driver
        update, brings the GPU back: after an update the log shows `the driver changed since it hung in a
        previous session ...; trying the hardware driver again` and the file is gone.
  - [ ] No false alarms: with the hardware driver and the watchdog on, leave OBS idle and minimised for a few
        minutes, record for a few minutes and stop, open and close the properties of a Screen Capture source,
        and attach `Debugger` to OBS and let it sit stopped for 30 s, then continue: no `Haiku GL watchdog:`
        line other than the start-up one appears.
  - [ ] `OBS_HAIKU_GL_WATCHDOG=0` disables the watchdog (`driver hang watchdog disabled`); `=5` makes it fire
        after 5 s. Loading a large scene collection at startup must not trigger it.

To test the software path on an NVIDIA machine too, start OBS once with `OBS_HAIKU_GL=osmesa ./obs`.

**If OBS fails to start** with "Failed to initialize video" or similar, look for `Haiku EGL:` /
`Haiku GL:` lines in the Terminal output (see the troubleshooting table in `BUILDING.md`).
**If it crashes**, the Debugger will offer to save a report. Save it (it goes to the Desktop) and run
`build-aux/haiku/collect-logs.sh`.

## 3. Sources

Add each with the **+** button under *Sources*.

- [ ] **Color Source**: fills the canvas with the chosen color.
- [ ] **Image**: load a PNG/JPEG.
- [ ] **Text (FreeType 2)**: text renders. Changing the font in its properties works (uses fontconfig).
- [ ] **Screen Capture** (haiku-capture): shows the desktop and updates live.
  - [ ] Toggling *Capture Cursor* shows/hides the mouse pointer.
  - [ ] At 1920×1080 and 30 fps, ProcessController shows reasonable CPU usage. Try 60 fps as well.
- [ ] **Window Capture (screen region)** (haiku-capture):
  - [ ] The *Window* list shows open windows as "[Application] Title"; OBS's own windows are not listed.
        *Refresh window list* updates it.
  - [ ] Pick a Terminal window: the source shows exactly that window. Move and resize the window: the source
        follows.
  - [ ] Minimize the window or switch to another workspace: the source pauses (keeps the last frame) and the log
        says so; restoring it resumes.
  - [ ] Put another window over it: the covering window appears in the capture (expected, documented) and the log
        mentions the window is covered.
  - [ ] *Include window border and tab* on/off changes whether the tab and border are captured.
  - [ ] Quit and restart OBS: the source finds the same window again.
- [ ] **Audio Input Capture (Media Kit)**:
  - [ ] The *Device* list shows "Default (Media preferences)" plus your input device(s).
  - [ ] Speaking into the mic moves its meter in the *Audio Mixer* dock.
  - [ ] The Terminal shows `[haiku-media] '<name>': capturing from 'default' (2 ch, 48000 Hz, format 0x...)`.
        Note the format value for the report.
  - [ ] Switching to a specific device in the list works.
- [ ] **Video Capture Device (Media Kit)** (only with a webcam supported by Haiku):
  - [ ] The device appears in the list and video shows in the preview.
  - [ ] Note the `capturing from ... (WxH, color space 0x...)` log line.
  - [ ] *Deactivate when not showing*: hide the source with the eye icon; the camera LED should go off.
- [ ] **Media Source** (FFmpeg): plays a local video file with sound.
- [ ] **VLC Video Source** (only if `vlc_devel` was installed): plays a playlist.

## 4. Desktop audio

**Before this test, note how to restore sound if something goes wrong:** run
`build/rundir/RelWithDebInfo/bin/obs-haiku-audio-tap --repair`, or open *Media* preferences and click
*Restart media services*.

1. Play something (MediaPlayer, or a video in WebPositive).
2. *Settings → Audio → Global Audio Devices → Desktop Audio*: choose "System mixer output".

- [ ] You **still hear** the playback normally, without new crackling or added delay.
- [ ] The *Desktop Audio* meter in the Audio Mixer moves with the playback.
- [ ] The Terminal shows `obs-haiku-audio-tap: tap inserted (48000 Hz, 2 channels, format 0x...)` and
      `[haiku-media] 'Desktop Audio': attached to the desktop audio tap`. Note the format.
- [ ] If the Cortex app is installed (`pkgman install cortex`), it shows "OBS Desktop Audio Tap" between the system mixer and
      the sound card while OBS runs.
- [ ] Record 30 s with only desktop audio: the recording has the played sound, in sync, without gaps or
      doubled/sped-up audio.
- [ ] Set *Desktop Audio* to "Disabled" (or quit OBS): within a second, the tap disappears from the media
      routing and playback continues normally (`obs-haiku-audio-tap: ...` lines in the Terminal). ProcessController
      no longer lists `obs-haiku-audio-tap`.
- [ ] Crash recovery: with desktop audio active, kill OBS from ProcessController (Kill). Within ~10 s the helper
      exits and sound continues. Then kill `obs-haiku-audio-tap` itself with Kill while OBS is running: OBS logs
      that the helper died, starts a new one and capture resumes; sound keeps working.
- [ ] Settings → Audio: the *Sample Rate* setting (44.1 or 48 kHz) different from the mixer's rate still gives
      correct pitch in recordings.

## 5. Audio monitoring

1. *Settings → Audio → Advanced → Monitoring Device*: "Default".
2. In the Audio Mixer, open the gear menu of the mic source → *Advanced Audio Properties* →
   *Audio Monitoring*: **Monitor and Output**.

- [ ] You hear yourself through the speakers/headphones with a short delay (roughly 50–250 ms).
- [ ] Volume changes on the source's fader change the monitored volume.
- [ ] Switching back to *Monitor Off* stops it. No crash when switching repeatedly.
- [ ] A Media Source with *Monitor Only (mute output)* is audible but its meter in the recording is silent.
- [ ] With Desktop Audio enabled, set *Monitor and Output* on Desktop Audio itself: there must be **no** echo or
      feedback (OBS skips monitoring of Desktop Audio). Monitoring another source is heard and is also captured by
      Desktop Audio (same as other platforms when monitoring and desktop audio use the same device).

## 6. Browser source and docks

- [ ] **Browser** source with the default URL (`https://obsproject.com/browser-source`): the page renders in the
      preview within a few seconds. ProcessController lists one `obs-haiku-browser` per browser source. No
      `obs-haiku-browser` entry appears in the Deskbar (after the first start).
- [ ] Width/Height 1280×720: the page re-lays out to that size.
- [ ] Transparency: set the URL to a page with a transparent background, e.g. a local file containing
      `<html><body style="background:transparent"><h1 style="color:red">Hello</h1></body></html>` (*Local file*
      checkbox). Put it over a color source: only the text is visible. If the whole page is opaque, note the log line
      `obs-haiku-browser: page rendered without an alpha channel`.
- [ ] *Custom CSS* `body { background: blue !important; }` turns the page blue immediately.
- [ ] *Refresh cache of current page* reloads it.
- [ ] Animation: a page with a CSS animation or a clock (e.g. `https://time.is`) updates continuously at the source
      frame rate.
- [ ] *Shutdown source when not visible*: hiding the source (eye icon) ends its `obs-haiku-browser` process;
      showing it starts one and the page comes back.
- [ ] *Interact* (right-click the source → Interact): clicking links, scrolling with the wheel, typing into a text
      field (letters, Backspace, arrows, Enter) all work.
- [ ] Crash recovery: kill an `obs-haiku-browser` process in ProcessController. OBS logs `browser helper ... exited
      unexpectedly` and the page comes back within a few seconds.
- [ ] Twitch/StreamElements-style alert page or a chat overlay URL you normally use: note whether it renders and
      animates (HaikuWebKit has no WebGL, so WebGL-based overlays won't).
- [ ] **Custom browser docks**: *Docks → Custom Browser Docks...*, add one with `https://obsproject.com`. The dock
      shows the page, follows dock resizing, scrolls with the wheel, links can be clicked, the dock's title follows
      the page title. Restart OBS: the dock comes back.

## 7. obs-websocket

- [ ] *Tools → WebSocket Server Settings* opens; enable the server (port 4455), *Show Connect Info* shows a QR code.
- [ ] From another machine (or WebPositive on `http://localhost` with a test page), connect with an obs-websocket
      5 client (e.g. https://github.com/obsproject/obs-websocket-js examples or the *OBS Blade* app) using the
      password: switching scenes and starting/stopping recording from the client works.

## 8. Recording

*Settings → Output*: Output Mode "Simple", Recording Path somewhere writable, Recording Format "Matroska
Video (.mkv)" or "MPEG-4 (.mp4)", Video Encoder "Software (x264)", Audio Encoder "AAC" (FFmpeg).

- [ ] *Start Recording*, record 30 seconds with screen capture and mic, *Stop Recording*.
- [ ] The file plays in MediaPlayer (or VLC) with picture and sound in sync.
- [ ] The *Stats* dock (*Docks → Stats*) shows no dropped/lagged frames at the default canvas size.
      Note the CPU usage and render time if frames are dropped.
- [ ] Try "Advanced" output mode with "Hybrid MP4" as well.

## 9. Streaming (optional)

- [ ] *Settings → Stream*: a service with a stream key (e.g. YouTube or Twitch, preferably an unlisted/test
      stream). *Start Streaming* connects and the stream plays on the service.
- [ ] Bandwidth test or a custom RTMP server if available.
- [ ] WHIP (WebRTC) to a test server, if `libdatachannel_devel` was installed.
- [ ] **SRT**: *Settings → Stream → Service: Custom*, server `srt://<other-machine>:9000?mode=caller` with a listener
      on the other machine (e.g. `ffplay -i "srt://0.0.0.0:9000?mode=listener"` or OBS's own Media Source in
      listener mode on another computer). The stream plays.
- [ ] **RIST**: server `rist://<other-machine>:9000`, receiver `ffplay rist://@:9000`. The stream plays.

## 10. Hotkeys

On Haiku, OBS's "Ctrl" is Haiku's **Command** modifier, which is the **Alt** key with Haiku's default keyboard
settings (the same key Haiku's own shortcuts use). OBS's "Alt" is Haiku's Control key.

- [ ] *Settings → Hotkeys*: click the field for *Start Recording* and press **F9**; the field shows "F9".
      Press **Alt+Shift+R** (physical keys, default Haiku settings); the field shows "Ctrl + Shift + R".
      Press the physical **Ctrl+R**; it shows "Alt + R".
- [ ] With another application focused (e.g. a Terminal), press the hotkey; recording starts. Press the
      Stop hotkey; it stops. (These are global hotkeys polled from the app_server.)
- [ ] Try a numpad key and a punctuation key. Note any key that shows the wrong name or doesn't trigger,
      along with your keyboard layout.

## 11. Windows and projectors

- [ ] Right-click the preview → *Windowed Projector (Preview)*: a separate window shows the canvas and
      resizes correctly.
- [ ] *Fullscreen Projector* on your monitor; Escape or right-click → close works.
- [ ] *Studio Mode*: two previews render; transitions animate.
- [ ] *View → Multiview*: renders all scenes.
- [ ] Source properties dialogs show their live preview at the top.

## 12. Tools

- [ ] *Tools → Automatic Scene Switcher*: the window list shows running applications (e.g. "Terminal",
      "WebPositive"). A rule switches scenes when you activate that application.
- [ ] *Tools → Scripts* (if scripting was built): add a Lua script; if Python was found, the Python
      settings tab shows the path.
- [ ] *Tools → Plugin Manager* opens.

## 13. Lifecycle

- [ ] Quit OBS and start it again: scenes, sources and settings are restored from
      `~/config/settings/obs-studio`.
- [ ] Start a second instance while one is running: OBS warns that it is already running.
- [ ] Quitting doesn't crash or hang. A clean exit doesn't trigger the "OBS didn't shut down properly"
      dialog on the next start.
- [ ] Logs exist in `~/config/settings/obs-studio/logs/`.
- [ ] After quitting, ProcessController shows no leftover `obs-haiku-audio-tap` or `obs-haiku-browser` (allow ~10 s),
      and system sound works.

## 14. Reporting back

Run:

```sh
build-aux/haiku/collect-logs.sh
```

It creates `obs-haiku-report-<date>.zip` on the Desktop with the build log, CMake cache, the last five OBS
logs, Debugger reports and system/package information. Send it along with this checklist (which boxes
passed, which failed, and what you saw).
