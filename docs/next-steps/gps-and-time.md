# GPS and time

[Next-steps index](README.md) · [Current features](../features/README.md)

Remaining integration and validation work, reviewed 2026-10-11. Linked feature guides describe implemented behavior; candidate approaches must be checked when implementing.

## Client integration

The [GPS manager](../features/gps.md#use-gnss-gps-manager) runs GNSS on demand (done 2026-10-04): opening the map holds a `map` lease, the ride recorder a `ride` lease for the whole recording, so changing pages or restarting the UI cannot stop an active ride; screen blanking has no effect on leases. After acquiring a lease, the UI or recorder reads gpsd: either libgps (`gpsd-dev` headers on the host, link against the rootfs `libgps.so` with our cross toolchain) or, if that link turns out awkward, the JSON protocol on 2947 directly, which is tiny and stable. The UI presents GPS off / starting modem / searching / fixed / retrying without conflating stack state with fix state. Starting a ride must not wait for a fix: elapsed time and other sensors begin immediately while gpsd searches. Decide libgps versus JSON when the UI work starts; the [existing GPS verification](../features/gps.md) does not depend on it.

Acceptance: the recorder's lease surviving UI restarts, the UI showing manager states without conflating them with fix state, and a reconnect plus a new lease after a manager restart (its leases die with its connections). LOC and manager behavior across a modem subsystem restart is untested.

## Time synchronization

Done: chrony steps the boot clock from the NTP pool over Wi-Fi or, offline, from GPS while a lease holds gpsd ([GPS time](../features/gps.md#gps-time), 2026-10-04). Since 2026-10-06 the clock boots from a wall-minus-RTC offset saved on `/data`, and chrony's drift file persists too ([storage: persistent state](../features/storage.md#persistent-state)). Remaining:

- **USB host as a source.** When plugged in, the host's chronyd (or any NTP server on the USB link) could be a `server 172.16.42.x` source. The host needs an NTP server for that; a test SNTP responder on the link measured the phone against the host with 1.8 ms round trips.

## Manual time, the time zone picker and GPS power modes

Left out of slice 2 (the Time page shows the fixed `tz` setting and the source,
see the [system UI guide](../features/system-ui.md#device-pages-m4)):

- **Manual time setting and a time zone picker.** The design has none; today the
  zone is the system zone file (tzdata since 2026-10-20, `/etc/localtime` linked
  to Asia/Kolkata), with the `tz` setting (a POSIX string, default `IST-5:30`) as
  the fallback when that file is missing, and the clock comes from GPS, chrony
  or the saved offset. Needs: a zone choice (tzdata is shipped, so a zone name
  per region), a picker page, and a manual set that goes through chrony
  (`chronyc settime`) or the RTC without fighting the sources. Blocked on whether a bike computer ever
  needs it: GPS gives UTC and the zone changes rarely, so decide after real use.
- **GPS power modes** (Precise, Balanced, Saver). The design has a mode row on the
  GPS page and a section on P13. Needs: a measurement of what each mode costs and
  buys (fix rate, constellation set, `LOC` session parameters, current with USB
  out; a GNSS session adds about +31 mA over `OFF` today) and a gps-manager option
  to select it. Blocked on that measurement, which belongs with the GNSS cache
  work below since both change how a session starts.

## Warm starts and assistance

New RAM-shadow GNSS state disappears across boots. The recorded terrace cold start was ≤78 seconds; repeat measurements before treating that as a typical startup time. Investigate two improvements without touching real EFS: persist the *shadow* (or the identified GNSS cache files it contains — design and review their restore path; sector-only RMTFS traffic does not identify filenames) to our own writable storage ([persistent storage](../features/storage.md)) and re-seed the shadow from it at boot; and using the existing [Wi-Fi](../features/wifi-ui.md), fetch the modem-advertised gpsOneXTRA source (`xtra3grcej.bin` in the 2026-10-06 query) and inject it plus time through LOC and measure TTFF before/after. Faster warm or assisted starts remain an expected benefit to verify.

### GNSS cache persistence

Next implementation task, based on the [file-level comparison](../../logs/gnss-efs-files-2026-10-11.txt):

1. **Define the cache allowlist and validity rules.** Start with `/GNSS/PE/AlmFile*` and `GloEphFile*`, whose contents changed during the measured session. Determine record/version and age checks before accepting saved data; keep GPS NV settings and unrelated modem state outside the initial scope.
2. **Save to `/data/v1/gnss`.** Turn the temporary read-only EFS diagnostic into a bounded collector coordinated with gps-manager after session teardown. Save complete generations atomically with file sizes/checksums, capture time, firmware identity and slot. Follow the existing chef-state ownership rules; missing or read-only storage must leave GNSS usable.
3. **Restore into the modem's RAM-backed EFS before LOC starts.** Establish that the chosen file-level restore path writes only to the RMTFS shadow. Keep `rmtfs -r` and original EFS/persist partitions protected. Reject incomplete, corrupt, incompatible or stale generations and fall back to the original boot state. Whole-image shadow restore remains an alternative to assess if selective restore cannot meet these requirements.
4. **Measure the benefit across reboots.** Compare fresh starts with and without restore under comparable signal, time and assistance conditions; repeat measurements rather than relying on one warm session. Verify continued fixes, manager teardown/recovery, safe fallback with unavailable storage, and unchanged original-partition hashes/write counters. Enable automatic restore only after these checks establish correct behavior and a useful startup benefit.

## Carrier work outside GPS

Nested `/readonly/firmware/image/modem_pr/...` MCFG reads remain unsupported by `tools/tftp/translate.c`. Review a narrowly scoped allowlist change before carrier/RIL work; GPS is already verified without it.

GNSS assistance research (2026-10-06): [source and validity observations](../../logs/gnss-assistance-research-2026-10-06.txt) showed then-valid orbit data and modem-advertised `xtra3grcej.bin`. The [fresh-boot comparison (2026-10-11)](../../logs/gnss-shadow-comparison-2026-10-11.txt) reproduced the first full `modemst1` shadow write with GPS OFF in two idle controls. A completed-fix run added full `modemst2` and later `modemst1` writes absent from the longer idle control, with no new TFTP requests after startup; physical EFS/persist write counters stayed zero. Auxiliary LOC queries confound strict GNSS attribution, and whole-image transfers do not identify changed bytes or files. The subsequent query-free export session is described below; EFS content attribution, restoration and TTFF benefit remain untested. The reported orbit validity ended before this test. Assistance upload and automatic time injection remain follow-ups.

Subsequent [diagnostic exports (2026-10-11)](../../logs/gnss-shadow-export-2026-10-11.txt) captured idle, fixed and post-release RMTFS shadows plus current backing-partition copies, without auxiliary LOC queries or replacing the daemon. `modemst2` changed between idle and fixed; `modemst1` changed again around release. Changes span every sector of each 2 MiB image, so those raw exports alone could not identify filenames or GNSS content. Original partition hashes stayed unchanged. Raw exports are retained privately on the host and under `/data/v1/gnss-research`; they are not automatically restored. The live `modemst1/2` partitions differ from older stock backups, so content analysis must use the captured live-source baseline. File-level investigation followed below; a reviewed restore design remains a follow-up.

[Read-only EFS file comparison (2026-10-11)](../../logs/gnss-efs-files-2026-10-11.txt) subsequently identified logical GNSS updates through `/dev/diag`: two idle captures of 139 files were identical; a warm fixed session changed four `/GNSS/PE/AlmFile*` records and three `GloEphFile*` records, and added two 128-byte GLONASS ephemeris records. The captured X3 assistance, GPS NV settings and receiver-estimate files stayed unchanged. Original partition hashes and zero write counters were verified. `/GNSS/PE` is now a concrete candidate for selective persistence; binary field decoding, validity/consistency rules, a reviewed restore design and controlled TTFF measurements remain. Raw encrypted-image decoding is not required for the demonstrated read-only file access.
