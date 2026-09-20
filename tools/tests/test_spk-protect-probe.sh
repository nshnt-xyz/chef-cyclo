#!/bin/sh
# Host regression test for initramfs/usr/bin/spk-protect-probe: the scripted
# live speaker-protection experiment. Runs it as a real subprocess against a
# fake /proc/asound + msm_subsys tree, a regular file standing in for
# /dev/kmsg with a stub dmesg that prints it, and stub speaker-test-tone /
# afe-debug / afe-topology-cal / tert-tx-hold on PATH that record their
# argv, stay resident until TERM (the two helpers) and append the kernel
# lines the real ones would provoke -- so the phase ordering, the helper
# lifecycle (started before the tone, TERMed after it, never left behind),
# the afe-debug on/off bracket on every exit path, the preconditions and
# the summary's per-phase extraction can all be asserted without a device.
set -eu
cd "$(dirname "$0")/../.."
SCRIPT=initramfs/usr/bin/spk-protect-probe
STT=initramfs/usr/bin/speaker-test-tone
[ -r "$SCRIPT" ] || { echo "cannot find $SCRIPT" >&2; exit 1; }

TMPROOT=$(mktemp -d /tmp/spk-protect-probe-test.XXXXXX)
trap 'rm -rf "$TMPROOT"' EXIT

pass=0
fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
eq() { if [ "$2" = "$3" ]; then ok; else bad "$1: expected '$2', got '$3'"; fi; }

# ---- constants mirror speaker-test-tone's; clamps behave identically ----
SPK_PROTECT_PROBE_SELFTEST=1 . "./$SCRIPT"
PROBE_DURATION_MAX=$DURATION_MAX
PROBE_AMPLITUDE_MAX=$AMPLITUDE_MAX_DBFS
PROBE_VOL_MAX=$VOL_MAX
PROBE_DEFAULT_DURATION=$DURATION
eq "probe clamp_duration over the cap" 5 "$(clamp_duration 10)"
eq "probe clamp_amplitude over the ceiling" -6 "$(clamp_amplitude 0)"
eq "probe clamp_amplitude under the ceiling" -30 "$(clamp_amplitude -30)"
eq "probe default duration is 3 s" 3 "$PROBE_DEFAULT_DURATION"
(
	SPEAKER_TEST_TONE_SELFTEST=1 . "./$STT"
	eq "DURATION_MAX equals speaker-test-tone's" "$DURATION_MAX" "$PROBE_DURATION_MAX"
	eq "AMPLITUDE_MAX_DBFS equals speaker-test-tone's (-6 dBFS ceiling)" "$AMPLITUDE_MAX_DBFS" "$PROBE_AMPLITUDE_MAX"
	eq "VOL_MAX equals speaker-test-tone's (15)" "$VOL_MAX" "$PROBE_VOL_MAX"
	eq "speaker-test-tone's default amplitude is still -20 dBFS (the probe passes no -a by default)" -20 "$AMPLITUDE_DBFS"
	[ "$fail" -eq 0 ]
) || fail=$((fail + 1))
# The subshell's pass count is lost; count its four checks here on success.
pass=$((pass + 4))

body=$(sed -n '/^card_ready() {/,/^}/p' "$SCRIPT" | grep -v '^[[:space:]]*#')
case "$body" in
*'[ -s'*|*'test -s'*) bad "card_ready() in $SCRIPT must not use -s (procfs size is 0)" ;;
*) ok ;;
esac

# ---- stubs ----
STUBDIR="$TMPROOT/stubbin"
mkdir -p "$STUBDIR"

# speaker-test-tone stub: records argv; on a tone run appends the kernel
# lines the real kernel logs for the TAS sequence into the fake kmsg --
# with SET_TOPOLOGY only when the afe-topology-cal stub is resident (its
# state file exists) -- and prints the tinyplay result line.
cat > "$STUBDIR/speaker-test-tone" <<'STUB'
#!/bin/sh
echo "speaker-test-tone $*" >> "$ARGV_LOG"
if [ "${1:-}" = -s ]; then
	echo "TAS2560_ALGO_FF_MODULE: DISABLE"
	exit 0
fi
phase_no=$(grep -c '^speaker-test-tone -p' "$ARGV_LOG")
if [ -e "$STATE_DIR/topology" ]; then
	echo "afe_send_port_topology_id: AFE set topology id 0x112fc  enable for port 0x1004 ret 0" >> "$KMSG"
else
	echo "afe_get_cal_topology_id: [AFE_TOPOLOGY_CAL] not initialized for this port 4100" >> "$KMSG"
fi
echo "afe_apr_send_pkt: DSP returned error[ADSP_EBADPARAM]" >> "$KMSG"
echo "afe_callback_debug_print: code = 0x10106 PL#0[0x0], PL#1[0x1000fc00], size = 24" >> "$KMSG"
echo "TAS2560_ALGO:tas2560_algo_get_ff_module Recieving Rx-Enable data 0" >> "$KMSG"
echo "speaker-test-tone: protection: set rc=0, read back: TAS2560_ALGO_FF_MODULE: DISABLE" >&2
if [ "${STUB_TONE_FAIL_PHASE:-0}" = "$phase_no" ]; then
	echo "speaker-test-tone: tinyplay exited with an error" >&2
	exit 1
fi
echo "speaker-test-tone: tinyplay finished" >&2
exit 0
STUB

cat > "$STUBDIR/afe-debug" <<'STUB'
#!/bin/sh
echo "afe-debug $*" >> "$ARGV_LOG"
case "$1" in
on) [ "${STUB_AFE_DEBUG_ON_FAIL:-0}" = 1 ] && { echo "afe-debug: no dynamic-debug site for function x" >&2; exit 1; } ;;
status) echo "afe_callback_debug_print: =p" ;;
esac
exit 0
STUB

# Resident helpers: print their ready line (unless told to fail), record
# a pid file, and exit 0 on TERM after removing their state file -- like
# the real ones release what they hold.
cat > "$STUBDIR/afe-topology-cal" <<'STUB'
#!/bin/sh
echo "afe-topology-cal${*:+ $*}" >> "$ARGV_LOG"
if [ "${STUB_TOPO_FAIL:-0}" = 1 ]; then
	echo "afe-topology-cal: AUDIO_SET_CALIBRATION (RX topology 0x000112fc) failed: Invalid argument" >&2
	exit 5
fi
echo "afe-topology-cal: installed RX topology 0x000112fc acdb_id 14 rate 48000 (cal_type 23, path 0, no shared memory)"
: > "$STATE_DIR/topology"
echo $$ > "$STATE_DIR/topology.pid"
trap 'rm -f "$STATE_DIR/topology"; echo "afe-topology-cal: released on signal 15"; exit 0' TERM
echo "afe-topology-cal: resident, holding /dev/msm_audio_cal open; SIGTERM/SIGINT releases the block"
while :; do sleep 0.05; done
STUB

cat > "$STUBDIR/tert-tx-hold" <<'STUB'
#!/bin/sh
echo "tert-tx-hold $*" >> "$ARGV_LOG"
if [ "${STUB_TX_FAIL:-0}" = 1 ]; then
	echo "tert-tx-hold: START failed: Broken pipe" >&2
	exit 7
fi
echo "tert-tx-hold: 'TERT MI2S_TX Hostless' is card 0 device 40 (/dev/snd/pcmC0D40c)"
echo "__afe_port_start: port id: 0x1005" >> "$KMSG"
echo "afe_get_cal_topology_id: [AFE_TOPOLOGY_CAL] not initialized for this port 4101" >> "$KMSG"
: > "$STATE_DIR/tx"
echo $$ > "$STATE_DIR/tx.pid"
trap 'rm -f "$STATE_DIR/tx"; echo "tert-tx-hold: DROP ok"; echo "tert-tx-hold: released on signal 15"; exit 0' TERM
echo "tert-tx-hold: holding /dev/snd/pcmC0D40c RUNNING (TERT_MI2S_TX back-end up); SIGTERM/SIGINT drops and closes it"
while :; do sleep 0.05; done
STUB

# Passed to the probe as an absolute DMESG path: a busybox sh built with
# FEATURE_SH_STANDALONE (Ubuntu's) would otherwise run its own dmesg applet
# instead of a stub found on PATH.
cat > "$STUBDIR/dmesg" <<'STUB'
#!/bin/sh
cat "$KMSG"
STUB
chmod +x "$STUBDIR"/*

setup_fixture() {
	FIX=$(mktemp -d "$TMPROOT/fixture.XXXXXX")
	mkdir -p "$FIX/asound" "$FIX/run" "$FIX/subsys/subsys0" "$FIX/subsys/subsys1" "$FIX/state"
	printf ' 0 [sdm660snd      ]: sdm660-asoc-s - sdm660-snd-card\n' > "$FIX/asound/cards"
	printf '00-00: MultiMedia1 (*) :  : playback 1 : capture 1\n00-40: TERT MI2S_TX Hostless (*) :  : capture 1\n' > "$FIX/asound/pcm"
	printf 'modem\n' > "$FIX/subsys/subsys0/name"
	printf 'adsp\n' > "$FIX/subsys/subsys1/name"
	printf 'related\n' > "$FIX/subsys/subsys1/restart_level"
	: > "$FIX/kmsg"
	: > "$FIX/argv"
	echo "$FIX"
}

run_probe() {
	# run_probe <args...>: stdout -> $FIX/out, stderr -> $FIX/err
	ASOUND_DIR="$FIX/asound" SUBSYS_DIR="$FIX/subsys" RUN_DIR="$FIX/run" \
		KMSG="$FIX/kmsg" DMESG="$STUBDIR/dmesg" ARGV_LOG="$FIX/argv" STATE_DIR="$FIX/state" \
		HELPER_POLL_DELAY=0.02 HELPER_POLL_ATTEMPTS=50 SETTLE_DELAY=0 \
		PATH="$STUBDIR:$PATH" \
		sh "$SCRIPT" "$@" > "$FIX/out" 2> "$FIX/err"
}

no_helper_left() {
	# Both stub helpers must be gone (pid files written by the stubs).
	for p in "$FIX/state/topology.pid" "$FIX/state/tx.pid"; do
		[ -r "$p" ] || continue
		if kill -0 "$(cat "$p")" 2>/dev/null; then
			kill -9 "$(cat "$p")" 2>/dev/null
			return 1
		fi
	done
	return 0
}

evidence_dir() {
	find "$FIX/run" -mindepth 1 -maxdepth 1 -type d -name 'spk-protect-probe-*' | head -n 1
}

# ================= success path =================
FIX=$(setup_fixture)
if run_probe; then ok; else bad "default run must exit 0: $(cat "$FIX/err")"; fi
OUT=$(evidence_dir)
if [ -n "$OUT" ]; then ok; else bad "an evidence dir must be created under RUN_DIR"; fi
if no_helper_left; then ok; else bad "helpers must not be left running after a clean run"; fi

# afe-debug bracket, status snapshots and phase order in the argv log.
want_argv="speaker-test-tone -s
afe-debug on
afe-debug status
speaker-test-tone -p -d 3
speaker-test-tone -s
tert-tx-hold -D 0
speaker-test-tone -p -d 3
speaker-test-tone -s
afe-topology-cal
tert-tx-hold -D 0
speaker-test-tone -p -d 3
speaker-test-tone -s
speaker-test-tone -p -d 3
speaker-test-tone -s
speaker-test-tone -s
afe-debug off"
eq "argv sequence: status, debug on, 4 phases (baseline, tx, topo+tx, restored), status, debug off" \
	"$want_argv" "$(cat "$FIX/argv")"

# Kernel windows: each phase's file holds exactly its own lines.
eq "phase files exist" "kmsg-baseline.log kmsg-restored.log kmsg-tx-topo.log kmsg-tx.log" \
	"$(cd "$OUT" && ls kmsg-*.log | tr '\n' ' ' | sed 's/ $//')"
if grep -q 'port id: 0x1005' "$OUT/kmsg-baseline.log"; then bad "baseline window must not contain the TX port start"; else ok; fi
if grep -q 'port id: 0x1005' "$OUT/kmsg-tx.log"; then ok; else bad "tx window must contain the TX port start (helper started inside the window)"; fi
if grep -q 'set topology id' "$OUT/kmsg-tx.log"; then bad "tx window must not show SET_TOPOLOGY (no topology helper)"; else ok; fi
if grep -q 'set topology id 0x112fc' "$OUT/kmsg-tx-topo.log" && grep -q 'port id: 0x1005' "$OUT/kmsg-tx-topo.log"; then ok; else bad "tx-topo window must show both SET_TOPOLOGY and the TX port start"; fi
if grep -q 'set topology id\|port id: 0x1005' "$OUT/kmsg-restored.log"; then bad "restored window must show neither helper's effect (reversibility)"; else ok; fi
if grep -q 'not initialized for this port 4100' "$OUT/kmsg-restored.log"; then ok; else bad "restored window must show the topology lookup failing again"; fi
eq "each window starts with its BEGIN marker" "spk-protect-probe: BEGIN tx-topo" "$(head -n 1 "$OUT/kmsg-tx-topo.log")"
eq "each window ends with its END marker" "spk-protect-probe: END tx-topo" "$(tail -n 1 "$OUT/kmsg-tx-topo.log")"

# Helper lifecycle: started before the tone, released after it, inside the window.
if grep -q 'released on signal 15' "$OUT/tert-tx-hold-tx.log" && grep -q 'released on signal 15' "$OUT/afe-topology-cal-tx-topo.log"; then ok; else bad "helpers must be TERMed and report release"; fi
eq "phases.txt records the helper states" \
	"baseline tone_rc=0 topology=none tx_hold=none
tx tone_rc=0 topology=none tx_hold=ready
tx-topo tone_rc=0 topology=ready tx_hold=ready
restored tone_rc=0 topology=none tx_hold=none" "$(cat "$OUT/phases.txt")"

# Summary content.
SUMMARY="$OUT/summary.txt"
if [ -r "$SUMMARY" ]; then ok; else bad "summary.txt must be written"; fi
eq "summary is also printed to stdout" "$(cat "$SUMMARY")" "$(cat "$FIX/out")"
if grep -q '^--- phase baseline: tone_rc=0' "$SUMMARY" && grep -q '^--- phase tx-topo: tone_rc=0 topology=ready tx_hold=ready' "$SUMMARY"; then ok; else bad "summary must head each phase with its states"; fi
tx_topo_section=$(sed -n '/^--- phase tx-topo/,/^--- phase restored/p' "$SUMMARY")
case "$tx_topo_section" in
*"AFE set topology id 0x112fc  enable for port 0x1004 ret 0"*) ok ;;
*) bad "tx-topo summary must show the SET_TOPOLOGY ack line" ;;
esac
case "$tx_topo_section" in
*"port id: 0x1005"*) ok ;;
*) bad "tx-topo summary must show the TX port start" ;;
esac
case "$tx_topo_section" in
*"DSP returned error[ADSP_EBADPARAM]"*) ok ;;
*) bad "summary must list every DSP returned error line" ;;
esac
case "$tx_topo_section" in
*"PL#0[0x0], PL#1[0x1000fc00]"*) ok ;;
*) bad "summary must show the TAS get-param response with its PL#0 status word" ;;
esac
case "$tx_topo_section" in
*"Recieving Rx-Enable data 0"*"read back: TAS2560_ALGO_FF_MODULE: DISABLE"*) ok ;;
*) bad "summary must show the kernel and tinymix FF readbacks" ;;
esac
case "$tx_topo_section" in
*"playback: tinyplay finished"*) ok ;;
*) bad "summary must say whether playback completed" ;;
esac
baseline_section=$(sed -n '/^--- phase baseline/,/^--- phase tx:/p' "$SUMMARY")
case "$baseline_section" in
*"(no port 0x1005 lines)"*) ok ;;
*) bad "baseline summary must say no TX port lines were seen" ;;
esac
case "$baseline_section" in
*"not initialized for this port 4100"*) ok ;;
*) bad "baseline summary must show the failed topology lookup" ;;
esac
for f in status-before.txt status-after.txt status-baseline.txt asound-tx.txt restart_level.txt afe-debug-on.log afe-debug-off.log afe-debug-status.txt tone-tx-topo.log; do
	if [ -r "$OUT/$f" ]; then ok; else bad "evidence dir must contain $f"; fi
done
eq "restart_level is recorded" related "$(cat "$OUT/restart_level.txt")"

# ================= flags: pass-through with clamps =================
FIX=$(setup_fixture)
if run_probe -d 10 -a -3 -v 15 -r 112FC; then ok; else bad "flag run must exit 0: $(cat "$FIX/err")"; fi
eq "-d clamps to 5, -a clamps to -6, -v 15 passes, in speaker-test-tone's flag shape" \
	"speaker-test-tone -p -d 5 -a -6 -v 15" "$(grep '^speaker-test-tone -p' "$FIX/argv" | head -n 1)"
eq "-r is handed to afe-topology-cal" "afe-topology-cal -r 112FC" "$(grep '^afe-topology-cal' "$FIX/argv")"
case "$(cat "$FIX/err")" in
*"clamping -d 10 to 5"*"clamping -a -3 to -6 dBFS"*) ok ;;
*) bad "clamps must be logged, got: $(cat "$FIX/err")" ;;
esac
no_helper_left || bad "helpers left after flag run"

FIX=$(setup_fixture)
if run_probe -v 16; then bad "-v 16 must be refused"; else ok; fi
eq "-v 16 refused before anything runs" "" "$(cat "$FIX/argv")"
if run_probe -d 0; then bad "-d 0 must be refused"; else ok; fi
if run_probe -a loud; then bad "-a loud must be refused"; else ok; fi
if run_probe -r zz; then bad "-r zz must be refused"; else ok; fi
if run_probe extra; then bad "a positional argument must be refused"; else ok; fi
eq "refused flags never reach the helpers" "" "$(cat "$FIX/argv")"

# ================= preconditions =================
FIX=$(setup_fixture)
: > "$FIX/asound/cards"
if run_probe; then bad "must refuse without the sdm660 card"; else ok; fi
eq "no card: nothing runs" "" "$(cat "$FIX/argv")"

FIX=$(setup_fixture)
printf 'system\n' > "$FIX/subsys/subsys1/restart_level"
if run_probe; then bad "must refuse when adsp restart_level is not related"; else ok; fi
case "$(cat "$FIX/err")" in
*"restart_level is 'system', not 'related'"*) ok ;;
*) bad "restart_level refusal must be explained, got: $(cat "$FIX/err")" ;;
esac
eq "wrong restart_level: nothing runs (no afe-debug on)" "" "$(cat "$FIX/argv")"

FIX=$(setup_fixture)
rm "$FIX/subsys/subsys1/name"
if run_probe; then bad "must refuse when the adsp subsystem is missing"; else ok; fi

FIX=$(setup_fixture)
if STUB_AFE_DEBUG_ON_FAIL=1 run_probe; then bad "must abort when afe-debug on fails"; else ok; fi
eq "afe-debug on failure: off is still issued, no tone played" \
	"speaker-test-tone -s
afe-debug on
afe-debug off" "$(cat "$FIX/argv")"

# ================= tone failure mid-run: later phases still run, cleanup holds =================
FIX=$(setup_fixture)
if STUB_TONE_FAIL_PHASE=2 run_probe; then ok; else bad "a failing tone in phase 2 must not fail the probe"; fi
OUT=$(evidence_dir)
eq "phase 2 tone failure recorded, phases 3-4 still ran" \
	"baseline tone_rc=0 topology=none tx_hold=none
tx tone_rc=1 topology=none tx_hold=ready
tx-topo tone_rc=0 topology=ready tx_hold=ready
restored tone_rc=0 topology=none tx_hold=none" "$(cat "$OUT/phases.txt")"
eq "afe-debug off is the last action" "afe-debug off" "$(tail -n 1 "$FIX/argv")"
case "$(sed -n '/^--- phase tx:/,/^--- phase tx-topo/p' "$OUT/summary.txt")" in
*"playback: tinyplay exited with an error"*) ok ;;
*) bad "summary must report the failed playback" ;;
esac
no_helper_left || bad "helpers left after tone failure run"

# ================= helper failures: recorded, tone still played, cleanup holds =================
FIX=$(setup_fixture)
if STUB_TOPO_FAIL=1 run_probe; then ok; else bad "a failing afe-topology-cal must not fail the probe"; fi
OUT=$(evidence_dir)
eq "topology helper failure recorded, tone still played" "tx-topo tone_rc=0 topology=failed tx_hold=ready" "$(grep '^tx-topo' "$OUT/phases.txt")"
case "$(cat "$FIX/err")" in
*"afe-topology-cal-tx-topo exited (rc=5) before reporting ready"*) ok ;;
*) bad "helper exit before ready must be logged with its rc, got: $(cat "$FIX/err")" ;;
esac
no_helper_left || bad "helpers left after topology failure run"

FIX=$(setup_fixture)
if STUB_TX_FAIL=1 run_probe; then ok; else bad "a failing tert-tx-hold must not fail the probe"; fi
OUT=$(evidence_dir)
eq "tx helper failure recorded in both tx phases" "tx tone_rc=0 topology=none tx_hold=failed
tx-topo tone_rc=0 topology=ready tx_hold=failed" "$(grep '^tx' "$OUT/phases.txt")"
no_helper_left || bad "helpers left after tx failure run"

# ================= interrupted mid-run: helpers killed, afe-debug off =================
FIX=$(setup_fixture)
(
	ASOUND_DIR="$FIX/asound" SUBSYS_DIR="$FIX/subsys" RUN_DIR="$FIX/run" \
		KMSG="$FIX/kmsg" DMESG="$STUBDIR/dmesg" ARGV_LOG="$FIX/argv" STATE_DIR="$FIX/state" \
		HELPER_POLL_DELAY=0.02 HELPER_POLL_ATTEMPTS=50 SETTLE_DELAY=5 \
		PATH="$STUBDIR:$PATH" \
		exec sh "$SCRIPT" > "$FIX/out" 2> "$FIX/err"
) &
PROBE_PID=$!
# Wait until the phase-2 TX holder is up (its pid file), then interrupt.
i=0
while [ ! -r "$FIX/state/tx.pid" ] && [ "$i" -lt 100 ]; do sleep 0.05; i=$((i + 1)); done
if [ -r "$FIX/state/tx.pid" ]; then ok; else bad "tx holder never came up for the interrupt test"; fi
kill -TERM "$PROBE_PID" 2>/dev/null
wait "$PROBE_PID" 2>/dev/null || true
sleep 0.2
if no_helper_left; then ok; else bad "TERM must kill the resident helpers"; fi
eq "TERM mid-run still turns afe-debug off" "afe-debug off" "$(tail -n 1 "$FIX/argv")"
case "$(cat "$FIX/err")" in
*"evidence in $FIX/run/spk-protect-probe-"*) ok ;;
*) bad "interrupted run must name its evidence dir" ;;
esac

echo "PASS: $pass/$((pass + fail)) checks passed"
[ "$fail" -eq 0 ]
