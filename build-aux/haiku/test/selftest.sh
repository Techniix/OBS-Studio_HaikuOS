#!/bin/bash
# Self-test of build-aux/haiku/test.sh on a non-Haiku host (developer use; not part of the maintainer's run).
#
#     build-aux/haiku/test/selftest.sh [scenario]
#
# Replaces ./obs and ./obs-haiku-gl-probe with shell stubs, so that the watchdog, hang detection, SIGTERM/SIGKILL
# handling, recording checks, summary and archive logic run end to end without Haiku. Needs ffmpeg/ffprobe
# (the stub OBS writes its "recording" with ffmpeg). HOME is pointed at a temporary directory.
#
# Scenarios (the stub OBS picks its behaviour from OBS_HAIKU_GL, like the real renderer switch):
#   mixed (default)  default run passes, osmesa run ignores SIGTERM (HANG via SIGKILL), forced EGL run stalls
#                    (HANG); the probe's --verbose run hangs (TIMEOUT); --no-stream exits 1
#   pass             every run passes
#   crash            the default run crashes (SIGSEGV), the rest pass
#   quick            like pass, with --quick (forced EGL run skipped)

set -u
SCENARIO="${1:-mixed}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TEST_SH="$HERE/../test.sh"

T="$(mktemp -d "${TMPDIR:-/tmp}/obs-selftest.XXXXXX")"
BIN="$T/rundir/RelWithDebInfo/bin"
mkdir -p "$BIN" "$T/rundir/RelWithDebInfo/obs-plugins" "$T/home/Desktop"
: >"$T/rundir/RelWithDebInfo/obs-plugins/text-freetype2.so"

# ---- stub probe ---------------------------------------------------------------------------------------
cat >"$BIN/obs-haiku-gl-probe" <<'EOF'
#!/bin/bash
case " $* " in
*" --help "*)
	echo "usage: obs-haiku-gl-probe [--frames N] [--timeout S] [--verbose] [--no-stream] [--fps N]"
	exit 1 ;;
*" --verbose "*)
	[ "${STUB_SCENARIO:-}" = mixed ] && { echo "stub probe: pretending to hang in a fence wait"; sleep 600; } ;;
*" --no-stream "*)
	[ "${STUB_SCENARIO:-}" = mixed ] && { echo "stub probe: failure"; exit 1; } ;;
esac
echo "stub probe: renderer llvmpipe, $* ok"
sleep 1
exit 0
EOF

# ---- stub OBS -----------------------------------------------------------------------------------------
cat >"$BIN/obs" <<'EOF'
#!/bin/bash
# Behaviour: ok | noexit (ignores SIGTERM) | stall (stops logging and sleeps) | crash (SIGSEGV after 3 s)
mode=ok
case "${STUB_SCENARIO:-}:${OBS_HAIKU_GL:-}" in
mixed:osmesa) mode=noexit ;;
mixed:egl) mode=stall ;;
crash:) mode=crash ;;
esac
base="$XDG_CONFIG_HOME/obs-studio"
mkdir -p "$base/logs"
log="$base/logs/$(date +%Y-%m-%d\ %H-%M-%S).txt"
rec="$(sed -n 's/^FilePath=//p' "$base/basic/profiles/HaikuTest/basic.ini")"
say() { echo "$(date +%H:%M:%S.000): $*" | tee -a "$log"; }
say "args: $*"
say "stub mode: $mode"
[ -f "$base/basic/scenes/HaikuTest.json" ] && say "scene collection found"
if [ "${OBS_HAIKU_GL:-}" = osmesa ]; then
	say "Haiku GL: OBS_HAIKU_GL=osmesa, using OSMesa"
elif [ "${OBS_HAIKU_GL:-}" = egl ]; then
	say "Haiku EGL: version 1.5, vendor: Mesa Project"
	say "Haiku GL: driver hang watchdog disabled (OBS_HAIKU_GL_WATCHDOG=0)"
else
	say "Haiku GL: the OpenGL driver failed its self-test: no result after 8 s, the driver hangs"
	say "Haiku GL: using the CPU renderer (OSMesa) instead of the hardware OpenGL driver."
fi
say "Loading up OpenGL on adapter Mesa llvmpipe"
start=0
finish() {
	local secs=$(( $(date +%s) - start ))
	[ "$secs" -lt 1 ] && secs=1
	ffmpeg -hide_banner -nostdin -loglevel error -f lavfi -i testsrc=size=1280x720:rate=30 \
		-f lavfi -i sine=frequency=440 -t "$secs" -c:v libx264 -preset ultrafast -c:a aac -y \
		"$rec/stub $(date +%H-%M-%S).mkv"
	say "==== Shutting down"
	exit 0
}
if [ "$mode" = noexit ] || [ "$mode" = stall ]; then trap '' TERM; else trap finish TERM; fi
sleep 2
start=$(date +%s)
say "==== Recording Start ==============================================="
n=0
while :; do
	n=$((n + 1))
	[ "$mode" = crash ] && [ "$n" -ge 3 ] && kill -SEGV $$
	if [ "$mode" = stall ] && [ "$n" -ge 2 ]; then sleep 1000; fi
	say "frame stats $n"
	sleep 1 & wait $!
done
EOF
chmod +x "$BIN/obs" "$BIN/obs-haiku-gl-probe"

args=(--no-build)
[ "$SCENARIO" = quick ] && args+=(--quick)
echo "### scenario $SCENARIO, temp $T"
export STUB_SCENARIO="$SCENARIO"
HOME="$T/home" OBS_TEST_ASSUME_HAIKU=0 OBS_TEST_BIN_DIR="$BIN" OBS_TEST_REC_SECS=12 OBS_TEST_STALL_SECS=6 \
	OBS_TEST_EXIT_GRACE=4 OBS_TEST_PROBE_TIMEOUT=5 OBS_TEST_STARTUP_TIMEOUT=20 \
	bash "$TEST_SH" "${args[@]}"
rc=$?
echo "### test.sh exit $rc"
echo "### stub processes still running:"
ps -eo pid,args | grep -F "$T" | grep -v grep || echo "(none)"
echo "### archive:"
ls -la "$T/home/Desktop"
echo "$T"
