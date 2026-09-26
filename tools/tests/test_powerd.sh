#!/bin/sh
# End-to-end host test for the real tools/powerd binary (host build) on a
# fake power_supply tree: option parsing and rejection, the TEST OVERRIDE
# kmsg lines, the startup system_temp_level reset, SIGUSR1 samples, the
# throttle writing the fake sysfs attribute, `powerd status`, the injected
# shutdown command (-x) running once after the marker/state are written,
# and the clean-exit level reset. The policy itself is covered in detail
# by test_powerd.c; this checks the wiring around it. Nothing is powered
# off: -x only touches a file.
set -u
cd "$(dirname "$0")/.."
BIN=$PWD/powerd
[ -x "$BIN" ] || { echo "build tools/powerd first (make -C tools)" >&2; exit 1; }

T=$(mktemp -d /tmp/powerd-test.XXXXXX)
PID=
cleanup() {
	[ -n "$PID" ] && kill "$PID" 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT

pass=0; fail=0
ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
check() { if eval "$2"; then ok; else bad "$1"; fi; }
wait_for() {
	i=0
	while [ "$i" -lt $(($2 * 10)) ]; do
		if eval "$3"; then ok; return 0; fi
		sleep 0.1; i=$((i + 1))
	done
	bad "$1 (timeout ${2}s)"
	return 1
}

S=$T/sys
put() { mkdir -p "$S/$1"; echo "$3" > "$S/$1/$2"; }
put battery status Charging
put battery health Good
put battery charge_type Fast
put battery present 1
put battery capacity 79
put battery temp 320
put battery voltage_now 4106923
put battery current_now 103027
put battery system_temp_level 3
put battery num_system_temp_levels 8
put bms voltage_ocv 4102057
put bms charge_full 5004000
put bms cycle_count 0
put bms soc_reporting_ready 1
put usb online 1
put usb real_type USB_CDP
put usb current_max 1500000
put usb input_current_now 144531
put usb typec_mode 'Source attached (default current)'
put dc online 0
put pc_port online 0
: > "$T/kmsg"

# --- bad options are refused before anything runs -----------------------------
"$BIN" -r "$S" -d "$T/run" -K "$T/kmsg" --warn 101 >/dev/null 2>&1
check "bad --warn exits 64" '[ $? -eq 64 ]'
"$BIN" -r "$S" -d "$T/run" -K "$T/kmsg" --throttle-set 40 --throttle-clear 41 >/dev/null 2>&1
check "clear >= set exits 64" '[ $? -eq 64 ]'
"$BIN" -r "$S" -d "$T/run" -K "$T/kmsg" --warn 30 >/dev/null 2>&1
check "warn >= rearm exits 64" '[ $? -eq 64 ]'
"$BIN" -r "$S" -d "$T/run" -K "$T/kmsg" --warn 10 --critical 11 >/dev/null 2>&1
check "critical > warn exits 64" '[ $? -eq 64 ]'
"$BIN" -r "$S" -d "$T/run" -K "$T/kmsg" --overtemp nan >/dev/null 2>&1
check "--overtemp nan exits 64" '[ $? -eq 64 ]'
"$BIN" -d "$T/run" status >/dev/null 2>&1
check "status without a state file exits 1" '[ $? -eq 1 ]'
check "nothing created by the refused runs" '[ ! -e "$T/run" ]'

# --- daemon with test overrides ----------------------------------------------
"$BIN" -r "$S" -d "$T/run" -K "$T/kmsg" -V '' -x "echo \$(cat $T/run/shutdown-pending) > $T/shutdown-ran" \
	--throttle-set 30 --throttle-clear 29 --step-ms 200 --confirm-gap-ms 200 --poll-s 1 &
PID=$!
wait_for "state file appears" 5 '[ -s "$T/run/state" ]'
check "startup reset system_temp_level from the stale 3" 'grep -qx 0 "$S/battery/system_temp_level" || grep -qx 1 "$S/battery/system_temp_level"'
check "TEST OVERRIDE logged" 'grep -q "powerd: TEST OVERRIDE: shutdown-cmd throttle-set=30 throttle-clear=29 step-ms=200 confirm-gap-ms=200 poll-s=1" "$T/kmsg"'
check "ready line" 'grep -q "powerd: ready: $S -> $T/run, warn 15 %, critical 5 %, off at 0 % / 3300 mV / 68.0 C, throttle on" "$T/kmsg"'
check "start line" 'grep -q "powerd: start: Charging, 79 %, 4.10 V, battery 32.0 C, source usb USB_CDP" "$T/kmsg"'
# 32 C >= the 30 C test set point: steps to the top level through real sysfs writes
wait_for "throttle reaches 7 in the fake sysfs" 5 'grep -qx 7 "$S/battery/system_temp_level"'
check "throttle kmsg" 'grep -q "powerd: charge throttle level 1 of 7 (battery 32.0 C, above set point)" "$T/kmsg"'
"$BIN" -d "$T/run" status > "$T/status.out"
check "status prints the state" 'grep -qx "throttle_level=7" "$T/status.out" && grep -qx "overrides=shutdown-cmd throttle-set=30 throttle-clear=29 step-ms=200 confirm-gap-ms=200 poll-s=1" "$T/status.out"'

# unplug: level back to 0, transition logged
put usb online 0
put battery status Discharging
kill -USR1 "$PID"
wait_for "unplug resets the level" 5 'grep -qx 0 "$S/battery/system_temp_level"'
check "unplug kmsg" 'grep -q "powerd: unplugged: on battery (79 %, 4.10 V)" "$T/kmsg"'
check "status kmsg" 'grep -q "powerd: status Charging -> Discharging" "$T/kmsg"'
check "signal row in log.csv" 'grep -q ",signal,Discharging,battery," "$T/run/log.csv"'

# empty battery: confirmed on the 200 ms test gap, -x runs once
put battery capacity 0
kill -USR1 "$PID"
wait_for "shutdown command ran" 5 '[ -s "$T/shutdown-ran" ]'
check "marker text reached the command" 'grep -q "^battery empty: 0 %, 4.10 V (capacity; 3 samples)" "$T/shutdown-ran"'
check "SHUTDOWN kmsg" 'grep -q "powerd: SHUTDOWN: battery empty" "$T/kmsg"'
check "state says shutdown" 'grep -qx "alert=shutdown" "$T/run/state"'
sleep 1
check "shutdown command ran once" '[ "$(grep -c "SHUTDOWN" "$T/kmsg")" -eq 1 ]'
check "daemon still alive after -x" 'kill -0 "$PID"'

# clean exit
kill -TERM "$PID"
wait "$PID"
check "exit status 0" '[ $? -eq 0 ]'
PID=
check "exiting kmsg" 'grep -q "powerd: exiting" "$T/kmsg"'

# --- a stale level with nothing to throttle is reset on exit too ------------------
put usb online 1
put battery status Charging
put battery capacity 50
: > "$T/kmsg"
"$BIN" -r "$S" -d "$T/run2" -K "$T/kmsg" -V '' -x "true" --throttle-set 30 --throttle-clear 29 --step-ms 200 &
PID=$!
wait_for "second daemon throttles" 5 'grep -qx 2 "$S/battery/system_temp_level"'
kill -TERM "$PID"
wait "$PID"
PID=
check "exit writes level 0" 'grep -qx 0 "$S/battery/system_temp_level"'
check "no stale shutdown marker in a fresh run dir" '[ ! -e "$T/run2/shutdown-pending" ]'

# --- review B1 repro: SDP attached, not charging, 0 %: must NOT shut down -------
put usb online 0
put pc_port online 1
put usb real_type USB
put battery status 'Not charging'
put battery capacity 0
put battery present 1
put bms soc_reporting_ready 1
rm -f "$T/sdp-shutdown"
"$BIN" -r "$S" -d "$T/run3" -K "$T/kmsg" -V '' -x "touch $T/sdp-shutdown" --confirm-gap-ms 100 --no-throttle &
PID=$!
wait_for "SDP daemon up" 5 '[ -s "$T/run3/state" ]'
for i in 1 2 3 4 5; do kill -USR1 "$PID"; sleep 0.2; done
check "SDP + Not charging at 0 %: no shutdown" '[ ! -e "$T/sdp-shutdown" ] && grep -qx "alert=none" "$T/run3/state"'
check "SDP source is usb/USB" 'grep -qx "source=usb" "$T/run3/state" && grep -qx "usb_type=USB" "$T/run3/state"'
kill -TERM "$PID"; wait "$PID"; PID=

echo "test_powerd.sh: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
