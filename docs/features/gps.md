# GPS, modem services, and gpsd

[Feature index](README.md) · [Build instructions](../building.md)

## Current status

Modem boot, LOC discovery/start/stop, NMEA streaming, and outdoor fixes were live-verified by 2026-09-18. The terrace run measured ≤78 seconds cold time-to-first-fix and 5 m uncertainty at the first position fix. gpsd received a matching 3D fix on 2026-09-19.

The clean standard-NetworkManager baseline verifies one shared resident modem owner at boot. Since 2026-10-04 [gps-manager](#use-gnss-gps-manager) runs LOC, gpsd and the NMEA pipeline on demand (live-verified: first lease to `RUNNING` at a window in 17 and 34 s on two fresh boots, failure recovery, `kill -9` and respawn); with GNSS leased the phone idles about 31 mA higher than with it `OFF`. Also since 2026-10-04 chrony takes [GPS time](#gps-time) from gpsd, so a lease sets the clock without network. Unattended logging was live-verified on a temporary ride image (2026-09-18, since dropped; see the [build log](../build-log.md)). Earlier device verification used manual modem startup.

## Components and lifecycle

`initramfs/usr/bin/gps-up` starts the modem dependencies, exposes `/run/qmux_socket`, and owns cleanup. Its helpers are:

- `tools/msmipc.{c,h}`, `tools/qrtr/`: AF_MSM_IPC adaptation and QMI support.
- `tools/irsc.c`: opens the IPC-router security gate before clients send.
- `tools/rmtfs/`: EFS service using read-only partition input and RAM-shadow writes.
- `tools/servreg-locator/`: service-registry replies from firmware domain descriptors.
- `tools/tftp/`: bounded TFTP/RFS service with read-only firmware and a RAM-backed writable namespace seeded from the stock persist backup at build time.
- `tools/qmux.{c,h}`, `tools/qmuxd-lite.c`: the bridge used by stock Alpine `qmicli`.
- `tools/nmea-broker.c`: follower stdout to checksum-verified NMEA UDP datagrams for gpsd.
- `tools/gps-manager.c`: leases on `/run/gps-manager.sock` run the LOC session, gpsd and the follower/broker pipeline on demand ([below](#use-gnss-gps-manager)).

The modem needs both service-registry and TFTP/RFS support to survive startup. `gps-up` does not automatically restart a crashed modem. Do not run competing owners or reopen the modem repeatedly after failure; retain evidence and reboot.

## Use GNSS: gps-manager

`tools/gps-manager.c` (inittab `::respawn:`) runs GNSS on demand on top of the shared modem owner. It never starts, signals or restarts `gps-up` and never changes the modem's operating mode. Idle, it only listens on `/run/gps-manager.sock`. A client takes a lease; the first lease brings GNSS up, and 30 s after the last lease goes it is torn down again. A lease lives on its connection, so a client that dies releases it.

```sh
gps-manager hold map          # lease and stream state lines until killed (kinds: map, ride)
gps-manager status            # one line: state, leases per kind, CID, counters, detail
gps-manager watch             # stream "state <S> <detail>" lines
gps-manager retry             # leave a parked FAILED (only acts in FAILED)
```

The protocol is one line per request: `lease map|ride`, `release map|ride`, `status`, `watch`, `retry`, answered with `ok ...` or `err ...`; watchers get `state <S> <detail>` on every change. Lease counts are per connection. States: `OFF`, `STARTING`, `ACQUIRING`, `RUNNING`, `STOPPING`, `FAILED`. `RUNNING` only means a status-A RMC arrived in the last 10 s; fix data stays in gpsd (`127.0.0.1:2947`), and nothing the manager logs or reports carries a position.

Bring-up: wait for the modem owner (`modem_owner_pid` in C, polled every 2 s; `FAILED` after 240 s, which recovers when an owner appears), allocate a LOC CID (`--loc-noop --client-no-release-cid`), set NMEA types `gga|rmc|gsv|gsa|vtg`, start LOC session 1, start `gpsd -N -n -b udp://127.0.0.1:20175`, then `qmicli --loc-follow-nmea | nmea-broker -n -t` with the broker's tee read back by the manager. Teardown reverses it: INT the follower, loc-stop, release the CID, stop gpsd. Each qmicli step is bounded at 25 s. loc-stop is sent whenever loc-start was attempted, since the modem may start the session even when the step fails. A failed loc-stop still releases; a failed release is counted as `leaked` in `status` and not retried; with the QMUX socket gone the qmicli steps are skipped. One window cannot be closed: an allocation step killed after the bridge allocated a CID but before qmicli printed it leaves that CID in `qmuxd-lite` without a session (a bridge slot, no GNSS cost), so a timed-out allocation also counts as (possibly) `leaked`. ACQUIRING/RUNNING changes reach watchers every time, but kmsg gets only the first fix of each bring-up.

Supervision: an owned process exiting, 60 s without an NMEA line, or the modem owner going away or being replaced tears the whole set down at once. While a lease remains it retries. A run is healthy after 120 s of NMEA since the pipeline started, which resets the failure streak; a failure before that is fast and adds one: retries wait 2, 4, 8, 16 s for streak 1 to 4 (2 s after a failure of a healthy run), and a streak of 5 parks it in `FAILED` until all leases are released or `retry` is sent. Without a lease it never retries. A lost owner is not counted; `FAILED` waits for one.

The manager is the only user of its CID. The bridge hands a CID's indications to whoever spoke on it last, so any other qmicli request on that CID stops the manager's NMEA stream. Manual LOC commands must allocate their own CID (`--loc-noop --client-no-release-cid`) and should not run a second gpsd while the manager has one.

Idle cost, measured 2026-10-04 (one unattended run, panel off, USB out, Wi-Fi radio off, BT as booted, 180 s blocks with the first 45 s skipped, four leased and three usable `OFF` blocks; a fourth `OFF` block was spoiled by a power-key press that turned the panel on between 1394 s and 1452 s): leased and `RUNNING` with a fix at a window (LOC engine plus gpsd, qmicli, broker and manager CPU) 125.9 mA (124.9 to 127.2 per block) against 95.4 mA `OFF` (94.3 to 97.1), +30.8 mA on block means (standard error 0.4) and +31.2 mA on medians. Indoor or cold acquisition, or Wi-Fi in use, may differ. So release leases when GNSS is not needed; the 30 s grace keeps short page changes cheap.

Restart safety: one instance holds an flock on `/run/gps-manager/lock`; a second exits. `/run/gps-manager/state` records the CID with the owner that served it and every child's pid and start time as soon as they exist. If the manager is killed, init respawns it; the new instance stops the recorded follower, broker and gpsd whose start time and name still match, then stops and releases the old CID once that same owner is ready (a replaced owner means the CID died with its bridge). Clients of the dead instance lost their leases with the connection and must lease again. The follower, the broker and the qmicli steps also get `PR_SET_PDEATHSIG` and die with the manager; gpsd loses it when it drops privileges, so the respawned instance is what stops it. Logs: kmsg `gps-manager:` lines (transitions only) and `/run/gps-manager/{steps,gpsd,follow,broker}.log`, each rotated to `.1` at a bring-up. RAM only.

## Start and inspect manually

For debugging only, with the manager `OFF` (no leases). Use a fresh baseline boot and the phone's telnet shell. New builds already start gps-up once. If readiness does not appear, inspect `/run/gps-up.log` rather than launching another copy. Proceed only once the service-version query succeeds. Substitute the allocated CID below; it is not guaranteed to be 1.

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

### GPS time

chrony (client-only, no UDP server or command listener) owns the wall clock, and GPS is one of its sources: while a lease keeps gpsd running, gpsd puts the NMEA time into NTP SHM unit 0 and chronyd reads it as `refclock SHM 0 refid GPS` (`initramfs/etc/chrony/chrony.conf`). So the broker keeps `-n`. This needs `CONFIG_SYSVIPC=y` (added 2026-10-04; before that `shmget` returned `ENOSYS`). `mkinitramfs.sh` and `mkboot.sh` refuse a kernel without it, because chronyd exits when it cannot attach a configured SHM segment.

- **Segments.** Units 0 and 1 are root-only (0600). chronyd attaches unit 0 at startup, before it drops to the `chrony` user. gpsd 3.27.3 attaches units 0 to 11 (`NTPSHMSEGS`, two per device slot) in `ntpshm_context_init()` while still root, then drops to `nobody` and keeps the mappings. Its UDP device uses the first, unit 0. Whichever of the two starts first creates unit 0. When gpsd stops, chrony clears the sample's valid flag after each read, so the source just goes unreachable. gpsd also creates NTP1 (0600), NTP2 to NTP11 (0666) and its export segment `0x47505344` (0666, 38504 bytes). Nobody removes any of these, so they persist after gpsd exits (`nattch` 0) and are reused by the next gpsd; they are harmless. The export segment holds the current fix and is world-readable, the same exposure as gpsd's 127.0.0.1:2947 listener. That is fine on this single-user device, but count it wherever positions are handled. Check with `ipcs -m`: `0x4e545030` root 600, `nattch` 1 idle, 2 while gpsd runs.
- **When samples flow.** After more than 3 fixes, gpsd ships one sample a second. It latches `CLOCK_REALTIME` on the first timed sentence of each burst: the modem delivers 4 GSV, GGA, VTG, RMC and GSA within about 1 ms, so that is the GGA after the GSVs. The 1970 boot clock does not stop the samples. On the NTP-disciplined clock the bursts arrived about 6 ms after the second. Against NTP the latched time was 13 ms late, with 0.15 to 0.7 ms standard deviation, hence `offset 0.013` and `precision 1e-3`. That NTP ran over Wi-Fi to pool servers that disagreed with each other by up to 55 ms. A cross-check against the USB host gave 13.8 ms, but the host is itself only within about ±80 ms of UTC. So the offset is calibrated to tens of ms of absolute accuracy, not better. With no PPS, treat GPS as a coarse source for the boot step and offline holdover, not a precision reference.
- **Selection.** `delay 1.0` gives GPS a root distance of about 0.5 s. The pool servers seen over Wi-Fi measured ±20 to 140 ms, so NTP was selected and GPS was neither combined nor marked a falseticker (`#-` in `chronyc sources`, `D` in `selectdata`). A server between about ±170 ms (combinelimit 3) and ±0.5 s would get GPS combined, which is harmless for a calibrated GPS; only a server worse than GPS's own 0.5 s would lose to it. With `delay 0.5` a ±130 ms server got GPS combined (`#+`); with `0.2` GPS was selected over NTP.
- **Steps.** `makestep 1.0 -1`: chrony steps any correction over 1 s whenever it occurs, not only during its first updates. There is no RTC, so the clock boots at 1970. GPS can arrive long after chronyd starts, or after a source that was off by seconds. Smaller corrections, such as NTP replacing GPS (tens of ms), are slewed.

Offline acceptance on 2026-10-04 (image `boot-gpstime2`, fresh boot at a window, no Wi-Fi profile, clock at 1970; see the [build log](../build-log.md)):

- **Lease and step.** Lease at 27.9 s uptime, `RUNNING` at 46.1 s. The first refclock poll with samples came at 62.3 s, still on the 1970 clock. chrony selected GPS and stepped once, by +1.79e9 s, at 94.5 s: 66.5 s after the lease, on the third poll at the default `poll 4`.
- **Release and holdover.** After the 30 s grace the manager was `OFF`, 32 s after release. GPS went unreachable (`#?`, reach 0), tracking held with leap status Normal, and nothing failed.
- **Re-lease.** A new lease got its first sample within 12 s and GPS was selected again.
- **Wi-Fi.** Bringing Wi-Fi up afterwards selected NTP within about 30 s without a step.

Clock against the USB host (SNTP over USB, 1.7 to 1.9 ms round trips; the host is chrony-synchronised but only within about ±80 ms of UTC itself): GPS-disciplined and in holdover the phone stayed 1.3 to 2.6 ms behind the host. Under NTP over this Wi-Fi it moved to 40 ms ahead.

Inspect it as root through the Unix socket: `chronyc -h /run/chrony/chronyd.sock sources -v` (also `sourcestats`, `selectdata`, `tracking`). chronyd runs with `-d`, and busybox init starts it with stdin, stdout and stderr on `/dev/null` (checked in `/proc/<pid>/fd`), so its log is lost. A step shows only in `tracking` or as a jump in wall-clock minus `CLOCK_MONOTONIC`.

The broker's own fallback still exists for images without chrony: without `-n` it sets the system time once per process, only if the clock is before 2000-01-01, and rejects pre-2020 RMC dates. It never writes the RTC.

## Transport and storage constraints

- **QMI is over `AF_MSM_IPC`, not QRTR**, on this 4.4 tree (`net/qrtr` doesn't exist; `net/ipc_router` does) — libqmi/ModemManager's QRTR/rpmsg path doesn't apply. `tools/msmipc.c` adapts the upstream `libqrtr` API onto `AF_MSM_IPC` so `tools/rmtfs/` and libqmi-side code don't need QRTR-specific ports.

- **A client `sendto()` on the modem's IPC-router socket blocks 30s** until a root process has run the IRSC ioctl once per boot and closed that socket (`tools/irsc.c` / `msmipc_irsc()`); `gps-up` runs it before starting `rmtfs`.

- **`AF_MSM_IPC` signals flow-control (RESUME_TX) as a 0-byte `recvfrom()`**, not a decodable packet — don't treat it as EOF or feed it to a QMI decoder.

- **`rmtfs -r` (RAM-shadow writes) is non-negotiable** until EFS writes are explicitly reviewed and approved: `gps-up` never mounts a partition read-write or writes `modemst1/2`/`fsc`/the active slot's `fsg_a`/`fsg_b` directly.

- **`fsg` is genuinely per-slot on this device — `fsg_a`/`fsg_b` only, no unsuffixed `fsg`** (confirmed against the live partition table; `modemst1`/`modemst2`/`fsc` *are* unsuffixed). An earlier version of `gps-up` assumed all of EFS was unsuffixed and deliberately never passed `rmtfs -S`; that assumption was wrong for `fsg` and would have made `rmtfs` unable to open it at all. `gps-up` now validates the active slot's `fsg_$SLOT` (exact partition, exact 10485760-byte size, from `androidboot.slot_suffix`) before exposing it, passes `rmtfs -P -r -S "$SLOT" -v`, and never inspects, links, sizes, or opens the other slot's `fsg`.

- **The modem's `msadp` debug-policy firmware file never exists**, and with `FW_LOADER_USER_HELPER_FALLBACK=y` every modem boot would otherwise stall 60s waiting for it — `gps-up` sets `/sys/class/firmware/timeout` to `1` before opening `/dev/subsys_modem` so this doesn't require a kernel config change.

Write modem sysfs control values without a trailing newline where required: the earlier newline bug prevented PIL startup. `AF_MSM_IPC` RMTFS requests carry sector offsets; preserve the corrected offset handling. QMUX CTL response flags differ from service flags; the old one-byte flag error made `qmicli` time out even though the modem was healthy. Detailed failures and fixes remain in the [build log](../build-log.md).

## Modify and verify

Run `make -C tools test` (which includes the GPS startup shell suite), plus the relevant component suites: `make -C tools/rmtfs test`, `make -C tools/servreg-locator test`, and `make -C tools/tftp test`. The tests cover transport/bridge framing, storage offsets, slot validation and lifecycle safeguards, service replies, bounded TFTP behavior, broker filtering/framing/time handling, and the GPS manager: `tests/test_gps-manager.c` drives its state machine through fake operations (order, leases and grace, failures, backoff and parking, owner loss, stale CIDs, timeouts) and `tests/test_gps-manager.sh` runs the binary with stub qmicli/gpsd and the real broker through a full cycle, `kill -9` and a respawn. Rebuild rootfs if packages change, then initramfs and boot image.

Live checks: modem survives startup, services are discovered, QMUX answers, LOC starts, outdoor NMEA/gpsd fixes agree, and teardown leaves no owned helpers. Capture image hashes and logs; preserve the no-writes-to-EFS/persist policy and verify it with appropriate evidence. Precise GPS archives may exist only locally; the public history retains redacted summaries.

The [GPS and time plan](../next-steps/gps-and-time.md) owns manager leases, supervision, time synchronization, and warm starts. Nested carrier MCFG paths remain outside the current TFTP allowlist and are not a GPS blocker.
