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
#                    (HANG); the probe's only-stage run hangs (TIMEOUT), only-stream exits 1
#   pass             every run passes
#   crash            the default run crashes (SIGSEGV), the rest pass
#   quick            like pass, with --quick (forced EGL run skipped)
#   nogpu            like pass, with --no-gpu (no probe, no EGL run, default run with OBS_HAIKU_GL=osmesa)
#   gpuonly          like pass, with --gpu-only (no osmesa run)
#   freeze           a first run is killed with SIGKILL (no traps: like a hard reset) while the probe's
#                    only-stage run is going; a second run must report it in 00-previous-incomplete/, with
#                    the progress file naming that probe command and the fake syslog's previous boot

set -u
SCENARIO="${1:-mixed}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TEST_SH="$HERE/../test.sh"

T="$(mktemp -d "${TMPDIR:-/tmp}/obs-selftest.XXXXXX")"
BIN="$T/rundir/RelWithDebInfo/bin"
mkdir -p "$BIN" "$T/rundir/RelWithDebInfo/obs-plugins" "$T/home/Desktop" "$T/varlog"
: >"$T/rundir/RelWithDebInfo/obs-plugins/text-freetype2.so"

# ---- fake syslog: two boots in syslog.1 + syslog, like syslog_daemon's size-based rotation ------------
printf '%s\n' "KERN: Welcome to syslog debug output!" "KERN: boot A line" "KERN: boot A last line" \
	"KERN: Welcome to syslog debug output!" "KERN: boot B (the frozen one) line" >"$T/varlog/syslog.1"
printf '%s\n' "KERN: boot B: nvidia: fence timeout" "KERN: Welcome to syslog debug output!" \
	"KERN: boot C (now)" >"$T/varlog/syslog"

# ---- stub probe ---------------------------------------------------------------------------------------
cat >"$BIN/obs-haiku-gl-probe" <<'EOF'
#!/bin/bash
# Which pattern is this? "only-X" = every --no-* switch except --no-X.
only=""
case " $* " in *" --no-stage "*) ;; *) case " $* " in *" --no-stream "*) only=stage ;; esac ;; esac
case " $* " in *" --no-stream "*) ;; *) case " $* " in *" --no-stage "*) only=stream ;; esac ;; esac
case " $* " in
*" --help "*)
	echo "usage: obs-haiku-gl-probe [--frames N] [--fps N] [--no-stream] [--no-stage] [--no-second-display]"
	echo "                          [--no-handoff] [--timeout S] [--verbose]"
	exit 1 ;;
esac
echo "stub probe: step: eglInitialize"
case "${STUB_SCENARIO:-}:$only" in
mixed:stage) echo "stub probe: step: glReadPixels (pretending to hang in a fence wait)"; sleep 600 ;;
mixed:stream) echo "stub probe: failure"; exit 1 ;;
freeze:stage) echo "stub probe: step: glReadPixels"; : >"$STUB_FREEZE_FLAG"; sleep 600 ;;
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

args=(--no-build --yes)
case "$SCENARIO" in
quick) args+=(--quick) ;;
nogpu) args+=(--no-gpu) ;;
gpuonly) args+=(--gpu-only) ;;
esac
echo "### scenario $SCENARIO, temp $T"
export STUB_SCENARIO="$SCENARIO" STUB_FREEZE_FLAG="$T/froze"
export HOME="$T/home" OBS_TEST_ASSUME_HAIKU=0 OBS_TEST_BIN_DIR="$BIN" OBS_TEST_REC_SECS=12 OBS_TEST_STALL_SECS=6 \
	OBS_TEST_EXIT_GRACE=4 OBS_TEST_PROBE_TIMEOUT=5 OBS_TEST_STARTUP_TIMEOUT=20 OBS_TEST_SYSLOG_DIR="$T/varlog"

if [ "$SCENARIO" = freeze ]; then
	# Run 1 in its own process group, then SIGKILL the whole group once the probe "hangs": no trap runs,
	# nothing is archived, like a hard reset.
	setsid bash "$TEST_SH" "${args[@]}" >"$T/run1.out" 2>&1 &
	pgid=$!
	for _ in $(seq 1 300); do [ -e "$T/froze" ] && break; sleep 0.2; done
	[ -e "$T/froze" ] || { echo "### FAIL: run 1 never reached the freezing probe"; cat "$T/run1.out"; exit 1; }
	kill -KILL -- "-$pgid"
	sleep 1
	echo "### run 1 killed; its directory:"
	ls "$T/home/obs-test-runs"
	echo "### run 2"
	export STUB_SCENARIO=pass
fi
bash "$TEST_SH" "${args[@]}"
rc=$?
echo "### test.sh exit $rc"
if [ "$SCENARIO" = freeze ]; then
	z="$(ls "$T"/home/Desktop/*.zip 2>/dev/null | head -n 1)"
	x="$T/unzipped"
	mkdir -p "$x" && (cd "$x" && unzip -q "$z")
	ok=1
	p="$(ls "$x"/*/00-previous-incomplete/*/progress.txt 2>/dev/null | head -n 1)"
	if [ -n "$p" ] && tail -n 1 "$p" | grep -q 'LAUNCH.*obs-haiku-gl-probe.*--no-second-display.*--no-handoff'; then
		echo "### ok: previous progress ends with: $(tail -n 1 "$p")"
	else
		echo "### FAIL: no previous progress.txt naming the frozen probe ($p)"; ok=0
	fi
	if grep -q 'glReadPixels' "$x"/*/00-previous-incomplete/*/*/04-probe/only-stage.log 2>/dev/null; then
		echo "### ok: the frozen probe's log reached the archive with its last step"
	else
		echo "### FAIL: only-stage.log missing"; ok=0
	fi
	if grep -q 'fence timeout' "$x"/*/00-previous-incomplete/syslog/syslog-previous-boot.txt 2>/dev/null &&
		! grep -q 'boot C' "$x"/*/00-previous-incomplete/syslog/syslog-previous-boot.txt; then
		echo "### ok: syslog-previous-boot.txt holds boot B only"
	else
		echo "### FAIL: syslog-previous-boot.txt wrong"; ok=0
	fi
	grep -q 'EARLIER RUNS THAT DID NOT FINISH' "$x"/*/SUMMARY.txt && echo "### ok: SUMMARY mentions it" ||
		{ echo "### FAIL: SUMMARY does not mention it"; ok=0; }
	left="$(ls "$T/home/obs-test-runs" 2>/dev/null)"
	[ -z "$left" ] && echo "### ok: ~/obs-test-runs cleaned up" || { echo "### FAIL: left in obs-test-runs: $left"; ok=0; }
	[ "$ok" = 1 ] && echo "### freeze scenario: ALL OK" || echo "### freeze scenario: FAILED"
fi
echo "### stub processes still running:"
ps -eo pid,args | grep -F "$T" | grep -v grep || echo "(none)"
echo "### archive:"
ls -la "$T/home/Desktop"
echo "$T"
