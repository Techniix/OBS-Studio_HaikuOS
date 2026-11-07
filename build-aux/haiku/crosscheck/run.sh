#!/bin/bash
# Configures OBS for Haiku with the emulated toolchain and syntax-checks every translation unit.
# Usage: run.sh [obs-source-dir] [extra cmake args...]
# Results: $HCHECK_WORK/build (build tree), $HCHECK_WORK/errors.log (all compiler errors/warnings)

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
export HCHECK_WORK="${HCHECK_WORK:-$HERE/work}"
SRC="$(cd "${1:-$HERE/../../..}" && pwd)"
shift || true
BUILD="$HCHECK_WORK/build"

rm -rf "$BUILD" "$HCHECK_WORK/errors.log"
mkdir -p "$BUILD"

cmake -S "$SRC" -B "$BUILD" -G Ninja \
	-DCMAKE_SYSTEM_NAME=Haiku \
	-DCMAKE_TOOLCHAIN_FILE="$HERE/haiku-toolchain.cmake" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
	-DENABLE_WEBSOCKET=OFF \
	-DENABLE_NEW_MPEGTS_OUTPUT=OFF \
	-DENABLE_SCRIPTING=OFF \
	"$@" 2>&1 | tee "$BUILD/configure.log"
[ "${PIPESTATUS[0]}" -eq 0 ] || { echo "!! configure failed"; exit 1; }

ninja -C "$BUILD" -k 0 2>&1 | tee "$BUILD/build.log" | grep -E "^\[|FAILED" | tail -3

echo
echo "==> Translation units: $(grep -c '"file"' "$BUILD/compile_commands.json")"
if [ -s "$HCHECK_WORK/errors.log" ]; then
	echo "==> Files with errors: $(grep -c '^=== .*(exit [1-9]' "$HCHECK_WORK/errors.log")"
	echo "==> Files with warnings only: $(grep -c '^=== .*(exit 0)' "$HCHECK_WORK/errors.log")"
	echo "    details: $HCHECK_WORK/errors.log"
else
	echo "==> No errors or warnings"
fi
grep -E "^FAILED" "$BUILD/build.log" | grep -v "\.o$" | head -20
