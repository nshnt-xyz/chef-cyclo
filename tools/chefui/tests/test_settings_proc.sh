#!/bin/sh
# Process-level check of chef-settings with its persistence worker alive:
# SIGUSR1 must always reach the UI thread's signalfd (a capture), never a
# thread's default terminating action. Runs the host build offscreen.
#   tests/test_settings_proc.sh BIN STATE
set -u
BIN=$1 STATE=$2
d=$(mktemp -d /tmp/chef-settings-proc.XXXXXX)
pid=
cleanup() { [ -n "$pid" ] && kill -KILL "$pid" 2>/dev/null; rm -rf "$d"; }
trap cleanup EXIT
fail() { echo "test-settings-proc: FAIL: $*" >&2; sed 's/^/  log: /' "$d/log" >&2; exit 1; }

# --als-* values: usage error (exit 2) for NaN, huge, fractional or out of range holds,
# and dark not below light; nothing starts
for bad in "--als-light-hold nan" "--als-light-hold 1e30" "--als-dark-hold 2.5" "--als-dark-hold 601" \
	   "--als-light nan" "--als-light inf" "--als-light-hold 0" "--als-dark 3000"; do
	# shellcheck disable=SC2086
	SDL_VIDEODRIVER=offscreen "$BIN" $bad --data "$d/data0" --run "$d/run0" >/dev/null 2>&1
	[ $? -eq 2 ] || { echo "test-settings-proc: FAIL: '$bad' was not a usage error" >&2; exit 1; }
done

SDL_VIDEODRIVER=offscreen "$BIN" --fake-state "$STATE" --data "$d/data" --run "$d/run" 2>"$d/log" &
pid=$!
# the worker is alive and blocks signals 1..31 (all but KILL and STOP,
# which cannot be blocked; the C library keeps a few realtime ones)
w=
i=0
while [ -z "$w" ]; do
	for t in /proc/$pid/task/*; do
		[ "$(cat "$t/comm" 2>/dev/null)" = ui-store ] && w=$t
	done
	[ -n "$w" ] && break
	i=$((i + 1)); [ $i -lt 100 ] || fail "no ui-store thread"; sleep 0.05
done
m=$(awk '/^SigBlk/ {print $2}' "$w/status")
[ $(( 0x$m & 0x7ffbfeff )) -eq $(( 0x7ffbfeff )) ] || fail "worker does not block all signals: SigBlk $m"
# every thread blocks SIGUSR1 (bit 9 = 0x200)
for t in /proc/$pid/task/*; do
	m=$(awk '/^SigBlk/ {print $2}' "$t/status")
	[ $(( 0x$m & 0x200 )) -ne 0 ] || fail "thread $(cat "$t/comm") does not block SIGUSR1"
done

snap=$d/run/chef-settings/snapshot.ppm
n=0
while [ $n -lt 20 ]; do
	n=$((n + 1))
	rm -f "$snap"
	kill -USR1 "$pid"
	i=0
	until [ -s "$snap" ]; do
		kill -0 "$pid" 2>/dev/null || fail "died after SIGUSR1 $n"
		i=$((i + 1)); [ $i -lt 100 ] || fail "no capture after SIGUSR1 $n"; sleep 0.05
	done
	[ "$(head -c 2 "$snap")" = P6 ] || fail "capture $n is not a PPM"
done
kill -0 "$pid" 2>/dev/null || fail "not alive after 20 captures"
kill -TERM "$pid"
wait "$pid"
rc=$?
pid=
[ $rc -eq 143 ] || fail "exit code $rc after SIGTERM (want 143)"
echo "test-settings-proc: 20 SIGUSR1 captures with the worker alive, clean SIGTERM exit"
