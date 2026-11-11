# Building OBS Studio on Haiku

These steps are for Haiku R1 (beta 5 or a recent nightly) on x86_64, building natively. Cross-compiling from
Linux is covered at the end.

## 1. Get the source

```sh
git clone https://github.com/Techniix/OBS-Studio_HaikuOS.git
cd OBS-Studio_HaikuOS
git submodule update --init --recursive deps/simde deps/asio deps/websocketpp deps/srt deps/librist \
    plugins/obs-websocket plugins/obs-browser
```

`build-aux/haiku/build.sh` fetches any of these that are missing, so the last step is optional when you use it.

| Submodule | Why Haiku needs it |
| --- | --- |
| `deps/simde` | SIMD intrinsics wrapper; not packaged in HaikuPorts |
| `deps/asio`, `deps/websocketpp` | obs-websocket (HaikuPorts' Asio 1.38 removed APIs WebSocket++ uses; 1.30.2 is bundled) |
| `deps/srt`, `deps/librist` | SRT/RIST output; built from source by `deps/srt-haiku` and `deps/librist-haiku` |
| `plugins/obs-websocket` | obs-websocket itself |
| `plugins/obs-browser` | Only `panel/browser-panel.hpp`, the browser dock interface; the CEF plugin is not built |

`libdshowcapture` is Windows-only and not needed.

## 2. Install dependencies

```sh
build-aux/haiku/install-deps.sh            # required + recommended
build-aux/haiku/install-deps.sh --optional # also FDK AAC
```

Or by hand:

| Purpose | Packages |
| --- | --- |
| Toolchain | `cmake ninja pkgconfig` (GCC comes with Haiku) |
| Core | `ffmpeg8_devel x264_devel jansson_devel curl_devel mbedtls3_devel zlib_devel uthash` |
| UI | `qt6_base_devel qt6_svg_devel` |
| Rendering | `mesa_devel mesa_swpipe` (Mesa 22: OSMesa; Mesa 25+: also `mesa_eglvnd libglvnd_devel`) |
| Text source | `freetype_devel fontconfig_devel` |
| Plugin manager, WebRTC | `nlohmann_json libdatachannel_devel` |
| Scripting (optional) | `luajit_devel swig python3.12` |
| Noise suppression (optional) | `speexdsp_devel rnnoise_devel` |
| VLC video source | `vlc_devel` |
| Browser source and docks | `haikuwebkit_devel` |
| FDK AAC encoder (optional) | `fdk_aac_devel` |

OBS 33 needs FFmpeg 8 (`ffmpeg8_devel`). If `pkgman` reports a conflict with an older `ffmpeg*_devel`
package, uninstall that one first.

OBS renders through Mesa's OSMesa library (Mesa 22.0.5, the current HaikuPorts version, has its Haiku EGL driver
disabled). When HaikuPorts publishes Mesa 25 with `mesa_eglvnd`, OBS uses EGL instead, without rebuilding; the
install script installs it automatically once it exists.

`haikuwebkit` (the engine of WebPositive) is part of a standard Haiku install; `haikuwebkit_devel` adds the headers
and libraries the browser helper links against.

qrcodegen (for obs-websocket's QR code), libsrt and librist aren't in HaikuPorts; the build uses a system copy if one
is installed and otherwise builds the bundled one.

## 3. Configure and build

The helper script picks sensible options, disables features whose dependencies are missing, and writes a
complete log:

```sh
build-aux/haiku/build.sh            # builds into ./build
build-aux/haiku/build.sh build-dbg -DCMAKE_BUILD_TYPE=Debug
```

Manual equivalent:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

### Options

| Option | Default on Haiku | Notes |
| --- | --- | --- |
| `ENABLE_WEBSOCKET` | ON | obs-websocket with bundled Asio/WebSocket++/qrcodegen. |
| `ENABLE_NEW_MPEGTS_OUTPUT` | ON | Native SRT/RIST output with bundled libsrt/librist. |
| `ENABLE_BROWSER` | ON | Browser source and docks via HaikuWebKit (`haikuwebkit_devel`). `build.sh` turns it off if the headers are missing. |
| `ENABLE_SCRIPTING` | ON | Needs SWIG, LuaJIT and Python. `ENABLE_SCRIPTING_LUA`/`ENABLE_SCRIPTING_PYTHON` toggle each. |
| `ENABLE_WEBRTC` | ON | WHIP output, needs `libdatachannel_devel`. |
| `ENABLE_VLC` | ON | VLC video source, needs `vlc_devel`. |
| `ENABLE_SPEEXDSP`, `ENABLE_RNNOISE` | ON | Noise suppression filter. RNNoise falls back to a bundled copy. |
| `CMAKE_COMPILE_WARNING_AS_ERROR` | OFF | Upstream uses ON; Haiku defaults to OFF. |
| `ENABLE_RELOCATABLE` | OFF | Use `$ORIGIN` RPATHs so the install tree can be moved. |
| `CMAKE_INSTALL_PREFIX` | `/boot/home/config/non-packaged` | |

AJA, DeckLink, NVENC/QSV/VA-API and the Linux capture plugins have no Haiku drivers and are skipped.

The build produces helper programs next to `obs` in `bin/`, including `obs-haiku-browser` (browser rendering).
OBS starts them itself; they must stay in the same directory as `obs`
(or be installed to `/boot/home/config/non-packaged/bin`).

## 4. Run

```sh
cd build/rundir/RelWithDebInfo/bin
./obs
```

Useful flags: `--verbose` (more log output), `--unfiltered_log` (don't collapse repeated log lines),
`--safe-mode` (no third-party plugins/scripts), `--multi` (skip the single-instance check).

Settings, scene collections and logs: `~/config/settings/obs-studio` (logs in `logs/`, one file per run).

## 5. Install

```sh
cmake --install build
/boot/home/config/non-packaged/bin/obs
```

## Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| `Required submodule 'obs-websocket' not available` | Fetch the submodules (step 1). |
| `HaikuWebKit not found` | `pkgman install haikuwebkit_devel`, or `-DENABLE_BROWSER=OFF`. |
| `plugins/obs-browser submodule missing` warning | Browser docks are skipped; fetch `plugins/obs-browser` (step 1). |
| Log says `[haiku-browser] ... helper ... is missing` | `obs-haiku-browser` isn't next to `obs`; rebuild or reinstall. |
| `Could NOT find FFmpeg (... minimum required is "8.0")` | Install `ffmpeg8_devel` (and remove older FFmpeg devel packages). |
| `Could NOT find OpenGL (missing: EGL)` | Install `mesa_devel`. |
| `Could not find a package configuration file provided by "Qt6"` | Install `qt6_base_devel qt6_svg_devel`. |
| `Could not find ... "MbedTLS"` | Install `mbedtls3_devel` (OBS needs mbedTLS 3, not 2). |
| `Could NOT find SIMDe` | The `deps/simde` submodule is missing, see step 1. |
| `Could not find ... "LibDataChannel"` | Install `libdatachannel_devel` or pass `-DENABLE_WEBRTC=OFF`. |
| Log says `Haiku EGL: eglInitialize failed` | Expected with Mesa 22; the next lines should say `using OSMesa`. |
| Log says `no OpenGL backend available` / `libOSMesa not found` | `pkgman install mesa mesa_devel`. |
| Log says `Haiku GL: OpenGL 3.3 is required` | The GL driver in use only offers an older version; check the GLInfo app. |

When reporting a problem, run `build-aux/haiku/collect-logs.sh` and attach the zip it puts on the Desktop.

## Cross-compiling from Linux

The build system selects the Haiku configuration from the target name, so a Haiku cross toolchain
(Haiku's `buildtools` with a sysroot of the required `_devel` packages) works:

```sh
cmake -S . -B build-haiku -G Ninja \
    -DCMAKE_SYSTEM_NAME=Haiku \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/haiku-toolchain.cmake
```

Pass `CMAKE_SYSTEM_NAME` on the command line as well as in the toolchain file; OBS decides the platform
before `project()` loads the toolchain. Qt cross builds also need `-DQT_HOST_PATH=<host Qt prefix>` for
moc/uic/rcc.

`build-aux/haiku/crosscheck/` contains the harness used to verify the port without a Haiku machine: clang
targeting `x86_64-unknown-haiku` against the real Haiku headers and dependency headers, with linking stubbed
out. See its README.
