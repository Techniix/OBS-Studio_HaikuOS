#!/bin/bash
# Unattended test run of the Haiku port: one command, one archive on the Desktop.
#
#     build-aux/haiku/test.sh [--no-build] [--no-pull] [--quick] [--keep]
#
#   --no-pull   do not run "git pull --ff-only" first
#   --no-build  do not pull and do not build; test the binaries that are already there
#   --quick     10 s recordings instead of 20 s, and skip the forced-GPU run (the one most likely to hang)
#   --keep      keep the temporary working directory (throwaway OBS configurations, unzipped report)
#
# Run it from a Terminal inside the desktop session (OBS needs the app_server). It needs no interaction: OBS
# windows open and close by themselves, please leave them alone. The last line printed is the archive to send.
#
# What it does, each step with its own log and a time limit:
#   1. system facts (packages, accelerants, Vulkan ICDs, environment, syslog)
#   2. git pull --ff-only (skipped on a dirty tree) and build-aux/haiku/build.sh
#   3. the GL driver self-test (obs-haiku-gl-probe) with every option its --help lists
#   4. OBS itself, once per renderer, recording a known scene from a throwaway configuration
#      (XDG_CONFIG_HOME points into the temporary directory; the real ~/config/settings/obs-studio is never
#      written, see build-aux/haiku/test/make-config.sh)
#   5. crash reports written while it ran
#   6. SUMMARY.txt and the archive
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

SAMPLE_MS=2000              # sampling interval while OBS runs
LISTIMAGE_AT_MS=5000        # when to record the libraries OBS has loaded
STALL_CPU_MS=30             # less CPU time than this per sample counts as "not running"
MAX_RECORDING_BYTES=$((8 * 1024 * 1024))

OPT_NO_BUILD=0
OPT_NO_PULL=0
OPT_QUICK=0
OPT_KEEP=0
ORIG_ARGS=("$@")
for arg in "$@"; do
	case "$arg" in
	--no-build) OPT_NO_BUILD=1 ;;
	--no-pull) OPT_NO_PULL=1 ;;
	--quick) OPT_QUICK=1 ;;
	--keep) OPT_KEEP=1 ;;
	-h | --help)
		sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "unknown option: $arg" >&2
		echo "usage: $0 [--no-build] [--no-pull] [--quick] [--keep]" >&2
		exit 2
		;;
	esac
done
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

RUNLOG=/dev/null
say() {
	echo "$*"
	echo "[$(date +%H:%M:%S)] $*" >>"$RUNLOG"
}
note() { echo "[$(date +%H:%M:%S)] $*" >>"$RUNLOG"; }

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
RT_HOOK=""
RT_ON_TIMEOUT=""
RT_EXIT=0
RT_MS=0
RT_TIMEOUT=0
RT_PID=0
run_to() {
	local tmo_ms=$(($1 * 1000)) log="$2" cwd="$3"
	shift 3
	local t0 el
	RT_TIMEOUT=0
	t0=$(now_ms)
	(cd "$cwd" 2>/dev/null || exit 126; exec "$@") >>"$log" 2>&1 </dev/null &
	RT_PID=$!
	track "$RT_PID"
	while alive "$RT_PID"; do
		el=$(($(now_ms) - t0))
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
	RT_HOOK=""
	RT_ON_TIMEOUT=""
}

# quiet_to <timeout s> <command...>: like run_to but only the captured stdout matters (printed), stderr dropped.
quiet_to() {
	local tmo="$1" tmp="$WORK/.quiet.$RANDOM"
	shift
	: >"$tmp"
	local t0 pid
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
		local saved_hook="$RT_HOOK" saved_to="$RT_ON_TIMEOUT"
		RT_HOOK=""
		RT_ON_TIMEOUT=""
		run_to 90 "$dir/$tag-debugger.log" "$dir" "$DEBUGGER" "--save-report=$dir/$tag-debugger.report" --team "$pid"
		[ "$RT_TIMEOUT" = 1 ] && echo "Debugger did not finish within 90 s" >>"$dir/$tag-debugger.log"
		RT_HOOK="$saved_hook"
		RT_ON_TIMEOUT="$saved_to"
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
	local out="$1" pid args mine killed=0 p
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
	done < <(list_teams)
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
# Working directory. A pull that changed the tree re-executes the (possibly updated) script with
# OBS_TEST_WORK set, so that the second instance continues in the same directory.
# ---------------------------------------------------------------------------------------------------------
if [ -n "${OBS_TEST_WORK:-}" ] && [ -d "${OBS_TEST_WORK:-}" ]; then
	WORK="$OBS_TEST_WORK"
else
	WORK="$(mktemp -d "${TMPDIR:-/tmp}/obs-test.XXXXXX" 2>/dev/null)"
	if [ -z "$WORK" ] || [ ! -d "$WORK" ]; then
		WORK="${TMPDIR:-/tmp}/obs-test.$$"
		mkdir -p "$WORK" || { echo "cannot create a temporary directory" >&2; exit 1; }
	fi
	: >"$WORK/.start" # crash reports newer than this file belong to this run
fi
export OBS_TEST_WORK="$WORK"
RUNLOG="$WORK/run.log"

trap 'echo; echo "interrupted, cleaning up"; cleanup_pids; exit 130' INT TERM
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
			export OBS_TEST_PULL_STATUS="$PULL_STATUS" OBS_TEST_PULL_DETAIL="$PULL_DETAIL"
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
mkdir -p "$REPORT/01-system" "$REPORT/02-build" "$REPORT/03-probe" "$REPORT/05-crash"
mv "$WORK/run.log" "$REPORT/00-run.log" 2>/dev/null
RUNLOG="$REPORT/00-run.log"
[ -f "$WORK/pull.log" ] && mv "$WORK/pull.log" "$REPORT/02-build/pull.log"

say "==> OBS Haiku test run: $GIT_BRANCH @ $GIT_COMMIT ($STAMP)"
say "    working directory: $WORK"
note "options: no-build=$OPT_NO_BUILD no-pull=$OPT_NO_PULL quick=$OPT_QUICK keep=$OPT_KEEP is_haiku=$IS_HAIKU tick=$TICK"

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
	say "==> 1. System facts"
	fact system.txt "uname -a" uname -a
	fact system.txt "date" date
	# version(1) prints the version resource of a file; libbe carries the Haiku revision
	[ -e /boot/system/lib/libbe.so ] && fact system.txt "version /boot/system/lib/libbe.so" version /boot/system/lib/libbe.so
	fact system.txt "sysinfo" sysinfo
	fact system.txt "screenmode" screenmode
	fact system.txt "df (temporary directory)" df -h "$WORK"
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
		echo "(the OBS runs below unset OBS_HAIKU_GL and OBS_HAIKU_GL_WATCHDOG and set them per run)"
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
	say "==> 2. Pull and build"
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
# Step 3: GL probe matrix
# ---------------------------------------------------------------------------------------------------------
PROBE_DIR="$REPORT/03-probe"
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
PROBE_BAD=""
# probe_run <name> <args...>
probe_run() {
	local name="$1"
	shift
	PROBE_CUR="$name"
	PROBE_LISTIMAGE_DONE=0
	echo "\$ ./obs-haiku-gl-probe $*" >"$PROBE_DIR/$name.log"
	RT_HOOK=probe_hook
	RT_ON_TIMEOUT=probe_on_timeout
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
	else
		status=FAIL
		PROBE_BAD="$PROBE_BAD $name=exit$RT_EXIT"
	fi
	printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$status" "$RT_EXIT" "$RT_MS" "$*" >>"$PROBE_DIR/results.tsv"
	say "    probe $name: $status (exit $RT_EXIT, $RT_MS ms)"
	sweep_leftovers "$PROBE_DIR/leftover-processes.txt"
}

step_probe() {
	say "==> 3. GL driver self-test (obs-haiku-gl-probe)"
	if [ -z "$BIN" ] || [ ! -x "$BIN/obs-haiku-gl-probe" ]; then
		result "3 gl-probe" SKIPPED "obs-haiku-gl-probe not found in ${BIN:-build/rundir/*/bin}"
		return
	fi
	printf 'name\tstatus\texit\tms\targs\n' >"$PROBE_DIR/results.tsv"

	# --help: only its text matters (which options exist); a non-zero exit is normal for older probes
	local help="$PROBE_DIR/help.log"
	run_to "$OBS_TEST_PROBE_TIMEOUT" "$help" "$BIN" ./obs-haiku-gl-probe --help
	note "probe --help: exit $RT_EXIT, timeout $RT_TIMEOUT"
	has_opt() { grep -q -e "$1" "$help" 2>/dev/null; }

	probe_run frames120 --frames 120
	has_opt '--verbose' && probe_run verbose --verbose
	has_opt '--no-stream' && probe_run no-stream --no-stream
	has_opt '--no-second-display' && probe_run no-second-display --no-second-display
	if has_opt '--fps' && has_opt '--frames'; then
		probe_run fps30-frames150 --fps 30 --frames 150
	fi

	local detail="$PROBE_OK/$PROBE_TOTAL runs ok"
	[ -n "$PROBE_BAD" ] && detail="$detail;$PROBE_BAD"
	if [ "$PROBE_TIMEOUTS" -gt 0 ]; then
		result "3 gl-probe" HANG "$detail"
	elif [ "$PROBE_OK" = "$PROBE_TOTAL" ]; then
		result "3 gl-probe" PASS "$detail"
	else
		result "3 gl-probe" FAIL "$detail"
	fi
}

# ---------------------------------------------------------------------------------------------------------
# Step 4: OBS scenario runs
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
		media_client play "$TONE_WAV" >>"$RUNLOG" 2>&1 </dev/null &
		TONE_PID=$!
		track "$TONE_PID"
		TONE_NOTE="tone: media_client play (3 s, 440 Hz, -20 dBFS)"
	elif have beep; then
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

# obs_run <name> <play tone: 1|0> [VAR=value ...]
obs_run() {
	local name="$1" tone="$2"
	shift 2
	local dir="$REPORT/04-obs-$name" xdg="$WORK/xdg-$name"
	local rec="$dir/recording" out="$dir/obs-stdout.log"
	mkdir -p "$dir"
	rm -rf "$xdg"
	say "==> 4. OBS run '$name' (${*:-no renderer override}), recording ${OBS_TEST_REC_SECS} s"

	# --- throwaway configuration -------------------------------------------------------------------
	local with_text=0 cfg
	if [ -n "$(find "$(dirname "$BIN")" -name 'text-freetype2*' 2>/dev/null | head -n 1)" ]; then with_text=1; fi
	cfg="$(bash "$HERE/test/make-config.sh" "$xdg" "$rec" "$with_text" 2>"$dir/make-config.err")"
	if [ $? -ne 0 ] || [ -z "$cfg" ]; then
		result "4 obs-$name" FAIL "could not write the throwaway configuration (make-config.err)"
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
	(
		cd "$BIN" || exit 126
		unset OBS_HAIKU_GL OBS_HAIKU_GL_WATCHDOG
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
	local expected=$((CFG_FPS * rec_ms / 1000)) probed=0
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
			echo "expected frames: $expected ($CFG_FPS fps x $rec_ms ms of recording by the clock), accepted +-20%"
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
	elif [ -z "$frames" ] || [ "$frames" -lt $((expected * 80 / 100)) ] || [ "$frames" -gt $((expected * 120 / 100)) ]; then
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
	result "4 obs-$name" "$status" "$detail"
}

step_obs() {
	if [ -z "$BIN" ] || [ ! -x "$BIN/obs" ]; then
		say "==> 4. OBS runs"
		result "4 obs-default" SKIPPED "no obs executable in ${BIN:-build/rundir/*/bin}"
		result "4 obs-osmesa" SKIPPED "no obs executable"
		result "4 obs-egl-forced" SKIPPED "no obs executable"
		return
	fi
	make_tone
	obs_run default 1
	obs_run osmesa 0 OBS_HAIKU_GL=osmesa
	# Last, because it is the run that can take the graphics driver (or the desktop) down with it.
	if [ "$OPT_QUICK" = 1 ]; then
		say "==> 4. OBS run 'egl-forced'"
		result "4 obs-egl-forced" SKIPPED "--quick"
	else
		obs_run egl-forced 0 OBS_HAIKU_GL=egl OBS_HAIKU_GL_WATCHDOG=0
	fi
}

# ---------------------------------------------------------------------------------------------------------
# Step 5: crash evidence
# ---------------------------------------------------------------------------------------------------------
step_crash() {
	say "==> 5. Crash evidence"
	local dir="$REPORT/05-crash" f n=0
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
		result "5 crash-evidence" FAIL "$n new crash report(s) copied to 05-crash/"
	else
		result "5 crash-evidence" PASS "no new crash reports"
	fi
}

# ---------------------------------------------------------------------------------------------------------
# Step 6: summary and archive
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
			echo "NOTE:    the build FAILED; steps 3 and 4 ran whatever binaries were already in $BIN"
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
		if [ -s "$REPORT/03-probe/frames120-listimage.txt" ] || ls "$REPORT"/03-probe/*-listimage.txt >/dev/null 2>&1; then
			echo "Graphics libraries loaded by the self-test:"
			gl_lib_paths "$REPORT"/03-probe/*-listimage.txt | sed 's/^/  /'
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
	local out="$OBS_TEST_OUT_DIR" archive=""
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
		echo "Could not create an archive (no zip or tar?). The report is left in:"
		echo "$REPORT"
		return
	fi
	if [ "$OPT_KEEP" = 1 ]; then
		echo "Temporary files kept in $WORK"
	else
		rm -rf "$WORK"
	fi
	echo "Send this file:"
	echo "$archive"
}

step_system
step_build
step_probe
step_obs
step_crash
step_summary
step_archive
trap - EXIT
exit 0
