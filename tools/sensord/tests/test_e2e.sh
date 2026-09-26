#!/bin/sh
# End-to-end: the real sensord main loop (tests/sensord-fake, i.e.
# sensord.c on tests/fake_ipc.c) against tests/fake_smgr, which plays the
# ADSP: REG2 reads/writes at "boot", then the SMGR service with sensor
# info, report add/delete and a sample stream, and a simulated DSP
# restart. Client side through sensord's own CLI mode. Registry data is
# the synthetic fixture. Run from tools/sensord (make test does).
set -u

T=$(mktemp -d /tmp/snsd.XXXXXX)	# short: AF_UNIX paths are <= 107 bytes
export SENSORD_FAKE_IPC="$T/ipc"
mkdir -p "$SENSORD_FAKE_IPC"
SOCK="$T/sock"
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

# Background client: timeout is the direct child, so killing $! stops it.
bg_cli() {
	timeout 20 ./tests/sensord-fake -S "$SOCK" "$@" &
}

start_pair() {
	./tests/sensord-fake -r "$T/reg" -m tests/fixtures/sns_reg.map -S "$SOCK" -v $1 \
		2>>"$T/sensord.log" &
	SPID=$!
	./tests/fake_smgr serve $2 >>"$T/smgr.log" 2>&1 &
	FPID=$!
	wait_for "$T/sensord.log" "smgr ready" 80 || fail "sensord never reached smgr ready ($1)"
}

stop_pair() {
	for p in $SPID $FPID; do
		kill "$p" 2>/dev/null
		wait "$p" 2>/dev/null
	done
	SPID=
	FPID=
}

./tests/fake_smgr mkreg "$T/reg" 1024 || { echo "FAIL: mkreg"; exit 1; }

# ---------------------------------------------------------- buffering
start_pair "" ""
grep -q "^REG2 OK" "$T/smgr.log" || fail "registry checks: $(grep REG2 "$T/smgr.log")"
# instance-0 lookup (the kernel ignores other requested instances with
# mask 0), then 0x3201 picked out of the list beside a 0x3202 decoy
grep -q "lookup 0x100 (all instances): .*0x3202@" "$T/sensord.log" || fail "decoy instance not listed"
grep -q "lookup 0x100 (all instances): .*0x3201@" "$T/sensord.log" || fail "0x3201 not listed"
grep -q "reg2: group write 1000 .*(RAM only)" "$T/sensord.log" || fail "registry write not logged"
grep -q "reg2: unsupported request, nacked" "$T/sensord.log" || fail "msg 0x06 not logged"

# a second daemon must not steal the socket or publish REG2 again
if ./tests/sensord-fake -r "$T/reg" -m tests/fixtures/sns_reg.map -S "$SOCK" 2>"$T/second.log"; then
	fail "second sensord started"
fi
grep -q "another sensord is serving" "$T/second.log" || fail "second sensord message: $(cat "$T/second.log")"
[ "$(ls "$SENSORD_FAKE_IPC" | grep -c '^s271\.')" -eq 1 ] || fail "REG2 published twice"

L=$(cli list)
echo "$L" | grep -q '"smgr":"ready"' || fail "list: smgr not ready: $L"
echo "$L" | grep -q '"sensor":"accel","unit":"m/s^2","smgr_id":0,"data_type":0,"present":true,"name":"BMI160 Accelerometer"' \
	|| fail "list: accel entry: $L"
echo "$L" | grep -q '"sensor":"illuminance".*"name":"EPL259x ALS/PS ALS \\"q\\""' \
	|| fail "list: escaped ALS name: $L"

W=$(cli -n 6 watch accel 50)
[ "$(echo "$W" | grep -c '^{"sensor":"accel"')" -eq 6 ] || fail "watch accel: $W"
echo "$W" | grep -q '"x":1,"y":0.5,"z":9.8066' || fail "accel conversion/axes: $W"
# timestamps strictly increasing, ~20 ms apart
echo "$W" | sed 's/.*"t":\([0-9]*\).*/\1/' | awk 'NR > 1 { d = $1 - p; if (d < 15000000 || d > 25000000) bad = 1 } { p = $1 } END { exit bad }' \
	|| fail "accel timestamps not 50 Hz monotonic: $W"
wait_for "$T/smgr.log" "DELETE report 1" 30 || fail "watch exit did not delete the accel report"
grep -q "ADD report 1 sensor 0 dt 0 rate 50 report_rate 50 notify 1 cal 0 dec 3" "$T/smgr.log" \
	|| fail "accel buffering request fields: $(grep ADD "$T/smgr.log")"

G=$(cli get illuminance)
echo "$G" | grep -q '^{"sensor":"illuminance","t":[0-9]*,"illuminance":123.5}$' || fail "get illuminance: $G"
G=$(cli get proximity)
echo "$G" | grep -q '^{"sensor":"proximity","t":[0-9]*,"near":1,"proximity":42}$' || fail "get proximity: $G"
G=$(cli get gyro 2>&1)
echo "$G" | grep -q '"err":"unknown sensor"' || fail "unknown sensor: $G"

# two clients: the report runs at the max, and drops back when one leaves
bg_cli -n 1000 watch magn 5 >"$T/magn5"
W5=$!
wait_for "$T/smgr.log" "sensor 20 dt 0 rate 5 " 30 || fail "magn 5 Hz report"
cli -n 10 watch magn 40 >"$T/magn40" || fail "magn 40 Hz watch"
grep -q "sensor 20 dt 0 rate 40 " "$T/smgr.log" || fail "magn report not raised to 40 Hz"
wait_for "$T/sensord.log" "change report 3: magn 5 Hz" 30 || fail "magn report not lowered back to 5 Hz"
kill "$W5" 2>/dev/null
wait "$W5" 2>/dev/null
wait_for "$T/smgr.log" "DELETE report 3" 30 || fail "magn report not deleted after last client"
N5=$(grep -c '^{"sensor":"magn"' "$T/magn5")
[ "$N5" -ge 1 ] && [ "$N5" -le 20 ] || fail "5 Hz client got $N5 samples (decimation)"

# a client that never reads: others keep streaming, it learns what it lost
./tests/fake_smgr slow "$SOCK" 5 >"$T/slow" &
SLOWP=$!
sleep 1
W=$(cli -n 20 watch accel 50)
[ "$(echo "$W" | grep -c '^{"sensor":"accel"')" -eq 20 ] || fail "watch during slow client"
wait "$SLOWP" || fail "slow client: $(cat "$T/slow")"

# DSP restart: SMGR vanishes and comes back on a new port; sensord
# re-reads the sensor info and re-adds the report a claimant still holds
bg_cli -n 100000 watch anglvel 20 >"$T/gyro"
WG=$!
wait_for "$T/smgr.log" "sensor 10 dt 0 rate 20 " 30 || fail "gyro report"
kill -USR1 "$FPID"
wait_for "$T/sensord.log" "lost (" 100 || fail "sensord did not notice the SMGR restart"
wait_for "$T/sensord.log" "smgr ready: 4 sensor(s)" 100
[ "$(grep -c 'sensor 10 dt 0 rate 20 ' "$T/smgr.log")" -ge 2 ] || \
	{ sleep 2; [ "$(grep -c 'sensor 10 dt 0 rate 20 ' "$T/smgr.log")" -ge 2 ]; } || \
	fail "gyro report not re-added after restart"
B=$(grep -c '^{"sensor":"anglvel"' "$T/gyro")
sleep 1
A=$(grep -c '^{"sensor":"anglvel"' "$T/gyro")
[ "$A" -gt "$B" ] || fail "gyro stream did not resume after restart ($B -> $A)"
kill "$WG" 2>/dev/null
wait "$WG" 2>/dev/null

S=$(cli status)
echo "$S" | grep -q '"reg2_reads":3,"reg2_writes":1,"reg2_misses":1' || fail "status counters: $S"
echo "$S" | grep -q '"smgr_resets":[1-9]' || fail "status resets: $S"

# SIGTERM: reports still owned are deleted on the way out
bg_cli -n 100000 watch accel 10 >/dev/null
WA=$!
wait_for "$T/smgr.log" "sensor 0 dt 0 rate 10 " 30 || fail "accel 10 Hz report"
DEL_BEFORE=$(grep -c "DELETE report 1" "$T/smgr.log")
# SIGTERM ends the poll at once (self-pipe), not at the 5 s check timeout
kill "$SPID"
i=0
while kill -0 "$SPID" 2>/dev/null && [ "$i" -lt 10 ]; do sleep 0.1; i=$((i + 1)); done
kill -0 "$SPID" 2>/dev/null && fail "sensord still running 1 s after SIGTERM"
wait "$SPID" 2>/dev/null
SPID=
sleep 0.3
[ "$(grep -c "DELETE report 1" "$T/smgr.log")" -gt "$DEL_BEFORE" ] || fail "no delete on SIGTERM"
[ -e "$SOCK" ] && fail "socket left behind"
kill "$WA" 2>/dev/null
wait "$WA" 2>/dev/null
stop_pair

# ----------------------------------------------------- periodic (-P)
: >"$T/smgr.log"
start_pair "-P" "-P"
W=$(cli -n 3 watch anglvel 10)
[ "$(echo "$W" | grep -c '^{"sensor":"anglvel","t":[0-9]*,"x":1,"y":0.5,"z":9.8066')" -eq 3 ] \
	|| fail "periodic watch: $W"
grep -q "ADD report 2 sensor 10 dt 0 rate 10 periodic" "$T/smgr.log" || fail "periodic add"
stop_pair

# ------------------------------------------------------- -R (no REG2)
: >"$T/smgr.log"
./tests/sensord-fake -R -S "$SOCK" 2>"$T/noreg.log" &
SPID=$!
wait_for "$T/noreg.log" "not serving REG2" 30 || fail "-R not logged"
ls "$SENSORD_FAKE_IPC" | grep -q '^s271\.' && fail "-R still published REG2"
kill "$SPID"
wait "$SPID" 2>/dev/null
SPID=

# refuses to start on a registry whose size does not match the map
head -c 1000 "$T/reg" >"$T/short"
if ./tests/sensord-fake -r "$T/short" -m tests/fixtures/sns_reg.map -S "$SOCK" 2>"$T/short.log"; then
	fail "started with a short registry"
fi
grep -q "map wants 1024" "$T/short.log" || fail "short registry message: $(cat "$T/short.log")"

grep -q "DECOY" "$T/smgr.log" && fail "sensord talked to the wrong SMGR instance"

if [ "$FAILS" -ne 0 ]; then
	echo "--- sensord.log"; tail -n 40 "$T/sensord.log"
	echo "--- smgr.log"; tail -n 20 "$T/smgr.log"
	echo "test_e2e.sh: $FAILS failure(s)"
	exit 1
fi
echo "test_e2e.sh: all passed"
