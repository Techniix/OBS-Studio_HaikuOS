#!/bin/bash
# Unattended test run of the Haiku port: one command, one archive on the Desktop.
#
#     build-aux/haiku/test.sh [--no-build] [--no-pull] [--quick] [--keep] [--no-gpu | --gpu-only] [--yes]
#
#   --no-pull   do not run "git pull --ff-only" first
#   --no-build  do not pull and do not build; test the binaries that are already there
#   --quick     10 s recordings instead of 20 s, and skip the forced-GPU run (the one most likely to hang)
#   --keep      keep the working directory (~/obs-test-runs/<time>: throwaway OBS configurations, unzipped report)
#   --no-gpu    never touch the GPU: no self-test, no forced EGL run, the default run uses OBS_HAIKU_GL=osmesa
#   --gpu-only  only the GPU steps (self-test, default renderer, forced EGL); skips the CPU-only OBS run
#   --yes       do not wait 5 s after the GPU warning
#
# Run it from a Terminal inside the desktop session (OBS needs the app_server). It needs no interaction: OBS
# windows open and close by themselves, please leave them alone. The last line printed is the archive to send.
#
# What it does, safest first, each step with its own log, a time limit and a checkpoint (see below):
#   1. system facts (packages, accelerants, Vulkan ICDs, environment, syslog)
#   2. git pull --ff-only (skipped on a dirty tree) and build-aux/haiku/build.sh
#   3. OBS with OBS_HAIKU_GL=osmesa (CPU renderer, the GPU is not touched at all)
#   4. the GL driver self-test (obs-haiku-gl-probe) on the GPU, cheapest pattern first, one feature at a time
#   5. OBS with the default renderer, then OBS with OBS_HAIKU_GL=egl (forced GPU) last
#      (each OBS run records a known scene from a throwaway configuration: XDG_CONFIG_HOME points into the
#      working directory; the real ~/config/settings/obs-studio is never written, see test/make-config.sh)
#   6. crash reports written while it ran
#   7. SUMMARY.txt and the archive
#
# An experimental graphics driver can freeze the whole machine (no kill, hard reset). So the working directory
# is on disk (~/obs-test-runs/<time>/, not /tmp), every log line is followed by sync, and progress.txt names
# the step and the exact command line before every launch. The next run finds the unfinished directory, prints
# its last progress lines and packs it, with the syslog of the previous boot, into 00-previous-incomplete/.
#
# Not -e on purpose: every step runs even if an earlier one failed.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"

# ---------------------------------------------------------------------------------------------------------
# Tunables (environment). The defaults are what the maintainer's run uses; the self-test under
# build-aux/haiku/test/selftest.sh shortens them and substitutes stub binaries.
# ---------------------------------------------------------------------------------------------------------
OBS_TEST_BIN_DIR="${OBS_TEST_BIN_DIR:-}"                    # directory holding ./obs (default: build/rundir/*/bin)
OBS_TEST_OUT_DIR="${OBS_TEST_OUT_DIR:-$HOME/Desktop}"       # where the archive goes
OBS_TEST_REC_SECS="${OBS_TEST_REC_SECS:-}"                  # seconds of recording per run (default 20, --quick 10)
OBS_TEST_STARTUP_TIMEOUT="${OBS_TEST_STARTUP_TIMEOUT:-120}" # seconds OBS gets to start recording
OBS_TEST_STALL_SECS="${OBS_TEST_STALL_SECS:-16}"            # no log growth and no CPU time for this long = hang
OBS_TEST_EXIT_GRACE="${OBS_TEST_EXIT_GRACE:-15}"            # seconds between SIGTERM and SIGKILL for OBS
OBS_TEST_PROBE_TIMEOUT="${OBS_TEST_PROBE_TIMEOUT:-30}"      # seconds per self-test run
OBS_TEST_BUILD_TIMEOUT="${OBS_TEST_BUILD_TIMEOUT:-14400}"   # seconds for build.sh
OBS_TEST_ASSUME_HAIKU="${OBS_TEST_ASSUME_HAIKU:-}"          # 1/0 overrides the uname check (self-test only)
OBS_TEST_RUNS_DIR="${OBS_TEST_RUNS_DIR:-$HOME/obs-test-runs}" # one directory per run, kept until it finished
OBS_TEST_SYNC="${OBS_TEST_SYNC:-1}"                         # 0: no sync after log lines (self-test speed only)
OBS_TEST_SYSLOG_DIR="${OBS_TEST_SYSLOG_DIR:-/var/log}"      # where syslog_daemon writes (self-test only)

SAMPLE_MS=2000              # sampling interval while OBS runs
LISTIMAGE_AT_MS=5000        # when to record the libraries OBS has loaded
STALL_CPU_MS=30             # less CPU time than this per sample counts as "not running"
MAX_RECORDING_BYTES=$((8 * 1024 * 1024))

OPT_NO_BUILD=0
OPT_NO_PULL=0
OPT_QUICK=0
OPT_KEEP=0
OPT_NO_GPU=0
OPT_GPU_ONLY=0
OPT_YES=0
ORIG_ARGS=("$@")
USAGE="usage: $0 [--no-build] [--no-pull] [--quick] [--keep] [--no-gpu | --gpu-only] [--yes]"
for arg in "$@"; do
	case "$arg" in
	--no-build) OPT_NO_BUILD=1 ;;
	--no-pull) OPT_NO_PULL=1 ;;
	--quick) OPT_QUICK=1 ;;
	--keep) OPT_KEEP=1 ;;
	--no-gpu) OPT_NO_GPU=1 ;;
	--gpu-only) OPT_GPU_ONLY=1 ;;
	--yes | -y) OPT_YES=1 ;;
	-h | --help)
		sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "unknown option: $arg" >&2
		echo "$USAGE" >&2
		exit 2
		;;
	esac
done
if [ "$OPT_NO_GPU" = 1 ] && [ "$OPT_GPU_ONLY" = 1 ]; then
	echo "--no-gpu and --gpu-only exclude each other" >&2
	echo "$USAGE" >&2
	exit 2
fi
[ "$OPT_NO_BUILD" = 1 ] && OPT_NO_PULL=1 # pulling without building would test binaries older than the source
if [ -z "$OBS_TEST_REC_SECS" ]; then
	[ "$OPT_QUICK" = 1 ] && OBS_TEST_REC_SECS=10 || OBS_TEST_REC_SECS=20
fi

if [ -n "$OBS_TEST_ASSUME_HAIKU" ]; then
	IS_HAIKU="$OBS_TEST_ASSUME_HAIKU"
elif [ "$(uname -s 2>/dev/null)" = "Haiku" ]; then
	IS_HAIKU=1
else
	IS_HAIKU=0
fi

# ---------------------------------------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------------------------------------
have() { command -v "$1" >/dev/null 2>&1; }

# Milliseconds since the epoch. bash 5 has EPOCHREALTIME (microseconds, locale decimal separator).
now_ms() {
	if [ -n "${EPOCHREALTIME:-}" ]; then
		local t="${EPOCHREALTIME/[.,]/}"
		echo $((10#$t / 1000))
	else
		echo $(($(date +%s) * 1000))
	fi
}

# GNU sleep takes fractions; anything else gets whole seconds.
if sleep 0.01 2>/dev/null; then TICK=0.1; else TICK=1; fi

# Durability: the machine this runs on may freeze hard (a graphics driver hang can wedge the kernel), and
# whatever is still in the file cache then is lost. sync after every log line keeps the run log and the
# progress file on disk up to the last line written; it costs a few ms per line.
durable() { [ "$OBS_TEST_SYNC" = 1 ] && have sync && sync; return 0; }

RUNLOG=/dev/null
say() {
	echo "$*"
	echo "[$(date +%H:%M:%S)] $*" >>"$RUNLOG"
	durable
}
note() {
	echo "[$(date +%H:%M:%S)] $*" >>"$RUNLOG"
	durable
}

# progress.txt: one line per step boundary and per launched command, written and synced BEFORE the command
# starts. After a hard reset its last line says what was running ("LAUNCH ..." without a "done" after it).
PROGRESS=/dev/null
STEP="start"
progress() {
	printf '%s  [%s]  %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$STEP" "$*" >>"$PROGRESS"
	durable
}
# checkpoint <step id> <title>: a step boundary
checkpoint() {
	STEP="$1"
	progress "=== step begins: $2"
	say "==> $1. $2"
}
# the exact command line, quoted so that it can be pasted back into a shell
cmdline() { printf '%q ' "$@" | sed 's/ $//'; }

alive() { kill -0 "$1" 2>/dev/null; }

# Every process this script starts is recorded here and killed on the way out if it is still around.
TRACKED_PIDS=()
track() { TRACKED_PIDS+=("$1"); }

# wait_gone <pid> <seconds>: 0 once the process is gone, 1 if it is still there after the time is up.
wait_gone() {
	local pid="$1" limit=$(($2 * 1000)) t0
	t0=$(now_ms)
	while alive "$pid"; do
		[ $(($(now_ms) - t0)) -ge "$limit" ] && return 1
		sleep "$TICK"
	done
	return 0
}

# terminate <pid> <grace seconds>: SIGTERM, then SIGKILL. Returns 0 if SIGTERM was enough.
terminate() {
	local pid="$1" grace="$2"
	alive "$pid" || return 0
	kill -TERM "$pid" 2>/dev/null
	wait_gone "$pid" "$grace" && return 0
	kill -KILL "$pid" 2>/dev/null
	wait_gone "$pid" 5
	return 1
}

# Portable watchdog (Haiku has no guaranteed `timeout`; where one exists this still gives the same
# behaviour and lets the hooks below look at the process while it runs):
#   run_to <timeout s> <log file> <working dir> <command...>
# Output is appended to the log. Sets RT_EXIT, RT_MS, RT_TIMEOUT (1 = killed by the watchdog), RT_PID.
#   RT_HOOK        optional function called as "<pid> <elapsed ms>" on every poll while the command runs
#   RT_ON_TIMEOUT  optional function called as "<pid>" just before a timed-out command is killed
#   RT_SYNC_MS     optional: sync every this many ms while the command runs (its log reaches the disk even
#                  if the machine freezes during the command)
# Every launch is announced in progress.txt (and synced) first.
RT_HOOK=""
RT_ON_TIMEOUT=""
RT_SYNC_MS=""
RT_EXIT=0
RT_MS=0
RT_TIMEOUT=0
RT_PID=0
run_to() {
	local tmo_s="$1" tmo_ms=$(($1 * 1000)) log="$2" cwd="$3"
	shift 3
	local t0 el next_sync="${RT_SYNC_MS:-}"
	RT_TIMEOUT=0
	progress "LAUNCH (limit $tmo_s s, cwd $cwd, log ${log#"$WORK"/}): $(cmdline "$@")"
	t0=$(now_ms)
	(cd "$cwd" 2>/dev/null || exit 126; exec "$@") >>"$log" 2>&1 </dev/null &
	RT_PID=$!
	track "$RT_PID"
	while alive "$RT_PID"; do
		el=$(($(now_ms) - t0))
		if [ -n "$RT_SYNC_MS" ] && [ "$el" -ge "$next_sync" ]; then
			next_sync=$((el + RT_SYNC_MS))
			durable
		fi
		[ -n "$RT_HOOK" ] && "$RT_HOOK" "$RT_PID" "$el"
		if [ "$el" -ge "$tmo_ms" ]; then
			RT_TIMEOUT=1
			[ -n "$RT_ON_TIMEOUT" ] && "$RT_ON_TIMEOUT" "$RT_PID"
			terminate "$RT_PID" 3
			break
		fi
		sleep "$TICK"
	done
	wait "$RT_PID" 2>/dev/null
	RT_EXIT=$?
	RT_MS=$(($(now_ms) - t0))
	progress "done: exit $RT_EXIT after $RT_MS ms$([ "$RT_TIMEOUT" = 1 ] && echo ', killed by the watchdog')"
	RT_HOOK=""
	RT_ON_TIMEOUT=""
	RT_SYNC_MS=""
}

# quiet_to <timeout s> <command...>: like run_to but only the captured stdout matters (printed), stderr dropped.
quiet_to() {
	local tmo="$1" tmp="$WORK/.quiet.$RANDOM"
	shift
	: >"$tmp"
	local t0 pid
	progress "LAUNCH (limit $tmo s): $(cmdline "$@")"
	t0=$(now_ms)
	("$@") >"$tmp" 2>/dev/null </dev/null &
	pid=$!
	track "$pid"
	while alive "$pid"; do
		if [ $(($(now_ms) - t0)) -ge $((tmo * 1000)) ]; then
			terminate "$pid" 2
			break
		fi
		sleep "$TICK"
	done
	wait "$pid" 2>/dev/null
	local rc=$?
	progress "done: exit $rc after $(($(now_ms) - t0)) ms"
	cat "$tmp"
	rm -f "$tmp"
	return $rc
}

file_size() {
	local n
	n=$(wc -c <"$1" 2>/dev/null) || n=0
	echo $((n + 0))
}

# ---------------------------------------------------------------------------------------------------------
# Process inspection. Haiku's ps (src/bin/ps.c) is not POSIX ps:
#   ps        one line per team:   "%-50s %5d %8d %4d %4d " = args, team id, thread count, gid, uid
#   ps -a     the same, each team followed by a thread table
#             "%-37s %5d %8s %4d %8llu %8lld " = thread name, id, state, priority, user ms, kernel ms
#             and a line of dashes
#   ps -as    as -a, with the semaphore a waiting thread blocks on appended to its line
# (options must be given as ONE argument: with two, ps.c treats the last one as a team name to match.)
# There is no per-team CPU column, so CPU time is the sum over the team's threads. team_info.args is 64
# bytes, so long command lines arrive truncated.
# ---------------------------------------------------------------------------------------------------------
HAIKU_TEAM_LINE='NF >= 5 && $NF ~ /^[0-9]+$/ && $(NF-1) ~ /^[0-9]+$/ && $(NF-2) ~ /^[0-9]+$/ && $(NF-3) ~ /^[0-9]+$/'

# list_teams: "<pid><TAB><command line>" for every process.
list_teams() {
	if [ "$IS_HAIKU" = 1 ]; then
		ps 2>/dev/null | awk "$HAIKU_TEAM_LINE"' {
			pid = $(NF-3); line = $0
			sub(/[ \t]+[0-9]+[ \t]+[0-9]+[ \t]+[0-9]+[ \t]+[0-9]+[ \t]*$/, "", line)
			print pid "\t" line
		}'
	else
		ps -eo pid=,args= 2>/dev/null | awk '{ pid = $1; $1 = ""; sub(/^ +/, ""); print pid "\t" $0 }'
	fi
}

# haiku_team_block <pid> <ps option>: the team line and thread table of one team.
haiku_team_block() {
	ps "$2" 2>/dev/null | awk -v pid="$1" "$HAIKU_TEAM_LINE"' { inblk = ($(NF-3) == pid); if (inblk) print; next }
		/^-----/ { inblk = 0; next }
		inblk { print }'
}

# proc_cpu_ms <pid>: CPU time used so far in ms, or nothing if it cannot be read.
proc_cpu_ms() {
	if [ "$IS_HAIKU" = 1 ]; then
		haiku_team_block "$1" -a | awk '
			NF >= 5 && $(NF-3) ~ /^(run|rdy|msg|zzz|sus|wait|\?\?\?)$/ && $NF ~ /^-?[0-9]+$/ && $(NF-1) ~ /^[0-9]+$/ {
				sum += $(NF-1) + $NF; n++
			}
			END { if (n) print sum }'
	elif [ -r "/proc/$1/stat" ]; then
		# fields 14/15 (utime/stime, clock ticks); the command name may contain spaces, so cut after ")"
		sed 's/^.*) //' "/proc/$1/stat" 2>/dev/null | awk '{ print ($12 + $13) * 10 }'
	fi
}

# thread_listing <pid>: every thread of the process with its state (and semaphore, on Haiku).
thread_listing() {
	if [ "$IS_HAIKU" = 1 ]; then
		echo "Thread                                   Id    State Prio    UTime    KTime  Semaphore"
		haiku_team_block "$1" -as
	else
		ps -L -o pid,tid,stat,time,comm -p "$1" 2>&1
	fi
}

DEBUGGER=""
for d in Debugger /boot/system/apps/Debugger; do
	if have "$d" || [ -x "$d" ]; then
		DEBUGGER="$d"
		break
	fi
done

# capture_stuck <pid> <dir> <tag>: everything that tells where a process that will be killed is stuck.
capture_stuck() {
	local pid="$1" dir="$2" tag="$3"
	alive "$pid" || return 0
	note "capturing state of stuck process $pid ($tag)"
	thread_listing "$pid" >"$dir/$tag-threads.txt" 2>&1
	list_teams >"$dir/$tag-ps.txt" 2>&1
	if have listimage; then
		quiet_to 20 listimage "$pid" >"$dir/$tag-listimage.txt"
	fi
	# Debugger --save-report (src/apps/debugger/Debugger.cpp) writes stack traces of all threads. The path
	# must be absolute and attached with "=", otherwise the report lands on the Desktop. What the team is
	# left in afterwards does not matter: it is killed right after this.
	if [ "$IS_HAIKU" = 1 ] && [ -n "$DEBUGGER" ]; then
		# run_to is not reentrant: this may be called from inside an outer run_to (RT_ON_TIMEOUT), whose
		# RT_PID and RT_TIMEOUT must survive, or the outer watchdog would kill the Debugger's PID again and
		# report the timed-out command as an ordinary failure.
		local saved_hook="$RT_HOOK" saved_to="$RT_ON_TIMEOUT" saved_sync="$RT_SYNC_MS"
		local saved_pid="$RT_PID" saved_tmo="$RT_TIMEOUT" saved_exit="$RT_EXIT" saved_ms="$RT_MS"
		RT_HOOK=""
		RT_ON_TIMEOUT=""
		run_to 90 "$dir/$tag-debugger.log" "$dir" "$DEBUGGER" "--save-report=$dir/$tag-debugger.report" --team "$pid"
		[ "$RT_TIMEOUT" = 1 ] && echo "Debugger did not finish within 90 s" >>"$dir/$tag-debugger.log"
		RT_HOOK="$saved_hook"
		RT_ON_TIMEOUT="$saved_to"
		RT_SYNC_MS="$saved_sync"
		RT_PID="$saved_pid"
		RT_TIMEOUT="$saved_tmo"
		RT_EXIT="$saved_exit"
		RT_MS="$saved_ms"
	fi
}

# The lines of a listimage dump that name a graphics library, and just their paths.
GL_LIB_RE_CS='GL|EGL'
GL_LIB_RE_CI='mesa|vulkan|zink|nvk|nouveau|gallium|glvnd|nvidia|quasar|swrast|llvmpipe|lavapipe|softpipe|accelerant|drm'
gl_lib_lines() {
	{
		grep -E "$GL_LIB_RE_CS" "$@" 2>/dev/null
		grep -iE "$GL_LIB_RE_CI" "$@" 2>/dev/null
	} | sort -u
}
gl_lib_paths() { gl_lib_lines "$@" | awk '{ print $NF }' | grep '/' | sort -u; }

# Is a process (by command line) one launched from the tested bin directory? Used to decide what may be
# killed. Haiku truncates the command line to 63 characters, so a prefix of the directory also counts.
BIN=""
from_bin_dir() {
	local first="${1%% *}"
	[ -n "$BIN" ] || return 1
	case "$first" in "$BIN"/*) return 0 ;; esac
	if [ "${#first}" -ge 60 ]; then
		case "$BIN/" in "$first"*) return 0 ;; esac
	fi
	return 1
}

BASELINE_PIDS=" "
# sweep_leftovers <file>: records OBS-related processes still running, and kills those that this script
# started (tracked PIDs) or that appeared after the script began AND run from the tested bin directory
# (helpers OBS spawns: obs-ffmpeg-mux, obs-haiku-gl-probe, an OBS restarted by its own GL watchdog).
sweep_leftovers() {
	local out="$1" pid args mine killed=0 p teams="$WORK/.teams.$$"
	# a file rather than process substitution, which needs /dev/fd or named pipes
	list_teams >"$teams"
	while IFS=$'\t' read -r pid args; do
		[ -n "$pid" ] || continue
		[ "$pid" = "$$" ] && continue
		mine=0
		for p in "${TRACKED_PIDS[@]:-}"; do
			[ "$p" = "$pid" ] && mine=1
		done
		if [ "$mine" = 0 ]; then
			case "$BASELINE_PIDS" in *" $pid "*) continue ;; esac
			case "$args" in
			*obs-haiku-* | *obs-ffmpeg-mux* | *gl-probe*) ;;
			*) from_bin_dir "$args" || continue ;;
			esac
			from_bin_dir "$args" && mine=1
		fi
		if [ "$mine" = 1 ]; then
			echo "killed (started by this run): $pid $args" >>"$out"
			terminate "$pid" 3
			killed=$((killed + 1))
		else
			echo "left alone (not started by this run): $pid $args" >>"$out"
		fi
	done <"$teams"
	rm -f "$teams"
	return 0
}

cleanup_pids() {
	local p
	for p in "${TRACKED_PIDS[@]:-}"; do
		[ -n "$p" ] && alive "$p" && kill -KILL "$p" 2>/dev/null
	done
}

# ---------------------------------------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------------------------------------
RESULTS=()
result() { # <step> <PASS|FAIL|HANG|SKIPPED|...> <details>
	RESULTS+=("$1|$2|$3")
	say "    => $1: $2  $3"
}

# ---------------------------------------------------------------------------------------------------------
# Earlier runs that never finished. Each run lives in $OBS_TEST_RUNS_DIR/<time>/ until its archive is
# written; FINISHED marks a completed one (kept with --keep), INTERRUPTED one stopped by a signal, REPORTED
# one already packed into a later report. Anything else was cut short without the script noticing: most
# likely the machine froze and was reset. Its progress.txt says what was running.
# ---------------------------------------------------------------------------------------------------------
REEXEC="${OBS_TEST_REEXEC:-}" # set when a pull re-executed the script: same run, continue quietly
PREV_RUNS=()
PACKED_RUNS=()
find_incomplete() {
	local d pid
	PREV_RUNS=()
	for d in "$OBS_TEST_RUNS_DIR"/*/; do
		d="${d%/}"
		[ -d "$d" ] && [ -f "$d/progress.txt" ] || continue
		[ -n "${OBS_TEST_WORK:-}" ] && [ "$d" -ef "$OBS_TEST_WORK" ] && continue
		[ -e "$d/FINISHED" ] || [ -e "$d/REPORTED" ] && continue
		# Another instance still at work? After a reset its PID may belong to anything, so only a running
		# test.sh with that PID counts.
		pid="$(cat "$d/pid" 2>/dev/null)"
		if [ -n "$pid" ] && alive "$pid" &&
			list_teams | awk -F'\t' -v p="$pid" '$1 == p && /test\.sh/ { f = 1 } END { exit !f }'; then
			continue
		fi
		PREV_RUNS+=("$d")
	done
}
# describe_prev <dir>: how it ended and the last lines of its progress file
describe_prev() {
	local d="$1"
	if [ -f "$d/INTERRUPTED" ]; then
		echo "$(basename "$d"): interrupted ($(head -n 1 "$d/INTERRUPTED")); last progress lines:"
	else
		echo "$(basename "$d"): NEVER FINISHED (machine frozen or reset?); last progress lines:"
	fi
	tail -n 5 "$d/progress.txt" 2>/dev/null | sed 's/^/    /'
}

find_incomplete
if [ -z "$REEXEC" ] && [ "${#PREV_RUNS[@]}" -gt 0 ]; then
	echo "Earlier run(s) in $OBS_TEST_RUNS_DIR that did not finish (packed into this report, 00-previous-incomplete/):"
	for d in "${PREV_RUNS[@]}"; do
		describe_prev "$d" | sed 's/^/  /'
	done
	echo
fi

# ---------------------------------------------------------------------------------------------------------
# GPU warning
# ---------------------------------------------------------------------------------------------------------
if [ -z "$REEXEC" ]; then
	if [ "$OPT_NO_GPU" = 1 ]; then
		echo "--no-gpu: the GPU is not used (no self-test, no EGL run; the default run uses OBS_HAIKU_GL=osmesa)."
	else
		echo "WARNING: the GPU steps can freeze the whole machine on an experimental graphics driver;" \
			"--no-gpu skips them.$([ "$OPT_YES" = 1 ] || echo ' Starting in 5 s, Ctrl-C to abort (--yes: no wait).')"
		[ "$OPT_YES" = 1 ] || sleep 5
	fi
fi

# ---------------------------------------------------------------------------------------------------------
# Working directory: on disk, not in /tmp, so that it survives a hard reset. A pull that changed the tree
# re-executes the (possibly updated) script with OBS_TEST_WORK and OBS_TEST_REEXEC set, so that the second
# instance continues in the same directory.
# ---------------------------------------------------------------------------------------------------------
if [ -n "${OBS_TEST_WORK:-}" ]; then
	WORK="$OBS_TEST_WORK"
else
	WORK="$OBS_TEST_RUNS_DIR/$(date +%Y%m%d-%H%M%S)"
	[ -e "$WORK" ] && WORK="$WORK-$$"
fi
mkdir -p "$WORK" 2>/dev/null
if [ ! -d "$WORK" ] || [ ! -w "$WORK" ]; then
	echo "cannot create the working directory $WORK (set OBS_TEST_WORK or OBS_TEST_RUNS_DIR)" >&2
	exit 1
fi
WORK="$(cd "$WORK" && pwd)"
export OBS_TEST_WORK="$WORK"
RUNLOG="$WORK/run.log"
PROGRESS="$WORK/progress.txt"
echo "$$" >"$WORK/pid"
if [ -z "$REEXEC" ]; then
	rm -f "$WORK/FINISHED" "$WORK/INTERRUPTED" "$WORK/REPORTED"
	: >"$WORK/.start" # crash reports newer than this file belong to this run
	progress "run started: $(cmdline "$0" "${ORIG_ARGS[@]}"); host: $(uname -srm 2>/dev/null)"
	have uptime && progress "uptime: $(uptime 2>/dev/null)"
else
	progress "re-executed after git pull"
fi

on_signal() {
	echo
	echo "interrupted ($1), cleaning up"
	echo "SIG$1 at $(date '+%Y-%m-%d %H:%M:%S') during [$STEP]" >"$WORK/INTERRUPTED"
	progress "interrupted by SIG$1"
	cleanup_pids
	exit 130
}
trap 'on_signal INT' INT
trap 'on_signal TERM' TERM
trap 'on_signal HUP' HUP
trap 'cleanup_pids' EXIT

BASELINE_PIDS=" $(list_teams | awk -F'\t' '{ printf "%s ", $1 }')"

git_ok() { have git && git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1; }

# ---------------------------------------------------------------------------------------------------------
# Step 2a: pull (before anything else, so that the facts and the report name describe the tested commit)
# ---------------------------------------------------------------------------------------------------------
PULL_STATUS="${OBS_TEST_PULL_STATUS:-}"
PULL_DETAIL="${OBS_TEST_PULL_DETAIL:-}"
if [ -z "$PULL_STATUS" ]; then
	if [ "$OPT_NO_PULL" = 1 ]; then
		PULL_STATUS=SKIPPED
		[ "$OPT_NO_BUILD" = 1 ] && PULL_DETAIL="--no-build" || PULL_DETAIL="--no-pull"
	elif ! git_ok; then
		PULL_STATUS=SKIPPED
		PULL_DETAIL="not a git checkout (or git missing)"
	elif [ -n "$(git -C "$SRC" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
		PULL_STATUS=SKIPPED
		PULL_DETAIL="working tree has local changes, not pulled"
		git -C "$SRC" status --short --untracked-files=no >"$WORK/pull.log" 2>&1
	else
		STEP="2a git-pull"
		say "==> git pull --ff-only"
		before="$(git -C "$SRC" rev-parse HEAD 2>/dev/null)"
		run_to 300 "$WORK/pull.log" "$SRC" git pull --ff-only
		after="$(git -C "$SRC" rev-parse HEAD 2>/dev/null)"
		if [ "$RT_TIMEOUT" = 1 ]; then
			PULL_STATUS=FAIL
			PULL_DETAIL="TIMEOUT after 300 s"
		elif [ "$RT_EXIT" != 0 ]; then
			PULL_STATUS=FAIL
			PULL_DETAIL="exit $RT_EXIT (see 02-build/pull.log); testing the tree as it is"
		elif [ "$before" != "$after" ]; then
			PULL_STATUS=PASS
			PULL_DETAIL="updated ${before:0:9} -> ${after:0:9}"
			# This script may have just been replaced on disk; bash reads scripts incrementally, so
			# start over with the new one instead of continuing with a mix of both.
			export OBS_TEST_PULL_STATUS="$PULL_STATUS" OBS_TEST_PULL_DETAIL="$PULL_DETAIL" OBS_TEST_REEXEC=1
			trap - EXIT
			exec bash "$0" "${ORIG_ARGS[@]}"
		else
			PULL_STATUS=PASS
			PULL_DETAIL="already up to date"
		fi
	fi
fi

# ---------------------------------------------------------------------------------------------------------
# Report directory
# ---------------------------------------------------------------------------------------------------------
if git_ok; then
	GIT_BRANCH="$(git -C "$SRC" rev-parse --abbrev-ref HEAD 2>/dev/null)"
	GIT_COMMIT="$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null)"
	GIT_DIRTY="$(git -C "$SRC" status --porcelain --untracked-files=no 2>/dev/null | wc -l | tr -d ' ')"
else
	GIT_BRANCH="nogit"
	GIT_COMMIT="unknown"
	GIT_DIRTY="?"
fi
[ -n "$GIT_BRANCH" ] || GIT_BRANCH="nobranch"
[ -n "$GIT_COMMIT" ] || GIT_COMMIT="unknown"
STAMP="$(date +%Y%m%d-%H%M%S)"
SAFE_BRANCH="$(printf '%s' "$GIT_BRANCH" | tr -c 'A-Za-z0-9._-' '_')"
REPORT_NAME="obs-test-$SAFE_BRANCH-$GIT_COMMIT-$STAMP"
REPORT="$WORK/$REPORT_NAME"
mkdir -p "$REPORT/01-system" "$REPORT/02-build" "$REPORT/04-probe" "$REPORT/07-crash"
mv "$WORK/run.log" "$REPORT/00-run.log" 2>/dev/null
RUNLOG="$REPORT/00-run.log"
[ -f "$WORK/pull.log" ] && mv "$WORK/pull.log" "$REPORT/02-build/pull.log"

say "==> OBS Haiku test run: $GIT_BRANCH @ $GIT_COMMIT ($STAMP)"
say "    working directory: $WORK"
note "options: no-build=$OPT_NO_BUILD no-pull=$OPT_NO_PULL quick=$OPT_QUICK keep=$OPT_KEEP no-gpu=$OPT_NO_GPU" \
	"gpu-only=$OPT_GPU_ONLY is_haiku=$IS_HAIKU tick=$TICK sync=$OBS_TEST_SYNC"
progress "report: $REPORT_NAME"

# ---------------------------------------------------------------------------------------------------------
# Step 0: earlier runs that did not finish, and the syslog of the boot they ran in
# ---------------------------------------------------------------------------------------------------------
# collect_syslogs <dir>: syslog_daemon (src/servers/syslog_daemon/syslog_output.cpp) appends to
# /var/log/syslog across boots and rotates by size only (syslog -> syslog.1 -> ... up to syslog_max_history,
# default 1, 512 kB each); there is no per-boot file. The kernel writes "Welcome to syslog debug output!" once
# per boot (src/system/kernel/debug/debug.cpp), so the boot before this one is the part between the last two
# such lines. /var/log/previous_syslog is the kernel's in-memory debug syslog of the previous session, saved
# at boot only when the boot loader's "Enable debug syslog" option was on in that session; it survives a
# warm reset and also holds what syslog_daemon had no time to write.
collect_syslogs() {
	local out="$1" d="$OBS_TEST_SYSLOG_DIR" f i all n
	mkdir -p "$out"
	all="$out/.all"
	: >"$all"
	for f in "$d"/syslog.9 "$d"/syslog.8 "$d"/syslog.7 "$d"/syslog.6 "$d"/syslog.5 "$d"/syslog.4 "$d"/syslog.3 \
		"$d"/syslog.2 "$d"/syslog.1 "$d"/syslog; do # oldest first
		[ -r "$f" ] || continue
		cp -p "$f" "$out/" 2>/dev/null
		cat "$f" >>"$all" 2>/dev/null
	done
	if [ -r "$d/previous_syslog" ]; then
		cp -p "$d/previous_syslog" "$out/previous_syslog" 2>/dev/null
		ls -la "$d/previous_syslog" >"$out/previous_syslog.ls" 2>&1
	fi
	{
		echo "syslog files copied from $d (oldest first): $(cd "$out" && ls syslog* 2>/dev/null | tr '\n' ' ')"
		[ -f "$out/previous_syslog" ] && echo "previous_syslog: present (debug syslog of the previous session)" ||
			echo "previous_syslog: absent (enable \"Enable debug syslog\" in the boot loader's debug menu to get one)"
	} >"$out/README.txt"
	n="$(grep -n 'Welcome to syslog debug output' "$all" 2>/dev/null | cut -d: -f1 | tail -n 2 | tr '\n' ' ')"
	set -- $n
	if [ $# = 2 ]; then
		sed -n "$1,$(($2 - 1))p" "$all" >"$out/syslog-previous-boot.txt"
		echo "syslog-previous-boot.txt: lines $1-$(($2 - 1)) of the files above (between the last two boot markers)" >>"$out/README.txt"
	elif [ $# = 1 ] && [ "$1" -gt 1 ]; then
		sed -n "1,$(($1 - 1))p" "$all" >"$out/syslog-previous-boot.txt"
		echo "syslog-previous-boot.txt: the lines before the only boot marker (the start of that boot was rotated away)" >>"$out/README.txt"
	else
		echo "syslog-previous-boot.txt: not written (no earlier boot found in the syslog files)" >>"$out/README.txt"
	fi
	rm -f "$all"
}

step_previous() {
	[ "${#PREV_RUNS[@]}" -gt 0 ] || return 0
	local dest="$REPORT/00-previous-incomplete" d name list f
	checkpoint 0 "Earlier runs that did not finish: ${#PREV_RUNS[@]} (00-previous-incomplete/)"
	mkdir -p "$dest"
	for d in "${PREV_RUNS[@]}"; do
		name="$(basename "$d")"
		mkdir -p "$dest/$name"
		describe_prev "$d" >"$dest/$name/HOW-IT-ENDED.txt"
		# everything, including the OBS logs still in the throwaway configurations (xdg-*), but no
		# recordings or other files over the size limit
		list="$WORK/.prev-files"
		(cd "$d" && find . -type f) >"$list" 2>/dev/null
		while IFS= read -r f; do
			f="${f#./}"
			case "$f" in *.wav | */.quiet.* | .teams.*) continue ;; esac
			if [ "$(file_size "$d/$f")" -gt "$MAX_RECORDING_BYTES" ]; then
				echo "$f ($(file_size "$d/$f") bytes)" >>"$dest/$name/NOT-COPIED-too-large.txt"
				continue
			fi
			mkdir -p "$dest/$name/$(dirname "$f")"
			cp -p "$d/$f" "$dest/$name/$f" 2>/dev/null
		done <"$list"
		rm -f "$list"
		echo "$REPORT_NAME" >"$d/REPORTED"
		PACKED_RUNS+=("$d")
		say "    $name: $(tail -n 1 "$d/progress.txt" 2>/dev/null)"
	done
	collect_syslogs "$dest/syslog"
	durable
}

# ---------------------------------------------------------------------------------------------------------
# Step 1: system facts
# ---------------------------------------------------------------------------------------------------------
SYS="$REPORT/01-system"

# fact <file> <title> <command...>: run a command (20 s limit) and file its output under a heading.
fact() {
	local file="$SYS/$1" title="$2"
	shift 2
	echo "== $title" >>"$file"
	if ! have "$1"; then
		echo "(command not found: $1)" >>"$file"
		echo >>"$file"
		return 1
	fi
	run_to 20 "$file" "$SRC" "$@"
	[ "$RT_TIMEOUT" = 1 ] && echo "(TIMEOUT after 20 s)" >>"$file"
	echo >>"$file"
	return 0
}

syslog_tail() {
	local f
	for f in /var/log/syslog /boot/system/var/log/syslog; do
		if [ -r "$f" ]; then
			tail -n 300 "$f" >"$SYS/$1" 2>&1
			return
		fi
	done
	echo "(no readable syslog)" >"$SYS/$1"
}

step_system() {
	checkpoint 1 "System facts"
	fact system.txt "uname -a" uname -a
	fact system.txt "date" date
	# version(1) prints the version resource of a file; libbe carries the Haiku revision
	[ -e /boot/system/lib/libbe.so ] && fact system.txt "version /boot/system/lib/libbe.so" version /boot/system/lib/libbe.so
	fact system.txt "sysinfo" sysinfo
	fact system.txt "screenmode" screenmode
	fact system.txt "df (working directory)" df -h "$WORK"
	fact system.txt "listdev" listdev

	{
		echo "branch: $GIT_BRANCH"
		echo "commit: $GIT_COMMIT"
		echo "tracked files with local changes: $GIT_DIRTY"
	} >"$SYS/git.txt"
	if git_ok; then
		fact git.txt "git log -1" git -C "$SRC" log -1 --date=iso
		fact git.txt "git status --short" git -C "$SRC" status --short
		fact git.txt "git submodule status" git -C "$SRC" submodule status
	fi

	# Packages. The package file names carry the versions even if pkgman is unusable.
	local d
	for d in /boot/system/packages /boot/home/config/packages; do
		[ -d "$d" ] && fact packages-files.txt "ls $d" ls -la "$d"
	done
	# "pkgman search -i -a" lists every installed package (src/bin/pkgman/command_search.cpp)
	fact packages-installed.txt "pkgman search -i -a" pkgman search -i -a
	{
		echo "== installed packages relevant to OBS (from pkgman, then from the package file names)"
		grep -hiE 'mesa|libglvnd|ffmpeg|qt6|haikuwebkit|x264|nvidia|quasar|vulkan|zink|nvk' \
			"$SYS/packages-installed.txt" 2>/dev/null
		echo
		grep -hiE 'mesa|libglvnd|ffmpeg|qt6|haikuwebkit|x264|nvidia|quasar|vulkan|zink|nvk' \
			"$SYS/packages-files.txt" 2>/dev/null | awk '{ print $NF }'
	} >"$SYS/packages.txt"

	for d in /boot/system/add-ons/accelerants /boot/system/non-packaged/add-ons/accelerants \
		/boot/home/config/non-packaged/add-ons/accelerants /boot/system/add-ons/opengl \
		/boot/system/non-packaged/add-ons/opengl; do
		if [ -d "$d" ]; then
			fact graphics.txt "ls -la $d" ls -la "$d"
		else
			printf '== ls -la %s\n(no such directory)\n\n' "$d" >>"$SYS/graphics.txt"
		fi
	done

	{
		echo "== environment (VK_*, EGL*, MESA*, LIBGL*, OBS_*, and related)"
		env | grep -E '^(VK_|EGL|MESA|LIBGL|OBS_|GALLIUM|ZINK|NVK|__GL|LIBRARY_PATH=|ADDON_PATH=|XDG_)' | sort
		echo
		echo "(the OBS runs below unset OBS_HAIKU_GL, OBS_HAIKU_GL_WATCHDOG and OBS_HAIKU_GL_PROBE and set them per run)"
	} >"$SYS/environment.txt" 2>&1

	# Vulkan ICD manifests: the standard directories plus whatever the environment names
	mkdir -p "$SYS/vulkan-icd"
	local f n=0 list=()
	for d in /boot/system/data/vulkan/icd.d /boot/system/non-packaged/data/vulkan/icd.d \
		/boot/home/config/data/vulkan/icd.d /boot/home/config/non-packaged/data/vulkan/icd.d; do
		for f in "$d"/*.json; do
			[ -f "$f" ] && list+=("$f")
		done
	done
	local IFS_SAVE="$IFS" names
	names="${VK_DRIVER_FILES:-}:${VK_ICD_FILENAMES:-}:${VK_ADD_DRIVER_FILES:-}"
	IFS=':'
	for f in $names; do
		[ -n "$f" ] && [ -f "$f" ] && list+=("$f")
	done
	IFS="$IFS_SAVE"
	{
		echo "VK_DRIVER_FILES=${VK_DRIVER_FILES:-(unset)}"
		echo "VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-(unset)}"
		echo
		for f in "${list[@]:-}"; do
			[ -n "$f" ] || continue
			n=$((n + 1))
			cp "$f" "$SYS/vulkan-icd/$n-$(basename "$f")" 2>/dev/null
			echo "$n-$(basename "$f")  <-  $f"
		done
		[ "$n" = 0 ] && echo "(no Vulkan ICD manifest found)"
	} >"$SYS/vulkan-icd/INDEX.txt"

	# What the system does when a program crashes: without a default_action here debug_server shows a
	# dialog and the crashed team stays around until someone answers it (src/servers/debug/DebugServer.cpp).
	f="$HOME/config/settings/system/debug_server/settings"
	if [ -f "$f" ]; then
		cp "$f" "$SYS/debug_server-settings.txt" 2>/dev/null
	else
		echo "(no $f: crashes open the debug_server dialog)" >"$SYS/debug_server-settings.txt"
	fi

	{
		echo "== tools this script looks for"
		for d in git zip tar ffprobe ffmpeg python3 perl listimage listdev screenmode sysinfo pkgman \
			media_client beep timeout; do
			printf '%-14s %s\n' "$d" "$(command -v "$d" 2>/dev/null || echo '(missing)')"
		done
		printf '%-14s %s\n' Debugger "${DEBUGGER:-(missing)}"
	} >"$SYS/tools.txt"

	syslog_tail syslog-before.txt
	result "1 system-facts" PASS "$(uname -srm 2>/dev/null)"
}

# ---------------------------------------------------------------------------------------------------------
# Step 2: build
# ---------------------------------------------------------------------------------------------------------
BUILD_DIR="$SRC/build"
BUILD_FAILED=0

find_bin_dir() {
	local d
	if [ -n "$OBS_TEST_BIN_DIR" ]; then
		BIN="$(cd "$OBS_TEST_BIN_DIR" 2>/dev/null && pwd)"
		return
	fi
	for d in "$BUILD_DIR"/rundir/RelWithDebInfo/bin "$BUILD_DIR"/rundir/*/bin; do
		if [ -x "$d/obs" ]; then
			BIN="$d"
			return
		fi
	done
	BIN=""
}

step_build() {
	checkpoint 2 "Pull and build"
	result "2a git-pull" "$PULL_STATUS" "$PULL_DETAIL"

	if [ "$OPT_NO_BUILD" = 1 ]; then
		result "2b build" SKIPPED "--no-build"
	else
		say "    building (this is the long part; log: 02-build/build.log)"
		run_to "$OBS_TEST_BUILD_TIMEOUT" "$REPORT/02-build/build.log" "$SRC" bash "$SRC/build-aux/haiku/build.sh"
		local secs=$((RT_MS / 1000))
		if [ "$RT_TIMEOUT" = 1 ]; then
			BUILD_FAILED=1
			result "2b build" FAIL "TIMEOUT after ${secs} s"
		elif [ "$RT_EXIT" != 0 ]; then
			BUILD_FAILED=1
			grep -E 'error:|Error [0-9]|FAILED:|^!!' "$REPORT/02-build/build.log" 2>/dev/null | head -n 80 \
				>"$REPORT/02-build/build-errors.txt"
			result "2b build" FAIL "exit $RT_EXIT after ${secs} s, $(wc -l <"$REPORT/02-build/build-errors.txt" |
				tr -d ' ') error lines (02-build/build-errors.txt)"
		else
			result "2b build" PASS "${secs} s"
		fi
	fi
	[ -f "$BUILD_DIR/CMakeCache.txt" ] && cp "$BUILD_DIR/CMakeCache.txt" "$REPORT/02-build/" 2>/dev/null

	find_bin_dir
	if [ -n "$BIN" ]; then
		{
			echo "bin directory: $BIN"
			ls -la "$BIN" 2>&1
			echo
			echo "== plugins"
			find "$(dirname "$BIN")" -name '*.so' 2>/dev/null | sort
		} >"$REPORT/02-build/rundir-listing.txt"
	else
		echo "no bin directory with an executable obs under $BUILD_DIR/rundir" >"$REPORT/02-build/rundir-listing.txt"
	fi
}

# ---------------------------------------------------------------------------------------------------------
# Step 4: GL probe matrix, on the GPU. Cheapest pattern first, then one feature at a time, then everything:
# if the machine freezes, the last LAUNCH line in progress.txt names the pattern that did it.
# ---------------------------------------------------------------------------------------------------------
PROBE_DIR="$REPORT/04-probe"
PROBE_LISTIMAGE_DONE=0
PROBE_CUR=""

probe_hook() { # <pid> <elapsed ms>: record the libraries of a probe that lives long enough
	if [ "$PROBE_LISTIMAGE_DONE" = 0 ] && [ "$2" -ge 600 ] && have listimage; then
		PROBE_LISTIMAGE_DONE=1
		quiet_to 10 listimage "$1" >"$PROBE_DIR/$PROBE_CUR-listimage.txt"
	fi
}
probe_on_timeout() { capture_stuck "$1" "$PROBE_DIR" "$PROBE_CUR-timeout"; }

PROBE_TOTAL=0
PROBE_OK=0
PROBE_TIMEOUTS=0
PROBE_NO_EGL=0 # runs that exited 2: no EGL display (e.g. HaikuPorts' Mesa 22), not a driver failure
PROBE_BAD=""
# probe_run <name> <args...>
probe_run() {
	local name="$1"
	shift
	PROBE_CUR="$name"
	PROBE_LISTIMAGE_DONE=0
	STEP="4 gl-probe $name"
	echo "\$ ./obs-haiku-gl-probe $*" >"$PROBE_DIR/$name.log"
	RT_HOOK=probe_hook
	RT_ON_TIMEOUT=probe_on_timeout
	RT_SYNC_MS=1000
	run_to "$OBS_TEST_PROBE_TIMEOUT" "$PROBE_DIR/$name.log" "$BIN" ./obs-haiku-gl-probe "$@"
	local status
	PROBE_TOTAL=$((PROBE_TOTAL + 1))
	if [ "$RT_TIMEOUT" = 1 ]; then
		status=TIMEOUT
		PROBE_TIMEOUTS=$((PROBE_TIMEOUTS + 1))
		PROBE_BAD="$PROBE_BAD $name=TIMEOUT"
	elif [ "$RT_EXIT" = 0 ]; then
		status=OK
		PROBE_OK=$((PROBE_OK + 1))
	elif [ "$RT_EXIT" = 2 ]; then
		status=NO-EGL
		PROBE_NO_EGL=$((PROBE_NO_EGL + 1))
		PROBE_BAD="$PROBE_BAD $name=no-EGL"
	else
		status=FAIL
		PROBE_BAD="$PROBE_BAD $name=exit$RT_EXIT"
	fi
	printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$status" "$RT_EXIT" "$RT_MS" "$*" >>"$PROBE_DIR/results.tsv"
	say "    probe $name: $status (exit $RT_EXIT, $RT_MS ms)"
	sweep_leftovers "$PROBE_DIR/leftover-processes.txt"
}

step_probe() {
	checkpoint 4 "GL driver self-test on the GPU (obs-haiku-gl-probe)"
	if [ "$OPT_NO_GPU" = 1 ]; then
		result "4 gl-probe" SKIPPED "--no-gpu"
		return
	fi
	if [ -z "$BIN" ] || [ ! -x "$BIN/obs-haiku-gl-probe" ]; then
		result "4 gl-probe" SKIPPED "obs-haiku-gl-probe not found in ${BIN:-build/rundir/*/bin}"
		return
	fi
	printf 'name\tstatus\texit\tms\targs\n' >"$PROBE_DIR/results.tsv"

	# --help: only its text matters (which options exist); a non-zero exit is normal for older probes.
	# It does not touch the GPU (gl-probe.c parses the arguments before anything else).
	local help="$PROBE_DIR/help.log"
	run_to "$OBS_TEST_PROBE_TIMEOUT" "$help" "$BIN" ./obs-haiku-gl-probe --help
	note "probe --help: exit $RT_EXIT, timeout $RT_TIMEOUT"
	has_opt() { grep -q -e "$1" "$help" 2>/dev/null; }

	# The probe's optional features (gl-probe.c usage()): the second "stream" upload every other frame, the
	# NV12 conversion with stage-surface readback, the second display (properties preview), the context
	# handoff to a second thread. Only switches the probe's --help lists are used. --verbose makes it print
	# (and flush) every GL step before running it; with the sync every second, a freeze leaves the step that
	# never returned at the end of its log.
	local feat avail=() base=() args=() g
	for feat in stream stage second-display handoff; do
		has_opt "--no-$feat" && avail+=("$feat")
	done
	has_opt '--verbose' && base+=(--verbose)
	note "probe features that can be switched off: ${avail[*]:-(none)}"

	# 1. every optional feature off, the fewest frames the probe accepts
	args=(${base[@]+"${base[@]}"})
	has_opt '--frames' && args+=(--frames 16)
	for g in ${avail[@]+"${avail[@]}"}; do args+=("--no-$g"); done
	probe_run minimal ${args[@]+"${args[@]}"}
	# 2. one feature at a time (with fewer than two switches that run would be the full one)
	if [ "${#avail[@]}" -ge 2 ]; then
		for feat in "${avail[@]}"; do
			args=(${base[@]+"${base[@]}"})
			has_opt '--frames' && args+=(--frames 16)
			for g in "${avail[@]}"; do [ "$g" != "$feat" ] && args+=("--no-$g"); done
			probe_run "only-$feat" "${args[@]}"
		done
	fi
	# 3. the full default, then the long paced variant
	args=(${base[@]+"${base[@]}"})
	has_opt '--frames' && args+=(--frames 120)
	probe_run full ${args[@]+"${args[@]}"}
	if has_opt '--fps' && has_opt '--frames'; then
		probe_run fps30-frames150 ${base[@]+"${base[@]}"} --fps 30 --frames 150
	fi

	local detail="$PROBE_OK/$PROBE_TOTAL runs ok"
	[ -n "$PROBE_BAD" ] && detail="$detail;$PROBE_BAD"
	if [ "$PROBE_TIMEOUTS" -gt 0 ]; then
		result "4 gl-probe" HANG "$detail"
	elif [ "$PROBE_NO_EGL" = "$PROBE_TOTAL" ]; then
		# nothing to test: OBS uses OSMesa on such a system (gl-haiku.c), which steps 3 and 5 cover
		result "4 gl-probe" SKIPPED "no EGL display (exit 2 in every run), OBS renders with OSMesa; $detail"
	elif [ "$PROBE_OK" = "$PROBE_TOTAL" ]; then
		result "4 gl-probe" PASS "$detail"
	else
		result "4 gl-probe" FAIL "$detail"
	fi
}

# ---------------------------------------------------------------------------------------------------------
# Steps 3, 5, 6: OBS scenario runs
# ---------------------------------------------------------------------------------------------------------
TONE_WAV=""
TONE_PID=""
TONE_NOTE="no tone played"

# A 3 s, 440 Hz, -20 dBFS stereo WAV, so that desktop audio has something to capture. Strictly optional.
make_tone() {
	local wav="$WORK/tone.wav"
	if have python3; then
		python3 -c '
import sys, wave, math, struct
w = wave.open(sys.argv[1], "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<hh", v, v)
    for v in (int(3276 * math.sin(2 * math.pi * 440 * i / 48000)) for i in range(48000 * 3))))
w.close()' "$wav" 2>>"$RUNLOG"
	elif have perl; then
		perl -e '
my ($f) = @ARGV; my $d = "";
for my $i (0 .. 48000 * 3 - 1) { my $v = int(3276 * sin(2 * 3.14159265358979 * 440 * $i / 48000)); $d .= pack("vv", $v & 0xffff, $v & 0xffff); }
open(my $o, ">", $f) or die; binmode $o;
print $o "RIFF" . pack("V", 36 + length($d)) . "WAVEfmt " . pack("VvvVVvv", 16, 1, 2, 48000, 192000, 4, 16) . "data" . pack("V", length($d)) . $d;
close $o;' "$wav" 2>>"$RUNLOG"
	fi
	[ -s "$wav" ] && TONE_WAV="$wav"
}

# Tools confirmed in Haiku's tree: media_client (src/bin/media_client, "media_client play <path>", plays through
# a BSoundPlayer until the file ends) and beep (src/bin/beep.cpp, plays the system "Beep" sound event).
play_tone() {
	TONE_PID=""
	if [ -n "$TONE_WAV" ] && have media_client; then
		progress "LAUNCH (background): media_client play $TONE_WAV"
		media_client play "$TONE_WAV" >>"$RUNLOG" 2>&1 </dev/null &
		TONE_PID=$!
		track "$TONE_PID"
		TONE_NOTE="tone: media_client play (3 s, 440 Hz, -20 dBFS)"
	elif have beep; then
		progress "LAUNCH (background): beep"
		beep >>"$RUNLOG" 2>&1 </dev/null &
		TONE_PID=$!
		track "$TONE_PID"
		TONE_NOTE="tone: beep (system beep event; silent if none is assigned)"
	else
		TONE_NOTE="no tone played (no media_client/beep, or no python3/perl to make the WAV)"
	fi
	note "$TONE_NOTE"
}

ALL_GL_PATHS="$WORK/.gl-paths"
: >"$ALL_GL_PATHS"

# obs_log_bytes <stdout capture> <xdg dir>: combined size of everything OBS has logged so far
obs_log_bytes() {
	local total f
	total=$(file_size "$1")
	for f in "$2"/obs-studio/logs/*.txt; do
		[ -f "$f" ] && total=$((total + $(file_size "$f")))
	done
	echo "$total"
}

newest_recording() {
	local f newest=""
	for f in "$1"/*; do
		[ -f "$f" ] || continue
		if [ -z "$newest" ] || [ "$f" -nt "$newest" ]; then newest="$f"; fi
	done
	echo "$newest"
}

# ffprobe/ffmpeg helpers; print nothing when the tool or the answer is missing.
ffp() { quiet_to 60 ffprobe -v error "$@" | head -n 1 | tr -d '\r'; }

# obs_run <step number> <name> <play tone: 1|0> [VAR=value ...]
obs_run() {
	local num="$1" name="$2" tone="$3"
	shift 3
	local dir="$REPORT/0$num-obs-$name" xdg="$WORK/xdg-$name"
	local rec="$dir/recording" out="$dir/obs-stdout.log"
	mkdir -p "$dir"
	rm -rf "$xdg"
	checkpoint "$num" "OBS run '$name' (${*:-no renderer override}), recording ${OBS_TEST_REC_SECS} s"
	STEP="$num obs-$name"

	# --- throwaway configuration -------------------------------------------------------------------
	local with_text=0 cfg
	if [ -n "$(find "$(dirname "$BIN")" -name 'text-freetype2*' 2>/dev/null | head -n 1)" ]; then with_text=1; fi
	cfg="$(bash "$HERE/test/make-config.sh" "$xdg" "$rec" "$with_text" 2>"$dir/make-config.err")"
	if [ $? -ne 0 ] || [ -z "$cfg" ]; then
		result "$num obs-$name" FAIL "could not write the throwaway configuration (make-config.err)"
		return
	fi
	local CFG_PROFILE="" CFG_COLLECTION="" CFG_FPS=30 CFG_CX=0 CFG_CY=0
	eval "$cfg"
	mkdir -p "$dir/config-seeded"
	cp -R "$xdg/obs-studio/." "$dir/config-seeded/" 2>/dev/null
	if have python3; then
		python3 -m json.tool "$xdg/obs-studio/basic/scenes/$CFG_COLLECTION.json" >/dev/null 2>"$dir/json-check.txt" &&
			echo "scene collection JSON is valid" >"$dir/json-check.txt"
	fi

	# --- start ---------------------------------------------------------------------------------------
	{
		echo "\$ cd $BIN"
		echo "\$ XDG_CONFIG_HOME=$xdg $* ./obs --multi --verbose --disable-updater --disable-missing-files-check" \
			"--profile $CFG_PROFILE --collection $CFG_COLLECTION --startrecording"
	} >"$dir/command.txt"
	progress "LAUNCH (cwd $BIN, log ${out#"$WORK"/}, OBS log in ${xdg#"$WORK"/}/obs-studio/logs):" \
		"env -u OBS_HAIKU_GL -u OBS_HAIKU_GL_WATCHDOG -u OBS_HAIKU_GL_PROBE XDG_CONFIG_HOME=$xdg $* ./obs --multi" \
		"--verbose --disable-updater --disable-missing-files-check --profile $CFG_PROFILE --collection" \
		"$CFG_COLLECTION --startrecording"
	(
		cd "$BIN" || exit 126
		unset OBS_HAIKU_GL OBS_HAIKU_GL_WATCHDOG OBS_HAIKU_GL_PROBE
		export XDG_CONFIG_HOME="$xdg"
		for kv in "$@"; do export "${kv?}"; done
		exec ./obs --multi --verbose --disable-updater --disable-missing-files-check \
			--profile "$CFG_PROFILE" --collection "$CFG_COLLECTION" --startrecording
	) >"$out" 2>&1 </dev/null &
	local pid=$!
	track "$pid"
	note "obs-$name started, pid $pid"

	# --- watch ---------------------------------------------------------------------------------------
	local t0 now el next_sample=0 rec_start=0 outcome="" listimage_done=0 tone_done=0 tone_started=0
	local cpu="" prev_cpu="" bytes=0 prev_bytes=-1 stall_ms=0 rec_bytes=0 f first_cpu="" last_cpu=""
	local stall_limit=$((OBS_TEST_STALL_SECS * 1000)) captured=0
	printf 'elapsed_ms\talive\tcpu_ms\tlog_bytes\trecording_bytes\tphase\n' >"$dir/samples.tsv"
	t0=$(now_ms)
	while :; do
		now=$(now_ms)
		el=$((now - t0))
		if ! alive "$pid"; then
			outcome=died
			break
		fi
		if [ "$rec_start" = 0 ]; then
			# frontend/widgets/OBSBasic_Recording.cpp:42 logs this once the recording output has started
			if grep -q '==== Recording Start' "$out" "$xdg"/obs-studio/logs/*.txt 2>/dev/null; then
				rec_start=$now
				note "obs-$name: recording started after $el ms"
			elif [ "$el" -ge $((OBS_TEST_STARTUP_TIMEOUT * 1000)) ]; then
				outcome=norec
				break
			fi
		elif [ $((now - rec_start)) -ge $((OBS_TEST_REC_SECS * 1000)) ]; then
			outcome=ok
			break
		fi

		if [ "$listimage_done" = 0 ] && [ "$el" -ge "$LISTIMAGE_AT_MS" ]; then
			listimage_done=1
			have listimage && quiet_to 20 listimage "$pid" >"$dir/listimage-5s.txt"
		fi

		if [ "$tone" = 1 ] && [ "$tone_done" = 0 ] && [ "$rec_start" != 0 ] && [ $((now - rec_start)) -ge 3000 ]; then
			tone_done=1
			tone_started=$now
			play_tone
		fi
		# media_client normally ends with the file; make sure it never outlives its welcome
		if [ -n "$TONE_PID" ] && [ "$tone_started" != 0 ] && [ $((now - tone_started)) -ge 8000 ]; then
			alive "$TONE_PID" && kill -TERM "$TONE_PID" 2>/dev/null
			TONE_PID=""
		fi

		if [ "$el" -ge "$next_sample" ]; then
			next_sample=$((el + SAMPLE_MS))
			durable # OBS's own log, as far as it got, onto the disk
			cpu="$(proc_cpu_ms "$pid")"
			bytes=$(obs_log_bytes "$out" "$xdg")
			f="$(newest_recording "$rec")"
			[ -n "$f" ] && rec_bytes=$(file_size "$f") || rec_bytes=0
			printf '%s\t1\t%s\t%s\t%s\t%s\n' "$el" "${cpu:-?}" "$bytes" "$rec_bytes" \
				"$([ "$rec_start" = 0 ] && echo startup || echo recording)" >>"$dir/samples.tsv"
			[ -z "$first_cpu" ] && first_cpu="$cpu"
			[ -n "$cpu" ] && last_cpu="$cpu"
			# Hang = alive, but the log does not grow and no CPU time is used, for OBS_TEST_STALL_SECS.
			# (A modal dialog nobody answers and a crashed team parked in debug_server look the same.)
			if [ -n "$cpu" ] && [ -n "$prev_cpu" ] && [ $((cpu - prev_cpu)) -lt "$STALL_CPU_MS" ] &&
				[ "$bytes" = "$prev_bytes" ]; then
				stall_ms=$((stall_ms + SAMPLE_MS))
			else
				stall_ms=0
			fi
			prev_cpu="$cpu"
			prev_bytes="$bytes"
			if [ "$stall_ms" -ge "$stall_limit" ]; then
				outcome=stalled
				break
			fi
		fi
		sleep "$TICK"
	done
	local watch_end
	watch_end=$(now_ms)
	note "obs-$name: watch ended: $outcome after $((watch_end - t0)) ms"
	progress "obs-$name: watch ended: $outcome after $((watch_end - t0)) ms, stopping it"

	# --- stop ----------------------------------------------------------------------------------------
	local stop_how="" exit_code="" rec_ms=0
	[ "$rec_start" != 0 ] && rec_ms=$((watch_end - rec_start))
	if [ "$outcome" = died ]; then
		stop_how="died by itself"
	else
		# a second look at the loaded libraries, now that the renderer has certainly been set up
		have listimage && quiet_to 20 listimage "$pid" >"$dir/listimage-end.txt"
		if [ "$outcome" = stalled ] || [ "$outcome" = norec ]; then
			capture_stuck "$pid" "$dir" "stuck"
			captured=1
		fi
		# frontend/obs-main.cpp:897 installs the SIGTERM handler; OBSApp::processSigTerm
		# (frontend/OBSApp.cpp:1952) saves and quits
		kill -TERM "$pid" 2>/dev/null
		if wait_gone "$pid" "$OBS_TEST_EXIT_GRACE"; then
			stop_how=SIGTERM
		else
			[ "$captured" = 0 ] && capture_stuck "$pid" "$dir" "no-exit"
			kill -KILL "$pid" 2>/dev/null
			wait_gone "$pid" 5
			stop_how=SIGKILL
		fi
	fi
	wait "$pid" 2>/dev/null
	exit_code=$?
	progress "done: obs-$name stopped ($stop_how), exit $exit_code"
	# OBS keeps recording until it has handled SIGTERM and stopped the output, after the watch ended (the
	# listimage above, the shutdown itself): frames up to here are legitimate, see the frame count check
	local stop_end rec_ms_max=0
	stop_end=$(now_ms)
	[ "$rec_start" != 0 ] && rec_ms_max=$((stop_end - rec_start))
	printf '%s\t0\t%s\t%s\t%s\tstopped\n' "$(($(now_ms) - t0))" "${last_cpu:-?}" "$(obs_log_bytes "$out" "$xdg")" \
		"$rec_bytes" >>"$dir/samples.tsv"
	if [ -n "$TONE_PID" ]; then
		alive "$TONE_PID" && kill -TERM "$TONE_PID" 2>/dev/null
		TONE_PID=""
	fi
	sweep_leftovers "$dir/leftover-processes.txt"

	# --- collect -------------------------------------------------------------------------------------
	mkdir -p "$dir/obs-log" "$dir/config-after"
	cp "$xdg"/obs-studio/logs/*.txt "$dir/obs-log/" 2>/dev/null
	cp "$xdg"/obs-studio/*.ini "$dir/config-after/" 2>/dev/null
	cp -R "$xdg/obs-studio/basic" "$dir/config-after/" 2>/dev/null
	[ -d "$xdg/obs-studio/crashes" ] && cp -R "$xdg/obs-studio/crashes" "$dir/obs-crashes" 2>/dev/null
	[ -f "$xdg/obs-studio/haiku-gl-hang" ] && cp "$xdg/obs-studio/haiku-gl-hang" "$dir/haiku-gl-hang.txt" 2>/dev/null

	# The OBS log file and the stdout capture carry the same lines; prefer the file, fall back to stdout.
	local logsrc=("$dir"/obs-log/*.txt)
	[ -f "${logsrc[0]}" ] || logsrc=("$out")
	grep -hE 'Haiku GL|Haiku EGL|OpenGL|[Rr]enderer|adapter|OSMesa|llvmpipe|zink|Zink' "${logsrc[@]}" 2>/dev/null |
		head -n 200 >"$dir/gl-lines.txt"
	grep -hiE 'CPU wait not satisfied|failed its self-test|Haiku GL watchdog:|hung in a previous session' \
		"${logsrc[@]}" 2>/dev/null | head -n 50 >"$dir/bad-lines.txt"
	grep -hE 'error|Error|ERROR|failed|Failed|warning: ' "${logsrc[@]}" 2>/dev/null | head -n 200 >"$dir/error-lines.txt"

	# Which renderer was really used: decided from the "Haiku GL:" lines, shown with the driver line.
	local renderer="unknown" gl_first
	if grep -qiE 'Haiku GL:.*(using OSMesa|falling back to OSMesa|using the CPU renderer)' "$dir/gl-lines.txt"; then
		renderer="OSMesa (CPU)"
	elif grep -qE 'OBS_HAIKU_GL=osmesa' "$dir/gl-lines.txt" || [ "${*#*OBS_HAIKU_GL=osmesa}" != "$*" ]; then
		renderer="OSMesa (CPU, forced)"
	elif grep -qE 'Haiku EGL: version' "$dir/gl-lines.txt"; then
		renderer="EGL (hardware driver)"
	fi
	gl_first="$(grep -hE 'Loading up OpenGL|OpenGL loaded|GL_RENDERER|[Rr]enderer:' "$dir/gl-lines.txt" | head -n 1 |
		sed 's/^[0-9:. ]*//')"
	[ -n "$gl_first" ] && renderer="$renderer [$gl_first]"

	if [ -s "$dir/listimage-5s.txt" ] || [ -s "$dir/listimage-end.txt" ]; then
		gl_lib_lines "$dir"/listimage-*.txt >"$dir/gl-libs.txt"
		gl_lib_paths "$dir"/listimage-5s.txt "$dir"/listimage-end.txt 2>/dev/null |
			sed "s|\$|   (obs-$name)|" >>"$ALL_GL_PATHS"
	fi

	# --- recording -----------------------------------------------------------------------------------
	local recfile recsize=0 vcodec="" vdim="" frames="" dur="" acodec="" maxvol="" meanvol="" audio="no audio stream"
	# expected: frames in the time recorded by the clock until the watch ended; expected_max: until OBS was gone
	local expected=$((CFG_FPS * rec_ms / 1000)) expected_max=$((CFG_FPS * rec_ms_max / 1000)) probed=0
	recfile="$(newest_recording "$rec")"
	{
		echo "recording directory: $rec"
		ls -la "$rec" 2>&1
	} >"$dir/recording.txt"
	if [ -n "$recfile" ]; then
		recsize=$(file_size "$recfile")
		if have ffprobe; then
			probed=1
			run_to 60 "$dir/ffprobe.txt" "$dir" ffprobe -v error -show_entries \
				"format=format_name,duration,size:stream=index,codec_type,codec_name,width,height,avg_frame_rate,r_frame_rate,channels,sample_rate" \
				-of default=noprint_wrappers=1 "$recfile"
			vcodec="$(ffp -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$recfile")"
			vdim="$(ffp -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x "$recfile")"
			frames="$(ffp -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 "$recfile" |
				tr -cd '0-9')"
			dur="$(ffp -show_entries format=duration -of csv=p=0 "$recfile")"
			acodec="$(ffp -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$recfile")"
		fi
		if [ -n "$acodec" ] && have ffmpeg; then
			# volumedetect prints mean_volume/max_volume; digital silence is about -91 dB
			run_to 60 "$dir/volumedetect.txt" "$dir" ffmpeg -hide_banner -nostdin -i "$recfile" -vn \
				-af volumedetect -f null -
			maxvol="$(sed -n 's/.*max_volume: *\(-\{0,1\}[0-9.inf]*\) dB.*/\1/p' "$dir/volumedetect.txt" | tail -n 1)"
			meanvol="$(sed -n 's/.*mean_volume: *\(-\{0,1\}[0-9.inf]*\) dB.*/\1/p' "$dir/volumedetect.txt" | tail -n 1)"
		fi
		if [ -n "$acodec" ]; then
			audio="audio $acodec"
			if [ -n "$maxvol" ]; then
				case "$maxvol" in
				-inf | -9[0-9]* | -8[0-9]* | -7[0-9]*) audio="$audio max ${maxvol} dB (SILENT)" ;;
				*) audio="$audio max ${maxvol} dB mean ${meanvol} dB (sound)" ;;
				esac
			fi
		fi
		{
			echo
			echo "file: $recfile"
			echo "size: $recsize bytes"
			echo "video: ${vcodec:-?} ${vdim:-?}, frames: ${frames:-?}, duration: ${dur:-?} s"
			echo "expected frames: $expected ($CFG_FPS fps x $rec_ms ms of recording by the clock until SIGTERM)" \
				"to $expected_max (until OBS had exited), accepted -20% / +20%"
			echo "$audio"
			[ "$tone" = 1 ] && echo "$TONE_NOTE"
		} >>"$dir/recording.txt"
		if [ "$recsize" -ge "$MAX_RECORDING_BYTES" ]; then
			echo "recording removed from the report: $recsize bytes is over the 8 MB limit" >>"$dir/recording.txt"
			rm -f "$recfile"
		fi
	fi

	# --- verdict -------------------------------------------------------------------------------------
	local status="" why="" bad=0
	if [ -s "$dir/bad-lines.txt" ]; then
		if grep -qiE 'CPU wait not satisfied|Haiku GL watchdog:' "$dir/bad-lines.txt"; then
			bad=1
		elif [ "$name" != default ]; then
			# In the default run a failed self-test followed by the CPU renderer is the designed
			# fallback, so it is reported (bad-lines.txt, renderer) but does not fail the run.
			bad=1
		fi
	fi
	if [ "$outcome" = died ]; then
		status=FAIL
		if [ "$exit_code" -ge 128 ]; then
			why="CRASHED (signal $((exit_code - 128))) after $(((watch_end - t0) / 1000)) s"
		else
			why="exited by itself with status $exit_code after $(((watch_end - t0) / 1000)) s"
		fi
	elif [ "$outcome" = stalled ]; then
		status=HANG
		why="alive but no log output and no CPU time for ${OBS_TEST_STALL_SECS} s (stopped with $stop_how)"
	elif [ "$outcome" = norec ]; then
		status=FAIL
		why="recording did not start within ${OBS_TEST_STARTUP_TIMEOUT} s (stopped with $stop_how)"
	elif [ "$stop_how" = SIGKILL ]; then
		status=HANG
		why="no exit within ${OBS_TEST_EXIT_GRACE} s of SIGTERM, needed SIGKILL"
	elif [ "$exit_code" != 0 ]; then
		status=FAIL
		why="exit status $exit_code after SIGTERM"
	elif [ "$bad" = 1 ]; then
		status=FAIL
		why="driver trouble in the log: $(head -n 1 "$dir/bad-lines.txt" | sed 's/^[0-9:. ]*//' | cut -c1-90)"
	elif [ -z "$recfile" ]; then
		status=FAIL
		why="no recording file"
	elif [ "$probed" = 0 ]; then
		status=UNVERIFIED
		why="ffprobe missing, recording not checked ($recsize bytes)"
	elif [ -z "$vcodec" ]; then
		status=FAIL
		why="recording has no video stream"
	elif [ -z "$frames" ] || [ "$frames" -lt $((expected * 80 / 100)) ] || [ "$frames" -gt $((expected_max * 120 / 100)) ]; then
		status=FAIL
		why="frame count off"
	else
		status=PASS
	fi
	local cpu_used="?"
	[ -n "$first_cpu" ] && [ -n "$last_cpu" ] && cpu_used="$((last_cpu - first_cpu)) ms"
	local detail="renderer: $renderer; "
	[ -n "$why" ] && detail="$detail$why; "
	detail="${detail}frames ${frames:-?}/$expected, ${vcodec:-no video} ${vdim:-}, ${dur:-?} s, $recsize bytes; $audio; cpu $cpu_used; stop: $stop_how, exit $exit_code"
	{
		echo "status: $status"
		echo "outcome of the watch loop: $outcome"
		echo "stopped with: $stop_how, exit status: $exit_code"
		echo "renderer: $renderer"
		echo "detail: $detail"
	} >"$dir/result.txt"
	result "$num obs-$name" "$status" "$detail"
}

have_obs() { [ -n "$BIN" ] && [ -x "$BIN/obs" ]; }

# Step 3: CPU only. OBS_HAIKU_GL=osmesa makes gl-haiku.c skip EGL entirely (no self-test either, and
# OBS_HAIKU_GL_PROBE=0 says so explicitly): if this freezes the machine, the GPU driver is not the cause.
step_obs_cpu() {
	if [ "$OPT_GPU_ONLY" = 1 ]; then
		checkpoint 3 "OBS run 'osmesa'"
		result "3 obs-osmesa" SKIPPED "--gpu-only"
	elif ! have_obs; then
		checkpoint 3 "OBS run 'osmesa'"
		result "3 obs-osmesa" SKIPPED "no obs executable in ${BIN:-build/rundir/*/bin}"
	else
		obs_run 3 osmesa 0 OBS_HAIKU_GL=osmesa OBS_HAIKU_GL_PROBE=0
	fi
}

# Steps 5 and 6, after the self-test: the default renderer (OBS's own self-test, then EGL on the GPU if it
# passes), then forced EGL without the hang watchdog, the run most likely to take the driver down.
step_obs_gpu() {
	if ! have_obs; then
		checkpoint 5 "OBS runs on the GPU"
		result "5 obs-default" SKIPPED "no obs executable in ${BIN:-build/rundir/*/bin}"
		result "6 obs-egl-forced" SKIPPED "no obs executable"
		return
	fi
	make_tone
	if [ "$OPT_NO_GPU" = 1 ]; then
		obs_run 5 default 1 OBS_HAIKU_GL=osmesa OBS_HAIKU_GL_PROBE=0
	else
		obs_run 5 default 1
	fi
	if [ "$OPT_NO_GPU" = 1 ]; then
		checkpoint 6 "OBS run 'egl-forced'"
		result "6 obs-egl-forced" SKIPPED "--no-gpu"
	elif [ "$OPT_QUICK" = 1 ]; then
		checkpoint 6 "OBS run 'egl-forced'"
		result "6 obs-egl-forced" SKIPPED "--quick"
	elif [ "$PROBE_TOTAL" -gt 0 ] && [ "$PROBE_NO_EGL" = "$PROBE_TOTAL" ]; then
		# OBS_HAIKU_GL=egl without EGL ends in OBS's "failed to initialize video" box, which would be
		# reported as a hang
		checkpoint 6 "OBS run 'egl-forced'"
		result "6 obs-egl-forced" SKIPPED "no EGL display (the self-test exited 2 in every run)"
	else
		obs_run 6 egl-forced 0 OBS_HAIKU_GL=egl OBS_HAIKU_GL_WATCHDOG=0
	fi
}

# ---------------------------------------------------------------------------------------------------------
# Step 7: crash evidence
# ---------------------------------------------------------------------------------------------------------
step_crash() {
	checkpoint 7 "Crash evidence"
	local dir="$REPORT/07-crash" f n=0
	for f in "$HOME"/Desktop/*obs*.report "$HOME"/Desktop/*gl-probe*.report \
		/boot/system/var/log/crash/app-*obs* /boot/system/var/log/crash/app-*gl-probe*; do
		[ -f "$f" ] || continue
		[ "$f" -nt "$WORK/.start" ] || continue
		[ -e "$dir/$(basename "$f")" ] && continue
		cp "$f" "$dir/" 2>/dev/null && n=$((n + 1))
	done
	# The hang record of the REAL configuration (written by earlier, normal OBS sessions): copied, never touched.
	f="$HOME/config/settings/obs-studio/haiku-gl-hang"
	if [ -f "$f" ]; then
		cp "$f" "$dir/real-config-haiku-gl-hang.txt" 2>/dev/null
		echo "present: $f (from a normal OBS session before this run)" >"$dir/hang-marker.txt"
	else
		echo "absent: $f" >"$dir/hang-marker.txt"
	fi
	syslog_tail syslog-after.txt
	if [ "$n" -gt 0 ]; then
		result "7 crash-evidence" FAIL "$n new crash report(s) copied to 07-crash/"
	else
		result "7 crash-evidence" PASS "no new crash reports"
	fi
}

# ---------------------------------------------------------------------------------------------------------
# Step 8: summary and archive
# ---------------------------------------------------------------------------------------------------------
step_summary() {
	local sum="$REPORT/SUMMARY.txt" r step status detail overall=PASS
	sweep_leftovers "$REPORT/leftover-processes.txt"
	{
		echo "OBS Studio Haiku port: automated test run"
		echo "commit:  $GIT_COMMIT  (branch $GIT_BRANCH, $GIT_DIRTY tracked file(s) with local changes)"
		echo "when:    $STAMP    host: $(uname -srm 2>/dev/null)"
		echo "options: ${ORIG_ARGS[*]:-(none)}"
		[ "$BUILD_FAILED" = 1 ] && [ -n "$BIN" ] &&
			echo "NOTE:    the build FAILED; steps 3 to 6 ran whatever binaries were already in $BIN"
		if [ "${#PACKED_RUNS[@]}" -gt 0 ]; then
			echo
			echo "EARLIER RUNS THAT DID NOT FINISH (details in 00-previous-incomplete/):"
			for r in "${PACKED_RUNS[@]}"; do
				describe_prev "$r" | sed 's/^/  /'
			done
		fi
		echo
		for r in "${RESULTS[@]}"; do
			step="${r%%|*}"
			r="${r#*|}"
			status="${r%%|*}"
			detail="${r#*|}"
			printf '%-18s %-10s %s\n' "$step" "$status" "$detail"
			case "$status" in PASS | SKIPPED) ;; *) overall=FAIL ;; esac
		done
		echo
		echo "OVERALL: $overall"
		echo
		echo "Graphics libraries loaded by OBS (from listimage):"
		if [ -s "$ALL_GL_PATHS" ]; then
			sort -u "$ALL_GL_PATHS" | sed 's/^/  /'
		else
			echo "  (none recorded: listimage unavailable, or OBS did not live long enough)"
		fi
		if ls "$REPORT"/04-probe/*-listimage.txt >/dev/null 2>&1; then
			echo "Graphics libraries loaded by the self-test:"
			gl_lib_paths "$REPORT"/04-probe/*-listimage.txt | sed 's/^/  /'
		fi
		if [ -s "$REPORT/leftover-processes.txt" ]; then
			echo
			echo "Processes found at the end:"
			sed 's/^/  /' "$REPORT/leftover-processes.txt"
		fi
	} >"$sum"
	echo
	echo "======================================================================"
	cat "$sum"
	echo "======================================================================"
}

step_archive() {
	STEP="8 archive"
	local out="$OBS_TEST_OUT_DIR" archive="" d
	progress "=== step begins: summary and archive"
	cp "$PROGRESS" "$REPORT/00-progress.txt" 2>/dev/null
	mkdir -p "$out" 2>/dev/null || out="$HOME"
	[ -d "$out" ] && [ -w "$out" ] || out="$HOME"
	if have zip; then
		archive="$out/$REPORT_NAME.zip"
		rm -f "$archive"
		(cd "$WORK" && zip -qr "$archive" "$REPORT_NAME") >>"$RUNLOG" 2>&1 || archive=""
	fi
	if [ -z "$archive" ] && have tar; then
		archive="$out/$REPORT_NAME.tar.gz"
		(cd "$WORK" && tar czf "$archive" "$REPORT_NAME") >>"$RUNLOG" 2>&1 || archive=""
	fi
	cleanup_pids
	if [ -z "$archive" ] || [ ! -s "$archive" ]; then
		progress "run finished, but no archive could be written"
		echo "finished $(date '+%Y-%m-%d %H:%M:%S'), no archive" >"$WORK/FINISHED"
		durable
		echo "Could not create an archive (no zip or tar?). The report is left in:"
		echo "$REPORT"
		return
	fi
	progress "run finished: $archive"
	echo "finished $(date '+%Y-%m-%d %H:%M:%S'): $archive" >"$WORK/FINISHED"
	durable
	if [ "$OPT_KEEP" = 1 ]; then
		echo "Working directory kept in $WORK"
	else
		rm -rf "$WORK"
		# Earlier unfinished runs are in this archive now (a run packed by a run that itself never
		# finished arrives nested in that one), so every REPORTED directory can go.
		for d in "$OBS_TEST_RUNS_DIR"/*/; do
			d="${d%/}"
			[ -f "$d/REPORTED" ] && rm -rf "$d"
		done
		rmdir "$OBS_TEST_RUNS_DIR" 2>/dev/null
	fi
	durable
	echo "Send this file:"
	echo "$archive"
}

step_previous
step_system
step_build
step_obs_cpu
step_probe
step_obs_gpu
step_crash
step_summary
step_archive
trap - EXIT
exit 0
