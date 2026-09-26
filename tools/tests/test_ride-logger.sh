#!/bin/sh
# Host regression test for initramfs-ride/usr/bin/ride-logger (the ride
# image's unattended GPS logger) and its HTTP extraction CGIs.
#
# gps-up and qmicli are replaced by stubs (RIDE_GPS_UP / RIDE_QMICLI), the
# kernel log, vibrator, fblog flag, battery and uptime by files in a
# throwaway tree, and every timeout is shortened through the RIDE_* knobs,
# so the supervision and failure paths run in seconds:
#   1. happy path: bring-up order, first-fix milestones, stamped NMEA,
#      position summaries, raw stream preserved byte for byte, heartbeat,
#      screen idle, HTTP status/file/tarball/stop/screen through a real
#      busybox httpd, clean stop (LOC stop, CID release, gps-up TERM)
#   2. TERM signal stop
#   3. gps-up dies before the socket appears -> FAILED, exit 1
#   4. gps-up dies while logging -> FAILED, screen woken, follower killed
#   5. follower dies once -> restarted; dies every time -> FAILED after the bound
#   6. follower stalls -> restarted
#   7. bridge never answers -> FAILED after the retry bound
#   8. --loc-set-nmea-types fails -> logging continues, status says so
#   9. second start while /run/ride exists -> refused
# The logger runs under busybox sh (the phone's shell) when available.
set -u
cd "$(dirname "$0")/../.."
LOGGER=$PWD/initramfs-ride/usr/bin/ride-logger
WWW=$PWD/initramfs-ride/usr/share/ride/www
[ -r "$LOGGER" ] || { echo "cannot find $LOGGER" >&2; exit 1; }
if busybox sh -c true 2>/dev/null; then SH="busybox sh"; else SH=sh; fi

T=$(mktemp -d /tmp/ride-test.XXXXXX)
trap 'cleanup' EXIT
cleanup() {
	exec 8>&- 9>&-
	[ -n "${KMSG_CAT:-}" ] && kill "$KMSG_CAT" "$VIB_CAT" 2>/dev/null
	[ -n "${LOGGER_PID:-}" ] && kill "$LOGGER_PID" 2>/dev/null
	[ -n "${HTTPD_PID:-}" ] && kill "$HTTPD_PID" 2>/dev/null
	pkill -f "$T/bin/stub-follow" 2>/dev/null
	pkill -f "$T/bin/stub-gps-up" 2>/dev/null
	rm -rf "$T"
}

pass=0; fail=0
ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
check() { if eval "$2"; then ok; else bad "$1"; fi; }
wait_for() {
	# wait_for <desc> <seconds> <shell test>
	i=0
	while [ "$i" -lt $(($2 * 10)) ]; do
		if eval "$3"; then ok; return 0; fi
		sleep 0.1; i=$((i + 1))
	done
	bad "$1 (timeout ${2}s)"
	return 1
}

# --- stubs -------------------------------------------------------------------
mkdir -p "$T/bin"
cat > "$T/bin/stub-gps-up" <<STUB
#!/bin/sh
# Modes via \$T/gps-mode: ok | die-early | die-later
T=$T
STUB
cat >> "$T/bin/stub-gps-up" <<'STUB'
mode=$(cat "$T/gps-mode")
echo "stub gps-up: mode $mode"
trap 'echo "gps-up: TERM"; rm -f "$T/qmux_socket"; echo term > "$T/gps-up.term"; exit 0' TERM
[ "$mode" = die-early ] && { echo "gps-up: dying early"; exit 3; }
sleep 0.5
python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1])' "$T/qmux_socket"
echo "gps-up: modem up, qmux bridge on $T/qmux_socket"
if [ "$mode" = die-later ]; then
	while [ ! -e "$T/gps-die-now" ]; do sleep 0.1; done
	echo "gps-up: rmtfs exited; stopping modem safely"; rm -f "$T/qmux_socket"; exit 1
fi
while :; do sleep 0.2; done
STUB
cat > "$T/bin/stub-qmicli" <<STUB
#!/bin/sh
# Records every call, answers like libqmi 1.38.0's qmicli does for the
# steps ride-logger uses.
T=$T
STUB
cat >> "$T/bin/stub-qmicli" <<'STUB'
echo "$*" >> "$T/qmicli.calls"
case " $* " in
*" --version "*) echo "qmicli 1.38.0"; exit 0 ;;
*" --get-service-version-info "*)
	n=$(cat "$T/ready-fails" 2>/dev/null || echo 0)
	if [ "$n" -gt 0 ]; then echo $((n - 1)) > "$T/ready-fails"; echo "error: Transaction timed out" >&2; exit 1; fi
	printf "[%s] Supported versions:\n\tctl (1.5)\n\tloc (2.0)\n" "$T/qmux_socket"; exit 0 ;;
*" --loc-noop --client-no-release-cid "*)
	printf "[%s] Client ID not released:\n\tService: 'loc'\n\t    CID: '7'\n" "$T/qmux_socket"; exit 0 ;;
*" --loc-noop "*) echo "released"; exit 0 ;;
*" --loc-set-nmea-types="*)
	[ -e "$T/nmea-fail" ] && { echo "error: couldn't set NMEA types: QMI protocol error (3): 'Internal'" >&2; exit 1; }
	echo "[$T/qmux_socket] Successfully set NMEA types"; exit 0 ;;
*" --loc-get-nmea-types "*) echo "Successfully retrieved NMEA types: gga, rmc, gsv, gsa, vtg"; exit 0 ;;
*" --loc-start "*) echo "[$T/qmux_socket] Successfully started location gathering"; echo started >> "$T/loc-start.calls"; exit 0 ;;
*" --loc-stop "*) echo "[$T/qmux_socket] Successfully stopped location gathering"; echo stopped >> "$T/loc-stop.calls"; exit 0 ;;
*" --loc-follow-nmea --loc-follow-position-report "*)
	exec python3 "$T/bin/stub-follow" "$T" ;;
esac
echo "stub qmicli: unexpected args: $*" >&2; exit 2
STUB
cat > "$T/bin/stub-follow" <<'STUB'
#!/usr/bin/env python3
# The follower stream (stands in for `qmicli --loc-follow-nmea
# --loc-follow-position-report`). Emits Qualcomm + standard NMEA and position
# blocks, no fix for the first 6 lines, then a fix. Copies every emitted line
# to $T/emitted for the byte-for-byte comparison with follow.raw. Handles
# SIGINT/SIGTERM with a real handler, as qmicli does (a shell stub could not:
# the logger starts it as a background job with SIGINT ignored), and flushes
# stdout per line, as g_print does.
# Controls: $T/follow-die (exit 1 after 30 lines, file removed unless
# $T/follow-die-always), $T/follow-stall (stop emitting after 30 lines).
import os, signal, sys, time
T = sys.argv[1]
runs = 0
try: runs = int(open(T + '/follow-runs').read())
except Exception: pass
open(T + '/follow-runs', 'w').write(str(runs + 1))
def on_int(*a): sys.stderr.write('follow: INT\n'); sys.stderr.flush(); sys.exit(0)
def on_term(*a): sys.stderr.write('follow: TERM\n'); sys.stderr.flush(); sys.exit(0)
signal.signal(signal.SIGINT, on_int)
signal.signal(signal.SIGTERM, on_term)
em = open(T + '/emitted', 'a')
def emit(l):
    sys.stdout.write(l + '\n'); sys.stdout.flush()
    em.write(l + '\n'); em.flush()
def pos(status, lat, lon, tech, unc, spd, sats):
    for l in ['[position report] status: ' + status,
              '   latitude:  %s degrees' % lat,
              '   longitude: %s degrees' % lon,
              '   circular horizontal position uncertainty:            n/a',
              '   horizontal elliptical uncertainty (semi-minor axis): 7070360.000000 meters',
              '   horizontal elliptical uncertainty (semi-major axis): %s meters' % unc,
              '   horizontal elliptical uncertainty azimuth:           n/a',
              '   horizontal confidence: n/a',
              '   horizontal reliability: medium',
              '   horizontal speed: ' + spd,
              '   speed uncertainty: n/a',
              '   altitude w.r.t. ellipsoid: 0.000000 meters',
              '   altitude w.r.t. mean sea level: -18.000000 meters',
              '   vertical uncertainty: n/a',
              '   vertical confidence: n/a',
              '   vertical reliability: medium',
              '   vertical speed: n/a',
              '   heading: 91.5',
              '   heading uncertainty: n/a',
              '   magnetic deviation: n/a',
              '   technology: ' + tech,
              '   position DOP:   0.000000',
              '   horizontal DOP: 0.000000',
              '   vertical DOP:   0.000000',
              '   UTC timestamp: 1789727728930 ms',
              '   Leap seconds: n/a',
              '   GPS time: n/a',
              '   time uncertainty: n/a',
              '   time source: unknown',
              '   sensor data usage: none',
              '   Fix count: 1',
              '   Satellites used: ' + sats,
              '   Altitude assumed: n/a']:
        emit(l)
i = 0
while True:
    i += 1
    if os.path.exists(T + '/follow-die') and i > 30:
        if not os.path.exists(T + '/follow-die-always'): os.unlink(T + '/follow-die')
        sys.stderr.write('follow: dying\n'); sys.stderr.flush(); sys.exit(1)
    if os.path.exists(T + '/follow-stall') and i > 30:
        while True: time.sleep(0.2)
    k = i % 6
    if k == 1: emit('$PQWM1,2436,470124228,1,7,22525130,0,672,8,-8,186,186,110,113,74,70,0,0,0, -2.66,  2.07,  0.00,  0.00*00')
    elif k == 2: emit('$GPGGA,103506.00,,,,,0,00,99.99,,,,,,*48' if i <= 6 else '$GPGGA,103507.00,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47')
    elif k == 3: emit('$GPRMC,103506.00,V,,,,,,,180926,,,N*7C' if i <= 6 else '$GNRMC,103507.00,A,4807.038,N,01131.000,E,12.3,84.4,180926,,,A*6A')
    elif k == 4: emit('$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74')
    elif k == 5:
        if i <= 6: pos('in-progress', '0.000000', '0.000000', 'cellular', '7070360.000000', 'n/a', 'n/a')
        else: pos('success', '48.117300', '11.516667', 'satellite', '12.500000', '3.400000 m/s', '8')
    else: emit('$GPVTG,84.4,T,,M,12.3,N,22.8,K,A*3D')
    time.sleep(0.1)
STUB
chmod 755 "$T/bin"/stub-*
printf '%s\n' 42 > "$T/capacity"; printf '%s\n' Discharging > "$T/status"
# sysfs fallback values (no powerd) and a powerd state file for case 1
printf '%s\n' 3987000 > "$T/voltage_now"; printf '%s\n' -412000 > "$T/current_now"; printf '%s\n' 315 > "$T/temp"
mkdir -p "$T/usbpsy" "$T/pcpsy"; printf '%s\n' 0 > "$T/usbpsy/online"; printf '%s\n' 0 > "$T/pcpsy/online"
printf 'uptime=10.0\nstatus=Charging\nsource=usb\nvoltage_mv=4106\ncurrent_ma=103\ntemp_c=32.0\n' > "$T/power-state.powerd"

# --- per-case environment ----------------------------------------------------
reset_tree() {
	rm -rf "$T/ride" "$T/qmux_socket" "$T/fblog.off" "$T/emitted" "$T/qmicli.calls" \
	       "$T/gps-up.term" "$T/follow-runs" "$T/follow-die" "$T/follow-die-always" "$T/follow-stall" \
	       "$T/loc-start.calls" "$T/loc-stop.calls" "$T/nmea-fail" "$T/ready-fails" "$T/gps-die-now"
	echo "${1:-ok}" > "$T/gps-mode"
	: > "$T/kmsg.log"; : > "$T/vib.log"   # truncate, never unlink: the collectors hold them open
	LOGGER_PID=
}
# The logger writes /dev/kmsg and the vibrator with a fresh `>` open per
# line (device semantics). A FIFO with a resident dummy writer (fd 8/9) and a
# collector keeps every line instead of the last one.
mkfifo "$T/kmsg" "$T/vib"
cat < "$T/kmsg" >> "$T/kmsg.log" & KMSG_CAT=$!
cat < "$T/vib" >> "$T/vib.log" & VIB_CAT=$!
exec 8> "$T/kmsg" 9> "$T/vib"
start_logger() {
	RIDE_DIR=$T/ride RIDE_QMICLI=$T/bin/stub-qmicli RIDE_GPS_UP=$T/bin/stub-gps-up \
	RIDE_QMUX_SOCKET=$T/qmux_socket RIDE_KMSG=$T/kmsg RIDE_VIBRATOR=$T/vib RIDE_FBLOG_OFF=$T/fblog.off \
	RIDE_BATTERY=$T RIDE_USB_PSY=$T/usbpsy RIDE_PC_PSY=$T/pcpsy RIDE_POWER_STATE=${POWER_STATE:-$T/no-power-state} RIDE_SETTLE_S=0 RIDE_SOCKET_WAIT_S=3 RIDE_READY_TRIES=3 RIDE_QMI_TIMEOUT_S=5 \
	RIDE_STALL_S=2 RIDE_SCREEN_OFF_AFTER_S=${SCREEN_OFF:-2} RIDE_FOLLOW_MAX_FAST_FAILS=3 RIDE_FOLLOW_FAST_S=5 \
	RIDE_HEARTBEAT_S=1 $SH "$LOGGER" > "$T/logger.out" 2>&1 &
	LOGGER_PID=$!
}
wait_logger() {
	# wait_logger <seconds>; sets rc to the exit code or "running"
	i=0
	while kill -0 "$LOGGER_PID" 2>/dev/null && [ "$i" -lt $(($1 * 10)) ]; do sleep 0.1; i=$((i + 1)); done
	if kill -0 "$LOGGER_PID" 2>/dev/null; then rc=running; else wait "$LOGGER_PID"; rc=$?; fi
}
status() { cat "$T/ride/status" 2>/dev/null; }
ev() { grep -q "$1" "$T/ride/events.log" 2>/dev/null; }
no_stubs_left() {
	! pgrep -f "$T/bin/stub-follow" >/dev/null && ! pgrep -f "$T/bin/stub-gps-up" >/dev/null
}

echo "test_ride-logger: logger under '$SH'"

# --- 1. happy path + HTTP ----------------------------------------------------
reset_tree ok
POWER_STATE=$T/power-state.powerd start_logger
wait_for "1: status LOGGING" 15 '[ "$(status)" = LOGGING ]'
check "1: bring-up order in events.log" '
	awk "/gps-up-start/{a=NR} /qmux-socket/{b=NR} /bridge-ready/{c=NR} /loc-cid 7/{d=NR} /nmea-types-set gga\\|rmc\\|gsv\\|gsa\\|vtg/{e=NR} /loc-start session=1/{f=NR} /follow-start n=1/{g=NR}
	     END{exit !(a&&b&&c&&d&&e&&f&&g && a<b && b<c && c<d && d<e && e<f && f<g)}" "$T/ride/events.log"'
check "1: set-nmea-types used the shared CID" 'grep -q -- "--client-cid=7 --client-no-release-cid --loc-set-nmea-types=gga|rmc|gsv|gsa|vtg" "$T/qmicli.calls"'
check "1: follower used the shared CID with both follow modes" 'grep -q -- "--client-cid=7 --client-no-release-cid --loc-follow-nmea --loc-follow-position-report" "$T/qmicli.calls"'
check "1: meta.txt has start uptime, qmicli version and nmea types" 'grep -q "^ride-logger start: uptime [0-9]" "$T/ride/meta.txt" && grep -q "qmicli 1.38.0" "$T/ride/meta.txt" && grep -q "gga|rmc" "$T/ride/meta.txt"'
sleep 0.3
check "1: kmsg got the OK line" 'grep -q "^ride: OK: logging" "$T/kmsg.log"'
check "1: two short buzzes on start" '[ "$(grep -c "^150$" "$T/vib.log")" = 2 ]'
wait_for "1: first-gga-fix milestone" 10 'ev "first-gga-fix quality=1 sats=08 utc=103507.00 lat=4807.038N lon=01131.000E alt=545.4"'
wait_for "1: first-rmc-valid milestone" 5 'ev "first-rmc-valid utc=103507.00 date=180926 lat=4807.038N lon=01131.000E"'
wait_for "1: first-position-fix milestone" 5 'ev "first-position-fix lat=48.117300 lon=11.516667 alt_msl=-18.000000 unc_m=12.500000 tech=satellite utc_ms=1789727728930"'
check "1: first-nmea milestone before first-gga-fix" 'awk "/first-nmea \\\$PQWM1/{a=NR} /first-gga-fix/{b=NR} END{exit !(a&&b&&a<b)}" "$T/ride/events.log"'
check "1: loc-start precedes the fix (TTFF reference)" 'awk "/loc-start session/{a=NR} /first-gga-fix/{b=NR} END{exit !(a&&b&&a<b)}" "$T/ride/events.log"'
check "1: nmea.log lines are uptime-stamped sentences" 'grep -q "^[0-9][0-9]*\.[0-9][0-9]* \$GPGGA,103506" "$T/ride/nmea.log" && ! grep -q "position report" "$T/ride/nmea.log"'
check "1: nmea.log carries no non-NMEA lines" '! grep -qv "^[0-9][0-9]*\.[0-9]* \\$" "$T/ride/nmea.log"'
check "1: positions.log summarises reports" 'grep -q "^[0-9][0-9.]* status=in-progress lat=0.000000 lon=0.000000 alt_msl=-18.000000 speed=n/a heading=91.5 unc_m=7070360.000000 tech=cellular utc_ms=1789727728930 sats=n/a$" "$T/ride/positions.log" && grep -q "status=success lat=48.117300 lon=11.516667 alt_msl=-18.000000 speed=3.400000 heading=91.5 unc_m=12.500000 tech=satellite utc_ms=1789727728930 sats=8$" "$T/ride/positions.log"'
sleep 0.3
check "1: kmsg announces the fixes" 'grep -q "^ride: GPS FIX (GGA quality 1, 08 sats)" "$T/kmsg.log" && grep -q "^ride: POSITION FIX lat=48.117300 lon=11.516667" "$T/kmsg.log"'
wait_for "1: heartbeat with counters" 5 'grep -q "heartbeat nmea=[1-9][0-9]* pos=[1-9][0-9]* gga_fix=[1-9][0-9]* pos_ok=[1-9][0-9]* batt=42% power=Charging,usb,4106mV,103mA,32.0C$" "$T/ride/events.log"'
check "1: meta.txt has the powerd power line" 'grep -q "^power: Charging,usb,4106mV,103mA,32.0C " "$T/ride/meta.txt"'
wait_for "1: screen idled after the delay" 5 '[ -e "$T/fblog.off" ] && ev "screen-off"'
check "1: kmsg says how to wake the screen" 'grep -q "^ride: idling the screen now (wake: rm $T/fblog.off; kill" "$T/kmsg.log"'

# HTTP extraction through a real busybox httpd against a copy of the www tree
# whose /log symlink points at this test's ride dir.
if busybox httpd --help >/dev/null 2>&1 && command -v curl >/dev/null 2>&1; then
	mkdir -p "$T/www"; cp -a "$WWW/cgi-bin" "$T/www/"; ln -sfn "$T/ride" "$T/www/log"
	PORT=18642
	RIDE_DIR=$T/ride RIDE_FBLOG_OFF=$T/fblog.off busybox httpd -f -p 127.0.0.1:$PORT -h "$T/www" > "$T/httpd.log" 2>&1 &
	HTTPD_PID=$!
	sleep 0.5
	U=http://127.0.0.1:$PORT
	check "1h: status page via /" 'curl -fsS "$U/" > "$T/index.txt" && grep -q "^status: LOGGING$" "$T/index.txt" && grep -q "^counters: nmea=" "$T/index.txt" && grep -q "first-gga-fix" "$T/index.txt" && grep -q "cgi-bin/ride.tgz" "$T/index.txt"'
	check "1h: /log/nmea.log serves the file unchanged" 'curl -fsS "$U/log/nmea.log" > "$T/nmea.http" && head -c "$(wc -c < "$T/nmea.http")" "$T/ride/nmea.log" | cmp -s - "$T/nmea.http"'
	check "1h: /log/nmea.log honours Range" 'curl -fsS -r 0-9 "$U/log/nmea.log" > "$T/range.http" && [ "$(wc -c < "$T/range.http")" = 10 ] && head -c 10 "$T/ride/nmea.log" | cmp -s - "$T/range.http"'
	check "1h: ride.tgz is a tarball of the log dir" 'curl -fsS -D "$T/tgz.hdr" "$U/cgi-bin/ride.tgz" > "$T/ride.tgz" && grep -qi "^Content-Type: application/gzip" "$T/tgz.hdr" && grep -qi "attachment; filename=\"ride-" "$T/tgz.hdr" && mkdir -p "$T/x" && tar -xzf "$T/ride.tgz" -C "$T/x" && [ -s "$T/x/ride/events.log" ] && [ -s "$T/x/ride/meta.txt" ] && [ -s "$T/x/ride/nmea.log" ] && [ -s "$T/x/ride/positions.log" ] && [ -s "$T/x/ride/follow.raw" ] && [ -s "$T/x/ride/gps-up.log" ] && [ -s "$T/x/ride/supervisor.log" ] && [ -s "$T/x/ride/qmicli-loc-alloc.txt" ]'
	check "1h: tarball follow.raw is a prefix of the live emitted stream" 'head -c "$(wc -c < "$T/x/ride/follow.raw")" "$T/emitted" | cmp -s - "$T/x/ride/follow.raw"'
	check "1h: screen?on wakes" 'curl -fsS "$U/cgi-bin/screen?on" | grep -q "fblog.off=absent" && [ ! -e "$T/fblog.off" ]'
	check "1h: screen?off idles" 'curl -fsS "$U/cgi-bin/screen?off" | grep -q "fblog.off=present" && [ -e "$T/fblog.off" ]'
	check "1h: cgi-bin/stop stops the logger cleanly" 'curl -fsS "$U/cgi-bin/stop" > "$T/stop.txt" && grep -q "^status: STOPPED (stop-file)$" "$T/stop.txt"'
	kill "$HTTPD_PID" 2>/dev/null; wait "$HTTPD_PID" 2>/dev/null; HTTPD_PID=
else
	echo "skip: no busybox httpd or curl on this host; HTTP checks not run"
	touch "$T/ride/stop"
fi
wait_logger 15
check "1: logger exit 0 after stop" '[ "$rc" = 0 ]'
check "1: status STOPPED (stop-file)" '[ "$(status)" = "STOPPED (stop-file)" ]'
check "1: teardown order: follower INT, loc-stop, release, gps-up TERM" '
	grep -q "follow: INT" "$T/ride/follow.err" && [ -e "$T/loc-stop.calls" ] && [ -e "$T/gps-up.term" ] &&
	grep -q -- "--client-cid=7 --client-no-release-cid --loc-stop --loc-session-id=1" "$T/qmicli.calls" &&
	grep -q -- "--client-cid=7 --loc-noop" "$T/qmicli.calls" &&
	awk "/follow-reader-eof/{a=NR} /qmicli-loc-stop exit=0/{b=NR} /qmicli-loc-release exit=0/{c=NR} /exit rc=0/{d=NR} END{exit !(a&&b&&c&&d&&a<b&&b<c&&c<d)}" "$T/ride/events.log"'
check "1: follow.raw is byte-identical to what the follower emitted" 'cmp -s "$T/emitted" "$T/ride/follow.raw"'
check "1: screen woken on stop" '[ ! -e "$T/fblog.off" ]'
sleep 0.3
check "1: one long-ish stop buzz" 'grep -q "^300$" "$T/vib.log"'
check "1: no stub processes left" 'no_stubs_left'
check "1: gps-up started exactly once" '[ "$(grep -c "stub gps-up: mode" "$T/ride/gps-up.log")" = 1 ]'

# --- 2. TERM signal ------------------------------------------------------------
reset_tree ok
SCREEN_OFF=0 start_logger
wait_for "2: LOGGING" 15 '[ "$(status)" = LOGGING ]'
sleep 0.5
kill -TERM "$LOGGER_PID"
wait_logger 15
check "2: exit 0 on TERM" '[ "$rc" = 0 ]'
check "2: STOPPED (signal)" '[ "$(status)" = "STOPPED (signal)" ]'
check "2: gps-up got TERM, loc stopped" '[ -e "$T/gps-up.term" ] && [ -e "$T/loc-stop.calls" ]'
check "2: screen never idled with RIDE_SCREEN_OFF_AFTER_S=0" '! ev "screen-off"'
check "2: sysfs power fallback without powerd (unplugged)" 'grep -q "^power: Discharging,battery,3987mV,-412mA,31.5C " "$T/ride/meta.txt" && grep -q "power=Discharging,battery,3987mV,-412mA,31.5C$" "$T/ride/events.log"'
check "2: no stub processes left" 'no_stubs_left'

# --- 3. gps-up dies before the socket ------------------------------------------
reset_tree die-early
printf '%s\n' 1 > "$T/pcpsy/online"	# SDP host port: usb/online 0, pc_port/online 1
start_logger
wait_logger 15
sleep 0.3
printf '%s\n' 0 > "$T/pcpsy/online"
check "3: exit 1" '[ "$rc" = 1 ]'
check "3: sysfs fallback sees an SDP input on pc_port" 'grep -q "^power: Discharging,usb,3987mV" "$T/ride/meta.txt"'
check "3: FAILED status names gps-up" 'case "$(status)" in "FAILED: gps-up exited (rc 3) before the qmux socket"*) true ;; *) false ;; esac'
check "3: kmsg FAILED line and evidence hint" 'grep -q "^ride: FAILED: gps-up exited" "$T/kmsg.log" && grep -q "^ride: evidence stays in" "$T/kmsg.log"'
check "3: three long buzzes" '[ "$(grep -c "^600$" "$T/vib.log")" = 3 ]'
check "3: no qmicli setup attempted" '! grep -q -- "--loc-noop" "$T/qmicli.calls" 2>/dev/null'
check "3: no stub processes left" 'no_stubs_left'

# --- 4. gps-up dies while logging ----------------------------------------------
reset_tree die-later
start_logger
wait_for "4: LOGGING" 15 '[ "$(status)" = LOGGING ]'
wait_for "4: screen idled" 5 '[ -e "$T/fblog.off" ]'
touch "$T/gps-die-now"
wait_logger 15
sleep 0.3
check "4: exit 1" '[ "$rc" = 1 ]'
check "4: FAILED: modem lifetime ended" 'case "$(status)" in "FAILED: gps-up exited (rc 1): modem lifetime ended"*) true ;; *) false ;; esac'
check "4: screen woken on failure" '[ ! -e "$T/fblog.off" ] && ev "screen-on"'
check "4: follower interrupted" 'grep -q "follow: INT" "$T/ride/follow.err"'
check "4: no loc-stop attempted on a dead modem" '[ ! -e "$T/loc-stop.calls" ]'
check "4: three long buzzes" '[ "$(grep -c "^600$" "$T/vib.log")" = 3 ]'
check "4: no stub processes left" 'no_stubs_left'

# --- 5. follower dies ------------------------------------------------------------
reset_tree ok
touch "$T/follow-die"
SCREEN_OFF=0 start_logger
wait_for "5a: LOGGING" 15 '[ "$(status)" = LOGGING ]'
wait_for "5a: follower restarted once" 15 'ev "follow-start n=2" && [ "$(cat "$T/follow-runs")" = 2 ]'
check "5a: follow-exit recorded with rc 1" 'ev "follow-exit rc=1 ran="'
wait_for "5a: still logging, fix found on run 2" 10 '[ "$(status)" = LOGGING ] && ev "first-gga-fix"'
check "5a: loc-start not repeated (session persists across follower restarts)" '[ "$(wc -l < "$T/loc-start.calls")" = 1 ]'
touch "$T/ride/stop"
wait_logger 15
check "5a: clean stop after a restart" '[ "$rc" = 0 ] && [ "$(status)" = "STOPPED (stop-file)" ]'
check "5a: no stub processes left" 'no_stubs_left'

reset_tree ok
touch "$T/follow-die" "$T/follow-die-always"
SCREEN_OFF=0 start_logger
wait_logger 30
check "5b: exit 1 after repeated follower deaths" '[ "$rc" = 1 ]'
check "5b: FAILED names the follower bound" '[ "$(status)" = "FAILED: follower died 3 times in a row within 5s" ]'
check "5b: exactly 3 follower runs" '[ "$(cat "$T/follow-runs")" = 3 ]'
check "5b: gps-up still terminated cleanly" '[ -e "$T/gps-up.term" ] && [ -e "$T/loc-stop.calls" ]'
check "5b: no stub processes left" 'no_stubs_left'

# --- 6. follower stalls -----------------------------------------------------------
reset_tree ok
touch "$T/follow-stall"
SCREEN_OFF=0 start_logger
wait_for "6: LOGGING" 15 '[ "$(status)" = LOGGING ]'
wait_for "6: stall detected and follower restarted" 15 'ev "follow-stall 2s" && ev "follow-start n=2"'
check "6: stalled follower was interrupted" 'grep -q "follow: INT" "$T/ride/follow.err"'
rm -f "$T/follow-stall"
touch "$T/ride/stop"
wait_logger 15
check "6: clean stop" '[ "$rc" = 0 ]'
check "6: no stub processes left" 'no_stubs_left'

# --- 7. bridge never ready ---------------------------------------------------------
reset_tree ok
echo 99 > "$T/ready-fails"
start_logger
wait_logger 30
check "7: exit 1" '[ "$rc" = 1 ]'
check "7: FAILED after the retry bound" '[ "$(status)" = "FAILED: bridge not answering the version query after 3 tries" ]'
check "7: three version queries" '[ "$(grep -c -- "--get-service-version-info" "$T/qmicli.calls")" = 3 ]'
check "7: gps-up terminated" '[ -e "$T/gps-up.term" ]'
check "7: no stub processes left" 'no_stubs_left'

# --- 8. nmea types fail ------------------------------------------------------------
reset_tree ok
touch "$T/nmea-fail"
SCREEN_OFF=0 start_logger
wait_for "8: logging continues with a warning status" 15 '[ "$(status)" = "LOGGING (nmea-types failed: only \$PQW* sentences)" ]'
sleep 0.3
check "8: kmsg warning" 'grep -q "^ride: WARNING: --loc-set-nmea-types=gga|rmc|gsv|gsa|vtg failed" "$T/kmsg.log"'
check "8: still buzzed OK" '[ "$(grep -c "^150$" "$T/vib.log")" = 2 ]'
touch "$T/ride/stop"
wait_logger 15
check "8: clean stop" '[ "$rc" = 0 ]'

# --- 9. double start -----------------------------------------------------------------
reset_tree ok
mkdir -p "$T/ride"
start_logger
wait_logger 5
sleep 0.3
check "9: refused while the dir exists" '[ "$rc" = 1 ] && grep -q "already running or stale" "$T/kmsg.log"'
check "9: nothing started" '[ ! -e "$T/qmicli.calls" ] && no_stubs_left'

echo "test_ride-logger: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
