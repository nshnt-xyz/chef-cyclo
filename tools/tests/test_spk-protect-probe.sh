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
# The probe exports the card it derived; record it so the test can check
# that speaker-test-tone is told the same card as tert-tx-hold -D.
echo "${CARD-unset}" > "$STATE_DIR/stt-card"
if [ "${1:-}" = -s ]; then
	echo "TAS2560_ALGO_FF_MODULE: DISABLE"
	exit 0
fi
phase_no=$(grep -c '^speaker-test-tone -p' "$ARGV_LOG")
if [ -e "$STATE_DIR/topology" ]; then
	echo "afe_send_port_topology_id: AFE set topology id 0x112fc  enable for port 0x1004 ret 0" >> "$KMSG"
elif [ -e "$STATE_DIR/neutralised" ]; then
	# What the kernel really prints once buffer 0 carries topology 0:
	# afe_get_cal_topology_id() rejects it and afe_send_port_topology_id()
	# returns early without sending anything.
	echo "afe_get_cal_topology_id: invalid topology id : [14, 0]" >> "$KMSG"
	echo "afe_send_port_topology_id: AFE set topology id 0x0  enable for port 0x1004 ret -22" >> "$KMSG"
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
# -N is the one-shot neutralise: write topology 0, log what the kernel would
# then print at the next port start, and EXIT (never resident -- closing the
# device frees nothing, so there is nothing to hold up). Scan the whole argv:
# the probe passes -S "" first so that only it writes the sentinel (G2).
stub_neutralise=0
for a in "$@"; do [ "$a" = -N ] && stub_neutralise=1; done
if [ "$stub_neutralise" = 1 ]; then
	if [ "${STUB_NEUTRALISE_FAIL:-0}" = 1 ]; then
		echo "afe-topology-cal: AUDIO_SET_CALIBRATION (RX topology 0x00000000) failed: Invalid argument" >&2
		exit 5
	fi
	rm -f "$STATE_DIR/topology"
	: > "$STATE_DIR/neutralised"
	echo "afe-topology-cal: neutralised the RX block: buffer 0 now carries topology 0x00000000 acdb_id 14 rate 48000"
	echo "afe-topology-cal: neutralised and exiting; the next RX port start will send no SET_TOPOLOGY."
	exit 0
fi
if [ "${STUB_TOPO_HANG:-0}" = 1 ]; then
	# Never reports ready: models an installer that may or may not have
	# completed its irreversible SET before we lost sight of it.
	while :; do sleep 0.05; done
fi
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
# STUB_DMESG_DROP stands in for a kernel ring buffer that lost part of a
# phase's window (wrapped, or the marker write never landed): every line
# containing that fixed string is withheld from the probe's dmesg.
cat > "$STUBDIR/dmesg" <<'STUB'
#!/bin/sh
if [ -n "${STUB_DMESG_DROP:-}" ]; then
	grep -Fv "$STUB_DMESG_DROP" "$KMSG"
else
	cat "$KMSG"
fi
STUB
chmod +x "$STUBDIR"/*

setup_fixture() {
	FIX=$(mktemp -d "$TMPROOT/fixture.XXXXXX")
	mkdir -p "$FIX/asound" "$FIX/run" "$FIX/subsys/subsys0" "$FIX/subsys/subsys1" "$FIX/state"
	# setup_fixture [CARD]: the sdm660 card number the probe must derive
	# from /proc/asound/cards rather than assume (default 0). The Loopback
	# line before it is what makes "the first card" the wrong answer.
	fix_card=${1:-0}
	printf ' 9 [Loopback       ]: Loopback - Loopback\n' > "$FIX/asound/cards"
	printf '%2s [sdm660snd      ]: sdm660-asoc-s - sdm660-snd-card\n' "$fix_card" >> "$FIX/asound/cards"
	printf '%02d-00: MultiMedia1 (*) :  : playback 1 : capture 1\n%02d-40: TERT MI2S_TX Hostless (*) :  : capture 1\n' \
		"$fix_card" "$fix_card" > "$FIX/asound/pcm"
	printf 'modem\n' > "$FIX/subsys/subsys0/name"
	printf 'adsp\n' > "$FIX/subsys/subsys1/name"
	# The kernel prints restart_levels[] verbatim and that table is
	# UPPERCASE (subsystem_restart.c:92), so this is what the node really
	# reads after audio-up's `echo related`. The 2026-09-22 live attempt
	# was blocked because the fixture said 'related' and the phone said
	# 'RELATED'; fixtures track the kernel, not the write side.
	printf 'RELATED\n' > "$FIX/subsys/subsys1/restart_level"
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

evidence_dirs() {
	find "$FIX/run" -mindepth 1 -maxdepth 1 -type d -name 'spk-protect-probe-*' | sort
}

run_marker() {
	# run_marker EVIDENCE_DIR: the kernel marker prefix that run wrote,
	# rebuilt from the run id in its directory name.
	printf 'spk-protect-probe[%s]' "$(basename "$1" | sed 's/^spk-protect-probe-//')"
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
afe-topology-cal -S 
tert-tx-hold -D 0
speaker-test-tone -p -d 3
speaker-test-tone -s
afe-topology-cal -S  -N
speaker-test-tone -p -d 3
speaker-test-tone -s
speaker-test-tone -s
afe-debug off"
eq "argv sequence: status, debug on, 4 phases (baseline, tx, topo+tx, explicit -N neutralise), status, debug off" \
	"$want_argv" "$(cat "$FIX/argv")"

# Kernel windows: each phase's file holds exactly its own lines.
eq "phase files exist (four; the last is neutralised, never 'restored')" "kmsg-baseline.log kmsg-neutralised.log kmsg-tx-topo.log kmsg-tx.log" \
	"$(cd "$OUT" && ls kmsg-*.log | tr '\n' ' ' | sed 's/ $//')"
if grep -q 'port id: 0x1005' "$OUT/kmsg-baseline.log"; then bad "baseline window must not contain the TX port start"; else ok; fi
if grep -q 'port id: 0x1005' "$OUT/kmsg-tx.log"; then ok; else bad "tx window must contain the TX port start (helper started inside the window)"; fi
if grep -q 'set topology id' "$OUT/kmsg-tx.log"; then bad "tx window must not show SET_TOPOLOGY (no topology helper)"; else ok; fi
if grep -q 'set topology id 0x112fc' "$OUT/kmsg-tx-topo.log" && grep -q 'port id: 0x1005' "$OUT/kmsg-tx-topo.log"; then ok; else bad "tx-topo window must show both SET_TOPOLOGY and the TX port start"; fi
# There must be NO phase after the topology one: the block cannot be freed
# from userspace (AFE_TOPOLOGY_CAL_TYPE has dealloc=NULL), so a later phase
# would run against the installed topology and could never be a baseline.
# The 2026-09-22 live run proved the old "restored" phase was still getting
# SET_TOPOLOGY sent with the helper dead.
# The phase after the topology install must be an EXPLICIT neutralisation,
# never a "restored" phase that relies on close. Closing /dev/msm_audio_cal
# frees nothing for AFE_TOPOLOGY_CAL_TYPE (dealloc=NULL), which the
# 2026-09-22 live run proved by still logging SET_TOPOLOGY with the helper
# dead. Neutralising means re-SETting buffer 0 with topology 0.
if [ -e "$OUT/kmsg-restored.log" ]; then bad "there must be no 'restored' phase"; else ok; fi
if grep -qE '^run_phase[[:space:]]+restored' "$SCRIPT"; then bad "the script must not RUN a 'restored' phase"; else ok; fi
if grep -qE '^PHASES=.*restored' "$SCRIPT"; then bad "'restored' must not be in PHASES"; else ok; fi
eq "neutralised is the last phase in phases.txt" "neutralised" "$(tail -n 1 "$OUT/phases.txt" | cut -d ' ' -f 1)"
eq "neutralised is the last phase in the summary" "neutralised" "$(grep '^--- phase ' "$OUT/summary.txt" | tail -n 1 | sed 's/^--- phase \([^:]*\):.*/\1/')"
eq "neutralised is last in PHASES" "neutralised" "$(sed -n 's/^PHASES="\(.*\)"$/\1/p' "$SCRIPT" | awk '{print $NF}')"
eq "the topology install is immediately followed by the neutralise phase" "run_phase neutralised 0 neutralise" \
	"$(grep -E '^run_phase ' "$SCRIPT" | tail -n 1)"
eq "the topology install is the second-to-last run_phase" "run_phase tx-topo 1 1" \
	"$(grep -E '^run_phase ' "$SCRIPT" | tail -n 2 | head -n 1)"
# The neutralise helper is a one-shot: it must be invoked with -N and must
# not be left resident (nothing to hold -- closing frees nothing).
if grep -q '^afe-topology-cal -S  -N$' "$FIX/argv"; then ok; else bad "the neutralise phase must call afe-topology-cal -N"; fi
# G2: the probe is the single sentinel writer for a probe run -- the helper
# must be told -S "" so it cannot write a different path (its built-in
# default is the fixed /run path; ours follows RUN_DIR).
eq "every helper invocation is given -S \"\" so only the probe writes the sentinel" 0 \
	"$(grep -c '^afe-topology-cal\( -r\| -N\|$\)' "$FIX/argv")"
eq "both helper invocations carry -S" 2 "$(grep -c '^afe-topology-cal -S ' "$FIX/argv")"
if [ -e "$FIX/state/neutralised" ]; then ok; else bad "the -N call must have taken effect"; fi
# The neutralised phase's own kernel window is the proof: topology 0 is
# rejected by afe_get_cal_topology_id, so no SET_TOPOLOGY goes out.
if grep -q 'topology id 0x0  enable for port 0x1004 ret -22' "$OUT/kmsg-neutralised.log"; then ok; else bad "the neutralised window must show the topology being rejected"; fi
if grep -q 'set topology id 0x112fc' "$OUT/kmsg-neutralised.log"; then bad "the neutralised window must NOT show 0x112fc being sent"; else ok; fi
if grep -q 'invalid topology id' "$OUT/kmsg-neutralised.log"; then ok; else bad "the neutralised window must show the kernel rejecting the zeroed block"; fi
# The summary must state, in so many words, that the topology survives.
if grep -q 'NEUTRALISED at the end of this run' "$OUT/summary.txt"; then ok; else bad "the summary must say the topology was neutralised"; fi
if grep -q 'REBOOT remains the only' "$OUT/summary.txt"; then ok; else bad "the summary must still name reboot as the only VERIFIED reset"; fi
if grep -qi 'dealloc=NULL' "$OUT/summary.txt"; then ok; else bad "the summary must cite why the block cannot be deleted"; fi
if grep -q 'still exists and can never be deleted' "$OUT/summary.txt"; then ok; else bad "the summary must not present neutralisation as a delete"; fi
if grep -q 'not a baseline\|NOT a baseline' "$OUT/summary.txt"; then ok; else bad "the summary must refuse to call the neutralised phase a baseline"; fi
MARKER1=$(run_marker "$OUT")
eq "each window starts with its BEGIN marker, carrying this run's id" "$MARKER1 BEGIN tx-topo" "$(head -n 1 "$OUT/kmsg-tx-topo.log")"
eq "each window ends with its END marker, carrying this run's id" "$MARKER1 END tx-topo" "$(tail -n 1 "$OUT/kmsg-tx-topo.log")"
case "$MARKER1" in
'spk-protect-probe['*']') ok ;;
*) bad "the run marker must be spk-protect-probe[<run id>], got '$MARKER1'" ;;
esac

# Helper lifecycle: started before the tone, released after it, inside the window.
if grep -q 'released on signal 15' "$OUT/tert-tx-hold-tx.log" && grep -q 'released on signal 15' "$OUT/afe-topology-cal-tx-topo.log"; then ok; else bad "helpers must be TERMed and report release"; fi
eq "phases.txt records the helper states and the kernel window status" \
	"baseline tone_rc=0 topology=none tx_hold=none window=ok
tx tone_rc=0 topology=none tx_hold=ready window=ok
tx-topo tone_rc=0 topology=ready tx_hold=ready window=ok
neutralised tone_rc=0 topology=neutralised tx_hold=none window=ok" "$(cat "$OUT/phases.txt")"
if grep -q 'WARNING: kernel window' "$OUT/summary.txt"; then bad "a complete run must not warn about its kernel windows"; else ok; fi

# Summary content.
SUMMARY="$OUT/summary.txt"
if [ -r "$SUMMARY" ]; then ok; else bad "summary.txt must be written"; fi
eq "summary is also printed to stdout" "$(cat "$SUMMARY")" "$(cat "$FIX/out")"
if grep -q '^--- phase baseline: tone_rc=0' "$SUMMARY" && grep -q '^--- phase tx-topo: tone_rc=0 topology=ready tx_hold=ready window=ok' "$SUMMARY"; then ok; else bad "summary must head each phase with its states"; fi
tx_topo_section=$(sed -n '/^--- phase tx-topo/,/^--- phase neutralised/p' "$SUMMARY")
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
# F5: the two readbacks are different evidence and must not be merged --
# each is under its own labelled heading, and the kernel line must appear
# under the kernel heading rather than the userspace one.
case "$tx_topo_section" in
*"FF readback (kernel log, q6afe/tas2560):"*"FF readback + protection log (speaker-test-tone/tinymix):"*) ok ;;
*) bad "summary must label the kernel and tinymix FF readbacks separately, got: $tx_topo_section" ;;
esac
kernel_ff=$(printf '%s\n' "$tx_topo_section" | sed -n '/FF readback (kernel log/,/FF readback + protection log/p')
case "$kernel_ff" in
*"Recieving Rx-Enable data 0"*) ok ;;
*) bad "the kernel FF section must hold the kernel's Rx-Enable line" ;;
esac
case "$kernel_ff" in
*"read back: TAS2560_ALGO_FF_MODULE"*) bad "the tinymix readback must not appear under the kernel FF heading" ;;
*) ok ;;
esac
user_ff=$(printf '%s\n' "$tx_topo_section" | sed -n '/FF readback + protection log/,$p')
case "$user_ff" in
*"read back: TAS2560_ALGO_FF_MODULE: DISABLE"*) ok ;;
*) bad "the speaker-test-tone FF section must hold the tinymix readback" ;;
esac
case "$user_ff" in
*"Recieving Rx-Enable"*) bad "the kernel line must not appear under the speaker-test-tone FF heading" ;;
*) ok ;;
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
eq "restart_level is recorded verbatim, as the kernel spells it" RELATED "$(cat "$OUT/restart_level.txt")"

# ================= flags: pass-through with clamps =================
FIX=$(setup_fixture)
if run_probe -d 10 -a -3 -v 15 -r 112FC; then ok; else bad "flag run must exit 0: $(cat "$FIX/err")"; fi
eq "-d clamps to 5, -a clamps to -6, -v 15 passes, in speaker-test-tone's flag shape" \
	"speaker-test-tone -p -d 5 -a -6 -v 15" "$(grep '^speaker-test-tone -p' "$FIX/argv" | head -n 1)"
eq "-r is handed to the topology install (the -N call takes no topology)" "afe-topology-cal -S  -r 112FC" "$(grep '^afe-topology-cal' "$FIX/argv" | head -n 1)"
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
printf 'SYSTEM\n' > "$FIX/subsys/subsys1/restart_level"
if run_probe; then bad "must refuse when adsp restart_level is not RELATED"; else ok; fi
case "$(cat "$FIX/err")" in
*"restart_level reads 'SYSTEM', which is not RELATED"*) ok ;;
*) bad "restart_level refusal must quote the verbatim readback, got: $(cat "$FIX/err")" ;;
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
eq "phase 2 tone failure recorded, later phases still ran" \
	"baseline tone_rc=0 topology=none tx_hold=none window=ok
tx tone_rc=1 topology=none tx_hold=ready window=ok
tx-topo tone_rc=0 topology=ready tx_hold=ready window=ok
neutralised tone_rc=0 topology=neutralised tx_hold=none window=ok" "$(cat "$OUT/phases.txt")"
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
eq "topology helper failure recorded, tone still played" "tx-topo tone_rc=0 topology=failed tx_hold=ready window=ok" "$(grep '^tx-topo' "$OUT/phases.txt")"
case "$(cat "$FIX/err")" in
*"afe-topology-cal-tx-topo exited (rc=5) before reporting ready"*) ok ;;
*) bad "helper exit before ready must be logged with its rc, got: $(cat "$FIX/err")" ;;
esac
no_helper_left || bad "helpers left after topology failure run"

FIX=$(setup_fixture)
if STUB_TX_FAIL=1 run_probe; then ok; else bad "a failing tert-tx-hold must not fail the probe"; fi
OUT=$(evidence_dir)
eq "tx helper failure recorded in both tx phases" "tx tone_rc=0 topology=none tx_hold=failed window=ok
tx-topo tone_rc=0 topology=ready tx_hold=failed window=ok" "$(grep '^tx' "$OUT/phases.txt")"
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

# ================= F3: the card number is derived, never assumed =================
# find_card as a pure function (the script is already sourced above).
CARDS_FIX="$TMPROOT/cards"
mkdir -p "$CARDS_FIX"
saved_asound=$ASOUND_DIR
ASOUND_DIR="$CARDS_FIX"
printf ' 9 [Loopback       ]: Loopback - Loopback\n 3 [sdm660snd      ]: sdm660-asoc-s - sdm660-snd-card\n' > "$CARDS_FIX/cards"
eq "find_card returns the sdm660 card, not the first card listed" 3 "$(find_card)"
printf '10 [sdm660snd      ]: sdm660-asoc-s - sdm660-snd-card\n' > "$CARDS_FIX/cards"
eq "find_card reads a two-digit card number" 10 "$(find_card)"
printf -- '--- no soundcards ---\n' > "$CARDS_FIX/cards"
eq "find_card is empty when no sdm660 card is listed" "" "$(find_card)"
rm -f "$CARDS_FIX/cards"
eq "find_card is empty when /proc/asound/cards is absent" "" "$(find_card)"
ASOUND_DIR=$saved_asound

# End to end on a non-zero card: tert-tx-hold -D, the exported CARD that
# speaker-test-tone reads, and the summary header must all name card 3.
FIX=$(setup_fixture 3)
if run_probe; then ok; else bad "run on card 3 must exit 0: $(cat "$FIX/err")"; fi
OUT=$(evidence_dir)
eq "tert-tx-hold is pointed at the derived card, not 0" "tert-tx-hold -D 3" "$(grep '^tert-tx-hold' "$FIX/argv" | head -n 1)"
eq "the derived card is exported so speaker-test-tone uses it too" 3 "$(cat "$FIX/state/stt-card")"
if grep -q 'card=3' "$OUT/summary.txt"; then ok; else bad "the summary header must name the derived card"; fi
eq "windows are still complete on a non-zero card" ok "$(sed -n 's/^baseline .*window=\([a-z]*\)$/\1/p' "$OUT/phases.txt")"
no_helper_left || bad "helpers left after the card-3 run"

# ================= F1: two runs in one boot keep their windows apart =================
# Same fake ring buffer for both runs: run 2 must cut out only its own
# BEGIN..END pairs, and must never splice run 1's BEGIN to run 2's END.
FIX=$(setup_fixture)
if run_probe; then ok; else bad "first of two runs must exit 0: $(cat "$FIX/err")"; fi
# The first run installed a topology that nothing in userspace can remove, so
# a plain second run in the same boot is refused: its phase 1 would not be a
# baseline. This is the lesson of the 2026-09-22 run, enforced.
if [ -e "$FIX/run/spk-protect-probe.topology-installed" ]; then ok; else bad "the first run must leave the topology sentinel"; fi
if grep -q 'dealloc=NULL' "$FIX/run/spk-protect-probe.topology-installed"; then ok; else bad "the sentinel must record why the topology cannot be removed"; fi
ARGV_BEFORE=$(wc -l < "$FIX/argv")
if run_probe; then bad "a second run in the same boot must be refused"; else ok; fi
case "$(cat "$FIX/err")" in
*"a topology was already installed in this boot"*"phase 1 would NOT be a baseline"*) ok ;;
*) bad "the refusal must explain that phase 1 would not be a baseline, got: $(cat "$FIX/err")" ;;
esac
eq "the refused second run touches nothing" "$ARGV_BEFORE" "$(wc -l < "$FIX/argv")"
# -F runs anyway, and then NO phase may be presented as a baseline.
if run_probe -F; then ok; else bad "-F must allow a second run: $(cat "$FIX/err")"; fi
case "$(cat "$FIX/err")" in
*"NO phase in this run is a baseline"*) ok ;;
*) bad "-F must warn that no phase is a baseline, got: $(cat "$FIX/err")" ;;
esac
FORCED=$(evidence_dirs | tail -n 1)
# F10: the label must name the state the earlier run actually recorded.
# Run 1 ended by neutralising, so these phases are pre-neutralised (no
# SET_TOPOLOGY going out) -- materially different from pre-installed, and
# still not a baseline.
eq "-F labels the carried-over phases with the recorded state" "pre-neutralised pre-neutralised ready neutralised" \
	"$(sed -n 's/^.* topology=\([a-z-]*\) .*/\1/p' "$FORCED/phases.txt" | tr '\n' ' ' | sed 's/ $//')"
# Two carried-over phases plus this run's own neutralised phase: in a -F
# run nothing is a baseline, and every one of them has to say so.
eq "-F leaves no phase presented as a baseline" 3 \
	"$(grep -c 'NOT a baseline' "$FORCED/summary.txt")"
if grep -q 'then neutralised, so no SET_TOPOLOGY is sent' "$FORCED/summary.txt"; then ok; else bad "a pre-neutralised phase must be described as such, not as pre-installed"; fi
eq "the sentinel records the neutralised state after run 1" "neutralised" \
	"$(sed -n 's/^state: //p' "$FIX/run/spk-protect-probe.topology-installed")"
DIRS=$(evidence_dirs)
eq "each allowed run gets its own evidence directory" 2 "$(printf '%s\n' "$DIRS" | grep -c .)"
RUN_A=$(printf '%s\n' "$DIRS" | sed -n 1p)
RUN_B=$(printf '%s\n' "$DIRS" | sed -n 2p)
MARKER_A=$(run_marker "$RUN_A")
MARKER_B=$(run_marker "$RUN_B")
if [ "$MARKER_A" != "$MARKER_B" ]; then ok; else bad "two runs must not share a marker id ($MARKER_A)"; fi
for d in "$RUN_A" "$RUN_B"; do
	m=$(run_marker "$d")
	for ph in baseline tx tx-topo neutralised; do
		eq "$(basename "$d")/$ph holds exactly one BEGIN marker, its own" 1 "$(grep -Fc "$m BEGIN $ph" "$d/kmsg-$ph.log")"
		eq "$(basename "$d")/$ph holds exactly one END marker, its own" 1 "$(grep -Fc "$m END $ph" "$d/kmsg-$ph.log")"
	done
	if grep -q 'WARNING: kernel window' "$d/summary.txt"; then bad "$(basename "$d") must not warn about its windows"; else ok; fi
done
if grep -Fq "$MARKER_A" "$RUN_B/kmsg-baseline.log"; then bad "run 2's window must not contain run 1's markers"; else ok; fi
if grep -Fq "$MARKER_B" "$RUN_A/kmsg-tx-topo.log"; then bad "run 1's window must not contain run 2's markers"; else ok; fi
# Run 2's baseline window must be its own four kernel lines, not everything
# the ring buffer accumulated since run 1 started.
# Run 1 ended by neutralising, so run 2's tones log the two "rejected"
# lines instead of the single "not initialized" one: 2 markers + 5 lines.
eq "run 2's baseline window is its own phase only (markers + its own kernel lines)" 7 \
	"$(grep -c . "$RUN_B/kmsg-baseline.log")"
no_helper_left || bad "helpers left after the repeated-run test"

# ================= F2: missing / truncated windows are reported, not read as negatives =================
# capture_window as a pure function, against a hand-written ring buffer.
WFIX="$TMPROOT/window"
mkdir -p "$WFIX"
cat > "$WFIX/dmesg" <<'STUB'
#!/bin/sh
cat "$WINDOW_RING"
STUB
chmod +x "$WFIX/dmesg"
saved_out_dir=$OUT_DIR
saved_dmesg=$DMESG
OUT_DIR="$WFIX"
DMESG="$WFIX/dmesg"
MARKER='spk-protect-probe[20260922-101112-4242]'
OTHER_MARKER='spk-protect-probe[20260922-090000-1111]'
WINDOW_RING="$WFIX/ring"
export WINDOW_RING
{
	echo "[    0.100000] unrelated line before the window"
	echo "[    1.000000] $MARKER BEGIN baseline"
	echo "[    1.100000] __afe_port_start: port id: 0x1004"
	echo "[    1.200000] $MARKER END baseline"
	echo "[    2.000000] unrelated line after the window"
} > "$WINDOW_RING"
eq "capture_window: both markers present -> ok" ok "$(capture_window baseline)"
eq "capture_window: the window is exactly BEGIN..END" 3 "$(grep -c . "$WFIX/kmsg-baseline.log")"
if grep -q 'unrelated line' "$WFIX/kmsg-baseline.log"; then bad "the window must not spill outside its markers"; else ok; fi

grep -Fv " END baseline" "$WINDOW_RING" > "$WINDOW_RING.tmp" && mv "$WINDOW_RING.tmp" "$WINDOW_RING"
eq "capture_window: BEGIN without END -> truncated" truncated "$(capture_window baseline)"
if grep -q 'port id: 0x1004' "$WFIX/kmsg-baseline.log"; then ok; else bad "a truncated window must still keep the lines it did capture"; fi

grep -Fv " BEGIN baseline" "$WINDOW_RING" > "$WINDOW_RING.tmp" && mv "$WINDOW_RING.tmp" "$WINDOW_RING"
eq "capture_window: no BEGIN -> missing" missing "$(capture_window baseline)"
eq "a missing window captures nothing" 0 "$(grep -c . "$WFIX/kmsg-baseline.log" || true)"

# Another run's markers around the same phase name must not form a window.
{
	echo "[   10.000000] $OTHER_MARKER BEGIN baseline"
	echo "[   10.100000] a previous run's kernel line"
	echo "[   10.200000] $OTHER_MARKER END baseline"
} > "$WINDOW_RING"
eq "capture_window ignores another run's markers for the same phase" missing "$(capture_window baseline)"
eq "and captures none of that run's lines" 0 "$(grep -c . "$WFIX/kmsg-baseline.log" || true)"
# A foreign BEGIN before our END must not be spliced into a window.
{
	echo "[   20.000000] $OTHER_MARKER BEGIN baseline"
	echo "[   20.100000] a previous run's kernel line"
	echo "[   21.000000] $MARKER END baseline"
} > "$WINDOW_RING"
eq "a foreign BEGIN is never spliced to this run's END" missing "$(capture_window baseline)"
OUT_DIR=$saved_out_dir
DMESG=$saved_dmesg

# End to end: dmesg that lost the END markers -> every phase truncated, the
# summary warns instead of presenting empty sections as a negative result.
FIX=$(setup_fixture)
if STUB_DMESG_DROP=" END " run_probe; then ok; else bad "lost END markers must not fail the run: $(cat "$FIX/err")"; fi
OUT=$(evidence_dir)
eq "every phase records a truncated window" "truncated truncated truncated truncated" \
	"$(sed -n 's/^.*window=\([a-z]*\)$/\1/p' "$OUT/phases.txt" | tr '\n' ' ' | sed 's/ $//')"
case "$(cat "$FIX/err")" in
*"phase baseline: kernel window truncated"*) ok ;;
*) bad "a truncated window must be logged as it happens, got: $(cat "$FIX/err")" ;;
esac
eq "the summary warns once per phase" 4 "$(grep -c 'WARNING: kernel window TRUNCATED' "$OUT/summary.txt")"
if grep -q 'NOT evidence the line never appeared' "$OUT/summary.txt"; then ok; else bad "the truncated warning must say an empty section proves nothing"; fi
no_helper_left || bad "helpers left after the truncated-window run"

FIX=$(setup_fixture)
if STUB_DMESG_DROP="spk-protect-probe[" run_probe; then ok; else bad "lost markers must not fail the run: $(cat "$FIX/err")"; fi
OUT=$(evidence_dir)
eq "every phase records a missing window" "missing missing missing missing" \
	"$(sed -n 's/^.*window=\([a-z]*\)$/\1/p' "$OUT/phases.txt" | tr '\n' ' ' | sed 's/ $//')"
eq "the summary warns MISSING once per phase" 4 "$(grep -c 'WARNING: kernel window MISSING' "$OUT/summary.txt")"
if grep -q 'prove nothing' "$OUT/summary.txt"; then ok; else bad "the missing warning must say the empty sections prove nothing"; fi
# The sections are still printed, and still read as empty -- but only under
# the warning, never as a bare negative.
if grep -q '(no port 0x1005 lines)' "$OUT/summary.txt"; then ok; else bad "the sections must still be printed under the warning"; fi
# The tone log is independent of the kernel ring, so playback is still known.
if grep -q 'playback: tinyplay finished' "$OUT/summary.txt"; then ok; else bad "playback status must survive a lost kernel window"; fi
no_helper_left || bad "helpers left after the missing-window run"

# ================= restart_level: the kernel's spelling, tolerantly compared =================
# Regression for the 2026-09-22 live attempt, which reached the phone and was
# refused here with the node correctly reading RELATED: restart_level_show()
# prints restart_levels[] verbatim (UPPERCASE), while restart_level_store()
# matches with strncasecmp, so audio-up's `echo related` reads back RELATED.
eq "normalize_level lowercases the kernel's UPPERCASE readback" related "$(normalize_level RELATED)"
eq "normalize_level leaves an already-lowercase readback alone" related "$(normalize_level related)"
eq "normalize_level handles mixed case" related "$(normalize_level ReLaTeD)"
eq "normalize_level strips surrounding whitespace and newlines" related "$(normalize_level '  RELATED
')"
eq "normalize_level does not turn SYSTEM into RELATED" system "$(normalize_level SYSTEM)"
eq "normalize_level on an empty readback stays empty" "" "$(normalize_level '')"

# Every spelling the kernel could hand us must be accepted, and the verbatim
# readback must survive into the log and the evidence file unrewritten.
for level in RELATED related ReLaTeD; do
	FIX=$(setup_fixture)
	printf '%s\n' "$level" > "$FIX/subsys/subsys1/restart_level"
	if run_probe; then ok; else bad "restart_level '$level' must be accepted: $(cat "$FIX/err")"; fi
	OUT=$(evidence_dir)
	eq "restart_level '$level' is stored verbatim, not normalised" "$level" "$(cat "$OUT/restart_level.txt")"
	case "$(cat "$FIX/err")" in
	*"restart_level reads '$level' (RELATED"*) ok ;;
	*) bad "the accepted restart_level '$level' must be logged verbatim, got: $(cat "$FIX/err")" ;;
	esac
	if grep -q "adsp restart_level: $level" "$OUT/summary.txt"; then ok; else bad "the summary must carry the verbatim restart_level '$level'"; fi
	eq "restart_level '$level' run played all four tones" 4 "$(grep -c '^speaker-test-tone -p' "$FIX/argv")"
	no_helper_left || bad "helpers left after the '$level' run"
done

# A trailing-whitespace readback must not be refused either.
FIX=$(setup_fixture)
printf 'RELATED \n' > "$FIX/subsys/subsys1/restart_level"
if run_probe; then ok; else bad "a restart_level with trailing whitespace must be accepted: $(cat "$FIX/err")"; fi

# ...and nothing that is not RELATED may pass, whatever its case.
for level in SYSTEM system System '' unexpected; do
	FIX=$(setup_fixture)
	printf '%s\n' "$level" > "$FIX/subsys/subsys1/restart_level"
	if run_probe; then bad "restart_level '$level' must be refused"; else ok; fi
	eq "restart_level '$level' refused before anything runs" "" "$(cat "$FIX/argv")"
done

# ================= F6: the kernel FF pattern must be real kernel strings =================
# The summary's kernel-side FF section used to carry an invented "FF set data"
# alternative that matches nothing in the kernel tree and nothing in any live
# log. Every alternative in that pattern is now checked against the driver.
TAS_ALGO=kernel/sound/soc/msm/tas2560-algo.c
if [ -r "$TAS_ALGO" ]; then
	FF_PATTERN=$(sed -n "s/.*section \"FF readback (kernel log[^\"]*:\" '\([^']*\)'.*/\1/p" "$SCRIPT")
	if [ -n "$FF_PATTERN" ]; then ok; else bad "could not read the kernel FF pattern out of $SCRIPT"; fi
	printf '%s\n' "$FF_PATTERN" | tr '|' '\n' | while IFS= read -r alt; do
		[ -n "$alt" ] || continue
		grep -Fq "$alt" "$TAS_ALGO" || echo "MISSING:$alt"
	done > "$TMPROOT/ff-alts"
	eq "every alternative in the kernel FF pattern is a real string in $TAS_ALGO" "" "$(cat "$TMPROOT/ff-alts")"
	# The two functions whose lines this section exists to show.
	for fn in tas2560_algo_get_ff_module tas2560_algo_set_ff_module; do
		if grep -q "$fn" "$TAS_ALGO"; then ok; else bad "$fn is gone from $TAS_ALGO; the FF section needs rechecking"; fi
	done
	# Proof the pattern actually matches what those functions print.
	for line in 'TAS2560_ALGO:tas2560_algo_get_ff_module Recieving Rx-Enable data 0' \
		    'TAS2560_ALGO:tas2560_algo_set_ff_module Sending Rx-Enable data 1'; do
		if printf '%s\n' "$line" | grep -Eqi "$FF_PATTERN"; then ok; else bad "the kernel FF pattern must match '$line'"; fi
	done
	if grep -q "FF set data" "$SCRIPT"; then
		grep -q "^\s*#.*FF set data" "$SCRIPT" && ok || bad "'FF set data' must not be used as a live pattern in $SCRIPT"
	else
		ok
	fi
else
	bad "$TAS_ALGO is missing; cannot check the kernel FF pattern"
fi

# ================= a failed neutralisation is never dressed up as success =================
FIX=$(setup_fixture)
if STUB_NEUTRALISE_FAIL=1 run_probe; then ok; else bad "a failing -N must not fail the probe: $(cat "$FIX/err")"; fi
OUT=$(evidence_dir)
eq "a failed neutralisation is recorded as such" "neutralised tone_rc=0 topology=neutralise-failed tx_hold=none window=ok" \
	"$(grep '^neutralised' "$OUT/phases.txt")"
if grep -q 'the neutralisation FAILED' "$OUT/summary.txt"; then ok; else bad "the summary must say the neutralisation failed"; fi
if grep -q 'not a baseline and not a neutralised phase' "$OUT/summary.txt"; then ok; else bad "a failed neutralisation must not be presented as either a baseline or a neutralised phase"; fi
# The install still happened, so the run must still say the topology is live.
if grep -q 'STILL INSTALLED' "$OUT/summary.txt"; then ok; else bad "with -N failed, the summary must say the topology is still installed"; fi
if grep -q 'afe-topology-cal -N' "$OUT/summary.txt" || grep -q 'REBOOT' "$OUT/summary.txt"; then ok; else bad "the summary must say how to clear it"; fi
no_helper_left || bad "helpers left after the failed-neutralise run"

# ================= F8: the latch is set BEFORE the irreversible installer runs =================
# A helper killed between exec and its ready line, or one that completed its
# SET and then died, is indistinguishable from one that never installed
# anything. Latching first is the conservative direction: a spurious record
# costs one reboot, a missed one costs a later run silently presenting a
# post-topology phase as a baseline.
FIX=$(setup_fixture)
if STUB_TOPO_FAIL=1 run_probe; then ok; else bad "a failing installer must not fail the probe: $(cat "$FIX/err")"; fi
OUT=$(evidence_dir)
if [ -e "$FIX/run/spk-protect-probe.topology-installed" ]; then ok; else bad "the topology sentinel must exist even when the installer FAILED to report ready (it may still have completed its SET)"; fi
# The run still went on to neutralise, so that is the final recorded state;
# what matters for F8 is that a record exists at all after a failed install.
eq "the run's final recorded state is the neutralisation it performed" "neutralised" \
	"$(sed -n 's/^state: //p' "$FIX/run/spk-protect-probe.topology-installed")"
eq "the phase still records the helper as failed" "tx-topo tone_rc=0 topology=failed tx_hold=ready window=ok" \
	"$(grep '^tx-topo' "$OUT/phases.txt")"
# And a later plain run is refused on the strength of that record.
if run_probe; then bad "after a failed installer, a second run must still be refused"; else ok; fi

# The sentinel written before the installer says so in as many words.
FIX=$(setup_fixture)
STUB_TOPO_HANG=1 run_probe > /dev/null 2>&1 &
PROBE_PID=$!
i=0
while [ ! -e "$FIX/run/spk-protect-probe.topology-installed" ] && [ "$i" -lt 200 ]; do sleep 0.05; i=$((i + 1)); done
SENTINEL_STATE=$(sed -n 's/^state: //p' "$FIX/run/spk-protect-probe.topology-installed" 2>/dev/null)
kill -TERM "$PROBE_PID" 2>/dev/null
wait "$PROBE_PID" 2>/dev/null || true
eq "while the installer is still running, the sentinel reads 'maybe'" "maybe" "$SENTINEL_STATE"
no_helper_left || bad "helpers left after the hang test"

# ================= G1: an unreadable or stateless sentinel is the worst case =================
# The sentinel is the only thing standing between a later run and silently
# presenting a post-topology phase as a baseline, so anything we cannot read
# has to be treated as "a topology is installed and live", never as clean.
for broken in 'no-state-line' 'garbage-state' 'empty'; do
	FIX=$(setup_fixture)
	case "$broken" in
	no-state-line) printf 'an earlier run wrote something else entirely\n' > "$FIX/run/spk-protect-probe.topology-installed" ;;
	garbage-state) printf 'state: \xff\xfe not-a-state\n' > "$FIX/run/spk-protect-probe.topology-installed" ;;
	empty)         : > "$FIX/run/spk-protect-probe.topology-installed" ;;
	esac
	# Still refused without -F: unreadable is not permission to proceed.
	if run_probe; then bad "[$broken] a second run must be refused whatever the sentinel says"; else ok; fi
	# With -F, every carried-over phase is labelled the worst case.
	if run_probe -F; then ok; else bad "[$broken] -F must still run: $(cat "$FIX/err")"; fi
	OUT=$(evidence_dirs | tail -n 1)
	eq "[$broken] an unreadable state is treated as pre-installed, the worst case" \
		"pre-installed pre-installed ready neutralised" \
		"$(sed -n 's/^.* topology=\([a-z-]*\) .*/\1/p' "$OUT/phases.txt" | tr '\n' ' ' | sed 's/ $//')"
	if grep -q 'still being sent' "$OUT/summary.txt"; then ok; else bad "[$broken] the summary must say the topology is still live"; fi
	no_helper_left || bad "[$broken] helpers left behind"
done

# A sentinel that cannot be read at all (no permission) is the same case.
FIX=$(setup_fixture)
printf 'state: yes\n' > "$FIX/run/spk-protect-probe.topology-installed"
chmod 000 "$FIX/run/spk-protect-probe.topology-installed"
if run_probe; then bad "an unreadable sentinel must still refuse a second run"; else ok; fi
if run_probe -F; then ok; else bad "-F must still run with an unreadable sentinel: $(cat "$FIX/err")"; fi
OUT=$(evidence_dirs | tail -n 1)
eq "an unreadable sentinel file is treated as pre-installed" "pre-installed" \
	"$(sed -n 's/^baseline .* topology=\([a-z-]*\) .*/\1/p' "$OUT/phases.txt")"
chmod 644 "$FIX/run/spk-protect-probe.topology-installed"
no_helper_left || bad "helpers left after the unreadable-sentinel run"

# ================= H1: the sentinel state tracks what is actually known =================
# "maybe" is the fail-safe written before the irreversible installer runs,
# not the resting state. afe-topology-cal prints "resident, holding" only
# after every SET has returned 0 (install_block runs first), so observing
# that line is positive proof the block is in the kernel and the record must
# advance to "yes". If the line never comes we stay at "maybe": the helper
# may still have completed its SET and died, and an unknown must never be
# downgraded to a clean slate.
sentinel_state() { sed -n 's/^state: //p' "$FIX/run/spk-protect-probe.topology-installed" 2>/dev/null; }

# A successful install, with the neutralise phase suppressed so the final
# recorded state is the install's own.
FIX=$(setup_fixture)
if STUB_NEUTRALISE_FAIL=1 run_probe; then ok; else bad "install-only run must exit 0: $(cat "$FIX/err")"; fi
eq "a successful install advances the record to yes" "yes" "$(sentinel_state)"
OUT=$(evidence_dir)
eq "and the phase records the helper as ready" "tx-topo tone_rc=0 topology=ready tx_hold=ready window=ok" \
	"$(grep '^tx-topo' "$OUT/phases.txt")"
no_helper_left || bad "helpers left after the install-only run"

# An installer that never reports ready must leave the record at "maybe" --
# it may have completed its SET before dying.
FIX=$(setup_fixture)
if STUB_TOPO_FAIL=1 STUB_NEUTRALISE_FAIL=1 run_probe; then ok; else bad "failed-installer run must exit 0: $(cat "$FIX/err")"; fi
eq "an installer that never reported ready leaves the record at maybe" "maybe" "$(sentinel_state)"
if [ -e "$FIX/run/spk-protect-probe.topology-installed" ]; then ok; else bad "a failed installer must still leave a record (F8)"; fi
no_helper_left || bad "helpers left after the failed-installer run"

# A hung installer (ready line never printed, process still alive) is the
# same unknown: maybe, never yes.
FIX=$(setup_fixture)
STUB_TOPO_HANG=1 run_probe > /dev/null 2>&1 &
PROBE_PID=$!
i=0
while [ ! -e "$FIX/run/spk-protect-probe.topology-installed" ] && [ "$i" -lt 200 ]; do sleep 0.05; i=$((i + 1)); done
HUNG_STATE=$(sentinel_state)
kill -TERM "$PROBE_PID" 2>/dev/null
wait "$PROBE_PID" 2>/dev/null || true
eq "a hung installer never advances the record past maybe" "maybe" "$HUNG_STATE"
no_helper_left || bad "helpers left after the hung-installer run"

# A successful -N advances it again, past the install's "yes".
FIX=$(setup_fixture)
if run_probe; then ok; else bad "full run must exit 0: $(cat "$FIX/err")"; fi
eq "a successful -N advances the record to neutralised" "neutralised" "$(sentinel_state)"
no_helper_left || bad "helpers left after the full run"

# A failing run must SAY so rather than print "PASS:" regardless and leave
# the exit status as the only signal -- the last line of a suite's output is
# what a reader (and `make test`'s log) actually sees. Exit behaviour is
# unchanged: 0 only when nothing failed.
if [ "$fail" -eq 0 ]; then
	echo "PASS: $pass/$((pass + fail)) checks passed"
else
	echo "FAILED: $fail of $((pass + fail)) checks failed ($pass passed)" >&2
	exit 1
fi
