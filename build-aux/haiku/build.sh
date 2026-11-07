#!/bin/bash
# Configures and builds OBS Studio for Haiku.
# Usage: build-aux/haiku/build.sh [build-dir] [extra cmake args...]
#
# The build log is written to <build-dir>/haiku-build.log (attach it when reporting build problems).

set -u
set -o pipefail

SRC="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$SRC/build}"
shift || true

if [ ! -f "$SRC/deps/simde/simde/simde-common.h" ]; then
	echo "==> Fetching the SIMDe submodule"
	git -C "$SRC" submodule update --init deps/simde || exit 1
fi

# Turn off features whose dependencies are missing instead of failing the configure step.
OPTS=()
have_header() {
	for d in /boot/system/develop/headers /boot/system/non-packaged/develop/headers \
		/boot/home/config/non-packaged/develop/headers; do
		[ -e "$d/$1" ] && return 0
	done
	return 1
}
command -v swig >/dev/null 2>&1 || OPTS+=(-DENABLE_SCRIPTING=OFF)
have_header speex/speex_preprocess.h || OPTS+=(-DENABLE_SPEEXDSP=OFF)
have_header rnnoise.h || OPTS+=(-DENABLE_RNNOISE=OFF)
have_header rtc/rtc.hpp || OPTS+=(-DENABLE_WEBRTC=OFF)
[ ${#OPTS[@]} -gt 0 ] && echo "==> Missing optional dependencies, adding: ${OPTS[*]}"

LOG="$BUILD/haiku-build.log"
mkdir -p "$BUILD"
: > "$LOG"

echo "==> Configuring (log: $LOG)"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	-DENABLE_WEBSOCKET=OFF \
	-DENABLE_NEW_MPEGTS_OUTPUT=OFF \
	"${OPTS[@]}" "$@" 2>&1 | tee -a "$LOG" || { echo "!! configure failed, see $LOG"; exit 1; }

echo "==> Building"
# -k 0: keep going so a single failing file doesn't hide the others
cmake --build "$BUILD" -- -k 0 2>&1 | tee -a "$LOG"
status=$?

if [ $status -ne 0 ]; then
	echo "!! build failed. Errors:"
	grep -E "error:|Error [0-9]|FAILED:" "$LOG" | head -60
	exit $status
fi

echo "==> Build finished. Run it with:"
echo "    cd \"$BUILD/rundir/RelWithDebInfo/bin\" && ./obs"
