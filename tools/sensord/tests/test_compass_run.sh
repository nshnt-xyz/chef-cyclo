#!/bin/sh
# Host test for initramfs/usr/bin/sensors-compass-run: the real script
# with second-long steps, against tests/sensord-fake (main sensord serving
# REG2, plus the script's -R -V helper with ROTVEC=1) and tests/fake_smgr
# as the ADSP (SMGR, ROTATION_VECTOR, and a registry write of group 2980
# standing in for the DSP learning its bias). Checks the calibration
# rounds (never calibrates / calibrates mid-round / already calibrated),
# the prompts, the files and steps tools/compass-check.py expects, that
# the helper is gone and no claim is left on the main sensord afterwards,
# an abort, and that compass-check.py reads the result. Run from
# tools/sensord (make test does).
set -u

T=$(mktemp -d /tmp/snsr.XXXXXX)
export SENSORD_FAKE_IPC="$T/ipc"
mkdir -p "$SENSORD_FAKE_IPC" "$T/out"
SOCK="$T/sock"
RUN=../../initramfs/usr/bin/sensors-compass-run
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
		CAL_S=0.3 CAL_ROUNDS=2 TURN_S=0.6 TILT_S=0.5 BAR_S=0.3 ROLL_S=0.3 SHAKE_S=0.3 \
		QUICK_S=0.6 GAP_S=0 COUNT_S=1 POLL_S=1 HELPER_WAIT=40 "$@"
}

run_script() {
	n=$1
	shift
	: >"$T/vib-$n"
	# shellcheck disable=SC2046 # word-split on purpose: no spaces in $T
	env $(script_env "$n" "$@") timeout 120 sh "$RUN" >"$T/run-$n.txt" 2>&1
}

no_claims() {
	sleep 0.5
	./tests/sensord-fake -S "$SOCK" list | grep -q '"claims":[1-9]' && fail "$1: claims left on the main sensord"
	./tests/sensord-fake -S "$SOCK" status | grep -q '"compass":"off"' || fail "$1: heading still on"
}

# the fake ADSP applies its learned bias to factory magn once this file
# exists (sensord's live calibrated flag: full minus factory)
export FAKE_MAG_LEARN_FILE="$T/learned"

./tests/fake_smgr mkreg "$T/reg" 1024 || { echo "FAIL: mkreg"; exit 1; }
chmod 0400 "$T/reg"
ln -s "$PWD/$RUN" "$T/compass-run"
RUN="$T/compass-run"

# --list needs nothing running
L=$(sh "$RUN" --list)
echo "$L" | grep -q "^ *1\. cal1.up  *10 s  FACE UP on the table, still$" || fail "--list: $L"
echo "$L" | grep -q "^ *7\. turn.0  *10 s  FLAT, top edge at your REFERENCE mark, still$" || fail "--list turn.0: $L"
echo "$L" | grep -q "^ *17\. quick  *8 s  " || fail "--list quick: $L"
echo "$L" | grep -q "^17 steps, about 320 s of holds and gaps$" || fail "--list total: $L"
echo "$L" | grep -q "at most 3 rounds" || fail "--list rounds"
echo "$L" | grep -q "^ *16\. shake  *10 s  DO: same way: TAP and SHAKE the phone$" || fail "--list DO marker: $L"
echo "$L" | grep -q "^ *14\. tilt.bar  *8 s  same way: HANDLEBAR" || fail "--list hold step marked DO"
echo "$L" | grep -q "one long buzz = the step starts: hold still (HOLD), or start the movement (DO);" || fail "--list legend"
printf '#!/bin/sh\nexec python3 -c "import time; print(\\"%%.3f\\" %% time.monotonic())"\n' >"$T/now"
chmod +x "$T/now"

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

# --------------------------------------------- never calibrates, ROTVEC=1
run_script nocal ROTVEC=1 || fail "nocal run exit $?: $(tail -n 5 "$T/run-nocal.txt")"
D=$(ls -d "$T"/out/nocal/compass-* 2>/dev/null | head -n 1)
[ -n "$D" ] || fail "no output directory"
K=$T/kmsg-nocal
S=$(tail -n 1 "$T/run-nocal.txt")
echo "$S" | grep -q "^compass: done in [0-9]* s, $D; heading lines [1-9][0-9]* (calibrated 0, disturbed [0-9]*); calibration no after 12 holds; accel [1-9][0-9]* anglvel [1-9][0-9]* magn [1-9][0-9]*; rotvec [1-9][0-9]*; error lines 0$" \
	|| fail "summary: $S"
grep -q "^compass: NOT CALIBRATED after 2 rounds: the DSP did not learn the magnetometer bias$" "$K" || fail "not-calibrated message"
grep -q "^compass: not calibrated yet: calibration round 2 of 2, now 23 steps$" "$K" || fail "round 2 message: $(grep round "$K")"
grep -q "^compass: ---- STEP 1/17: FACE UP on the table, still$" "$K" || fail "first step header: $(head -n 8 "$K")"
grep -q "^compass: ---- STEP 13/23: FLAT, top edge at your REFERENCE mark, still$" "$K" || fail "turn.0 header after 2 rounds"
grep -q "^compass: ---- STEP 23/23: FLAT at the reference; at the LONG BUZZ turn 90 deg CLOCKWISE QUICKLY, then hold still$" "$K" || fail "last step"
grep -q "^compass:      move the phone now (0 s)$" "$K" || fail "move line"
[ "$(grep -c "^compass:      move the phone now (3 s)$" "$K")" -eq 2 ] || fail "move time after face down"
# movement steps say DO, still ones HOLD
grep -q "^compass:      DO 0.5 s: same way: SLOWLY tilt up to UPRIGHT (screen to you) and back, twice$" "$K" || fail "do line"
# the live pose against its target during the tilt steps (fake phone:
# pitch 2.9, roll -5.8)
grep -q "^compass:      pitch 3, aim 90 (upright) and back to 0$" "$K" || fail "tilt.up aim line: $(grep aim "$K")"
grep -q "^compass:      pitch 3, aim 60$" "$K" || fail "tilt.bar aim line"
grep -q "^compass:      roll -6, aim +-30 left and right; pitch 3, aim 90 (upright)$" "$K" || fail "tilt.roll aim line"
grep -q "^compass: ---- STEP [0-9]*/[0-9]*: same way: hold it UPRIGHT in front of you like a steering wheel and tilt it left/right about 30 deg$" "$K" \
	|| fail "roll wording"
[ "$(grep -c 'aim' "$K")" -eq 3 ] || fail "aim lines outside the tilt steps: $(grep aim "$K")"
for t in "same way: hold it UPRIGHT in front of you" "same way: TAP and SHAKE" "FLAT at the reference; at the LONG BUZZ"; do
	grep -q "^compass:      DO [0-9.]* s: $t" "$K" || fail "DO line for: $t"
	grep -q "^compass:      HOLD [0-9.]* s: $t" "$K" && fail "HOLD line for a movement step: $t"
done
grep -q "^compass:      HOLD 0.6 s: FLAT, top edge at your REFERENCE mark, still$" "$K" || fail "hold line"
grep -q "^compass:      HOLD 0.3 s: same way: HANDLEBAR POSE" "$K" || fail "tilt.bar hold line"
grep -q "^compass: DONE, thank you$" "$K" || fail "done line"
[ "$(cat "$T/vib-nocal")" = 120 ] || fail "vibrator not driven"
[ "$(grep -c ' STEP ' "$D/steps.txt")" -eq 23 ] || fail "steps: $(grep -c ' STEP ' "$D/steps.txt")"
for id in cal1.up cal2.right turn.0 turn.90 turn.180 turn.270 turn.360 tilt.flat tilt.up tilt.bar tilt.roll shake quick; do
	grep -q "^[0-9.]* STEP $id: .* ([0-9.]* s)$" "$D/steps.txt" || fail "step $id missing"
done
grep -q "^# clock uptime [0-9.]* sample_t [0-9]*$" "$D/steps.txt" || fail "clock line"
grep -q "^# cal not calibrated after 12 holds (2 rounds) at [0-9.]*$" "$D/steps.txt" || fail "cal line: $(grep '^# cal' "$D/steps.txt")"
grep -q '^{"sensor":"heading","t":[0-9]*,"heading":[0-9.]*,"pitch":[-0-9.]*,"roll":[-0-9.]*,"calibrated":false,"disturbed":false,"accuracy":180.0,"cal_source":"\(registry\|live\)","mag_bias":0.000}$' "$D/heading.jsonl" \
	|| fail "heading capture: $(head -n 2 "$D/heading.jsonl")"
grep -q '"heading":90.00' "$D/rotvec.jsonl" || fail "rotvec capture: $(head -n 2 "$D/rotvec.jsonl")"
for f in accel anglvel magn; do
	[ "$(grep -c "^{\"sensor\":\"$f\"" "$D/$f.jsonl")" -gt 0 ] || fail "$f capture"
done
grep -q '"compass":"on"' "$D/status.jsonl" || fail "status poll"
grep -q '"rotvec":"on"' "$D/status-rotvec-end.json" || fail "rotvec helper status"
grep -q "rotvec enabled: instance" "$D/sensord-rotvec.log" || fail "rotvec helper log"
wait_for "$T/smgr.log" "ROTVEC DISABLE instance 7$" 30 || fail "rotvec not disabled at the end"
ls "$D"/*.sock >/dev/null 2>&1 && fail "helper socket left"
pgrep -f "sensord-fake -R -i 40 -V -S $D" >/dev/null && fail "helper left running"
no_claims nocal
python3 ../compass-check.py "$D" --ref 90 >"$T/check.txt" 2>&1 || fail "compass-check: $(tail -n 5 "$T/check.txt")"
grep -q "heading from live" "$T/check.txt" || fail "compass-check source: $(head -n 3 "$T/check.txt")"
# never calibrated: every scored criterion is NOT CALIBRATED, whatever
# its numbers (the absolute error would be within 10 deg here)
grep -q "   1 turns    NOT CALIBRATED: holds .*(only 0% of the scored heading lines calibrated)$" "$T/check.txt" \
	|| fail "compass-check turns not flagged: $(grep turns "$T/check.txt")"
grep -q "   5 absolute NOT CALIBRATED: heading 8[78]\.[0-9] vs ref 90.0, error .*(only 0% of the scored heading lines calibrated)$" "$T/check.txt" \
	|| fail "compass-check absolute not flagged: $(grep absolute "$T/check.txt")"
grep -q "PASS" "$T/check.txt" && fail "compass-check passed something on uncalibrated data: $(grep PASS "$T/check.txt")"
grep -q "turn.0  *mean .* cal   0% dist" "$T/check.txt" || fail "compass-check per-step calibrated fraction"
grep -q "rotvec-heading" "$T/check.txt" || fail "compass-check rotvec comparison"
grep -q "Traceback" "$T/check.txt" && fail "compass-check traceback"

# ------------------------------------------- calibrates during round 1
: >"$T/vib-mid"
# shellcheck disable=SC2046
env $(script_env mid CAL_S=1) timeout 120 sh "$RUN" >"$T/run-mid.txt" 2>&1 &
RP=$!
i=0
while ! grep -q "STEP cal1.down" "$T"/out/mid/compass-*/steps.txt 2>/dev/null; do
	i=$((i + 1))
	[ "$i" -ge 300 ] && { fail "mid: never reached cal1.down"; break; }
	sleep 0.1
done
touch "$FAKE_MAG_LEARN_FILE"	# the ADSP applies its learned bias now
wait "$RP" || fail "mid run exit"
K=$T/kmsg-mid
# calibrated from the data 3 s after the bias is applied: a few 1 s holds later
grep -q "^compass: CALIBRATED after [3-6] holds; now 1[4-7] steps$" "$K" || fail "mid: calibrated message: $(grep -i calib "$K")"
D=$(ls -d "$T"/out/mid/compass-* | head -n 1)
grep -q "^# cal calibrated after [3-6] holds at [0-9.]* (source and bias: live \[0.4010,0.2290,0.2540\])$" "$D/steps.txt" || fail "mid: cal line: $(grep '^# cal' "$D/steps.txt")"
grep -q "^compass:      calibration source and bias (G): live \[0.4010,0.2290,0.2540\]$" "$K" || fail "mid: source line"
grep -q '"calibrated":true,.*"cal_source":"live","mag_bias":0.52[78]' "$D/heading.jsonl" || fail "mid: live source on heading lines"
grep -q '"calibrated":true' "$D/heading.jsonl" || fail "mid: heading never calibrated"
# calibrated before turn.0: the scored windows are calibrated
python3 ../compass-check.py "$D" --ref 90 >"$T/check-mid.txt" 2>&1 || fail "mid: compass-check"
grep -q "NOT CALIBRATED" "$T/check-mid.txt" && fail "mid: calibrated run flagged: $(grep 'NOT CAL' "$T/check-mid.txt")"
grep -q "   5 absolute PASS: heading 8[78]\.[0-9] vs ref 90.0" "$T/check-mid.txt" || fail "mid: absolute: $(grep absolute "$T/check-mid.txt")"
grep -q "^compass: ---- STEP 1[4-7]/1[4-7]: FLAT at the reference; at the LONG BUZZ" "$K" || fail "mid: renumbered last step: $(grep 'STEP' "$K" | tail -n 1)"
tail -n 1 "$T/run-mid.txt" | grep -q "calibration yes after [3-6] holds; accel" || fail "mid summary: $(tail -n 1 "$T/run-mid.txt")"
no_claims mid

# ------------------------------------------------ already calibrated
run_script cal || fail "cal run exit $?"
K=$T/kmsg-cal
grep -q "^compass: magnetometer already calibrated: no calibration holds$" "$K" || fail "already calibrated message"
grep -q "^compass: ---- STEP 1/11: FLAT, top edge at your REFERENCE mark, still$" "$K" || fail "cal: first step"
D=$(ls -d "$T"/out/cal/compass-* | head -n 1)
grep -q "^# cal already calibrated at " "$D/steps.txt" || fail "cal: cal line"
grep -q "STEP cal1" "$D/steps.txt" && fail "cal: calibration holds run anyway"
[ -e "$D/rotvec.jsonl" ] && fail "cal: rotvec captured without ROTVEC=1"
no_claims cal

# ------------------------------------------------------------ abort
: >"$T/vib-abort"
# shellcheck disable=SC2046
env $(script_env abort ROTVEC=1 TURN_S=30) sh "$RUN" >"$T/run-abort.txt" 2>&1 &
AP=$!
i=0
while ! grep -q "STEP turn.0" "$T"/out/abort/compass-*/steps.txt 2>/dev/null; do
	i=$((i + 1))
	[ "$i" -ge 300 ] && { fail "abort: never reached turn.0"; break; }
	sleep 0.1
done
sleep 0.3
kill "$AP"
i=0
while kill -0 "$AP" 2>/dev/null && [ "$i" -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
kill -0 "$AP" 2>/dev/null && fail "abort: script still running 5 s after TERM"
wait "$AP"
rc=$?
[ "$rc" -eq 130 ] || fail "abort: exit $rc, want 130"
AD=$(ls -d "$T"/out/abort/compass-* | head -n 1)
pgrep -f "$AD" >/dev/null && fail "abort: helper or capture left: $(pgrep -af "$AD")"
pgrep -f "$RUN" >/dev/null && fail "abort: script or poller left: $(pgrep -af "$RUN")"
grep -q "DONE" "$T/run-abort.txt" && fail "abort: carried on after TERM"
last=$(grep "ROTVEC ENABLE period" "$T/smgr.log" | tail -n 1 | sed 's/.*instance //')
wait_for "$T/smgr.log" "ROTVEC DISABLE instance $last$" 30 || fail "abort: rotvec instance $last not disabled"
no_claims abort
kill -0 "$SPID" 2>/dev/null || fail "main sensord died"

if [ "$FAILS" -ne 0 ]; then
	echo "--- run"; tail -n 20 "$T/run-nocal.txt"
	echo "--- smgr.log"; tail -n 20 "$T/smgr.log"
	echo "test_compass_run.sh: $FAILS failure(s)"
	exit 1
fi
echo "test_compass_run.sh: all passed"
