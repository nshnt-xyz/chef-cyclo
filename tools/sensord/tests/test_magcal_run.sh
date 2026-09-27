#!/bin/sh
# Host test for initramfs/usr/bin/sensors-magcal-run: the real script with
# second-long steps, against tests/sensord-fake (main sensord serving REG2,
# plus the script's own -R helpers) and tests/fake_smgr as the ADSP (SMGR
# and QMAG_CAL). Checks the calibration selects on the wire, QMAG_CAL on
# and off, the files and steps the host analysis expects, that the
# helpers are gone and the main sensord untouched afterwards, and that
# tools/mag-cal-check.py reads the result (constant fake samples: it must
# not crash on degenerate data). Run from tools/sensord (make test does).
set -u

T=$(mktemp -d /tmp/snsm.XXXXXX)
export SENSORD_FAKE_IPC="$T/ipc"
mkdir -p "$SENSORD_FAKE_IPC" "$T/out"
SOCK="$T/sock"
RUN=../../initramfs/usr/bin/sensors-magcal-run
FAILS=0
SPID=
FPID=

cleanup() {
	[ -n "$SPID" ] && kill "$SPID" 2>/dev/null
	[ -n "$FPID" ] && kill "$FPID" 2>/dev/null
	wait 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT

fail() {
	echo "FAIL: $*"
	FAILS=$((FAILS + 1))
}

wait_for() {
	i=0
	while ! grep -q -- "$2" "$1" 2>/dev/null; do
		[ "$i" -ge "${3:-50}" ] && return 1
		sleep 0.1
		i=$((i + 1))
	done
}

# script_env NAME [VAR=VALUE...]: the environment for one run
script_env() {
	n=$1
	shift
	echo SENSORD="$PWD/tests/sensord-fake" MAIN_SOCK="$SOCK" OUT_BASE="$T/out/$n" \
		KMSG="$T/kmsg-$n" VIB="$T/vib-$n" SENSORS_UP_LOG="$T/none" NOW_CMD="$T/now" \
		FACE_S=0.4 FIG8_S=1 QFIG8_S=2 TURN_S=1 GAP_S=0 COUNT_S=1 HELPER_WAIT=40 "$@"
}

run_script() {
	: >"$T/vib-$1"
	# shellcheck disable=SC2046 # word-split on purpose: no spaces in $T
	env $(script_env "$1" MODE="$1") timeout 120 sh "$RUN" >"$T/run-$1.txt" 2>&1
}

# abort NAME PATTERN: start a run, TERM it once PATTERN is in its steps,
# then check nothing it started is left and the main sensord is clean.
abort() {
	: >"$T/vib-$1"
	# shellcheck disable=SC2046
	env $(script_env "$1" QFIG8_S=30) sh "$RUN" >"$T/run-$1.txt" 2>&1 &
	AP=$!
	i=0
	while ! grep -q "$2" "$T"/out/"$1"/magcal-*/steps.txt 2>/dev/null; do
		i=$((i + 1))
		[ "$i" -ge 300 ] && { fail "$1: never reached $2"; break; }
		sleep 0.1
	done
	sleep 0.3
	kill "$AP"
	i=0
	while kill -0 "$AP" 2>/dev/null && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
	kill -0 "$AP" 2>/dev/null && fail "$1: script still running 5 s after TERM"
	wait "$AP"
	rc=$?
	[ "$rc" -eq 130 ] || fail "$1: exit $rc, want 130"
	AD=$(ls -d "$T"/out/"$1"/magcal-* | head -n 1)
	pgrep -f "$AD" >/dev/null && fail "$1: helper or capture left: $(pgrep -af "$AD")"
	pgrep -f "$RUN" >/dev/null && fail "$1: script or poller left: $(pgrep -af "$RUN")"
	sleep 0.5
	./tests/sensord-fake -S "$SOCK" list | grep -q '"claims":[1-9]' && fail "$1: claims left on the main sensord"
	grep -q "DONE" "$T/run-$1.txt" && fail "$1: carried on after TERM"
}

./tests/fake_smgr mkreg "$T/reg" 1024 || { echo "FAIL: mkreg"; exit 1; }
# run through a path unique to this test, so the leftover checks only
# see this test's own processes
ln -s "$PWD/$RUN" "$T/magcal-run"
RUN="$T/magcal-run"

# --list needs nothing running
L=$(sh "$RUN" --list)
echo "$L" | grep -q "^ *1\. a.up  *10 s  FACE UP on the table, still$" || fail "--list: $L"
echo "$L" | grep -q "^16 steps, about 404 s of holds and gaps$" || fail "--list total: $L"
echo "$L" | grep -q "^ *16\. q.turn" || fail "--list count: $L"
echo "$L" | grep -q "one long buzz" || fail "--list legend"
L=$(MODE=sequential sh "$RUN" --list)
echo "$L" | grep -q "part q (magn: qmag)$" || fail "--list sequential q: $L"
echo "$L" | grep -q "^29 steps, about 664 s" || fail "--list sequential total: $L"
# step times on sensord's clock (CLOCK_MONOTONIC; the phone uses uptime)
printf '#!/bin/sh\nexec python3 -c "import time; print(\\"%%.3f\\" %% time.monotonic())"\n' >"$T/now"
chmod +x "$T/now"

# no sensord yet: refuses before doing anything
if env SENSORD="$PWD/tests/sensord-fake" MAIN_SOCK="$SOCK" OUT_BASE="$T/out/none" KMSG="$T/kmsg" \
	sh "$RUN" >"$T/norun.txt" 2>&1; then
	fail "ran without a sensord"
fi
grep -q "run sensors-up first" "$T/norun.txt" || fail "no-sensord message: $(cat "$T/norun.txt")"

./tests/sensord-fake -r "$T/reg" -m tests/fixtures/sns_reg.map -S "$SOCK" 2>"$T/main.log" &
SPID=$!
./tests/fake_smgr serve >"$T/smgr.log" 2>&1 &
FPID=$!
wait_for "$T/main.log" "smgr ready" 80 || fail "main sensord never ready"

# ------------------------------------------------------------ concurrent
run_script concurrent || fail "concurrent run exit $?: $(tail -n 5 "$T/run-concurrent.txt")"
D=$(ls -d "$T"/out/concurrent/magcal-* 2>/dev/null | head -n 1)
[ -n "$D" ] || fail "no output directory"
S=$(tail -n 1 "$T/run-concurrent.txt")
echo "$S" | grep -q "^magcal: done in [0-9]* s, $D; magn lines" || fail "summary: $S"
for f in a-magn-full a-magn-factory a-magn-raw q-magn-full q-magn-qmag q-magn-factory q-magn-raw \
	 a-accel a-anglvel q-accel q-anglvel; do
	n=$(grep -c '^{"sensor"' "$D/$f.jsonl" 2>/dev/null)
	[ "${n:-0}" -gt 0 ] || fail "$f: no samples"
done
echo "$S" | grep -q " a/full=[1-9][0-9]* .* q/qmag=[1-9][0-9]*" || fail "summary counts: $S"
echo "$S" | grep -q "qmag state on instance 7 enables 1 reports [1-9][0-9]* errors 1 last accuracy [0-3];" \
	|| fail "summary qmag: $S"
echo "$S" | grep -q "error lines 0$" || fail "summary errors: $S"
[ "$(grep -c ' STEP ' "$D/steps.txt")" -eq 16 ] || fail "steps: $(cat "$D/steps.txt")"
grep -q "^[0-9.]* STEP a.up: FACE UP on the table, still (0.4 s)$" "$D/steps.txt" || fail "step line shape"
grep -q "^[0-9.]* STEP q.turn: FACE UP on table, 1 slow full circle (1 s)$" "$D/steps.txt" || fail "turn step"
# the prompt format that worked live: numbered step, move time, hold
K=$T/kmsg-concurrent
grep -q "^magcal: ---- STEP 1/16: FACE UP on the table, still$" "$K" || fail "first step header: $(head -n 12 "$K")"
grep -q "^magcal: ---- STEP 9/16: FIGURE-8 in the air, keep going$" "$K" || fail "q.fig8 header"
grep -q "^magcal:      HOLD 2 s: FIGURE-8 in the air, keep going$" "$K" || fail "hold line"
grep -q "^magcal:      move the phone now (0 s)$" "$K" || fail "move line"
# 3 s more to move after face down (GAP_S=0 here), twice per run
[ "$(grep -c "^magcal:      move the phone now (3 s)$" "$K")" -eq 2 ] || fail "move time after face down"
grep -q "^magcal: ---- STEP 16/16: FACE UP on table, 1 slow full circle$" "$K" || fail "last step"
grep -Eq "then:|GO " "$K" && fail "old prompt format"
[ "$(cat "$T/vib-concurrent")" = 120 ] || fail "vibrator not driven: $(cat "$T/vib-concurrent")"
grep -q "^ *1\. a.up" "$T/run-concurrent.txt" || fail "step list not printed at the start"
grep -q "^# clock uptime [0-9.]* sample_t [0-9]*$" "$D/steps.txt" || fail "clock line: $(head -n 2 "$D/steps.txt")"
grep -q '"bias":\[-0.25,0.125,-0.375\]' "$D/q-magn-qmag.jsonl" || fail "qmag capture without bias"
grep -q '"bias"' "$D/q-magn-full.jsonl" && fail "main sensord carries QMAG fields"
grep -q "qmag report 1: instance 7" "$D/sensord-qmag.log" || fail "qmag helper log"
grep -q '"qmag":"on"' "$D/q-status.jsonl" || fail "q-status poll"
# the calibration selects on the wire: main full (cal 0), helpers 1 and 2
grep -q "sensor 20 dt 0 rate 20 report_rate 20 notify 1 cal 0 " "$T/smgr.log" || fail "full magn report"
grep -q "sensor 20 dt 0 rate 20 report_rate 20 notify 1 cal 1 " "$T/smgr.log" || fail "factory magn report"
grep -q "sensor 20 dt 0 rate 20 report_rate 20 notify 1 cal 2 " "$T/smgr.log" || fail "raw magn report"
# helpers use their own report IDs (-i 10/20/30; magn is channel 2)
grep -q "ADD report 13 sensor 20 dt 0 rate 20 report_rate 20 notify 1 cal 1 " "$T/smgr.log" || fail "factory helper report id"
grep -q "ADD report 23 sensor 20 dt 0 rate 20 report_rate 20 notify 1 cal 2 " "$T/smgr.log" || fail "raw helper report id"
grep -q "ADD report 33 sensor 20 dt 0 rate 20 report_rate 20 notify 1 cal 0 " "$T/smgr.log" || fail "qmag helper report id"
grep -q "QMAG ENABLE period none instance 7" "$T/smgr.log" || fail "QMAG_CAL not enabled"
wait_for "$T/smgr.log" "QMAG DISABLE instance 7$" 30 || fail "QMAG_CAL not disabled at the end"
# helpers gone, sockets and main sensord still serving, no report left
ls "$D"/*.sock >/dev/null 2>&1 && fail "helper sockets left"
pgrep -f "sensord-fake -R -S $D" >/dev/null && fail "helper sensord left running"
kill -0 "$SPID" 2>/dev/null || fail "main sensord died"
./tests/sensord-fake -S "$SOCK" status | grep -q '"smgr":"ready"' || fail "main sensord not ready after"
sleep 0.5
./tests/sensord-fake -S "$SOCK" list | grep -q '"claims":[1-9]' && fail "claims left on the main sensord"
# the host analysis reads it (degenerate constant data must not crash it)
python3 ../mag-cal-check.py "$D" >"$T/check.txt" 2>&1 || fail "mag-cal-check: $(tail -n 5 "$T/check.txt")"
grep -q "VERDICT q-magn-qmag after fit" "$T/check.txt" || fail "mag-cal-check output: $(head -n 20 "$T/check.txt")"
grep -q "Traceback" "$T/check.txt" && fail "mag-cal-check traceback"

# ------------------------------------------------------------ sequential
run_script sequential || fail "sequential run exit $?: $(tail -n 5 "$T/run-sequential.txt")"
D=$(ls -d "$T"/out/sequential/magcal-* 2>/dev/null | head -n 1)
for f in full-magn-full factory-magn-factory raw-magn-raw q-magn-qmag; do
	n=$(grep -c '^{"sensor"' "$D/$f.jsonl" 2>/dev/null)
	[ "${n:-0}" -gt 0 ] || fail "sequential $f: no samples"
done
[ -e "$D/a-magn-full.jsonl" ] && fail "sequential wrote phase a"
for f in q-magn-full q-magn-factory q-magn-raw; do
	[ -e "$D/$f.jsonl" ] && fail "sequential phase q is concurrent: $f"
done
grep -q "STEP raw.fig8" "$D/steps.txt" || fail "sequential steps"
grep -q "STEP full.turn" "$D/steps.txt" && fail "sequential has a phase-a turn"

# ------------------------------------------------------------ aborts
: >"$T/smgr.log.mark"
abort abort-a "STEP a.down"
grep -c "QMAG ENABLE" "$T/smgr.log" >"$T/enables-before-q"
abort abort-q "STEP q.fig8"
wait_for "$T/smgr.log" "QMAG ENABLE period none" 10 || fail "abort-q: QMAG never enabled"
last=$(grep "QMAG ENABLE period" "$T/smgr.log" | tail -n 1 | sed 's/.*instance //')
wait_for "$T/smgr.log" "QMAG DISABLE instance $last$" 30 || fail "abort-q: QMAG instance $last not disabled"
# after both aborts: no QMAG instance left on in the fake
en=$(grep -c "QMAG ENABLE period" "$T/smgr.log")
dis=$(grep -c "QMAG DISABLE instance [0-9]*$" "$T/smgr.log")
[ "$en" -eq "$dis" ] || fail "QMAG enables $en vs disables $dis"

if [ "$FAILS" -ne 0 ]; then
	echo "--- run"; tail -n 20 "$T/run-concurrent.txt"
	echo "--- smgr.log"; tail -n 20 "$T/smgr.log"
	echo "test_magcal_run.sh: $FAILS failure(s)"
	exit 1
fi
echo "test_magcal_run.sh: all passed"
