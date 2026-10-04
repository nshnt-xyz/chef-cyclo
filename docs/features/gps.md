# GPS, modem services, and gpsd

[Feature index](README.md) · [Build instructions](../building.md)

## Current status

Modem boot, LOC discovery/start/stop, NMEA streaming, and outdoor fixes were live-verified by 2026-09-18. The terrace run measured ≤78 seconds cold time-to-first-fix and 5 m uncertainty at the first position fix. gpsd received a matching 3D fix on 2026-09-19.

The clean standard-NetworkManager baseline verifies one shared resident modem owner at boot. Baseline LOC acquisition and gpsd remain opt-in. Unattended logging was live-verified on a temporary ride image (2026-09-18, since dropped; see the [build log](../build-log.md)). Earlier device verification used manual modem startup.

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

Use a fresh baseline boot and the phone's telnet shell. New builds already start gps-up once. If readiness does not appear, inspect `/run/gps-up.log` rather than launching another copy. Proceed only once the service-version query succeeds. Substitute the allocated CID below; it is not guaranteed to be 1.

```sh
. /usr/lib/chef/modem-owner.sh
n=0; until modem_owner_pid >/dev/null; do
    n=$((n+1)); [ "$n" -lt 240 ] || break; sleep 1
done
modem_owner_pid >/dev/null || exit 1
qmicli -d /run/qmux_socket --get-service-version-info > /dev/null   # bridge ready (retry if it times out)
qmicli -d /run/qmux_socket --loc-noop --client-no-release-cid         # note the CID
CID=1   # replace 1 with the CID returned above
qmicli -d /run/qmux_socket --client-cid=$CID --client-no-release-cid --loc-set-nmea-types='gga|rmc|gsv|gsa|vtg'
qmicli -d /run/qmux_socket --client-cid=$CID --client-no-release-cid --loc-start --loc-session-id=1
gpsd -N -n -b -D2 udp://127.0.0.1:20175 > /run/gpsd.log 2>&1 &
mkdir -p /run/gps
qmicli -d /run/qmux_socket --client-cid=$CID --client-no-release-cid --loc-follow-nmea 2> /run/gps/follow.err \
  | nmea-broker -n -t -l /run/gps/nmea.log > /run/gps/follow.raw 2> /run/gps/broker.log &
(printf '?WATCH={"enable":true,"json":true}\n'; sleep 15) | nc 127.0.0.1 2947 | tee /run/gps/gpsd-watch.json
```

The recorded live test received `DEVICES`, populated `SKY`, and `TPV mode:3`, with position/time/speed/track matching raw NMEA. A missing fix indoors is not proof of a transport failure; inspect satellites and raw reports. Logs in `/run/gps` are RAM-only.

While the follower runs, send no other request on its CID (for example `--loc-get-gnss-sv-info` with `--client-cid=$CID`): the bridge then stops delivering NMEA to the follower. Query satellites before starting the follower or after stopping it. gpsd still reports `TPV mode:3` with the 1970 boot clock; it only logs `date ... more than a year in the future!` warnings until the clock is set.

## Stop cleanly

Interrupt the running `qmicli --loc-follow-nmea` process and let the broker drain/exit. Using the same allocated CID, send `--loc-stop` with `--loc-session-id=1` and `--client-no-release-cid`; then release that CID with `--loc-noop` without `--client-no-release-cid`. Stop gpsd and your follower/broker. Leave the shared gps-up owner running for Wi-Fi and other clients; do not signal or reap it. It performs support/modem cleanup at orderly system shutdown.

## Cellular RF

There is no SIM and no RIL, and nothing in the image changes the modem's operating mode. Measured on 2026-10-04 on baseline `12fc50c3` (see the [build log](../build-log.md)):

- **Boot default.** DMS reports the operating mode `shutting-down` (DMS mode 5 in libqmi) as soon as the bridge answers. It held until we changed it: 31 min on one fresh boot (through a LOC session) and 7.7 min on another. NAS reports `not-registered-searching`, system info shows no service on GSM, WCDMA or LTE, and signal info answers `InformationUnavailable`. UIM reports both slots absent, later `no-atr-received`. We found no authoritative source on why a RIL-less modem sits in `shutting-down`; this is observed behavior.
- **Idle current.** One unattended run of eight 180 s blocks (two boot-default blocks first, then `low-power` and `online` alternating; panel off, USB out, Wi-Fi radio off, BT as booted, no LOC session, 1 Hz `current_now` with the first 20 s of each block skipped, about 97 mA total, 34 to 37 C). The boot default matched `low-power`: -0.7 mA on block means (standard error 1.3), -0.5 mA on medians (0.4). So the `searching` label shows no measurable current cost; it does not prove the RF is off. Explicit `online` cost +2.8 mA on block means (standard error 0.9; adjacent pairs 3.8, 3.3 and 1.8 mA) and +0.9 mA on medians, mostly from more bursts above 150 mA. A linear drift of +0.2 mA per block is fitted out, but the default blocks all came first, so their comparison is order-confounded with it.
- **`low-power` keeps GNSS and Wi-Fi.** In `low-power` (volatile, not written to NV: every fresh boot came back in `shutting-down`) NAS reads `not-registered`. At a window a LOC session tracked 24 SVs and gave status-A RMC plus gpsd `TPV mode:3` (the modem's own GSA reported a 2D fix; warm starts, first status-A RMC 2.4 to 15.8 s after LOC start), and NetworkManager associated on 5 GHz with ping 5/5. Three switches (`online`, `low-power`, `online`) with Wi-Fi associated kept it pinging; across those and the idle-current run's six switches modem `crash_count` stayed 0, with no EFS or persist writes. A boot-default control at the same window (first status-A RMC 16.9 s, 23 SVs, mean GSV SNR 25.1 against 24.0 dB-Hz, median HDOP 0.6 against 0.8) ran 45 min apart, so it is not a controlled pair.

So the baseline sets no mode at boot. Read or change it by hand through the shared bridge:

```sh
qmicli -d /run/qmux_socket --dms-get-operating-mode
qmicli -d /run/qmux_socket --nas-get-serving-system     # registration state
qmicli -d /run/qmux_socket --dms-set-operating-mode=low-power
```

Treat setting a mode as one-way: once anything sets `online` (or `low-power`) we know no way back to `shutting-down` short of a reboot (every fresh boot came back in it); behavior after a modem subsystem restart is untested. Use `low-power` to turn cellular RF off again. Never use `persistent-low-power` (written to NV) or `offline` (leaving it needs a modem reset). A readback straight after a set can still show the old mode; it settles within 3 s, so sleep briefly before reading back.

## Broker, time, and client behavior

gpsd is packaged in the rootfs and runs as `nobody`. Keep its client listener loopback-only (no `-G`). `initramfs/init` must configure `lo`; without it gpsd bind and broker sends fail.

The broker sends one `$…*hh\r\n` UDP datagram per checksum-verified sentence to 127.0.0.1:20175. It discards position-report blocks, chatter, and invalid checksums. A FIFO is unsuitable for this feed because gpsd treats non-tty files as replay and stops at EOF. UDP tolerates independent process restarts but packets sent without a listener are lost.

`-t` tees input unchanged; `-l FILE` writes `<uptime> <sentence>`. The terrace feed delivered GGA/RMC/GSA/VTG and three GSV sentences per 1 Hz cycle, with `$GP` talkers; GLONASS also appeared in QMI position reports. Empty pre-fix sentences still had valid checksums.

New builds run client-only chrony with no UDP server/command listener. Use broker
`-n` (as above) so chrony owns wall-clock adjustment; GPS SHM remains unavailable.
For an explicit offline fallback without chrony synchronization, the broker sets system time once per process only if the clock is before 2000-01-01; pre-2020 RMC dates are rejected. `-n` disables this. It never writes the RTC. Initial gpsd bogus-time warnings are expected before that step. This kernel's `shmget` returns `ENOSYS`; JSON clients work, but chrony SHM integration needs kernel support or a different handoff.

## Transport and storage constraints

- **QMI is over `AF_MSM_IPC`, not QRTR**, on this 4.4 tree (`net/qrtr` doesn't exist; `net/ipc_router` does) — libqmi/ModemManager's QRTR/rpmsg path doesn't apply. `tools/msmipc.c` adapts the upstream `libqrtr` API onto `AF_MSM_IPC` so `tools/rmtfs/` and libqmi-side code don't need QRTR-specific ports.

- **A client `sendto()` on the modem's IPC-router socket blocks 30s** until a root process has run the IRSC ioctl once per boot and closed that socket (`tools/irsc.c` / `msmipc_irsc()`); `gps-up` runs it before starting `rmtfs`.

- **`AF_MSM_IPC` signals flow-control (RESUME_TX) as a 0-byte `recvfrom()`**, not a decodable packet — don't treat it as EOF or feed it to a QMI decoder.

- **`rmtfs -r` (RAM-shadow writes) is non-negotiable** until EFS writes are explicitly reviewed and approved: `gps-up` never mounts a partition read-write or writes `modemst1/2`/`fsc`/the active slot's `fsg_a`/`fsg_b` directly.

- **`fsg` is genuinely per-slot on this device — `fsg_a`/`fsg_b` only, no unsuffixed `fsg`** (confirmed against the live partition table; `modemst1`/`modemst2`/`fsc` *are* unsuffixed). An earlier version of `gps-up` assumed all of EFS was unsuffixed and deliberately never passed `rmtfs -S`; that assumption was wrong for `fsg` and would have made `rmtfs` unable to open it at all. `gps-up` now validates the active slot's `fsg_$SLOT` (exact partition, exact 10485760-byte size, from `androidboot.slot_suffix`) before exposing it, passes `rmtfs -P -r -S "$SLOT" -v`, and never inspects, links, sizes, or opens the other slot's `fsg`.

- **The modem's `msadp` debug-policy firmware file never exists**, and with `FW_LOADER_USER_HELPER_FALLBACK=y` every modem boot would otherwise stall 60s waiting for it — `gps-up` sets `/sys/class/firmware/timeout` to `1` before opening `/dev/subsys_modem` so this doesn't require a kernel config change.

Write modem sysfs control values without a trailing newline where required: the earlier newline bug prevented PIL startup. `AF_MSM_IPC` RMTFS requests carry sector offsets; preserve the corrected offset handling. QMUX CTL response flags differ from service flags; the old one-byte flag error made `qmicli` time out even though the modem was healthy. Detailed failures and fixes remain in the [build log](../build-log.md).

## Modify and verify

Run `make -C tools test` (which includes the GPS startup shell suite), plus the relevant component suites: `make -C tools/rmtfs test`, `make -C tools/servreg-locator test`, and `make -C tools/tftp test`. The tests cover transport/bridge framing, storage offsets, slot validation and lifecycle safeguards, service replies, bounded TFTP behavior, and broker filtering/framing/time handling. Rebuild rootfs if packages change, then initramfs and boot image.

Live checks: modem survives startup, services are discovered, QMUX answers, LOC starts, outdoor NMEA/gpsd fixes agree, and teardown leaves no owned helpers. Capture image hashes and logs; preserve the no-writes-to-EFS/persist policy and verify it with appropriate evidence. Precise GPS archives may exist only locally; the public history retains redacted summaries.

The [GPS and time plan](../next-steps/gps-and-time.md) owns manager leases, supervision, time synchronization, and warm starts. Nested carrier MCFG paths remain outside the current TFTP allowlist and are not a GPS blocker.
