# GPS and time

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Client integration

The [GPS manager](../features/gps.md#use-gnss-gps-manager) runs GNSS on demand (done 2026-10-04): opening the map holds a `map` lease, the ride recorder a `ride` lease for the whole recording, so changing pages or restarting the UI cannot stop an active ride; screen blanking has no effect on leases. After acquiring a lease, the UI or recorder reads gpsd: either libgps (`gpsd-dev` headers on the host, link against the rootfs `libgps.so` with our cross toolchain) or, if that link turns out awkward, the JSON protocol on 2947 directly, which is tiny and stable. The UI presents GPS off / starting modem / searching / fixed / retrying without conflating stack state with fix state. Starting a ride must not wait for a fix: elapsed time and other sensors begin immediately while gpsd searches. Decide libgps versus JSON when the UI work starts; the [existing GPS verification](../features/gps.md) does not depend on it.

Acceptance: the recorder's lease surviving UI restarts, the UI showing manager states without conflating them with fix state, and a reconnect plus a new lease after a manager restart (its leases die with its connections). LOC and manager behavior across a modem subsystem restart is untested.

## Time synchronization

Alpine v3.24 `main` has `chrony` 4.8-r7 (397 KB installed, `libcap` + `libseccomp`) and `openntpd`; busybox `ntpd` has no refclock, so chrony. New standard-NetworkManager builds start a client-only NTP-pool chronyd at boot
with volatile state, port0/cmdport0 and makestep1.0 3. Clean baseline0d17
verified automatic1970-to-current synchronization, certificate-checked HTTPS
and signed apk installation, with no manual chrony refresh. GPS/time architecture
modernization is deferred; no separate time unit or new GPS fallback is added. GPS pipelines should use broker -n while chrony owns the clock.
Future GPS integration sources: gpsd's SHM refclock (`refclock SHM 0 refid GPS`, no PPS), the USB host's chronyd when plugged in, NTP pool once Wi-Fi exists. **New live prerequisite:** this kernel returns `ENOSYS` for every gpsd `shmget`, so enable `CONFIG_SYSVIPC` and re-test the gpsd SHM export first (or choose a non-SHM gpsd/chrony handoff). `makestep 1 -1` so the 1970 boot clock is stepped, not slewed. The PM660 RTC (`rtc0`) is present but write-disabled in DT and only counts from battery-connect: no `rtcsync`; later persist a wall−RTC offset on writable storage ([persistent storage](storage-and-boot.md#persistent-storage)). Chrony supersedes the broker clock step once this path works.

## Warm starts and assistance

New RAM-shadow GNSS state disappears across boots. The recorded terrace cold start was ≤78 seconds; repeat measurements before treating that as a typical startup time. Investigate two improvements without touching real EFS: persist the *shadow* (or only the GNSS state files it contains — find them in the `rmtfs` read/write trace) to our own writable storage ([persistent storage](storage-and-boot.md#persistent-storage)) and re-seed the shadow from it at boot; and once [Wi-Fi](connectivity-and-sensors.md#wi-fi) exists, fetch gpsOneXTRA (`xtra3grc.bin`) and inject it plus time through LOC and measure TTFF before/after. Faster warm or assisted starts remain an expected benefit to verify.

## Carrier work outside GPS

Nested `/readonly/firmware/image/modem_pr/...` MCFG reads remain unsupported by `tools/tftp/translate.c`. Review a narrowly scoped allowlist change before carrier/RIL work; GPS is already verified without it.
