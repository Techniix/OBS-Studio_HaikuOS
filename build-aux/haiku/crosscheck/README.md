# Haiku cross-check harness

Verifies that the whole project configures and compiles for Haiku **without a Haiku machine**. It was used
to develop this port and is meant for checking changes (including upstream rebases) before testing on real
hardware.

## How it works

- `haiku-toolchain.cmake` makes CMake target `Haiku`/`x86_64`, so all `OS_HAIKU`, `$<PLATFORM_ID:Haiku>` and
  CMake's own `Platform/Haiku.cmake` logic runs exactly as on Haiku.
- Dependencies are found in a fake sysroot laid out like Haiku (`/boot/system/develop/headers`,
  `/boot/system/develop/lib`, `/boot/system/lib/cmake`). It holds the **real headers** of the dependency
  versions HaikuPorts ships (FFmpeg 8, mbedTLS 3, curl, x264, FreeType, EGL, nlohmann_json, libdatachannel,
  SpeexDSP, RNNoise, LuaJIT, uthash…) and empty stub `.so` files.
- Qt is built for the host from the same Qt 6 release series as HaikuPorts' `qt6_base`; its headers and
  moc/uic/rcc are used for the frontend.
- `hwrap.py` is the "compiler". It runs `clang --target=x86_64-unknown-haiku -nostdinc` with the Haiku
  system headers from `haiku/haiku`. CMake's configure-time checks are compiled for real. Project sources are
  compiled with `-fsyntax-only`. Links only create placeholder files.
- Every compile that fails or warns is appended to `work/errors.log`.

## What it catches and what it doesn't

Catches: CMake configure errors on Haiku, missing/incorrect `#include`s, Haiku API misuse (wrong types,
signatures, constants), code in `#ifdef` branches that doesn't compile for Haiku, and missing moc/uic
outputs.

Doesn't catch: GCC-only diagnostics (Haiku builds with GCC 13), link errors (missing symbols or libraries),
anything that only shows at runtime. The C++ standard library headers come from the host's libstdc++, with a
small shim (`shim/stdlib.h`) for the two functions Haiku's libroot lacks. The fontconfig header is a stub
(`fake-configs/fontconfig.h`) because its repository isn't reachable from GitHub.

## Usage

```sh
build-aux/haiku/crosscheck/setup.sh          # once: headers, sysroot, host Qt (1-2 h for Qt)
build-aux/haiku/crosscheck/run.sh            # configure + syntax-check everything
```

`run.sh` passes extra arguments to CMake, e.g. `run.sh . -DENABLE_FRONTEND=OFF`. Scripting is off because
SWIG isn't part of the harness. Set `HCHECK_WORK` to keep the work directory elsewhere.

Expected result for this tree: configure succeeds, and `errors.log` has no Haiku-related errors.
