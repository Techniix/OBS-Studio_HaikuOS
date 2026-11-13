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

**Screen recording permission.** The first capture of an application shows the *Screen recording* prompt, and the
capture sources wait for the answer, which would leave an unattended run without a picture. On this branch `test.sh` therefore runs the step
`screen-permission` first: it saves the current decisions (`screen-recording-before.txt` and
`screen-recording-after.txt` in the archive) and allows OBS with

```sh
ScreenRecording --set application/x-vnd.obs-studio allow
```

The permission stays granted afterwards, as if the prompt had been answered. To undo it, run
`ScreenRecording --set application/x-vnd.obs-studio ask` (the next capture asks again) or use the *ScreenRecording*
preferences. The step is skipped when the `ScreenRecording` command is missing.

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

> **Renga branch.** This checklist is for the `renga` branch on Renga. Nothing changed for Renga (screen and
> window capture, desktop and application audio, audio monitoring routing, hot-plug, the browser helper IPC and
> crash-log lines) has been built with GCC or run yet; those sections are the first real test of that code. The
> status paragraph below describes the earlier build on stock Haiku.

**Status (2026-11-07):** on the maintainer's machine (x86-64, RTX 5070) the port builds, starts and records an MP4
with Screen Capture, using the OSMesa renderer (`OBS_HAIKU_GL=osmesa`; before the driver self-test was added, the nvidia-haiku Zink path hung on a fence
wait, a driver issue being fixed there). The rest of this list is still open.

Before that first build, it was checked with a harness
(`build-aux/haiku/crosscheck/`), which also covers the browser helper and the browser
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
- [ ] `build/rundir/RelWithDebInfo/bin/` contains `obs` and `obs-haiku-browser`.

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
- [ ] **Screen Capture** (haiku-capture). The screen recording permission applies; if `test.sh` ran earlier on
      this machine, OBS is already allowed (see "Screen recording permission" below). To test the prompt, first run
      `ScreenRecording --set application/x-vnd.obs-studio ask`.
  - [ ] Adding the source shows the *Screen recording* prompt and the properties say it is waiting; OBS's UI
        (preview, menus, other dialogs) stays fully responsive meanwhile. *Allow* starts the capture at once and the
        desktop shows in the preview.
  - [ ] *Deny*: the properties say OBS isn't allowed and show *Try again*; one press shows the prompt again
        (exactly once; the log has one request per press, O-16). Deny and press *Try again* three times in a row:
        the prompt comes back each time. *Deny* with "Remember my decision": *Try again* shows no prompt and the properties stay
        "not allowed" until the ScreenRecording preferences allow OBS.
  - [ ] Close the prompt without answering: the properties say the prompt was closed without an answer (not "not
        allowed") and show *Try again*, which shows the prompt again.
  - [ ] Quit OBS while the prompt is still open: OBS quits at once and normally, without a crash dialog.
  - [ ] With OBS allowed and a source capturing, run `ScreenRecording --set application/x-vnd.obs-studio ask` and
        hide and show the source (or toggle *Capture Cursor*): the prompt appears and the properties say "waiting",
        not "not allowed".
  - [ ] While capturing, the Screen recording indicator is shown. *Stop* from the indicator ends the capture; the
        properties show the "stopped" message with *Start capture*, and it does not restart by itself. *Start capture*
        resumes.
  - [ ] While OBS records (and a capture source is capturing), the recording indicator shows OBS's recording size,
        growing about once a second, and counted once with two capture sources; it shows no size after recording stops.
  - [ ] With two or more capture sources (Screen and Window Capture) running, *Stop* from the indicator stops all of
        them. *Start capture* in the properties of any one of them restarts all of them.
  - [ ] *Capture* = *Region*: set Left/Top/Width/Height; the source shows exactly that part of the screen and
        follows changes to the numbers. A region outside the screen shows the "outside the screen" message.
  - [ ] Toggling *Capture Cursor* shows/hides the mouse pointer.
  - [ ] An idle desktop produces no new frames: the 10 s `LOG_DEBUG` line (with `--verbose`) shows about 10
        frames read (the once-a-second idle check) and few wakeups. Moving a window produces wakeups and changed
        frames, never more frames read than the frame rate allows. Note wakeups, frames read, changed and output
        frames and the `NextFrame()` average/max times.
  - [ ] At 1920×1080 and 30 fps, ProcessController shows reasonable CPU usage, and the desktop and pointer stay
        smooth while recording. Try 60 fps as well.
- [ ] **Window Capture (screen region)** (haiku-capture):
  - [ ] The *Window* list shows open windows as "[Application] Title"; OBS's own windows are not listed.
        *Refresh window list* updates it.
  - [ ] Pick a Terminal window: the source shows exactly that window. Move and resize the window: the source
        follows.
  - [ ] Minimize the window, hide it, or switch to another workspace: the source pauses (keeps the last frame;
        the log says why); restoring it resumes at once.
  - [ ] Close the window while it is minimized or on another workspace: within about a second the picture is
        cleared and the properties say the window isn't open.
  - [ ] Close the window: the picture is cleared and the properties say the window isn't open. Open it again
        (same application and title): the capture resumes without touching the source.
  - [ ] Put another window over part of it: with *When the window is covered* = *Capture anyway* (the default)
        the covering window appears in the capture (expected until the compositor, F-33). With *Pause while any
        part of it is covered* the source pauses until the window is uncovered; with *Pause while it is fully
        covered* it pauses only when another window hides all of it.
  - [ ] *Include window border and tab* on/off changes whether the tab and border are captured.
  - [ ] Quit and restart OBS: the source finds the same window again.
- [ ] **Audio Input Capture (Media Kit)**:
  - [ ] The *Device* list shows "Default (Media preferences)" plus your input device(s).
  - [ ] Speaking into the mic moves its meter in the *Audio Mixer* dock.
  - [ ] The Terminal shows `[haiku-media] '<name>': capturing from 'default' (2 ch, 48000 Hz, format 0x...)`.
        Note the format value for the report.
  - [ ] Switching to a specific device in the list works.
  - [ ] Hot-plug: with a USB microphone selected, unplug it. The list shows "<name> (not connected)" and the source
        goes silent without errors in a loop. Plug it in again: capture resumes without re-saving the source. A microphone plugged in later appears in the list.
- [ ] **Video Capture Device (Media Kit)** (only with a webcam supported by Haiku):
  - [ ] The device appears in the list and video shows in the preview. The list does not contain "Virtual Camera".
  - [ ] Note the `lists N video modes` and `capturing (WxH, color space 0x..., F fps, ...)` log lines.
  - [ ] *Resolution* lists the camera's sizes (after it was opened once; the dialog refills by itself) and
        *Frame rate* the rates of the chosen size. Pick a size and a lower rate (for example 15 fps): the picture
        changes size, it moves visibly slower, and the log line says `15.00 fps; 15.00 fps asked for` (or the
        camera's nearest rate). "Device default" for both gives the camera's own choice.
  - [ ] With Demo Video Producer: *Resolution* lists 640 x 480 and 320 x 240, *Frame rate* 29.97, 15 and 5 fps.
  - [ ] With OBS's virtual camera running, a scene collection whose source names "Virtual Camera" shows it as
        "(shows OBS's own output; not available)" and stays black (no picture of OBS's own output). If Media
        preferences has the Virtual Camera as the video input, "Default" stays black too and the log says why.
  - [ ] Hot-plug: unplug the camera while it is shown. The entry stays as "(not connected)"; plug it in again and the
        picture returns. A camera plugged in later appears in the list. The list also contains Demo Video Producer.
  - [ ] *Deactivate when not showing*: hide the source with the eye icon; the camera LED should go off.
- [ ] **Media Source** (FFmpeg): plays a local video file with sound.
- [ ] **VLC Video Source** (only if `vlc_devel` was installed): plays a playlist.

## 4. Desktop audio and application audio

Both record the system mixer through a loopback; nothing is inserted into the playback path, so no repair step
is needed if OBS crashes.

1. Play something (MediaPlayer, or a video in WebPositive).
2. *Settings → Audio → Global Audio Devices → Desktop Audio*: choose "System mixer (everything that plays)".

- [ ] You **still hear** the playback normally, without new crackling or added delay.
- [ ] The *Desktop Audio* meter in the Audio Mixer moves with the playback, also with the master volume low.
- [ ] Record 30 s with only desktop audio: the recording has the played sound, in sync, without gaps or
      doubled/sped-up audio. If the sync is off, note the offset (the buffers are stamped from the mixer's time source).
- [ ] Setting *Desktop Audio* to "Disabled" (or quitting OBS) leaves playback unaffected. Killing OBS from
      ProcessController does the same.
- [ ] Settings → Audio: the *Sample Rate* setting (44.1 or 48 kHz) different from the mixer's rate still gives
      correct pitch in recordings.
- [ ] Changing the default output in Media preferences while capturing: capture keeps working (the loopback
      records every output), and the audio stays in sync with the video (no audible jump). The log may show
      "time stamps from the mixer's time source, as last converted" and then "... the mixer's time source" again.
- [ ] **OBS's monitoring is left out.** Add a Media Source and set it to *Monitor and Output*, with Desktop Audio
      active. You hear the source, and the recording contains it **once** (no echo). Set the monitoring device
      to another output and record again: still once.
- [ ] *Monitor and Output* on Desktop Audio itself: you hear the played sound a second time, slightly delayed,
      and there is **no** feedback howl; the recording has it once. The log has no `Prevented feedback-loop` line
      (it would mean OBS has no application signature).
- [ ] A browser source with sound (page audio) is still recorded through Desktop Audio.
- [ ] **Application Audio Capture**: add the source. The *Application* list shows applications that play audio
      ("(playing audio)") and running GUI applications; OBS itself is not listed. The list is read in the
      background: on the first opening it may fill in a moment after the dialog appears. *Refresh* updates it
      (shortly after the click), and with the dialog open an application that starts or stops playing shows up
      within a few seconds without pressing *Refresh*. The dialog opens without delay.
  - [ ] Select MediaPlayer: its sound is captured and the meter moves; a second application playing at the same
        time is **not** in the recording.
  - [ ] Quit and start MediaPlayer again: the capture continues without touching the source.
  - [ ] Quit OBS and start it again with MediaPlayer closed: the source shows "<name> (not running)" and starts
        capturing when the application plays.
  - [ ] Monitoring Application Audio Capture of another application (*Monitor and Output*) gives no echo.
  - [ ] The captured level does not change with the master volume.
- [ ] OBS has an application signature: `ScreenRecording --list` shows (after the first capture) or the log shows `application/x-vnd.obs-studio`
      (needed for routing, self-exclusion and the screen permission). Note if it does not.

## 5. Audio monitoring

Monitoring plays through the system mixer (one `BSoundPlayer` per source); OBS's streams are routed to the
chosen device by name (the mixer's `renga:routes`).

1. *Settings → Audio → Advanced → Monitoring Device*: "Default".
2. In the Audio Mixer, open the gear menu of the mic source → *Advanced Audio Properties* →
   *Audio Monitoring*: **Monitor and Output**.

- [ ] You hear yourself through the speakers/headphones with a short delay (roughly 50–250 ms).
- [ ] Volume changes on the source's fader change the monitored volume.
- [ ] Switching back to *Monitor Off* stops it. No crash when switching repeatedly.
- [ ] A Media Source with *Monitor Only (mute output)* is audible but its meter in the recording is silent.
- [ ] **Monitoring device routing.** The list contains the mixer's output names. Choose a device other than the
      default output (for example a USB headset): the monitored sound moves there while other applications stay on
      the default output; Media preferences or Cortex show the OBS stream routed to that output. Choose "Default"
      again and the sound comes back. The route is set in the background, so the sound may start on the default
      output and move within a moment; the Settings dialog applies without a pause.
- [ ] Choose a device, unplug it, then plug it in: monitoring plays on the default output meanwhile and moves
      back to the device by itself within a few seconds, without touching OBS. Also start OBS with the device
      unplugged (the settings show it as invalid): monitoring plays on the default output and moves to the device
      when it is plugged in.
- [ ] Set OBS's stream to another output in Media preferences, then switch a source's monitoring off and on: it
      returns to OBS's monitoring device.
- [ ] If the log says OBS has no application signature the mixer could know its streams by, routing cannot work;
      send the log line.

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
- [ ] A page that stays still and then changes (type into a text field after a few seconds without input): the
      change shows in the preview at once, not a second or two late.
- [ ] *Shutdown source when not visible*: hiding the source (eye icon) ends its `obs-haiku-browser` process;
      showing it starts one and the page comes back.
- [ ] *Interact* (right-click the source → Interact): clicking links, scrolling with the wheel, typing into a text
      field (letters, Backspace, arrows, Enter) all work.
- [ ] Helper death is detected immediately: kill an `obs-haiku-browser` process in ProcessController (Kill). The
      log shows the helper's exit at that moment (not 10 s later) and the page comes back within a few seconds.
      Quitting OBS ends every helper at once (ProcessController shows none).
- [ ] Crash report line: make a helper crash (for example `kill -SEGV <team>` from a Terminal, or Debugger's
      *Save report* on one). The log shows one line `browser helper (team N) crashed (...); crash report:
      /boot/system/var/log/crash/app-obs-haiku-browser-....txt`, the file exists and its header names that team,
      and the line is also shown in OBS's System logs. The page is back within about 5-10 s. Note what the
      line says if no report is found.
- [ ] Hung helper: stop one with `kill -STOP <team>` and leave it. After the 30 s ping timeout OBS logs a stall,
      restarts the helper and the page comes back.
- [ ] A page whose size grows (change Width/Height while the page is shown) keeps rendering with no flicker to a
      stale size.
- [ ] **Control audio via OBS**: open a page that plays sound (a YouTube video, or a local file with an
      `<audio autoplay loop>` element). Without the option, the sound is heard and Desktop Audio's meter moves; the
      source is not in the audio mixer. Turn the option on: the page does **not** reload; at once the source
      appears in the audio mixer and its meter moves, the speakers go silent and Desktop Audio's meter stops. The
      log shows `browser helper team N plays loopback-only` and `recording the page's audio ... (team N, ...
      Hz)`. Audio preferences show the stream with Output "None (loopback only)". Record a few seconds: the
      page's sound is in the recording once, in sync with the picture.
- [ ] With the option on, remove the source and add it again (or restart OBS) with the autoplay page: not even
      the first sound is heard on the speakers. The log shows `started browser helper (team N, page audio
      controlled by OBS)`, then `browser helper team N plays loopback-only`, and no `could not be made
      loopback-only in time`. Try it also on a busy system (a build running).
- [ ] With the option on, kill the helper in ProcessController: after it restarts, the log shows `browser helper
      team M plays loopback-only` and `page audio now follows browser helper team M`, and the sound comes back in
      the source only, without a blip on the speakers. Two rerouted sources with different pages: each source's
      meter shows only its own page.
- [ ] Turn the option off again: the page keeps playing without a reload, its sound is heard and recorded by
      Desktop Audio (log: `browser helper team N plays on the speakers again`), and the source leaves the audio
      mixer. A browser dock playing sound is heard and recorded by Desktop Audio at all times. Quit OBS with a
      rerouted source playing: no sound reaches the speakers while it closes. A new OBS run's helpers play as
      usual (nothing is remembered).
- [ ] Restart the media services (Media preferences) with a rerouted source playing: the log shows `the page audio
      loopback went away`, then `plays loopback-only` again, and the sound returns in the source within a few
      seconds.
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

### Hardware encoders (haiku-video-accel)

No hardware back end exists yet, so these steps test the plumbing with Renga's software reference back end. That
back end encodes VP9 only (its FFmpeg has no H.264/HEVC/AV1 encoder), so the encoder to use is VP9.

- [ ] Plain `./obs`: the log has `[haiku-video-accel] N video acceleration back end(s); choice "auto"`, lists
      the `software` back end as "software", and says "no usable back end encodes ..." for h264, hevc, av1 and
      vp9. *Settings → Output* (Advanced) lists no "Hardware ..." encoder.
- [ ] `RENGA_VIDEO_ACCELERATION=software ./obs` (without the second variable): still nothing offered; the log
      says to set `OBS_HAIKU_VA_ALLOW_SOFTWARE=1`.
- [ ] `RENGA_VIDEO_ACCELERATION=software OBS_HAIKU_VA_ALLOW_SOFTWARE=1 ./obs`: the log has
      `offering "VP9 via software (software test back end)" (haiku_va_vp9) ... SOFTWARE TEST BACK END` and the
      profile `profile0`.
- [ ] *Settings → Output*, Output Mode "Advanced", tab *Recording*: Type "Standard", Recording Format
      "Matroska Video (.mkv)", Video Encoder "VP9 via software (software test back end)". The encoder's
      properties show the back end line, the yellow software warning, Rate Control (switching it swaps Bitrate
      and Quality), Keyframe Interval and Profile ("Profile 0 (8-bit)" only).
- [ ] Record 10 seconds of a screen capture with Target bitrate 6000 Kbps and Keyframe Interval 2 s. The log has
      `vp9 profile0 on software (...): WxH at F fps, bitrate 6000 kbps, keyframe every N frames, NV12 surfaces`
      and no `[haiku-video-accel]` errors or warnings.
- [ ] `ffprobe -v error -show_entries stream=codec_name,width,height,avg_frame_rate:format=duration file.mkv`
      says `vp9`, the canvas output size, OBS's frame rate and a duration of about 10 s.
- [ ] `ffprobe -v error -select_streams v -skip_frame nokey -show_entries frame=pts_time -of csv file.mkv` lists
      a key frame at 0 and then about every 2 s. The file plays in MediaPlayer.
- [ ] Same with Rate Control "Constant quality" (quality 70) and Keyframe Interval 0: the recording works.
- [ ] Rescale the recording output to more than 4096x2304 (e.g. 5120x2880) and start recording: OBS shows its
      normal "failed to start" error, the log has `... is larger than the software back end encodes ...`, and OBS
      keeps running. Set the size back.
- [ ] Start and stop a recording five times in a row: every file is valid and OBS does not crash on stop or quit.
- [ ] Once a hardware back end is installed (Quasar Q-3), with plain `./obs`: "Hardware H.264 (<back end>)" etc.
      are offered without either variable. Record to MP4 and stream with "Hardware H.264", and check with `ffprobe`
      that the stream is `h264` with SPS/PPS (the file plays in another player). Note any log warning about
      packets out of presentation order or unknown codec configuration formats.

## 9. Streaming (optional)

- [ ] *Settings → Stream*: a service with a stream key (e.g. YouTube or Twitch, preferably an unlisted/test
      stream). *Start Streaming* connects and the stream plays on the service.
- [ ] Bandwidth test or a custom RTMP server if available.
- [ ] WHIP (WebRTC) to a test server, if `libdatachannel_devel` was installed.
- [ ] **SRT**: *Settings → Stream → Service: Custom*, server `srt://<other-machine>:9000?mode=caller` with a listener
      on the other machine (e.g. `ffplay -i "srt://0.0.0.0:9000?mode=listener"` or OBS's own Media Source in
      listener mode on another computer). The stream plays.
- [ ] **RIST**: server `rist://<other-machine>:9000`, receiver `ffplay rist://@:9000`. The stream plays.

### Virtual camera (haiku-virtualcam)

Needs a Renga image with the virtual camera add-on (`d6f1bc7b` or later). CodyCam is the reference consumer.

- [ ] The log has no `[haiku-virtualcam] virtual_camera.media_addon not found`, and the controls dock shows
      *Start Virtual Camera* with its gear button. (On an image without the add-on the button is absent and the
      log has that line.)
- [ ] With a scene that has a recognisable picture, click *Start Virtual Camera*. The log has
      `[haiku-virtualcam] feeding the Virtual Camera with WxH at F fps` (the output resolution and FPS from
      *Settings → Video*) and `==== Virtual Camera Start`. The button changes to *Stop Virtual Camera*.
- [ ] Open CodyCam, choose *Camera → Virtual Camera*: it shows OBS's program, moving, in the right colours (red is
      red, not blue) and the right way up. Change scenes in OBS: CodyCam follows within a frame or two.
- [ ] While it runs, a second consumer shows the same feed: e.g. the Media preferences' video input set to
      "Virtual Camera", or a second CodyCam.
- [ ] Gear button: Output Type "Preview", "Scene" and "Source" each show the chosen picture in
      CodyCam (the camera restarts when the type is changed while running).
- [ ] *Stop Virtual Camera*: CodyCam shows the dark grey "no signal" frame within about a second; the log has
      `[haiku-virtualcam] N frames sent, M dropped`, `[haiku-virtualcam] stopped` and `==== Virtual Camera Stop`;
      M is 0 or small on an idle system. Start and stop five times in a row: no hang, no crash, and the camera
      works each time.
- [ ] **Busy system**: with CodyCam showing the camera, load the machine (a build with all cores, or
      `kill -STOP` the `media_addon_server` for 10 s, then `kill -CONT`). OBS's preview, other outputs and
      recording keep running without stutter (the video thread never waits for the camera); the log shows
      `frames dropped in the last minute` lines while the camera lags, and the camera picks up again afterwards.
      With `media_addon_server` stopped, *Stop Virtual Camera* returns at once.
- [ ] Change *Settings → Video → Output (Scaled) Resolution* (e.g. 1920x1080 to 1280x720) while stopped, then start
      again: the log names the new size and CodyCam still shows a full, undistorted picture.
- [ ] **Busy**: while OBS feeds the camera, run the Renga test `media_vcam_test` (or a second OBS instance with
      `--multi`) and start its virtual camera: it is refused (`B_BUSY`). The reverse: start the other feeder
      first, then *Start Virtual Camera* in OBS: OBS shows "Failed to start virtual camera" with "Another
      application is already feeding Renga's Virtual Camera ...", and the button stays *Start Virtual Camera*.
- [ ] **Media services gone**: while the camera runs, restart the media services (Media preferences → *Restart
      media services*). Within a second or two OBS shows "Virtual camera stopped" with "The connection to Renga's
      Virtual Camera was lost ...", the button returns to *Start Virtual Camera*, and OBS keeps running (no
      SIGPIPE). After the services are back, *Start Virtual Camera* works again.
- [ ] **Media services not running**: quit media_server and media_addon_server (e.g. with ProcessController),
      then *Start Virtual Camera*: the error says the media services are not running.
- [ ] Quit OBS while the virtual camera runs: no hang on exit; CodyCam falls back to "no signal".
- [ ] Note the *Stats* dock's render/encode lag with the camera running at the canvas size (the output converts
      to BGRA on the CPU and copies each frame twice).
- [ ] Do **not** add the "Virtual Camera" device as a *Video Capture Device* source in the OBS that feeds it
      (it would show itself, a feedback loop). Just check it is listed there.

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
- [ ] After quitting, ProcessController shows no leftover `obs-haiku-browser`, and system sound works.

## 14. Reporting back

Run:

```sh
build-aux/haiku/collect-logs.sh
```

It creates `obs-haiku-report-<date>.zip` on the Desktop with the build log, CMake cache, the last five OBS
logs, Debugger reports and system/package information. Send it along with this checklist (which boxes
passed, which failed, and what you saw).
