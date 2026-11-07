#!/bin/bash
# Gathers everything useful for debugging the Haiku port into one archive on the Desktop.
# Usage: build-aux/haiku/collect-logs.sh [build-dir]

set -u

SRC="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$SRC/build}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$(mktemp -d)/obs-haiku-report-$STAMP"
mkdir -p "$OUT"

{
	echo "== uname";              uname -a
	echo "== git";                git -C "$SRC" log -1 --oneline; git -C "$SRC" status --short | head -50
	echo "== gcc";                gcc --version 2>&1 | head -1
	echo "== cmake";              cmake --version 2>&1 | head -1
	echo "== packages"
	for p in ffmpeg8 qt6_base mesa mesa_eglvnd libglvnd x264 jansson curl mbedtls3 libdatachannel luajit; do
		pkgman search -i "$p" 2>/dev/null | grep -w "$p" | head -3
	done
	echo "== screen";             screenmode 2>/dev/null
	echo "== media add-ons";      ls /boot/system/add-ons/media 2>/dev/null
} > "$OUT/system.txt" 2>&1

[ -f "$BUILD/haiku-build.log" ] && cp "$BUILD/haiku-build.log" "$OUT/"
[ -f "$BUILD/CMakeCache.txt" ] && cp "$BUILD/CMakeCache.txt" "$OUT/"
[ -f "$BUILD/CMakeFiles/CMakeConfigureLog.yaml" ] && cp "$BUILD/CMakeFiles/CMakeConfigureLog.yaml" "$OUT/"

LOGS="$HOME/config/settings/obs-studio/logs"
if [ -d "$LOGS" ]; then
	mkdir -p "$OUT/obs-logs"
	ls -t "$LOGS"/*.txt 2>/dev/null | head -5 | while read -r f; do cp "$f" "$OUT/obs-logs/"; done
fi

# Haiku's Debugger saves crash reports to the Desktop
for f in "$HOME"/Desktop/obs*-debug-*.report "$HOME"/Desktop/*obs*.report; do
	[ -f "$f" ] && cp "$f" "$OUT/"
done

ARCHIVE="$HOME/Desktop/obs-haiku-report-$STAMP.zip"
(cd "$(dirname "$OUT")" && zip -qr "$ARCHIVE" "$(basename "$OUT")")
echo "Report written to $ARCHIVE"
