#!/bin/sh
# Host integration test for tools/gps-manager: the real daemon with stub
# qmicli and gpsd scripts (named so their comm matches), the real
# nmea-broker, a fake modem owner (this shell, with its own start time) and
# a real Unix socket as the QMUX bridge. Covers the single-instance lock, a
# full lease -> RUNNING -> release -> grace -> ordered teardown cycle, a
# kill -9 of the manager (with the follower quiet, so follower and broker
# can only die through PR_SET_PDEATHSIG; the TERM-ignoring gpsd stub
# survives like the real privilege-dropping gpsd), the respawned instance
# cleaning up the survivor and releasing the old CID, a kill -9 during the
# grace period, the GPS switch (disable while leased: torn down at once,
# saved through a stub chef-state, read back by a respawned instance, new
# leases refused; a slow gps-load: leases wait, then the default, and it
# is bounded even after a client set the switch; a slow save drained at
# SIGTERM; a hung save killed after 10 s and the newer value saved), an
# orderly SIGTERM, and the broker's own exit when its tee reader is gone
# (EPIPE). Only recorded pids are ever signalled.
set -eu
cd "$(dirname "$0")/.."
GM=$PWD/build/host/gps-manager
BROKER=$PWD/build/host/nmea-broker
[ -x "$GM" ] && [ -x "$BROKER" ] || { echo "build gps-manager and nmea-broker first" >&2; exit 1; }

T=$(mktemp -d /tmp/gps-manager-it.XXXXXX)
# no syslog socket (never the host's /dev/log): log lines go to kmsg,
# which a non-root test cannot write
export CHEF_LOG_SOCKET="$T/no-syslog"
PIDS=
cleanup() {
	for p in $PIDS; do kill -9 "$p" 2>/dev/null || true; done
	for f in "$T"/run/state "$T"/run/state.1; do
		[ -f "$f" ] || continue
		for p in $(awk '$1 != "cid" && $1 != "owner" { print $2 }' "$f"); do kill -9 "$p" 2>/dev/null || true; done
	done
	rm -rf "$T"
}
trap cleanup EXIT

pass=0
fail=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
check() { if eval "$2"; then ok; else bad "$1"; fi; }

mkdir -p "$T/bin" "$T/lock" "$T/run"
cat > "$T/bin/qmicli" <<EOF
#!/bin/sh
echo "\$*" >> "$T/qmicli.log"
case " \$* " in
*" --loc-follow-nmea "*)
	trap 'exit 0' INT
	while :; do
		if [ ! -e "$T/quiet" ]; then
			printf '\$GPGGA,120000.00,,,,,0,00,99.9,,,,,,*48\r\n'
			printf '\$GPRMC,120000.00,A,0000.000,N,00000.000,E,0.0,,041026,,,A*55\r\n'
		fi
		sleep 0.2
	done ;;
*" --client-cid="*) echo ok ;;
*" --loc-noop --client-no-release-cid "*|*" --client-no-release-cid --loc-noop "*)
	n=\$(( \$(cat "$T/cid" 2>/dev/null || echo 0) + 1 ))
	echo \$n > "$T/cid"
	printf "[x] Client ID not released:\n\tService: 'loc'\n\t    CID: '%d'\n" \$n ;;
esac
exit 0
EOF
cat > "$T/bin/gpsd" <<'EOF'
#!/bin/sh
trap '' TERM
while :; do sleep 0.2; done
EOF
cat > "$T/bin/chef-state" <<EOF
#!/bin/sh
echo "\$*" >> "$T/chef-state.log"
case "\$1" in
gps-load) echo \$\$ > "$T/loader.pid"; [ ! -e "$T/load-slow" ] || exec sleep 30
	[ ! -e "$T/load-child" ] || exec "$T/bin/load-child"
	cat "$T/gps-enabled" 2>/dev/null || echo "enabled 1" ;;
gps-save) echo \$\$ > "$T/saver.pid"; [ ! -e "$T/save-hang" ] || exec sleep 30
	if [ -e "$T/save-hang-child" ]; then
		# as chef-state's locked(): a descendant holds the lock and hangs
		( flock 9; sh -c 'echo \$\$ > "$T/child.pid"; exec sleep 30' ) 9> "$T/save.lock" &
		wait
		exit 1
	fi
	[ ! -e "$T/save-slow" ] || sleep 1
	( flock 9; echo "enabled \$2" > "$T/gps-enabled" ) 9> "$T/save.lock" ;;
esac
EOF
chmod +x "$T/bin/qmicli" "$T/bin/gpsd" "$T/bin/chef-state"
ln -s "$BROKER" "$T/bin/nmea-broker"

# fake owner = this shell; fake bridge = a bound Unix socket
start=$(awk 'sub(/^.*\) /, "") { print $20 }' /proc/$$/stat)
echo "$$ $start" > "$T/lock/owner"
touch "$T/lock/ready"
python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1])' "$T/qmux"

S=$T/sock
run_gm() {
	"$GM" -S "$S" -d "$T/run" -q "$T/qmux" -o "$T/lock" -b "$T/bin" -s "$T/bin/chef-state" -g 2 -v \
		2>> "$T/gm.err" &
	GMPID=$!
	PIDS="$PIDS $GMPID"
	i=0; until [ -S "$S" ] && "$GM" -S "$S" status > /dev/null 2>&1; do
		i=$((i + 1)); [ $i -lt 100 ] || { bad "manager did not come up"; return 1; }; sleep 0.1
	done
}
state() { "$GM" -S "$S" status 2>/dev/null | sed -n 's/^{"type":"status","state":"\([A-Z]*\)".*/\1/p'; }
wait_state() {	# wait_state STATE SECONDS
	i=0; until [ "$(state)" = "$1" ]; do
		i=$((i + 1)); [ $i -lt $(($2 * 10)) ] || return 1; sleep 0.1
	done
}
rec() { awk -v k="$1" '$1 == k { print $2 }' "$T/run/state" 2>/dev/null; }
alive() { [ -n "$1" ] && kill -0 "$1" 2>/dev/null && ! grep -q '^State:.*Z' "/proc/$1/status" 2>/dev/null; }
comm() { cat "/proc/$1/comm" 2>/dev/null; }

# --- lock, idle start
rm -f "$S"
run_gm
check "starts OFF" '[ "$(state)" = OFF ]'
rc=0; "$GM" -S "$T/other.sock" -d "$T/run" -q "$T/qmux" -o "$T/lock" -b "$T/bin" > /dev/null 2> "$T/second.err" || rc=$?
check "second instance refused" '[ $rc = 1 ] && grep -q "already running" "$T/second.err" && [ ! -e "$T/other.sock" ]'
check "first instance unharmed" '[ "$(state)" = OFF ]'

# --- lease, RUNNING, children recorded and named
"$GM" -S "$S" hold map > "$T/hold1.out" &
H=$!; PIDS="$PIDS $H"
check "lease reaches RUNNING" 'wait_state RUNNING 10'
F=$(rec follower); B=$(rec broker); G=$(rec gpsd)
check "children recorded and alive" 'alive "$F" && alive "$B" && alive "$G"'
check "children names" '[ "$(comm "$F")" = qmicli ] && [ "$(comm "$B")" = nmea-broker ] && [ "$(comm "$G")" = gpsd ]'
check "CID 1 recorded with the owner" '[ "$(rec cid)" = 1 ] && [ "$(rec owner)" = $$ ]'
check "bring-up order" 'grep -n "" "$T/qmicli.log" | tr "\n" " " | grep -q "1:-d .*--client-no-release-cid --loc-noop 2:-d .*--client-cid=1 --client-no-release-cid --loc-set-nmea-types=gga|rmc|gsv|gsa|vtg 3:-d .*--client-cid=1 --client-no-release-cid --loc-start --loc-session-id=1 4:-d .*--client-cid=1 --client-no-release-cid --loc-follow-nmea"'
check "children inherit only stdio" '[ "$(ls /proc/$F/fd | wc -l)" -le 4 ] && [ "$(ls /proc/$B/fd | wc -l)" -le 4 ]'
check "holder got its lease, then watch, no hello" 'head -n 3 "$T/hold1.out" | tr "\n" " " | grep -q "^{\"type\":\"ok\",\"cmd\":\"lease\",\"kind\":\"map\",\"held\":1} {\"type\":\"ok\",\"cmd\":\"watch\"} {\"type\":\"switch\",\"enabled\":true} $" && ! grep -q "\"hello\"" "$T/hold1.out"'
check "watcher saw the states" 'grep -q "^{\"type\":\"state\",\"state\":\"RUNNING\"," "$T/hold1.out"'
check "watcher saw the age and the first fix of the bring-up" 'grep -Eq "^\{\"type\":\"state\",\"state\":\"RUNNING\",\"detail\":\"CID 1, fix\",\"up_ms\":[0-9]+,\"fix_ms\":[0-9]+\}$" "$T/hold1.out"'
check "every watcher line is one JSON object" 'python3 -c "import json,sys; [json.loads(l) for l in open(sys.argv[1])]" "$T/hold1.out"'
rc=0; "$GM" -S "$S" retry > "$T/retry.out" || rc=$?
check "an error reply: printed, exit 1" '[ $rc = 1 ] && [ "$(cat "$T/retry.out")" = "{\"type\":\"error\",\"cmd\":\"retry\",\"reason\":\"not failed\"}" ]'

# --- release: grace, ordered teardown (gpsd needs its KILL), OFF
kill "$H"
check "teardown ends OFF" 'wait_state OFF 20'
check "follower, broker and gpsd gone" '! alive "$F" && ! alive "$B" && ! alive "$G"'
check "loc-stop then release of CID 1" 'tail -n 2 "$T/qmicli.log" | tr "\n" " " | grep -q -- "--client-cid=1 --client-no-release-cid --loc-stop --loc-session-id=1 -d .* --client-cid=1 --loc-noop $"'
check "state file has no CID" '[ "$(rec cid)" = -1 ] && [ -z "$(rec gpsd)" ]'
check "logs rotated per bring-up" '[ -f "$T/run/steps.log" ] && grep -q -- "--loc-stop" "$T/run/steps.log"'

# --- kill -9 of the manager while leased
"$GM" -S "$S" hold ride > "$T/hold2.out" &
H=$!; PIDS="$PIDS $H"
check "second lease RUNNING" 'wait_state RUNNING 10'
check "second bring-up kept the first logs" '[ -f "$T/run/steps.log.1" ]'
F=$(rec follower); B=$(rec broker); G=$(rec gpsd)
check "fresh CID 2" '[ "$(rec cid)" = 2 ]'
# quiet follower: no writes, so no EPIPE/SIGPIPE; only PR_SET_PDEATHSIG ends them
touch "$T/quiet"
sleep 0.5
kill -9 "$GMPID"
sleep 1
rm -f "$T/quiet"
check "follower and broker died with the manager" '! alive "$F" && ! alive "$B"'
check "gpsd stub survived like the real one" 'alive "$G"'
check "holder saw the manager go" 'i=0; while alive "$H" && [ $i -lt 20 ]; do sleep 0.1; i=$((i+1)); done; ! alive "$H"'
check "state still names CID 2 and gpsd" '[ "$(rec cid)" = 2 ] && [ "$(rec gpsd)" = "$G" ]'
lines=$(wc -l < "$T/qmicli.log")
run_gm
check "respawn killed the old gpsd" 'i=0; while alive "$G" && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; ! alive "$G"'
check "respawn stopped and released CID 2" 'wait_state OFF 10 && tail -n +$((lines + 1)) "$T/qmicli.log" | tr "\n" " " | grep -q -- "--client-cid=2 --client-no-release-cid --loc-stop --loc-session-id=1 -d .* --client-cid=2 --loc-noop $"'
check "nothing recorded after the respawn cleanup" 'i=0; until [ "$(rec cid)" = -1 ] || [ $i -ge 50 ]; do sleep 0.1; i=$((i+1)); done; [ "$(rec cid)" = -1 ]'

# --- kill -9 during the grace period
"$GM" -S "$S" hold map > "$T/hold3.out" &
H=$!; PIDS="$PIDS $H"
check "third lease RUNNING with CID 3" 'wait_state RUNNING 10 && [ "$(rec cid)" = 3 ]'
G=$(rec gpsd)
kill "$H"
sleep 0.5
check "in grace" '"$GM" -S "$S" status | grep -q "\"grace\":true,"'
kill -9 "$GMPID"
run_gm
check "after kill -9 in grace: OFF, gpsd gone, CID 3 released" 'i=0; while alive "$G" && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; ! alive "$G" && wait_state OFF 10 && grep -q -- "--client-cid=3 --loc-noop" "$T/qmicli.log"'

# --- the GPS switch
"$GM" -S "$S" hold map > "$T/hold5.out" &
H=$!; PIDS="$PIDS $H"
check "switch: leased and RUNNING" 'wait_state RUNNING 10'
F=$(rec follower); B=$(rec broker); G=$(rec gpsd); C=$(rec cid)
check "switch: disable answers" '[ "$("$GM" -S "$S" disable)" = "{\"type\":\"ok\",\"cmd\":\"disable\",\"enabled\":false}" ]'
check "switch: torn down at once, no grace" 'wait_state DISABLED 10 && ! alive "$F" && ! alive "$B" && ! alive "$G" && grep -q -- "--client-cid=$C --loc-noop" "$T/qmicli.log"'
check "switch: the lease is held, the state says disabled" '"$GM" -S "$S" status | grep -q "\"state\":\"DISABLED\",\"enabled\":false,\"leases\":{\"map\":1,"'
check "switch: saved" 'i=0; until grep -q "enabled 0" "$T/gps-enabled" 2>/dev/null || [ $i -ge 30 ]; do sleep 0.1; i=$((i+1)); done; [ "$(cat "$T/gps-enabled")" = "enabled 0" ]'
check "switch: watchers told" 'grep -q "^{\"type\":\"switch\",\"enabled\":false}$" "$T/hold5.out" && grep -q "^{\"type\":\"state\",\"state\":\"DISABLED\"," "$T/hold5.out"'
"$GM" -S "$S" hold ride > "$T/hold6.out" &
H2=$!; PIDS="$PIDS $H2"
check "switch: a new lease is refused" 'i=0; until grep -q "\"reason\":\"disabled\"" "$T/hold6.out" || [ $i -ge 30 ]; do sleep 0.1; i=$((i+1)); done; grep -q "^{\"type\":\"error\",\"cmd\":\"lease\",\"reason\":\"disabled\"}$" "$T/hold6.out"'
kill "$H2"
check "switch: still DISABLED a while later" 'sleep 3; [ "$(state)" = DISABLED ] && [ "$(rec cid)" = -1 ]'
kill -9 "$GMPID"
run_gm
check "switch: read back after a respawn" 'wait_state DISABLED 5 && grep -q "^gps-load" "$T/chef-state.log"'
"$GM" -S "$S" hold map > "$T/hold7.out" &
H=$!; PIDS="$PIDS $H"
check "switch: refused after the respawn too" 'i=0; until grep -q "\"reason\":\"disabled\"" "$T/hold7.out" || [ $i -ge 30 ]; do sleep 0.1; i=$((i+1)); done; grep -q "\"reason\":\"disabled\"" "$T/hold7.out"'
kill "$H"
check "switch: enable" '[ "$("$GM" -S "$S" enable)" = "{\"type\":\"ok\",\"cmd\":\"enable\",\"enabled\":true}" ] && wait_state OFF 5'
check "switch: enabled saved" 'i=0; until grep -q "enabled 1" "$T/gps-enabled" || [ $i -ge 30 ]; do sleep 0.1; i=$((i+1)); done; [ "$(cat "$T/gps-enabled")" = "enabled 1" ]'
# a gps-load that hangs: leases wait, then the default (enabled) after 5 s
touch "$T/load-slow"
kill -9 "$GMPID"
run_gm
"$GM" -S "$S" hold map > "$T/hold8.out" &
H=$!; PIDS="$PIDS $H"
check "switch: while reading, the lease waits" 'sleep 1; "$GM" -S "$S" status > "$T/st8"; grep -q "\"state\":\"OFF\",\"enabled\":true,\"leases\":{\"map\":1,.*\"detail\":\"reading the GPS switch\"}$" "$T/st8" || { cat "$T/st8" >&2; false; }'
check "switch: after the 5 s bound, enabled and up" 'wait_state RUNNING 15'
rm -f "$T/load-slow"
kill "$H"
check "switch: back OFF" 'wait_state OFF 20'

# a client switch while gps-load still hangs: the loader is still bounded
touch "$T/load-slow"
kill -9 "$GMPID"
run_gm
check "switch: disable while reading answers" '[ "$("$GM" -S "$S" disable)" = "{\"type\":\"ok\",\"cmd\":\"disable\",\"enabled\":false}" ] && wait_state DISABLED 3'
LP=$(cat "$T/loader.pid")
check "switch: the hung loader is killed by its bound anyway" 'sleep 6; ! alive "$LP"'
rm -f "$T/load-slow"
check "switch: still DISABLED after the bound (the client won)" '[ "$(state)" = DISABLED ]'
# a slow save, then SIGTERM: the manager waits for it (bounded)
touch "$T/save-slow"
"$GM" -S "$S" enable > /dev/null
kill "$GMPID"
check "switch: SIGTERM drains the slow save" 'i=0; while alive "$GMPID" && [ $i -lt 60 ]; do sleep 0.1; i=$((i+1)); done; ! alive "$GMPID" && [ "$(cat "$T/gps-enabled")" = "enabled 1" ]'
rm -f "$T/save-slow"
# a save that hangs: killed after 10 s, the newer value saved after it
run_gm
check "switch: up after the drain" 'wait_state OFF 5'
touch "$T/save-hang"
"$GM" -S "$S" disable > /dev/null
i=0; until [ -s "$T/saver.pid" ] && alive "$(cat "$T/saver.pid")" || [ $i -ge 30 ]; do sleep 0.1; i=$((i+1)); done
SP=$(cat "$T/saver.pid")
rm -f "$T/save-hang"
"$GM" -S "$S" enable > /dev/null
check "switch: a hung save is killed after 10 s and the newer value saved" 'sleep 12; ! alive "$SP" && [ "$(cat "$T/gps-enabled")" = "enabled 1" ] && grep -q "gps-save still running after 10 s: killed" "$T/gm.err"'

# a save whose descendant holds the lock: the whole group goes at the
# bound, and the newer value then gets the lock
rm -f "$T/child.pid"
touch "$T/save-hang-child"
"$GM" -S "$S" disable > /dev/null
i=0; until [ -s "$T/child.pid" ] || [ $i -ge 30 ]; do sleep 0.1; i=$((i+1)); done
CP=$(cat "$T/child.pid")
rm -f "$T/save-hang-child"
"$GM" -S "$S" enable > /dev/null
check "switch: the hung save's lock holder dies with it, the newer value saved" 'alive "$CP" && sleep 12 && ! alive "$CP" && [ "$(cat "$T/gps-enabled")" = "enabled 1" ]'
# the same at orderly exit: no descendant outlives the manager
rm -f "$T/child.pid"
touch "$T/save-hang-child"
"$GM" -S "$S" disable > /dev/null
i=0; until [ -s "$T/child.pid" ] || [ $i -ge 30 ]; do sleep 0.1; i=$((i+1)); done
CP=$(cat "$T/child.pid")
rm -f "$T/save-hang-child"
kill "$GMPID"
check "switch: SIGTERM with a hung save: bounded exit, no descendant left" 'i=0; while alive "$GMPID" && [ $i -lt 80 ]; do sleep 0.1; i=$((i+1)); done; ! alive "$GMPID" && sleep 0.5 && ! alive "$CP" && grep -q "save unfinished" "$T/gm.err"'
run_gm
"$GM" -S "$S" enable > /dev/null
check "switch: saving works after that" 'sleep 1; [ "$(cat "$T/gps-enabled")" = "enabled 1" ]'
# a hung gps-load with a descendant: killed with its group at the bound
rm -f "$T/child.pid"
cat > "$T/bin/load-child" <<EOF2
#!/bin/sh
sh -c 'echo \$\$ > "$T/child.pid"; exec sleep 30' &
wait
EOF2
chmod +x "$T/bin/load-child"
touch "$T/load-child"
kill -9 "$GMPID"
run_gm
i=0; until [ -s "$T/child.pid" ] || [ $i -ge 30 ]; do sleep 0.1; i=$((i+1)); done
CP=$(cat "$T/child.pid")
check "switch: a hung loader's descendant dies at the 5 s bound" 'alive "$CP" && sleep 6 && ! alive "$CP" && wait_state OFF 2'
rm -f "$T/load-child"

# --- orderly SIGTERM while running
"$GM" -S "$S" hold map > "$T/hold4.out" &
H=$!; PIDS="$PIDS $H"
check "fourth lease RUNNING" 'wait_state RUNNING 10'
F=$(rec follower); B=$(rec broker); G=$(rec gpsd)
kill "$GMPID"
check "SIGTERM: manager exits after its teardown" 'i=0; while alive "$GMPID" && [ $i -lt 150 ]; do sleep 0.1; i=$((i+1)); done; ! alive "$GMPID"'
check "SIGTERM: everything stopped and CID 4 released" '! alive "$F" && ! alive "$B" && ! alive "$G" && grep -q -- "--client-cid=4 --loc-noop" "$T/qmicli.log" && [ ! -e "$T/run/state" ]'

# --- nmea-broker exits when its tee reader (the manager) is gone
mkfifo "$T/bin.in" "$T/bin.out"
"$BROKER" -n -t < "$T/bin.in" > "$T/bin.out" 2> /dev/null &
BP=$!; PIDS="$PIDS $BP"
exec 3> "$T/bin.in" 4< "$T/bin.out"
exec 4<&-
printf '$GPGGA,120000.00,,,,,0,00,99.9,,,,,,*48\r\n' >&3 || true
rc=0; wait "$BP" || rc=$?
exec 3>&-
check "broker exits 1 once its tee reader is gone" '[ $rc = 1 ]'

if [ "$fail" -eq 0 ]; then
	echo "PASS: $pass/$((pass + fail)) checks passed"
else
	echo "FAILED: $fail of $((pass + fail)) checks failed ($pass passed)" >&2
	sed 's/^/  gm: /' "$T/gm.err" >&2
	exit 1
fi
