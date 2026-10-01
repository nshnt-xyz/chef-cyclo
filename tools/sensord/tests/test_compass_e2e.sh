#!/bin/sh
# End-to-end for the heading and rotvec channels: the real sensord main
# loop, built with ASan/UBSan (tests/sensord-fake-san), against
# tests/fake_smgr over the fake transport. Checks that claiming heading
# adds the accel/anglvel/magn reports (50/50/20 Hz, calibration full) and
# releasing it removes them, the rate max with other clients, the
# heading lines (fake phone: heading 88.13, pitch 2.90, roll -5.81), the
# calibrated flag (live: full minus the internal factory report, with the
# fake's learned bias switched on by a file; the registry group 2980 write
# only before the live state is known), declination,
# get, status, -M/-D, -c magn=factory, and the rotation vector client
# (-V): lifecycle through release, SIGTERM, SIGHUP, SMGR-only and whole
# DSP restarts, a slow and an in-flight ENABLE, and no contact without
# -V. Run from tools/sensord (make test does).
set -u

T=$(mktemp -d /tmp/snsc.XXXXXX)	# short: AF_UNIX paths are <= 107 bytes
export SENSORD_FAKE_IPC="$T/ipc"
mkdir -p "$SENSORD_FAKE_IPC"
SOCK="$T/sock"
FAILS=0
SPID=
FPID=
DAEMON=./tests/sensord-fake-san
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS=halt_on_error=1

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

# wait_for FILE PATTERN [TENTHS]
wait_for() {
	i=0
	while ! grep -q -- "$2" "$1" 2>/dev/null; do
		[ "$i" -ge "${3:-50}" ] && return 1
		sleep 0.1
		i=$((i + 1))
	done
}

cli() {
	timeout 10 ./tests/sensord-fake -S "$SOCK" "$@"
}

bg_cli() {
	timeout 30 ./tests/sensord-fake -S "$SOCK" "$@" &
}

start_pair() {
	$DAEMON -r "$T/reg" -m tests/fixtures/sns_reg.map -S "$SOCK" -v $1 2>>"$T/sensord.log" &
	SPID=$!
	./tests/fake_smgr serve >>"$T/smgr.log" 2>&1 &
	FPID=$!
	wait_for "$T/sensord.log" "smgr ready" 100 || fail "sensord never reached smgr ready ($1)"
}

stop_pair() {
	for p in $SPID $FPID; do
		kill "$p" 2>/dev/null
		wait "$p" 2>/dev/null
	done
	SPID=
	FPID=
	grep -q "ERROR: AddressSanitizer\|runtime error" "$T/sensord.log" && fail "sanitizer: $(grep -A5 'Sanitizer\|runtime error' "$T/sensord.log" | head -8)"
	grep -q "DECOY" "$T/smgr.log" && fail "sensord talked to a decoy instance"
	: >"$T/smgr.log"
	: >"$T/sensord.log"
}

# heading lines: FILE -> "min max" of the heading field
heading_range() {
	sed -n 's/.*"heading":\([0-9.]*\),.*/\1/p' "$1" |
		awk 'NR == 1 { lo = hi = $1 } { if ($1 < lo) lo = $1; if ($1 > hi) hi = $1 } END { print lo, hi }'
}

# group 2980 as the DSP wrote it live once learned (126 bytes): version 1,
# valid 1, bias words -15790 -25877 16690 (Q16 gauss, SMGR frame)
G2980="0100000001005 2c2ffffeb9affff32410000"
G2980=$(echo "$G2980" | tr -d ' ')
while [ ${#G2980} -lt 252 ]; do G2980="${G2980}00"; done

# the fake applies a learned bias to factory magn once this file exists
export FAKE_MAG_LEARN_FILE="$T/learned"

./tests/fake_smgr mkreg "$T/reg" 1024 || { echo "FAIL: mkreg"; exit 1; }
chmod 0400 "$T/reg"

# ------------------------------------------------------------ heading
start_pair "-V -Q" ""
grep -q "magnetometer bias in the registry (registry loaded): 0.0000 0.0000 0.0000 gauss (SMGR frame, group 2980) -> not calibrated" \
	"$T/sensord.log" || fail "registry calibration at load: $(grep 'magnetometer' "$T/sensord.log")"
L=$(cli list)
echo "$L" | grep -q '{"sensor":"heading","unit":"deg","virtual":true,"inputs":"accel 50 Hz, anglvel 50 Hz, magn 20 Hz","present":true,"max_hz":50,"rate":0,"claims":0,"mount":"portrait","calibrated":false}' \
	|| fail "list heading entry: $L"
echo "$L" | grep -q '{"sensor":"rotvec","unit":"quaternion","virtual":true,"present":true,"max_hz":20,"rate":0,"claims":0,"state":"idle"}' \
	|| fail "list rotvec entry: $L"

cli -n 25 watch heading 10 >"$T/h1" || fail "watch heading"
# pitch/roll within 0.25 deg: the fake gyro's constant rate is a bias the
# filter learns slowly (steady-state tilt error rate / KP_ACC = 0.2 deg)
PR='"pitch":\(2\.[6-9][0-9]\|3\.[01][0-9]\),"roll":-5\.[5-9][0-9]'
# before the live state is known (3 s) the registry is the source; with
# -V the ADSP rotation vector's accuracy rides along once it reports
RV='\(,"rotvec_accuracy":[0-3]\)\?'
[ "$(grep -c '^{"sensor":"heading","t":[0-9]*,"heading":[0-9.]*,'"$PR"',"calibrated":false,"disturbed":false,"accuracy":180.0,"cal_source":"\(registry\|live\)","mag_bias":0.000'"$RV"'}$' "$T/h1")" -eq 25 ] \
	|| fail "heading lines: $(head -n 3 "$T/h1")"
set -- $(heading_range "$T/h1")
awk -v lo="$1" -v hi="$2" 'BEGIN { exit !(lo >= 87.6 && hi <= 88.7) }' || fail "heading $1..$2, want 88.13 +- 0.5"
# 10 Hz: 25 lines over about 2.4 s of stamps
awk -F'"t":' '{ split($2, a, ","); t[NR] = a[1] } END { d = (t[NR] - t[1]) / 1e9; exit !(d > 2.1 && d < 2.7) }' "$T/h1" \
	|| fail "heading rate not 10 Hz"
grep -q "ADD report 1 sensor 0 dt 0 rate 50 report_rate 50 notify 1 cal 0 " "$T/smgr.log" || fail "accel 50 Hz not added: $(grep ADD "$T/smgr.log")"
grep -q "ADD report 2 sensor 10 dt 0 rate 50 report_rate 50 notify 1 cal 0 " "$T/smgr.log" || fail "anglvel 50 Hz not added"
grep -q "ADD report 3 sensor 20 dt 0 rate 20 report_rate 20 notify 1 cal 0 " "$T/smgr.log" || fail "magn 20 Hz full not added"
grep -q "ADD report 6 sensor 20 dt 0 rate 5 report_rate 5 notify 1 cal 1 " "$T/smgr.log" || fail "internal factory magn 5 Hz not added: $(grep ADD "$T/smgr.log")"
grep -q "heading on: accel 50 Hz, anglvel 50 Hz, magn 20 Hz (calibration full) and magn 5 Hz (factory) claimed internally, rotation vector (-V); mount portrait; magnetometer NOT calibrated yet (source registry)" \
	"$T/sensord.log" || fail "heading on log: $(grep 'heading on' "$T/sensord.log")"
cli list | grep -q magn_factory && fail "internal channel listed"
R=$(cli -n 1 watch magn_factory)
echo "$R" | grep -q '"err":"unknown sensor","sensor":"magn_factory"' || fail "internal channel claimable: $R"
# -V: the heading uses the rotation vector (accuracy on its lines)
grep -q "ROTVEC ENABLE period 0 rate 20" "$T/smgr.log" || fail "rotvec not enabled for the heading with -V"
grep -q '"rotvec_accuracy":[0-3]}$' "$T/h1" || fail "no rotvec_accuracy on heading lines"
for r in 1 2 3 6; do
	wait_for "$T/smgr.log" "DELETE report $r" 30 || fail "report $r not deleted when heading was released"
done
wait_for "$T/sensord.log" "heading off (no claims)" 10 || fail "heading off log"
# -Q: the heading's internal magn claim counts as magn use
grep -q "QMAG ENABLE period none" "$T/smgr.log" || fail "QMAG not enabled for the heading's magn"
wait_for "$T/smgr.log" "QMAG DISABLE instance" 30 || fail "QMAG not disabled when heading went"
wait_for "$T/smgr.log" "ROTVEC DISABLE instance" 30 || fail "rotvec not disabled when heading went"

# rate max with other clients, both ways
: >"$T/smgr.log"
bg_cli -n 100000 watch heading 5 >"$T/h2"
WH=$!
wait_for "$T/smgr.log" "ADD report 3 sensor 20 dt 0 rate 20 " 50 || fail "heading claim (2)"
cli -n 10 watch accel 100 >/dev/null || fail "accel 100 beside heading"
grep -q "sensor 0 dt 0 rate 100 " "$T/smgr.log" || fail "accel not raised to 100 Hz"
wait_for "$T/sensord.log" "change report 1: accel 50 Hz" 30 || fail "accel not lowered back to the heading's 50 Hz"
cli -n 10 watch magn 40 >/dev/null || fail "magn 40 beside heading"
wait_for "$T/sensord.log" "change report 3: magn 20 Hz" 30 || fail "magn not lowered back to 20 Hz"
cli -n 3 watch magn 5 >/dev/null || fail "magn 5 beside heading"
[ "$(grep -c 'report 3 sensor 20 dt 0 rate 5 ' "$T/smgr.log")" -eq 0 ] || fail "magn lowered below the heading's 20 Hz"
grep -q "DELETE report [1236]" "$T/smgr.log" && fail "an input deleted while heading is claimed"

# the live state: factory == full (nothing learned yet): known, not calibrated
wait_for "$T/sensord.log" "magnetometer NOT calibrated (source live, live): live bias 0.0000 0.0000 0.0000 gauss (factory - full, device frame, known)" 60 \
	|| fail "live state not known: $(grep 'magnetometer' "$T/sensord.log")"
# a 2980 write now does not override what SMGR applies (the live state)
./tests/fake_smgr reg2write 2980 "$G2980" >"$T/w" 2>&1 || fail "reg2write: $(cat "$T/w")"
wait_for "$T/sensord.log" "magnetometer bias in the registry (DSP write): -0.2409 -0.3949 0.2547 gauss (SMGR frame, group 2980) -> calibrated" 30 \
	|| fail "registry write not seen: $(grep magnetometer "$T/sensord.log")"
cli -n 3 watch heading 10 >"$T/h3"
grep -q '"calibrated":false,"disturbed":false,"accuracy":180.0,"cal_source":"live","mag_bias":0.000' "$T/h3" \
	|| fail "registry overrode the live state: $(tail -n 1 "$T/h3")"
wait_for "$T/sensord.log" "registry written back to $T/reg" 60 || fail "group 2980 write not written back"
# the ADSP applies its learned bias: calibrated from the data, 3 s later
touch "$FAKE_MAG_LEARN_FILE"
wait_for "$T/sensord.log" "magnetometer calibrated (source live, live): live bias 0.4010 0.2290 0.2540 gauss (factory - full, device frame, known)" 80 \
	|| fail "live calibration: $(grep 'magnetometer' "$T/sensord.log" | tail -n 3)"
sleep 3
cli -n 5 watch heading 10 >"$T/h3"
grep -q '"calibrated":true,"disturbed":false,"accuracy":[0-9]\.[0-9],"cal_source":"live","mag_bias":0.52[78]'"$RV"'}$' "$T/h3" \
	|| fail "calibrated heading lines: $(tail -n 1 "$T/h3")"
S=$(cli status)
echo "$S" | grep -q '"compass":"on","compass_clients":[12],"mount":"portrait","calibrated":true,"cal_source":"live","mag_bias":\[0.4010,0.2290,0.2540\],"mag_bias_pairs":[1-9][0-9]*,"mag_bias_reg":\[-0.240936,-0.394852,0.254669\],"disturbed":false,"accuracy":[0-9.]*,"rotvec_accuracy":[0-3],"declination":null,' \
	|| fail "status compass fields: $S"

# declination
D=$(cli declination 1.5)
[ "$D" = '{"ok":"declination","declination":1.50}' ] || fail "declination reply: $D"
cli -n 3 watch heading 10 >"$T/h4"
grep -q '"true_heading":\(89\.[0-9]*\|90\.[0-2][0-9]\),"declination":1.50}$' "$T/h4" || fail "true_heading: $(tail -n 1 "$T/h4")"
D=$(cli declination bogus)
[ "$D" = '{"err":"bad declination"}' ] || fail "bad declination: $D"
D=$(cli declination off)
[ "$D" = '{"ok":"declination","declination":null}' ] || fail "declination off: $D"
G=$(cli get heading)
echo "$G" | grep -q '^{"sensor":"heading","t":[0-9]*,"heading":8[78]\.[0-9]*,'"$PR"',"calibrated":true,"disturbed":false,"accuracy":[0-9.]*,"cal_source":"live","mag_bias":0.52[78]'"$RV"'}$' \
	|| fail "get heading: $G"
kill "$WH" 2>/dev/null
wait "$WH" 2>/dev/null
for r in 1 2 3 6; do
	wait_for "$T/smgr.log" "DELETE report $r" 30 || fail "report $r not deleted after the last heading client"
done
stop_pair
rm -f "$FAKE_MAG_LEARN_FILE"

# -M, -D, -c magn=factory
start_pair "-M upright -D -2.25 -c magn=factory" ""
cli list | grep -q '"sensor":"heading"[^}]*"mount":"upright"' || fail "-M upright not in list"
cli -n 3 watch heading 10 >"$T/h5"
# the fake phone lies flat: upright's forward (-z) is vertical, no heading
grep -q '"accuracy":180.0,"cal_source":"registry","mag_bias":0.000,"true_heading":[0-9.]*,"declination":-2.25}$' "$T/h5" || fail "-D line: $(tail -n 1 "$T/h5")"
grep -q "sensor 20 dt 0 rate 20 report_rate 20 notify 1 cal 1 " "$T/smgr.log" || fail "magn factory for the heading"
# full minus factory means nothing when magn itself is factory: no
# internal factory report, and never calibrated
grep -q "rate 5 report_rate 5 notify 1 cal 1 " "$T/smgr.log" && fail "internal factory report with -c magn=factory"
./tests/fake_smgr reg2write 2980 "$G2980" >/dev/null 2>&1
wait_for "$T/sensord.log" "but magn calibration is not full (-c) -> calibrated" 30 || fail "registry write with factory magn"
cli -n 3 watch heading 10 >"$T/h5"
grep -q '"calibrated":true' "$T/h5" && fail "factory magn counted as calibrated"
stop_pair
if $DAEMON -S "$SOCK" -M sideways 2>/dev/null; then fail "-M sideways accepted"; fi
if $DAEMON -S "$SOCK" -D 200 2>/dev/null; then fail "-D 200 accepted"; fi

# ------------------------------------------------------------ rotvec (-V)
start_pair "" ""
R=$(cli -n 1 watch rotvec)
echo "$R" | grep -q '"err":"not enabled (sensord -V)","sensor":"rotvec"' || fail "rotvec without -V: $R"
sleep 0.3
grep -Eq "ROTVEC (ATTR|ENABLE|DISABLE)" "$T/smgr.log" && fail "rotvec service contacted without -V"
stop_pair

start_pair "-V" ""
bg_cli -n 100000 watch rotvec 10 >"$T/rv"
WR=$!
wait_for "$T/smgr.log" "ROTVEC ENABLE period 0 rate 20 coord none notify 0/0 instance 7" 50 \
	|| fail "rotvec enable fields: $(grep ROTVEC "$T/smgr.log")"
[ "$(grep -n 'ROTVEC ATTR' "$T/smgr.log" | cut -d: -f1)" -lt "$(grep -n 'ROTVEC ENABLE' "$T/smgr.log" | cut -d: -f1)" ] \
	|| fail "rotvec attributes not queried before enable"
grep -q "lookup 0x112 (all instances): .*0x3202@" "$T/sensord.log" || fail "rotvec decoy instance not listed"
grep -q "rotvec enable (report period 0, sample rate 20 Hz, suspend notify apps/no; no coordinate TLV, as the stock HAL)" \
	"$T/sensord.log" || fail "rotvec enable log"
wait_for "$T/sensord.log" "rotvec error indication: error 3 for our instance 7" 50 || fail "rotvec error indication"
wait_for "$T/rv" '"heading":90.00,"pitch":0.00,"roll":0.00}$' 50 || fail "rotvec lines: $(tail -n 1 "$T/rv")"
grep -q '^{"sensor":"rotvec","t":[0-9]*,"x":0,"y":0,"z":-0.707107,"w":0.707107,"accuracy":[0-3],"coord":1,"heading":90.00,' "$T/rv" \
	|| fail "rotvec line fields: $(tail -n 1 "$T/rv")"
grep -q "rotvec report 1: instance 7 ts [0-9]* q x 0.00000 y 0.00000 z -0.70711 w 0.70711 (|q| 1.0000) accuracy 0 coord 1" \
	"$T/sensord.log" || fail "rotvec report log"
grep -q "sensor 0 dt\|sensor 20 dt" "$T/smgr.log" && fail "rotvec claimed SMGR reports"
S=$(cli status)
echo "$S" | grep -q '"rotvec":"on","rotvec_instance":7,"rotvec_enables":1,"rotvec_inds":[1-9][0-9]*,"rotvec_errors":1,"rotvec_last_error":3,' \
	|| fail "status rotvec fields: $S"
# SMGR alone restarts: disabled while it is away, enabled again after
kill -USR2 "$FPID"
wait_for "$T/smgr.log" "ROTVEC DISABLE instance 7$" 100 || fail "rotvec not disabled on SMGR loss"
wait_for "$T/sensord.log" "rotvec disable instance 7 (smgr not ready)" 10 || fail "rotvec disable reason"
wait_for "$T/smgr.log" "ROTVEC ENABLE period 0 rate 20 coord none notify 0/0 instance 8" 100 || fail "rotvec not re-enabled after SMGR"
# the whole DSP: the instance goes with it; enabled again on the new port
kill -USR1 "$FPID"
wait_for "$T/sensord.log" "rotvec 1:[0-9]* lost (" 100 || fail "rotvec loss not noticed"
wait_for "$T/smgr.log" "ROTVEC ENABLE period 0 rate 20 coord none notify 0/0 instance 9" 150 || fail "rotvec not re-enabled after DSP restart"
[ "$(grep -c 'ROTVEC ATTR' "$T/smgr.log")" -eq 1 ] || fail "rotvec attributes asked more than once"
# release
kill "$WR" 2>/dev/null
wait "$WR" 2>/dev/null
wait_for "$T/smgr.log" "ROTVEC DISABLE instance 9$" 30 || fail "rotvec not disabled on release"
wait_for "$T/sensord.log" "rotvec disabled: instance 9 (result 0 err 0)" 30 || fail "rotvec disable response"
cli status | grep -q '"rotvec":"idle","rotvec_instance":-1,' || fail "status after rotvec release"
# SIGTERM while enabled
bg_cli -n 100000 watch rotvec >/dev/null
WR=$!
wait_for "$T/smgr.log" "ROTVEC ENABLE .* instance 10" 50 || fail "rotvec not enabled again"
sleep 0.3
kill "$SPID"
wait "$SPID" 2>/dev/null
SPID=
wait_for "$T/smgr.log" "ROTVEC DISABLE instance 10$" 10 || fail "rotvec not disabled on SIGTERM"
kill "$WR" 2>/dev/null
wait "$WR" 2>/dev/null
grep -q "DISABLE instance [0-9]* unknown" "$T/smgr.log" && fail "disable for an unknown instance"
stop_pair

# SIGHUP (closed telnet session), heading and rotvec both on
start_pair "-V" ""
bg_cli -n 100000 watch rotvec >/dev/null
WR=$!
bg_cli -n 100000 watch heading >/dev/null
WH=$!
wait_for "$T/sensord.log" "rotvec enabled: instance 7" 50 || fail "HUP case: rotvec not enabled"
wait_for "$T/smgr.log" "ADD report 3 sensor 20" 50 || fail "HUP case: heading inputs"
kill -HUP "$SPID"
wait "$SPID" 2>/dev/null
SPID=
wait_for "$T/smgr.log" "ROTVEC DISABLE instance 7$" 30 || fail "HUP: rotvec not disabled"
for r in 1 2 3 6; do grep -q "DELETE report $r" "$T/smgr.log" || fail "HUP: report $r not deleted"; done
[ -e "$SOCK" ] && fail "HUP: socket left behind"
kill "$WR" "$WH" 2>/dev/null
wait "$WR" "$WH" 2>/dev/null
stop_pair

# a slow DSP: ENABLE answered after 4.5 s is sent exactly once
export FAKE_ROTVEC_ENABLE_DELAY_MS=4500
start_pair "-V" ""
bg_cli -n 100000 watch rotvec >/dev/null
WR=$!
wait_for "$T/smgr.log" "ROTVEC ENABLE REPLY instance 7" 100 || fail "slow rotvec enable never answered"
wait_for "$T/sensord.log" "rotvec enabled: instance 7" 20 || fail "slow rotvec enable not accepted"
[ "$(grep -c 'ROTVEC ENABLE period' "$T/smgr.log")" -eq 1 ] || fail "rotvec ENABLE resent: $(grep ROTVEC "$T/smgr.log")"
grep -q "resending" "$T/sensord.log" && fail "a rotvec request was resent"
kill "$WR" 2>/dev/null
wait "$WR" 2>/dev/null
wait_for "$T/smgr.log" "ROTVEC DISABLE instance 7$" 30 || fail "slow case: rotvec not disabled on release"
stop_pair
# SIGTERM with the ENABLE in flight: waited for, its instance disabled
FAKE_ROTVEC_ENABLE_DELAY_MS=700
start_pair "-V" ""
bg_cli -n 100000 watch rotvec >/dev/null
WR=$!
wait_for "$T/smgr.log" "ROTVEC ENABLE period 0 rate 20 coord none notify 0/0 instance 7" 50 || fail "in-flight: enable not sent"
kill "$SPID"
wait "$SPID" 2>/dev/null
SPID=
wait_for "$T/smgr.log" "ROTVEC DISABLE instance 7$" 30 || fail "rotvec ENABLE in flight at exit: instance left: $(grep ROTVEC "$T/smgr.log")"
kill "$WR" 2>/dev/null
wait "$WR" 2>/dev/null
stop_pair
unset FAKE_ROTVEC_ENABLE_DELAY_MS

if [ "$FAILS" -ne 0 ]; then
	echo "--- sensord.log"; tail -n 40 "$T/sensord.log"
	echo "--- smgr.log"; tail -n 20 "$T/smgr.log"
	echo "test_compass_e2e.sh: $FAILS failure(s)"
	exit 1
fi
echo "test_compass_e2e.sh: all passed"
