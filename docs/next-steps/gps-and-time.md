# GPS and time

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Client integration

The [GPS manager](../features/gps.md#use-gnss-gps-manager) runs GNSS on demand (done 2026-10-04): opening the map holds a `map` lease, the ride recorder a `ride` lease for the whole recording, so changing pages or restarting the UI cannot stop an active ride; screen blanking has no effect on leases. After acquiring a lease, the UI or recorder reads gpsd: either libgps (`gpsd-dev` headers on the host, link against the rootfs `libgps.so` with our cross toolchain) or, if that link turns out awkward, the JSON protocol on 2947 directly, which is tiny and stable. The UI presents GPS off / starting modem / searching / fixed / retrying without conflating stack state with fix state. Starting a ride must not wait for a fix: elapsed time and other sensors begin immediately while gpsd searches. Decide libgps versus JSON when the UI work starts; the [existing GPS verification](../features/gps.md) does not depend on it.

Acceptance: the recorder's lease surviving UI restarts, the UI showing manager states without conflating them with fix state, and a reconnect plus a new lease after a manager restart (its leases die with its connections). LOC and manager behavior across a modem subsystem restart is untested.

## Time synchronization

Done: chrony steps the boot clock from the NTP pool over Wi-Fi or, offline, from GPS while a lease holds gpsd ([GPS time](../features/gps.md#gps-time), 2026-10-04). Remaining:

- **RTC offset persistence.** The PM660 RTC (`rtc0`) is write-disabled in DT and only counts from battery connect, so there is no `rtcsync`. Persisting a wall-minus-RTC offset on writable storage ([persistent storage](storage-and-boot.md#persistent-storage)) would give a roughly right clock at boot before any source.
- **USB host as a source.** When plugged in, the host's chronyd (or any NTP server on the USB link) could be a `server 172.16.42.x` source. The host needs an NTP server for that; a test SNTP responder on the link measured the phone against the host with 1.8 ms round trips.

## Warm starts and assistance

New RAM-shadow GNSS state disappears across boots. The recorded terrace cold start was ≤78 seconds; repeat measurements before treating that as a typical startup time. Investigate two improvements without touching real EFS: persist the *shadow* (or only the GNSS state files it contains — find them in the `rmtfs` read/write trace) to our own writable storage ([persistent storage](storage-and-boot.md#persistent-storage)) and re-seed the shadow from it at boot; and once [Wi-Fi](connectivity-and-sensors.md#wi-fi) exists, fetch gpsOneXTRA (`xtra3grc.bin`) and inject it plus time through LOC and measure TTFF before/after. Faster warm or assisted starts remain an expected benefit to verify.

## Carrier work outside GPS

Nested `/readonly/firmware/image/modem_pr/...` MCFG reads remain unsupported by `tools/tftp/translate.c`. Review a narrowly scoped allowlist change before carrier/RIL work; GPS is already verified without it.
