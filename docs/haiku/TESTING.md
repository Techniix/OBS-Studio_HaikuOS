# Testing the Haiku port

A step-by-step checklist for the first runs on real hardware. Work through it in order; each step says what
should happen and what to collect if it doesn't. Tick boxes as you go and note anything odd. Exact error
text and log lines are the most useful thing to send back.

> **Before you start:** run `pkgman update` and reboot if it updated the system, so Haiku, Mesa and Qt are
> current. Testing with headphones avoids feedback in the audio monitoring tests.

## 0. What was verified before this test

The port has **not** been built on Haiku yet. Without a Haiku machine, it was checked with a harness
(`build-aux/haiku/crosscheck/`):

- CMake configures the whole project as `CMAKE_SYSTEM_NAME=Haiku` against Haiku-layout dependencies.
- Every C/C++ file in the build, including Qt moc/uic output, compiles (syntax-only) with clang for
  `x86_64-unknown-haiku` against the real Haiku system headers and real dependency headers.

What that can't catch: GCC-specific errors, link errors (missing libraries or symbols), and runtime
behavior. Those are what this test is for.

## 1. Build

```sh
cd ~/OBS-Studio_HaikuOS            # wherever you cloned it (with --recursive)
git pull && git submodule update --init deps/simde
build-aux/haiku/install-deps.sh
build-aux/haiku/build.sh
```

- [ ] `install-deps.sh` finishes without "!!" lines for the required packages.
- [ ] CMake configure succeeds. The summary at the end lists enabled/disabled features; note which
      plugins are disabled.
- [ ] The build finishes.

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
  Haiku EGL: version 1.5, vendor: Mesa Project
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
      ...
  ```
  If a module is missing from that list, search the log for its name; libobs logs why it failed to load
  (usually a missing shared library).
- [ ] The preview area in the middle shows the canvas (black when there are no sources) and **not** a
      frozen or garbage image.
- [ ] Resizing the main window resizes the preview without flicker or stretching artifacts.

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

## 4. Audio monitoring

1. *Settings → Audio → Advanced → Monitoring Device*: "Default".
2. In the Audio Mixer, open the gear menu of the mic source → *Advanced Audio Properties* →
   *Audio Monitoring*: **Monitor and Output**.

- [ ] You hear yourself through the speakers/headphones with a short delay (roughly 50–250 ms).
- [ ] Volume changes on the source's fader change the monitored volume.
- [ ] Switching back to *Monitor Off* stops it. No crash when switching repeatedly.
- [ ] A Media Source with *Monitor Only (mute output)* is audible but its meter in the recording is silent.

## 5. Recording

*Settings → Output*: Output Mode "Simple", Recording Path somewhere writable, Recording Format "Matroska
Video (.mkv)" or "MPEG-4 (.mp4)", Video Encoder "Software (x264)", Audio Encoder "AAC" (FFmpeg).

- [ ] *Start Recording*, record 30 seconds with screen capture and mic, *Stop Recording*.
- [ ] The file plays in MediaPlayer (or VLC) with picture and sound in sync.
- [ ] The *Stats* dock (*Docks → Stats*) shows no dropped/lagged frames at the default canvas size.
      Note the CPU usage and render time if frames are dropped.
- [ ] Try "Advanced" output mode with "Hybrid MP4" as well.

## 6. Streaming (optional)

- [ ] *Settings → Stream*: a service with a stream key (e.g. YouTube or Twitch, preferably an unlisted/test
      stream). *Start Streaming* connects and the stream plays on the service.
- [ ] Bandwidth test or a custom RTMP server if available.
- [ ] WHIP (WebRTC) to a test server, if `libdatachannel_devel` was installed.

## 7. Hotkeys

- [ ] *Settings → Hotkeys*: click the field for *Start Recording* and press **F9**; the field shows "F9".
      Try a combination like **Ctrl+Shift+R**; it shows "Control + Shift + R".
- [ ] With another application focused (e.g. a Terminal), press the hotkey; recording starts. Press the
      Stop hotkey; it stops. (These are global hotkeys polled from the app_server.)
- [ ] Try a numpad key and a punctuation key. Note any key that shows the wrong name or doesn't trigger,
      along with your keyboard layout.

## 8. Windows and projectors

- [ ] Right-click the preview → *Windowed Projector (Preview)*: a separate window shows the canvas and
      resizes correctly.
- [ ] *Fullscreen Projector* on your monitor; Escape or right-click → close works.
- [ ] *Studio Mode*: two previews render; transitions animate.
- [ ] *View → Multiview*: renders all scenes.
- [ ] Source properties dialogs show their live preview at the top.

## 9. Tools

- [ ] *Tools → Automatic Scene Switcher*: the window list shows running applications (e.g. "Terminal",
      "WebPositive"). A rule switches scenes when you activate that application.
- [ ] *Tools → Scripts* (if scripting was built): add a Lua script; if Python was found, the Python
      settings tab shows the path.
- [ ] *Tools → Plugin Manager* opens.

## 10. Lifecycle

- [ ] Quit OBS and start it again: scenes, sources and settings are restored from
      `~/config/settings/obs-studio`.
- [ ] Start a second instance while one is running: OBS warns that it is already running.
- [ ] Quitting doesn't crash or hang. A clean exit doesn't trigger the "OBS didn't shut down properly"
      dialog on the next start.
- [ ] Logs exist in `~/config/settings/obs-studio/logs/`.

## 11. Reporting back

Run:

```sh
build-aux/haiku/collect-logs.sh
```

It creates `obs-haiku-report-<date>.zip` on the Desktop with the build log, CMake cache, the last five OBS
logs, Debugger reports and system/package information. Send it along with this checklist (which boxes
passed, which failed, and what you saw).
