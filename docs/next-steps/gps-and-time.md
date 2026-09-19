# GPS and time

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## GPS manager

The proposed baseline image will start one lightweight `gps-manager` from inittab; it is the sole owner and supervisor of `gps-up`, the QMI LOC CID/session, `gpsd`, and the `qmicli --loc-follow-nmea | nmea-broker` pipeline. The modem remains off at boot. Local consumers acquire reference-counted leases over a Unix stream socket such as `/run/gps-manager.sock`: opening the map holds a temporary `map` lease, while the ride recorder holds a `ride` lease for the entire recording. A lease is represented by the live socket connection, so process death releases it automatically. The ride lease belongs to the recorder rather than the UI, so changing pages or restarting the UI cannot stop an active ride. The first lease drives `OFF → STARTING → ACQUIRING → RUNNING`; the final release starts a short idle grace period (initially 30 s) and then drives `STOPPING → OFF`, avoiding a cold modem restart during brief page changes. Screen blanking has no effect on leases.

Startup order: start one `gps-up`, wait for `/run/qmux_socket`, prove the bridge responds, allocate a LOC CID, set the NMEA types, start LOC session 1, start `gpsd`, then start the follower/broker pipeline. Teardown order: interrupt the follower/broker, stop LOC, release the CID, stop `gpsd`, then terminate `gps-up` and let its existing cleanup shut down QMUX/RMTFS/servreg/TFTP, close the modem and unmount firmware. The manager reports at least `OFF`, `STARTING`, `ACQUIRING`, `RUNNING`, `STOPPING` and `FAILED`; fix/no-fix remains gpsd data. If any owned process dies, tear down the rest as one unit and retry with bounded backoff only while a lease remains. Do **not** put the individual GPS processes in separate inittab `respawn` entries: that can leave a partial stack alive or repeatedly reopen `/dev/subsys_modem`. Only the manager is respawned. Promote the ride image's silence/five-deaths supervision rules into this manager rather than into the broker.

## Client integration

After acquiring a manager lease, the UI or recorder reads gpsd — either libgps (`gpsd-dev` headers on the host, link against the rootfs `libgps.so` with our cross toolchain) or, if that link turns out awkward, the JSON protocol on 2947 directly, which is tiny and stable. The UI presents GPS off / starting modem / searching / fixed / retrying without conflating stack state with fix state. Starting a ride must not wait for a fix: elapsed time and other sensors begin immediately while gpsd searches. Decide libgps versus JSON when the UI work starts; the [existing GPS verification](../features/gps.md) does not depend on it.

Acceptance: first/last lease startup and shutdown, recorder surviving UI restarts, bounded recovery while demand remains, and no surviving partial stack after failure. Compare modem-up/off idle current and verify LOC restart behavior across a modem failure. Current manual operation is in the [GPS guide](../features/gps.md).

## Time synchronization

Alpine v3.24 `main` has `chrony` 4.8-r7 (397 KB installed, `libcap` + `libseccomp`) and `openntpd`; busybox `ntpd` has no refclock, so chrony. Planned sources: gpsd's SHM refclock (`refclock SHM 0 refid GPS`, no PPS), the USB host's chronyd when plugged in, NTP pool once Wi-Fi exists. **New live prerequisite:** this kernel returns `ENOSYS` for every gpsd `shmget`, so enable `CONFIG_SYSVIPC` and re-test the gpsd SHM export first (or choose a non-SHM gpsd/chrony handoff). `makestep 1 -1` so the 1970 boot clock is stepped, not slewed. The PM660 RTC (`rtc0`) is present but write-disabled in DT and only counts from battery-connect: no `rtcsync`; later persist a wall−RTC offset on writable storage ([persistent storage](storage-and-boot.md#persistent-storage)). Chrony supersedes the broker clock step once this path works.

## GNSS-only RF

Determine whether the modem scans or camps on cellular while providing GPS; SIM state and RF behavior have not been checked. Query `qmicli --dms-get-operating-mode`, then investigate a mode that disables cellular RF while keeping GNSS available. The earlier candidates were DMS low-power or a NAS/airplane configuration; neither is verified to preserve GNSS on this device. Re-verify a LOC fix in the chosen mode and measure the [idle-current difference](power-and-reliability.md#suspend-and-idle-power).

## Warm starts and assistance

New RAM-shadow GNSS state disappears across boots. The recorded terrace cold start was ≤78 seconds; repeat measurements before treating that as a typical startup time. Investigate two improvements without touching real EFS: persist the *shadow* (or only the GNSS state files it contains — find them in the `rmtfs` read/write trace) to our own writable storage ([persistent storage](storage-and-boot.md#persistent-storage)) and re-seed the shadow from it at boot; and once [Wi-Fi](connectivity-and-sensors.md#wi-fi) exists, fetch gpsOneXTRA (`xtra3grc.bin`) and inject it plus time through LOC and measure TTFF before/after. Faster warm or assisted starts remain an expected benefit to verify.

## Carrier work outside GPS

Nested `/readonly/firmware/image/modem_pr/...` MCFG reads remain unsupported by `tools/tftp/translate.c`. Review a narrowly scoped allowlist change before carrier/RIL work; GPS is already verified without it.
