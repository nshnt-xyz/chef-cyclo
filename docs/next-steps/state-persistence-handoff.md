# Volatile state onto /data (handoff)

[Next steps index](README.md) · [Storage guide](../features/storage.md) · [Install layout handoff](install-layout-handoff.md) · [Live testing](../live-testing.md)

**Status:** started 2026-10-06 at the user's request. Handed to Herdr agents
`rootfs_impl` (implementation, live runs) and `rootfs_review` (review),
coordinated by `rootfs_research`, who keeps the research (including the GNSS
part, [E](#e-gnss-assistance-research-rootfs_research-not-in-this-handoff)).

## Goal

Things that are still lost at every boot move onto the `/data` layout from
phase 2 ([storage guide](../features/storage.md), reserved directories at the
end). Each one is small and provable live; together they are the groundwork
for recording rides. In order:

| | State | Directory | Why |
|---|---|---|---|
| A | Crash logs (pstore) and a boot history | `/data/v1/crash` | ramoops keeps a panic record for one boot only; the shutdown hang showed what that costs |
| B | Wall-clock offset to the RTC, chrony drift | `/data/v1/time`, `/data/v1/chrony` | the clock boots at 1970-01-19 and stays wrong until Wi-Fi or a GPS fix |
| C | Power log | `/data/v1/power` | battery history across boots |
| D | Magnetometer hard-iron bias (REG2 group 2980) | `/data/v1/sensors` | the ADSP relearns it from zero every boot, and only with slow, varied holds |

Every consumer must still work, exactly as today, on a RAM-only boot
(`chef-storage` fell back: missing, foreign or failed `/data`), and must never
make that boot fail.

## Facts checked (rootfs_research, 2026-10-06, installed pair, uptime 512 s)

- **RTC:** `rtc0` is `qpnp_rtc`, `since_epoch` 1611357 (18.6 days: it counts
  from battery connect), `hctosys=1`, so the kernel sets the clock to
  1970-01-19 at boot. Write-disabled in DT, so no `rtcsync`/`hwclock -w`.
  The wall-minus-RTC offset is constant while the battery stays connected
  (crystal drift only); a battery disconnect resets the RTC, which shows up as
  `since_epoch` smaller than the one saved with the offset.
- **chrony:** synced over Wi-Fi to stratum 4 within the first minutes. Config
  `/etc/chrony/chrony.conf`: `driftfile /run/chrony/drift`, `makestep 1.0 -1`,
  command socket `/run/chrony/chronyd.sock`. No drift file existed yet at
  512 s: chronyd writes it about hourly and on exit. Runs as user `chrony`
  (drops privileges), respawned by inittab. `/var/lib/chrony` exists
  (tmpfs, root 0750, empty).
- **pstore:** `pstore` is in `/proc/filesystems`, nothing mounted on
  `/sys/fs/pstore`. ramoops: `mem_address` 0xaf000000, `mem_size` 768 KiB,
  `record_size` 128 KiB, `console_size` 256 KiB, `pmsg_size` 256 KiB,
  `dump_oops=1`, `ecc=0`. From the shutdown hang: the oops record
  (`dmesg-ramoops-0`) survived exactly one boot (the first after
  `bootreason=kernel_panic`) and nothing survives a cold PON. Whether
  `console-ramoops` (the previous boot's kernel log) survives an ordinary warm
  `reboot` is not known: find out, because if it does, every boot can keep the
  previous boot's log. This boot: `androidboot.bootreason=power_key_press`.
- **powerd:** log `/run/power/log.csv` (+ `.1`), 1 MiB cap (`-L`), CSV header
  `uptime,utc,reason,...`; first row `5.3,1970-01-19T15:27:30Z,start,...`
  (so the wall column is 1970 until chrony steps). About 170 bytes per row,
  45 rows in 512 s; 60 s polls normally, 5 s ticks only on change. `state` in
  the same directory is rewritten on every sample: it must stay on `/run`.
- **Registry group 2980:** 126 bytes; the ADSP reads it over REG2 when the
  magn report is set up (`group read 2980 from 5:80` and `5:19` in
  `~/chef-cyclo-evidence/regreuse-live-20260927T101308Z`) and writes it when
  the report is deleted, not when it learns
  ([sensors guide](../features/sensors.md#magnetometer-calibration)). The bias
  items are 3903..3905. `sensord` keeps its registry copy on `/run` only and
  refuses (`EXDEV`) to write it anywhere but tmpfs/ramfs: keep that rule.
  `sensors-up` is manual (not in inittab).
- **/data:** ext4 `chefdata`, 344 KiB used, mounted
  `noatime,errors=remount-ro,commit=5`. `/data/v1/durability-payload`
  (256 KiB, 1970-01-18) is a leftover from the phase 2 power-loss test; delete
  it as part of this work and say so in the build log.
- **Shutdown:** `::shutdown:/usr/bin/chef-storage shutdown` then
  `umount -a -r`; `chef-storage shutdown` TERMs then KILLs every process with a
  file open on `/data` before the read-only remount. busybox init runs the
  `::shutdown` lines in order before its global SIGTERM and does not respawn
  during them. `::restart` runs `chef-storage shutdown` too.

## Design

One shared contract, then one section per consumer. The implementation may
choose the code shape (one `chef-state` script with `boot`/`save`
subcommands, flags on the daemons, or both), but these points are fixed:

- `chef-storage boot`'s `consumers()` creates the new directories with fixed
  owners and modes (root 0700, `chrony`'s directory `chrony:chrony` 0700),
  the same way it does for BlueZ and NetworkManager, and a failure there does
  not roll back the BlueZ/NetworkManager binds: these consumers are optional.
- Nothing is restored from or saved to `/data` unless `/data` is the mounted
  `chefdata` filesystem (`mountpoint -q /data` and the layout marker).
  Otherwise each consumer runs from `/run` as today and logs one line saying
  so.
- Every saved file is written atomically: temp file in the same directory,
  `fsync`, `rename`, `fsync` of the directory. A torn or invalid file is
  ignored with a log line (kmsg, so it shows on the panel), never trusted and
  never fatal.
- Saved state is validated before use (format version, length, checksum,
  plausible ranges); the restore path has host tests for every refusal.
- A new `::shutdown` line runs the save step **before**
  `chef-storage shutdown`; the `::restart` line gets it too. Every step there
  is bounded (`timeout`), and the shutdown must not get slower than about
  2 s in total.
- Write load stays tiny: no file is rewritten more often than once a minute in
  steady state; record the measured bytes/day from `/proc/diskstats` for
  `userdata`.
- Never write `persist`, EFS, or anything outside `userdata`, `system_a` and
  `boot_a`.

### A. Crash logs and boot history

- Early in boot (right after `chef-storage boot` in `/init`, before
  `exec /sbin/init`, so a boot that crashes again still saved the last one):
  mount pstore read-only on `/sys/fs/pstore`, and if it holds any record copy
  all of them into `/data/v1/crash/<n>-<bootreason>/` with a small `info`
  file (boot id, bootreason, `since_epoch`, the restored wall time if B has
  run, file list with sizes and SHA-256). Unmount after. Do not erase the
  records (no `rm` in pstore): the next boot clears them anyway, and an
  identical record seen twice (same hashes) is not copied again.
- Append one line per boot to `/data/v1/crash/boots.log`: sequence number,
  boot id, `androidboot.bootreason`, `since_epoch`, wall time if known,
  whether pstore had records. This alone makes panics, watchdog bites and
  hard resets countable over weeks.
- Retention: keep the newest 32 record directories and at most 16 MiB in
  `/data/v1/crash`; `boots.log` rotates at 256 KiB with one old copy.
- Show the result on the log screen (one kmsg line: "crash: saved previous
  boot's records to ..." or nothing).
- Find out whether `console-ramoops` survives an ordinary `reboot` and a
  `poweroff` + Power on. Record the answer in the storage guide.

### B. Clock: RTC offset and chrony drift

- **Save** `/data/v1/time/rtc-offset` (version, wall seconds minus
  `since_epoch`, the `since_epoch` and wall time at the save, boot id) only
  while chrony is synchronised to a real source (`chronyc tracking` over the
  local socket: leap status normal, reference not local/unsynchronised).
  Sample at an RTC second edge (poll `since_epoch` every ~10 ms, at most
  1.1 s) so the offset is good to about 10 ms. Saved every 15 minutes while
  synced and once at shutdown; skip the write if the offset moved by less
  than 0.1 s and the last save is under 6 hours old.
- **Restore** in `/init` right after `chef-storage boot`: if the file is
  valid and `since_epoch` is not below the saved one (no battery
  disconnect), set the clock to `since_epoch + offset`; otherwise, if a saved
  wall time exists and the clock is behind it, set the clock to that saved
  time as a floor (fake-hwclock style). Log which one was used and write
  `/run/chef-time` (`source=rtc-offset|floor|none`, `saved_age_s=...`) for the
  future UI and recorder, which must still treat this time as unverified
  until chrony syncs. chrony's `makestep 1.0 -1` corrects any later error,
  so nothing in chrony.conf changes for this.
- **chrony drift:** bind `/data/v1/chrony` onto `/var/lib/chrony` in
  `chef-storage` (`chrony:chrony` 0700) and change `driftfile` to
  `/var/lib/chrony/drift`, the same pattern as BlueZ. On a RAM-only boot it is
  the plain tmpfs directory, which already exists. Because chronyd writes the
  drift file only about hourly and on exit, and `chef-storage shutdown`
  freezes `/data` before init's global SIGTERM, the shutdown save step stops
  chronyd itself (TERM, wait bounded for it to exit) so the drift file is
  written while `/data` is still writable. Check that respawn does not bring
  it back during shutdown.
- Acceptance: after a reboot with no Wi-Fi and no GPS, the clock within 2 s
  of the host's (host chrony or SNTP compare as in
  [GPS time](../features/gps.md#gps-time)) from the first kmsg line after
  `/init` on; `/run/chef-time` says `rtc-offset`; a corrupt or missing file
  gives `none` and a normal boot; drift file present after a reboot and
  chronyd reading it (`chronyc tracking` frequency right from the start).

### C. Power log

- powerd gets a separate log directory (flag), so `state` and
  `shutdown-pending` stay on `/run/power`; inittab passes
  `/data/v1/power` when `/data` is ours, else nothing (today's behaviour).
- Raise the cap to 4 MiB with one rotation (about three weeks at the measured
  rate). Each boot starts with a `start` row that also carries the boot id so
  boots can be told apart; keep the existing columns otherwise unchanged
  (the stock-cloned format is documented).
- No `fsync` per row: `commit=5` bounds the loss to seconds, which is fine for
  this log. powerd holding the file open means `chef-storage shutdown` will
  TERM it before the read-only remount: make sure powerd's own poweroff paths
  (low battery, temperature) still complete, and that the last rows reach the
  disk (closing the log on TERM is enough).
- Acceptance: rows from before a reboot are still there after it, a RAM-only
  boot logs to `/run/power` as today, and a powerd-initiated poweroff
  (existing `--test` overrides, as in the powerd live tests) still powers off
  cleanly.

### D. Magnetometer bias (group 2980)

- Save only group 2980 (126 bytes), never the whole registry: the rest of
  `sns.reg` is factory data from `persist` and must keep coming from there.
  File `/data/v1/sensors/mag-group-2980` with version, group id, length, the
  SHA-256 of the pristine `persist` copy it was learned against
  (`/run/sensors/sns.reg.persist`), the SHA-256 of `sns_reg.map`, the bytes,
  and a checksum.
- Save when the ADSP writes group 2980 with a nonzero bias (items
  3903..3905) and on `sensord` exit. All-zero writes (the ADSP writes those
  before it has learned anything) never replace a saved nonzero bias.
- Restore when `sensors-up` makes a fresh copy from `persist`: overlay the
  saved group onto the `/run` copy before `sensord` serves it, only if both
  hashes match, the length matches the map and the bias is plausible
  (each axis within 2 G). `SENSORS_FRESH=1` or a new flag skips the overlay.
  The same-boot reuse path is unchanged.
- `sensord`'s rule that its registry write-back goes only to tmpfs/ramfs
  stays; the saved group is a separate, narrow file.
- Key live question: does the ADSP apply a seeded bias from the first sample?
  Acceptance: learn a bias (slow face-by-face holds, as in the
  [guided magcal run](../features/sensors.md#magnetometer-calibration), using
  the [guided prompt style](../live-testing.md)), reboot, run `sensors-up`
  and claim `heading` without moving the phone: `factory` minus `full`
  matches the saved bias and the heading reports calibrated within its 3 s
  rule. If the ADSP ignores the seed, report that with evidence instead of
  working around it. This step needs the user to hold the phone: ask through
  `rootfs_research` first.

### E. GNSS assistance (research, rootfs_research; not in this handoff)

The modem's EFS is served from a RAM shadow (`rmtfs -r`) and the TFTP
`/readwrite` namespace from a RAM shadow seeded at start. The diagnostic stdout flush now exposes complete RMTFS traffic promptly.
The bounded RAM session recorded two 2 MiB block-shadow writes and no new
TFTP requests, but no GNSS fix; these writes cannot yet be attributed to
GNSS rather than delayed startup or housekeeping. Persisting a shadow or
injecting assistance remains a separate decision for the user.

## Rules

- Remaining interactive tests are user-deferred (D was done live on
  2026-10-10, [log](../../logs/mag-seed-live-2026-10-10.txt)). When resumed, ask through
  `rootfs_research` before anything needing them (holding the phone for D,
  unplug, cold boot, hard reset).
  Fastboot boots of test images and ordinary reboots need no further
  permission.
- Flashing: after the RAM image (`out/boot-ram.img` built from the same tree)
  has passed a `fastboot boot`, install the new `system_a` + `boot_a` pair
  with `scripts/mkinstall.sh` exactly as in phase 3, each flash only after
  `rootfs_review` clears the exact hashes, `current-slot` `a` checked first,
  always explicit `_a` suffixes. Keep the current pair (`e72a3039` +
  `81a5c5ef`) in `out/` as the fallback.
- Kernel unchanged.
- Host tests in the existing style for every restore refusal and the
  RAM-only path (fake sysfs, fake RTC, fake pstore, fake `chronyc`, fixtures
  for group 2980); `make -C tools test` green.
- Regression on the installed pair: Wi-Fi + HTTPS, BT, chrony, GPS manager
  lease, sensors, buttond poweroff, `/data` binds, 20 ordinary reboots with
  clean shutdowns (the save step must not bring the hang back or slow the
  shutdown), and diskstats showing no writes to `persist` or EFS.
- Raw evidence in `~/chef-cyclo-evidence/state-persistence-20261006/`
  (private), a credential-free summary in `logs/`, a build log entry with all
  hashes, docs updated (storage guide table: what is persistent now; sensors,
  battery and GPS time guides; `storage-and-boot.md`, `gps-and-time.md` and
  `sensors.md` remaining lists). Stage the diff, do not commit; report to
  `rootfs_research` with the staged diff SHA-256. A part that is blocked (for
  example D waiting for the user) must not hold up reporting the others.

## Acceptance audit, resumed 2026-10-06

The installed first implementation has 20 clean ordinary reboots: two in
`reboots-newpair-try` and 18 in `reboots-newpair`, not 18 total. Those runs
validate the prior installed hashes, not later code fixes. The exact mapping
and remaining live checks are in [the acceptance audit](../../logs/state-persistence-2026-10-06.txt).
D was pending guided learning and seeded-first-sample acceptance; the user deferred remaining interactive tests on 2026-10-06. Preparation was cleaned up and no guided capture ran then. Update 2026-10-10: D accepted live on the installed pair (learn, save, reboot, seed applied from the first samples; [log](../../logs/mag-seed-live-2026-10-10.txt)).
Normal shutdown still targets about 2 s; blocking/failing operations retain
an explicit outer 5 s TERM plus 1 s KILL cap (an acknowledged failure-path
deviation from that target), with truthful outcome codes rather than a success
claim after timeout. Independent review and same-tree RAM boot precede each
new exact-hash flash clearance.

E research is recorded separately in [GNSS assistance research](../../logs/gnss-assistance-research-2026-10-06.txt):
valid predicted orbit data already exists; the modem advertises
`xtra3grcej.bin`; the flushed diagnostic run exposed block-shadow writes without establishing
GNSS attribution or a fix. Persistence and assistance injection remain
separate future decisions.
