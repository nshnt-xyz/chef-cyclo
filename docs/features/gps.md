# GPS, modem services, and gpsd

[Feature index](README.md) · [Build instructions](../building.md)

## Current status

Modem boot, LOC discovery/start/stop, NMEA streaming, and outdoor fixes were live-verified by 2026-09-18. The terrace run measured ≤78 seconds cold time-to-first-fix and 5 m uncertainty at the first position fix. gpsd received a matching 3D fix on 2026-09-19.

The baseline keeps GPS manual while a demand-driven manager is pending. This is no longer waiting on initial modem/LOC verification. The [temporary ride image](ride-logging.md) owns an automatic logging lifecycle, but does not run the gpsd/broker pipeline described here.

## Components and lifecycle

`initramfs/usr/bin/gps-up` starts the modem dependencies, exposes `/run/qmux_socket`, and owns cleanup. Its helpers are:

- `tools/msmipc.{c,h}`, `tools/qrtr/`: AF_MSM_IPC adaptation and QMI support.
- `tools/irsc.c`: opens the IPC-router security gate before clients send.
- `tools/rmtfs/`: EFS service using read-only partition input and RAM-shadow writes.
- `tools/servreg-locator/`: service-registry replies from firmware domain descriptors.
- `tools/tftp/`: bounded TFTP/RFS service with read-only firmware and a RAM-backed writable namespace seeded from the stock persist backup at build time.
- `tools/qmux.{c,h}`, `tools/qmuxd-lite.c`: the bridge used by stock Alpine `qmicli`.
- `tools/nmea-broker.c`: follower stdout to checksum-verified NMEA UDP datagrams for gpsd.

The modem needs both service-registry and TFTP/RFS support to survive startup. `gps-up` does not automatically restart a crashed modem. Do not run competing owners or reopen the modem repeatedly after failure; retain evidence and reboot.

## Start and inspect manually

Use a fresh baseline boot and the phone's telnet shell. Start one `gps-up`. If the socket does not appear, inspect `/run/gps-up.log` and stop the wait rather than launching another copy. Proceed only once the service-version query succeeds. Substitute the allocated CID below; it is not guaranteed to be 1.

```sh
gps-up > /run/gps-up.log 2>&1 &
while [ ! -S /run/qmux_socket ]; do sleep 1; done; sleep 2
qmicli -d /run/qmux_socket --get-service-version-info > /dev/null   # bridge ready (retry if it times out)
qmicli -d /run/qmux_socket --loc-noop --client-no-release-cid         # note the CID
CID=1   # replace 1 with the CID returned above
qmicli -d /run/qmux_socket --client-cid=$CID --client-no-release-cid --loc-set-nmea-types='gga|rmc|gsv|gsa|vtg'
qmicli -d /run/qmux_socket --client-cid=$CID --client-no-release-cid --loc-start --loc-session-id=1
gpsd -N -n -b -D2 udp://127.0.0.1:20175 > /run/gpsd.log 2>&1 &
mkdir -p /run/gps
qmicli -d /run/qmux_socket --client-cid=$CID --client-no-release-cid --loc-follow-nmea 2> /run/gps/follow.err \
  | nmea-broker -t -l /run/gps/nmea.log > /run/gps/follow.raw 2> /run/gps/broker.log &
(printf '?WATCH={"enable":true,"json":true}\n'; sleep 15) | nc 127.0.0.1 2947 | tee /run/gps/gpsd-watch.json
```

The recorded live test received `DEVICES`, populated `SKY`, and `TPV mode:3`, with position/time/speed/track matching raw NMEA. A missing fix indoors is not proof of a transport failure; inspect satellites and raw reports. Logs in `/run/gps` are RAM-only.

## Stop cleanly

Interrupt the running `qmicli --loc-follow-nmea` process and let the broker drain/exit. Using the same allocated CID, send `--loc-stop` with `--loc-session-id=1` and `--client-no-release-cid`; then release that CID with `--loc-noop` without `--client-no-release-cid`. Stop gpsd, then send TERM to the `gps-up` process so its cleanup stops helpers, closes the modem, and unmounts firmware. Confirm the helpers and `/run/qmux_socket` are gone. The ride image supplies its own [stop endpoint](ride-logging.md#retrieve-logs-and-stop).

## Broker, time, and client behavior

gpsd is packaged in the rootfs and runs as `nobody`. Keep its client listener loopback-only (no `-G`). `initramfs/init` must configure `lo`; without it gpsd bind and broker sends fail.

The broker sends one `$…*hh\r\n` UDP datagram per checksum-verified sentence to 127.0.0.1:20175. It discards position-report blocks, chatter, and invalid checksums. A FIFO is unsuitable for this feed because gpsd treats non-tty files as replay and stops at EOF. UDP tolerates independent process restarts but packets sent without a listener are lost.

`-t` tees input unchanged; `-l FILE` writes `<uptime> <sentence>`. The terrace feed delivered GGA/RMC/GSA/VTG and three GSV sentences per 1 Hz cycle, with `$GP` talkers; GLONASS also appeared in QMI position reports. Empty pre-fix sentences still had valid checksums.

On the first valid status-A RMC, the broker sets system time once per process only if the clock is before 2000-01-01; pre-2020 RMC dates are rejected. `-n` disables this. It never writes the RTC. Initial gpsd bogus-time warnings are expected before that step. This kernel's `shmget` returns `ENOSYS`; JSON clients work, but chrony SHM integration needs kernel support or a different handoff.

## Transport and storage constraints

- **QMI is over `AF_MSM_IPC`, not QRTR**, on this 4.4 tree (`net/qrtr` doesn't exist; `net/ipc_router` does) — libqmi/ModemManager's QRTR/rpmsg path doesn't apply. `tools/msmipc.c` adapts the upstream `libqrtr` API onto `AF_MSM_IPC` so `tools/rmtfs/` and libqmi-side code don't need QRTR-specific ports.

- **A client `sendto()` on the modem's IPC-router socket blocks 30s** until a root process has run the IRSC ioctl once per boot and closed that socket (`tools/irsc.c` / `msmipc_irsc()`); `gps-up` runs it before starting `rmtfs`.

- **`AF_MSM_IPC` signals flow-control (RESUME_TX) as a 0-byte `recvfrom()`**, not a decodable packet — don't treat it as EOF or feed it to a QMI decoder.

- **`rmtfs -r` (RAM-shadow writes) is non-negotiable** until EFS writes are explicitly reviewed and approved: `gps-up` never mounts a partition read-write or writes `modemst1/2`/`fsc`/the active slot's `fsg_a`/`fsg_b` directly.

- **`fsg` is genuinely per-slot on this device — `fsg_a`/`fsg_b` only, no unsuffixed `fsg`** (confirmed against the live partition table; `modemst1`/`modemst2`/`fsc` *are* unsuffixed). An earlier version of `gps-up` assumed all of EFS was unsuffixed and deliberately never passed `rmtfs -S`; that assumption was wrong for `fsg` and would have made `rmtfs` unable to open it at all. `gps-up` now validates the active slot's `fsg_$SLOT` (exact partition, exact 10485760-byte size, from `androidboot.slot_suffix`) before exposing it, passes `rmtfs -P -r -S "$SLOT" -v`, and never inspects, links, sizes, or opens the other slot's `fsg`.

- **The modem's `msadp` debug-policy firmware file never exists**, and with `FW_LOADER_USER_HELPER_FALLBACK=y` every modem boot would otherwise stall 60s waiting for it — `gps-up` sets `/sys/class/firmware/timeout` to `1` before opening `/dev/subsys_modem` so this doesn't require a kernel config change.

Write modem sysfs control values without a trailing newline where required: the earlier newline bug prevented PIL startup. `AF_MSM_IPC` RMTFS requests carry sector offsets; preserve the corrected offset handling. QMUX CTL response flags differ from service flags; the old one-byte flag error made `qmicli` time out even though the modem was healthy. Detailed failures and fixes remain in the [build log](../build-log.md).

## Modify and verify

Run `make -C tools test` (which includes the GPS startup and ride-logger shell suites), plus the relevant component suites: `make -C tools/rmtfs test`, `make -C tools/servreg-locator test`, and `make -C tools/tftp test`. The tests cover transport/bridge framing, storage offsets, slot validation and lifecycle safeguards, service replies, bounded TFTP behavior, and broker filtering/framing/time handling. Rebuild rootfs if packages change, then initramfs and boot image.

Live checks: modem survives startup, services are discovered, QMUX answers, LOC starts, outdoor NMEA/gpsd fixes agree, and teardown leaves no owned helpers. Capture image hashes and logs; preserve the no-writes-to-EFS/persist policy and verify it with appropriate evidence. Precise GPS archives may exist only locally; the public history retains redacted summaries.

The [GPS and time plan](../next-steps/gps-and-time.md) owns manager leases, supervision, time synchronization, GNSS-only RF, and warm starts. Nested carrier MCFG paths remain outside the current TFTP allowlist and are not a GPS blocker.
