# Temporary ride GPS logging

[Feature index](README.md) · [Build instructions](../building.md)

A separate, `fastboot boot`-only image that logs GPS unattended so the phone can be unplugged for a ride. **Everything it records lives in RAM (`/run/ride`, tmpfs): it is lost the moment the phone powers off, reboots or the battery dies. Pull the logs over USB *before* rebooting.** Nothing is flashed and nothing is written to any internal partition; `gps-up` runs exactly as in the baseline image (RAM-shadow EFS, read-only firmware mount).

```sh
scripts/mkride.sh                      # host tests, out/initramfs-ride.cpio.gz, out/boot-ride.img (+ sha256)
fastboot boot out/boot-ride.img        # never flash it; Power+VolDown returns to Android
```

## Startup and supervision

What happens after boot, all narrated on the panel (`ride:` lines through `fblog`) and in `/run/ride/supervisor.log`:

1. `ride-logger` (inittab `once`, never respawned) starts the one `gps-up` of this boot, waits for `/run/qmux_socket`, checks the bridge with `--get-service-version-info`, allocates a LOC client, sets NMEA types `gga|rmc|gsv|gsa|vtg`, starts session 1 and runs a single `qmicli --loc-follow-nmea --loc-follow-position-report` (one client, one process — host-verified to register both event kinds in one request).
2. **Success**: `ride: OK: logging …` on the panel, two short buzzes, `status` = `LOGGING`. 45 s later the panel is idled (`/run/fblog.off` + fblog restart, the documented protocol) to save power — the phone looks off but is logging. **Failure** at any point (gps-up dies, no socket, bridge silent, LOC start refused, follower dying repeatedly): `ride: FAILED: <reason>` on the panel, three long buzzes, the panel is woken again if it had been idled, `status` = `FAILED: …`, and the evidence stays in `/run/ride`. The modem is never restarted; a failed boot is simply rebooted.
3. During the ride the follower is supervised: it is restarted if it exits or produces nothing for 60 s (the LOC session persists in the modem); five deaths in a row within 30 s count as failure. A heartbeat with counters and battery % goes to `events.log` every 60 s.

## Retrieve logs and stop

Getting the logs back (reconnect USB while it is still booted; the host gets its address by DHCP as usual):

```sh
curl http://172.16.42.1/                              # status: state, counters, last events, last NMEA/position
curl -o ride.tgz http://172.16.42.1/cgi-bin/ride.tgz   # everything under /run/ride as a tarball
curl -O http://172.16.42.1/log/nmea.log               # or any single file (Range supported)
curl http://172.16.42.1/cgi-bin/stop                  # optional: stop cleanly (LOC stop, CID release, gps-up TERM)
curl 'http://172.16.42.1/cgi-bin/screen?on'           # wake the panel (or: telnet → rm /run/fblog.off; kill $(pidof fblog))
telnet 172.16.42.1                                    # recovery shell as always; `reboot` when done
```

## Recorded files

Files: `meta.txt` (image/kernel/battery/start uptime + wallclock — note the clock is 1970 until something sets it), `events.log` (`<uptime> <utc> <event>`: `gps-up-start`, `qmux-socket`, `bridge-ready`, `loc-start` (the time-to-first-fix reference), `first-nmea`, `first-gga-fix`, `first-rmc-valid`, `first-position-fix`, heartbeats, follower restarts, stop/failure), `nmea.log` (`<uptime> <sentence>`; `cut -d' ' -f2-` gives the pristine NMEA stream), `positions.log` (one line per position report), `follow.raw` (the follower's stdout byte for byte), `follow.err`, `qmicli-*.txt` (verbose raw frames of every setup/teardown step), `gps-up.log`, `supervisor.log`, `status`, `counters`.

## Limitations

The panel cannot be woken from the phone itself (no touch/key handling; use the host), `lpm_levels.sleep_disabled=1` is still on the cmdline so idle power is not optimised, and the modem stays powered for the whole ride. USB unplug/replug (NCM re-enumeration, DHCP, HTTP) was live-verified on the 2026-09-18 terrace run; cold time-to-first-fix measured there was ≤ 78 s with a 5 m first fix.

The ride overlay does not run `buttond`; baseline power-tap screen control and software hold-to-power-off do not apply. Its NMEA/position logging pipeline also does not include the baseline's manually started gpsd/broker path.

## Modify and verify

Sources: `initramfs-ride/usr/bin/ride-logger`, `initramfs-ride/etc/inittab`, and the HTTP CGIs under `initramfs-ride/usr/share/ride/www/`. Start from the prerequisites in [building](../building.md): `mkride.sh` expects the kernel, rootfs, toolchains, and stock inputs to exist.

Run `sh tools/tests/test_ride-logger.sh`. `scripts/mkride.sh` runs this suite and packs separate ride artifacts without replacing the baseline boot image. Live verification should cover automatic startup, screen idle/wake, follower supervision, USB unplug/replug, HTTP archive extraction, and clean stop. Reboot only after evidence has been downloaded.

Known recorded warts: the `bridge-ready services=N` counter undercounts (use `qmicli-version-info.txt`), and a running archive can include an empty `follow.fifo`. Exact traces are local-only evidence. See the [2026-09-18 ride-image build-log entries](../build-log.md), [GPS guide](gps.md), and [future persistent ride recorder](../next-steps/ui-and-ride-app.md).
