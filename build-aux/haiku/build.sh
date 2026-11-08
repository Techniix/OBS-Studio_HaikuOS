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

# Submodules used on Haiku (libdshowcapture isn't needed; of obs-browser only the panel interface header is used)
SUBMODULES=(deps/simde deps/asio deps/websocketpp deps/srt deps/librist plugins/obs-websocket plugins/obs-browser)
if [ -e "$SRC/.git" ]; then
	# Always update: after a pull that moves a submodule pin, an existing checkout would otherwise stay on the old commit
	echo "==> Updating submodules: ${SUBMODULES[*]}"
	git -C "$SRC" submodule update --init --recursive "${SUBMODULES[@]}" || exit 1
else
	missing=()
	for m in "${SUBMODULES[@]}"; do
		[ -e "$SRC/$m/.git" ] || missing+=("$m")
	done
	[ ${#missing[@]} -gt 0 ] && echo "!! Not a git checkout, cannot fetch submodules: ${missing[*]}"
fi

# Turn off features whose dependencies are missing instead of failing the configure step. Features whose
# dependencies are present are turned on explicitly, so a feature switched off by an earlier run (the value stays
# in the CMake cache) comes back once its dependency is installed.
OPTS=()
DISABLED=()
HEADER_DIRS=(/boot/system/develop/headers /boot/system/non-packaged/develop/headers
	/boot/home/config/non-packaged/develop/headers)
# have_header <path relative to the headers directory, may contain glob patterns>
have_header() {
	local d f
	for d in "${HEADER_DIRS[@]}"; do
		for f in "$d"/$1; do
			[ -e "$f" ] && return 0
		done
	done
	return 1
}
# feature <option> <condition...>: -D<option>=ON if the condition succeeds, OFF otherwise
feature() {
	local opt="$1"
	shift
	if "$@"; then
		OPTS+=("-D$opt=ON")
	else
		OPTS+=("-D$opt=OFF")
		DISABLED+=("$opt")
	fi
}
have_luajit() { have_header 'luajit*/luajit.h' || have_header luajit.h; }
have_python() { command -v python3 >/dev/null 2>&1 && have_header 'python3*/Python.h'; }
have_scripting() {
	command -v swig >/dev/null 2>&1 || return 1
	have_luajit || have_python
}

feature ENABLE_SCRIPTING have_scripting
feature ENABLE_SCRIPTING_LUA have_luajit
feature ENABLE_SCRIPTING_PYTHON have_python
feature ENABLE_SPEEXDSP have_header speex/speex_preprocess.h
# RNNoise needs no package: without rnnoise_devel the bundled copy (plugins/obs-filters/rnnoise) is used.
# Passed explicitly to undo the OFF that older versions of this script stored in the cache.
OPTS+=(-DENABLE_RNNOISE=ON)
feature ENABLE_WEBRTC have_header rtc/rtc.hpp
feature ENABLE_VLC have_header vlc/libvlc.h
feature ENABLE_BROWSER have_header WebView.h
feature ENABLE_LIBFDK have_header fdk-aac/aacenc_lib.h
[ ${#DISABLED[@]} -gt 0 ] && echo "==> Missing optional dependencies, turning off: ${DISABLED[*]}"

LOG="$BUILD/haiku-build.log"
mkdir -p "$BUILD"
: > "$LOG"

echo "==> Configuring (log: $LOG)"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
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
