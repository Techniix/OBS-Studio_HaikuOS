# Building OBS Studio on Haiku

These steps are for Haiku R1 (beta 5 or a recent nightly) on x86_64, building natively. Cross-compiling from
Linux is covered at the end.

## 1. Get the source

```sh
git clone https://github.com/Techniix/OBS-Studio_HaikuOS.git
cd OBS-Studio_HaikuOS
git submodule update --init deps/simde
```

SIMDe comes from the `deps/simde` submodule, so it must be checked out. `plugins/obs-websocket` is only
needed if you build obs-websocket (`-DENABLE_WEBSOCKET=ON`, which also needs `qrcodegencpp`). The build
script fetches SIMDe itself if it's missing.

The `obs-browser` and `libdshowcapture` submodules aren't used on Haiku.

## 2. Install dependencies

```sh
build-aux/haiku/install-deps.sh            # required + recommended
build-aux/haiku/install-deps.sh --optional # also VLC, FDK AAC
```

Or by hand:

| Purpose | Packages |
| --- | --- |
| Toolchain | `cmake ninja pkgconfig` (GCC comes with Haiku) |
| Core | `ffmpeg8_devel x264_devel jansson_devel curl_devel mbedtls3_devel zlib_devel uthash` |
| UI | `qt6_base_devel qt6_svg_devel` |
| Rendering | `mesa_devel mesa_eglvnd libglvnd_devel` |
| Text source | `freetype_devel fontconfig_devel` |
| Plugin manager, WebRTC | `nlohmann_json libdatachannel_devel` |
| Scripting (optional) | `luajit_devel swig python3.12` |
| Noise suppression (optional) | `speexdsp_devel rnnoise_devel` |
| VLC source, FDK AAC (optional) | `vlc_devel fdk_aac_devel` |

OBS 33 needs FFmpeg 8 (`ffmpeg8_devel`). If `pkgman` reports a conflict with an older `ffmpeg*_devel`
package, uninstall that one first.

`mesa_eglvnd` provides the EGL driver used at runtime; without it OBS can't create its OpenGL context.

## 3. Configure and build

The helper script picks sensible options, disables features whose dependencies are missing, and writes a
complete log:

```sh
build-aux/haiku/build.sh            # builds into ./build
build-aux/haiku/build.sh build-dbg -DCMAKE_BUILD_TYPE=Debug
```

Manual equivalent:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_WEBSOCKET=OFF \
    -DENABLE_NEW_MPEGTS_OUTPUT=OFF
cmake --build build
```

### Options

| Option | Default on Haiku | Notes |
| --- | --- | --- |
| `ENABLE_WEBSOCKET` | ON | Turn **off**: obs-websocket needs `qrcodegencpp`, which isn't in HaikuPorts. |
| `ENABLE_NEW_MPEGTS_OUTPUT` | ON | Turn **off**: needs `libsrt` and `librist`. |
| `ENABLE_SCRIPTING` | ON | Needs SWIG, LuaJIT and Python. `ENABLE_SCRIPTING_LUA`/`ENABLE_SCRIPTING_PYTHON` toggle each. |
| `ENABLE_WEBRTC` | ON | WHIP output, needs `libdatachannel_devel`. |
| `ENABLE_SPEEXDSP`, `ENABLE_RNNOISE` | ON | Noise suppression filter. RNNoise falls back to a bundled copy. |
| `CMAKE_COMPILE_WARNING_AS_ERROR` | OFF | Upstream uses ON; Haiku defaults to OFF. |
| `ENABLE_RELOCATABLE` | OFF | Use `$ORIGIN` RPATHs so the install tree can be moved. |
| `CMAKE_INSTALL_PREFIX` | `/boot/home/config/non-packaged` | |

Browser source, AJA, DeckLink, VST and the Linux capture plugins are skipped automatically.

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
| `Required submodule 'obs-websocket' not available` | Pass `-DENABLE_WEBSOCKET=OFF` (the build script does), or fetch the submodule. |
| `Could NOT find FFmpeg (... minimum required is "8.0")` | Install `ffmpeg8_devel` (and remove older FFmpeg devel packages). |
| `Could NOT find OpenGL (missing: EGL)` | Install `libglvnd_devel` and `mesa_devel`. |
| `Could not find a package configuration file provided by "Qt6"` | Install `qt6_base_devel qt6_svg_devel`. |
| `Could not find ... "MbedTLS"` | Install `mbedtls3_devel` (OBS needs mbedTLS 3, not 2). |
| `Could NOT find SIMDe` | The `deps/simde` submodule is missing, see step 1. |
| `Could not find ... "LibDataChannel"` | Install `libdatachannel_devel` or pass `-DENABLE_WEBRTC=OFF`. |
| Log says `Haiku EGL: eglInitialize failed` | `mesa_eglvnd` isn't installed, or no EGL vendor JSON is found. |
| Log says `Haiku GL: OpenGL 3.3 is required` | The GL driver in use only offers an older version; check the GLInfo app. |

When reporting a problem, run `build-aux/haiku/collect-logs.sh` and attach the zip it puts on the Desktop.

## Cross-compiling from Linux

The build system selects the Haiku configuration from the target name, so a Haiku cross toolchain
(Haiku's `buildtools` with a sysroot of the required `_devel` packages) works:

```sh
cmake -S . -B build-haiku -G Ninja \
    -DCMAKE_SYSTEM_NAME=Haiku \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/haiku-toolchain.cmake \
    -DENABLE_WEBSOCKET=OFF -DENABLE_NEW_MPEGTS_OUTPUT=OFF
```

Pass `CMAKE_SYSTEM_NAME` on the command line as well as in the toolchain file; OBS decides the platform
before `project()` loads the toolchain. Qt cross builds also need `-DQT_HOST_PATH=<host Qt prefix>` for
moc/uic/rcc.

`build-aux/haiku/crosscheck/` contains the harness used to verify the port without a Haiku machine: clang
targeting `x86_64-unknown-haiku` against the real Haiku headers and dependency headers, with linking stubbed
out. See its README.
