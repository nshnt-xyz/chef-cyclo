#!/bin/sh
# End-to-end host test for the real tools/powerd binary (host build) on a
# fake power_supply tree: option parsing and rejection, the TEST OVERRIDE
# kmsg lines, the startup system_temp_level reset, SIGUSR1 samples, the
# throttle writing the fake sysfs attribute, `powerd status`, the injected
# shutdown command (-x) running once after the marker/state are written,
# and the clean-exit level reset; with -l, rows batched in log.pending,
# flushed by the poll timeout when the -F interval is up (no sample needed)
# and once on SIGTERM. The policy itself is covered in detail
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
check "ready line" 'grep -q "powerd: ready: $S -> $T/run, log $T/run, warn 15 %, critical 5 %, off at 0 % / 3300 mV / 68.0 C, throttle on" "$T/kmsg"'
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
"$BIN" -r "$S" -d "$T/run2" -l "$T/log2/power" -K "$T/kmsg" -V '' -x "true" --throttle-set 30 --throttle-clear 29 --step-ms 200 &
PID=$!
wait_for "second daemon throttles" 5 'grep -qx 2 "$S/battery/system_temp_level"'
check "-l: log in the log dir, state in the run dir" '[ -s "$T/log2/power/log.csv" ] && [ ! -e "$T/run2/log.csv" ] && [ -s "$T/run2/state" ]'
check "-l: start row carries the boot id" 'sed -n 2p "$T/log2/power/log.csv" | grep -Eq ",start,.*,[0-9a-f-]{36}\$"'
check "-l: ready line names the log dir" 'grep -q "powerd: ready: $S -> $T/run2, log $T/log2/power (batched every 600 s)," "$T/kmsg"'
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

# --- -l batching: SIGTERM flushes the pending rows once -----------------------
put pc_port online 0
put usb online 1
put usb real_type USB_CDP
put battery status Charging
put battery capacity 50
: > "$T/kmsg"
"$BIN" -r "$S" -d "$T/run4" -l "$T/log4" -K "$T/kmsg" -V '' -x true --no-throttle &
PID=$!
wait_for "batched daemon: start row flushed at once" 5 '[ "$(cat "$T/log4/log.csv" 2>/dev/null | wc -l)" = 2 ]'
kill -USR1 "$PID"
wait_for "signal row waits in log.pending" 5 'grep -q ",signal," "$T/run4/log.pending" 2>/dev/null'
check "signal row not yet in the log dir" '! grep -q ",signal," "$T/log4/log.csv"'
kill -TERM "$PID"; wait "$PID"; PID=
check "TERM: signal row flushed" 'grep -q ",signal," "$T/log4/log.csv" && [ "$(wc -l < "$T/log4/log.csv")" = 3 ]'
check "TERM: log.pending empty" '[ ! -s "$T/run4/log.pending" ]'

# --- -l batching: the flush deadline wakes the loop between samples ------------
# 1 h polls, no throttle ticks: only the -F 1 deadline can flush the row.
"$BIN" -r "$S" -d "$T/run5" -l "$T/log5" -F 1 -K "$T/kmsg" -V '' -x true --no-throttle --poll-s 3600 &
PID=$!
wait_for "second batched daemon up" 5 '[ "$(cat "$T/log5/log.csv" 2>/dev/null | wc -l)" = 2 ]'
kill -USR1 "$PID"
wait_for "signal row flushed by the -F 1 deadline" 4 'grep -q ",signal," "$T/log5/log.csv"'
check "only the start and signal rows" '[ "$(wc -l < "$T/log5/log.csv")" = 3 ] && [ ! -s "$T/run5/log.pending" ]'
"$BIN" -r "$S" -d "$T/run6" -l "$T/log6" -F x >/dev/null 2>&1
check "bad -F exits 64" '[ $? -eq 64 ]'
kill -TERM "$PID"; wait "$PID"; PID=

# --- -l batching: the SIGTERM exit flush is fsynced (file and directory) -------
if command -v strace >/dev/null 2>&1 &&
   strace -f -qq -e trace=fsync -o /dev/null true 2>/dev/null; then
	strace -f -qq -e trace=fsync -o "$T/strace.out" \
		"$BIN" -r "$S" -d "$T/run8" -l "$T/log8" -K "$T/kmsg" -V '' -x true --no-throttle &
	PID=$!
	wait_for "traced daemon: start row" 5 '[ "$(cat "$T/log8/log.csv" 2>/dev/null | wc -l)" = 2 ]'
	kill -USR1 "$(pgrep -P "$PID" -x powerd || echo "$PID")"
	wait_for "traced: signal row pending" 5 'grep -q ",signal," "$T/run8/log.pending" 2>/dev/null'
	check "no fsync before the exit flush" '! grep -q "fsync(" "$T/strace.out"'
	kill -TERM "$(pgrep -P "$PID" -x powerd || echo "$PID")"; wait "$PID"; PID=
	check "TERM: exit flush fsyncs log.csv and the directory" '[ "$(grep -c "fsync(.*= 0" "$T/strace.out")" = 2 ]'
	check "TERM: row on disk" 'grep -q ",signal," "$T/log8/log.csv"'
else
	echo "test_powerd.sh: strace unavailable: the SIGTERM exit fsync is NOT covered (test-powerd builds without main)" >&2
fi

# --- flush: the rows a stopped powerd left in log.pending (chef-state) ---------
mkdir -p "$T/run7" "$T/log7"
printf '%s\n' "1.0,x,poll,a" "2.0,x,poll,b" > "$T/run7/log.pending"
printf '3.0,x,po' >> "$T/run7/log.pending"
: > "$T/kmsg"
"$BIN" -d "$T/run7" -l "$T/log7" -K "$T/kmsg" flush
check "flush exits 0" '[ $? -eq 0 ]'
check "flush: header and the two complete rows" '[ "$(head -c 7 "$T/log7/log.csv")" = "uptime," ] && [ "$(sed -n 2,3p "$T/log7/log.csv" | tr "\n" " ")" = "1.0,x,poll,a 2.0,x,poll,b " ]'
check "flush: torn tail dropped, pending empty" '! grep -q "^3.0" "$T/log7/log.csv" && [ ! -s "$T/run7/log.pending" ]'
"$BIN" -d "$T/run7" -l "$T/log7" -K "$T/kmsg" flush
check "flush with nothing pending is a no-op" '[ $? -eq 0 ] && [ "$(wc -l < "$T/log7/log.csv")" = 3 ]'
printf 'uptime,utc,reason\n9,x,start\n' > "$T/log7/log.csv"
printf '%s\n' "4.0,x,poll,c" > "$T/run7/log.pending"
"$BIN" -d "$T/run7" -l "$T/log7" -K "$T/kmsg" flush
check "flush: another header rotated away" '[ "$(cat "$T/log7/log.csv.1")" = "$(printf "uptime,utc,reason\n9,x,start")" ] && grep -q "^4.0,x,poll,c" "$T/log7/log.csv"'
i=0; : > "$T/run7/log.pending"
while [ $i -lt 30 ]; do printf '%s,x,poll,%0100d\n' "$i" 0 >> "$T/run7/log.pending"; i=$((i + 1)); done
"$BIN" -d "$T/run7" -l "$T/log7" -L 2048 -K "$T/kmsg" flush
check "flush: rotated at -L, both files within it" '[ "$(wc -c < "$T/log7/log.csv")" -le 2048 ] && [ "$(wc -c < "$T/log7/log.csv.1")" -le 2048 ] && grep -q "^29,x,poll" "$T/log7/log.csv"'
printf '%s\n' "5.0,x,poll,d" > "$T/run7/log.pending"
"$BIN" -d "$T/run7" -l "$T/missing" -K "$T/kmsg" flush
check "flush into a missing log dir: exit 1, rows kept, one kmsg line" '[ $? -eq 1 ] && grep -q "^5.0" "$T/run7/log.pending" && grep -q "powerd: flush: log.csv in $T/missing: .*rows left in $T/run7/log.pending" "$T/kmsg" && [ ! -e "$T/missing" ]'
"$BIN" -d "$T/run7" -K "$T/kmsg" flush >/dev/null 2>&1
check "flush without -l exits 64" '[ $? -eq 64 ]'

echo "test_powerd.sh: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
