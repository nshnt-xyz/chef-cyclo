#!/bin/sh
# Ride image status page (busybox httpd serves this for "/"). Plain text.
PATH=/sbin:/usr/sbin:/bin:/usr/bin
D=${RIDE_DIR:-/run/ride}
printf 'Content-Type: text/plain; charset=utf-8\r\n\r\n'
echo "chef-cyclo ride logger"
echo "uptime: $(cut -d' ' -f1 /proc/uptime) s   wallclock: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "battery: $(cat /sys/class/power_supply/battery/capacity 2>/dev/null)% $(cat /sys/class/power_supply/battery/status 2>/dev/null)"
echo "status: $(cat "$D/status" 2>/dev/null || echo 'no ride logger (no /run/ride)')"
echo "counters: $(cat "$D/counters" 2>/dev/null || echo none yet)"
echo "processes: gps-up=$(pidof gps-up || echo -) qmicli=$(pidof qmicli || echo -) ride-logger=$(pidof ride-logger || echo -) fblog=$(pidof fblog || echo -) fblog.off=$([ -e /run/fblog.off ] && echo yes || echo no)"
echo
echo "events.log (last 15):"
tail -n 15 "$D/events.log" 2>/dev/null | sed 's/^/  /'
echo
echo "last NMEA lines:"
tail -n 3 "$D/nmea.log" 2>/dev/null | sed 's/^/  /'
echo "last position:"
tail -n 1 "$D/positions.log" 2>/dev/null | sed 's/^/  /'
echo
echo "files:"
ls -l "$D" 2>/dev/null | sed 's/^/  /'
echo
echo "fetch everything:  curl -o ride.tgz http://172.16.42.1/cgi-bin/ride.tgz"
echo "single file:       curl -O http://172.16.42.1/log/nmea.log"
echo "stop logging:      curl http://172.16.42.1/cgi-bin/stop"
echo "screen:            curl 'http://172.16.42.1/cgi-bin/screen?on'   (or ?off)"
echo "shell:             telnet 172.16.42.1   (then: reboot)"
