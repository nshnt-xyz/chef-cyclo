# Persistent data

[Feature guides](README.md) · [Install handoff](../archive/install-layout-handoff.md)

Phase 2 adds writable storage on `userdata`, discovered by its unique
`PARTNAME` in sysfs. It requires slot `_a`, exactly 55,289,298,432 bytes
(107,986,911 sectors), an unmounted block device with matching kernel device
identity, no swap use and no block holders. All other partitions are outside
this command's scope.

Provisioning is explicit and destructive:

```sh
chef-storage format --yes-erase-userdata
```

This command refuses an already provisioned `chefdata` filesystem, an
unreadable existing ext4 label, missing confirmation, and every failed device
preflight. It never runs at boot. Before provisioning, save the old raw
superblock and `dumpe2fs -h` outside the repository and complete the handoff's
review and phone loop proof. The command creates ext4 with label `chefdata`,
`/chef-layout` containing `chef-cyclo-data-v1`, and private versioned service
directories. The positive feature allowlist and dedicated
`/etc/chef/mke2fs.conf` avoid modern distribution feature defaults unsupported
by the phone's kernel; format and mount both disable discard.

Before services start, `/init` runs `chef-storage boot`. It checks the label
and marker with bounded read-only probes, runs `e2fsck -p` with a 60-second
TERM deadline and five-second KILL escalation, and accepts only exit 0 or 1.
It mounts `/data` with `noatime,nodiscard,commit=5,errors=remount-ro` and binds:

| Persistent directory | Service path |
|---|---|
| `/data/v1/bluetooth` | `/var/lib/bluetooth` |
| `/data/v1/networkmanager/system-connections` | `/run/NetworkManager/system-connections` |

Directories are root-owned and mode 0700; connection profile files are mode
0600. NetworkManager continues using its existing keyfile path. Do not put
pairing keys or Wi-Fi credentials in logs, Git, or shared evidence.
Missing, foreign or corrupt storage falls back to RAM and records its reason
in `/run/chef/log/storage.log` and the kernel log, readable with `dmesg`
(`chef-storage boot` runs before syslogd starts; its later lines are in
`logread`, its shutdown lines stay in `dmesg`, see [logging](logging.md)).
The RAM paths each consumer falls back to are listed in
[runtime files](runtime-files.md).
`chef-storage status` shows the decision and mount status.

The five-second journal commit interval bounds ordinary metadata transaction
age without a constant eMMC write load. It does not guarantee that a recent
application write survives sudden power loss: applications must sync files
and their parent directories when durability matters. Ext4 journal replay
and the bounded boot check handle interrupted transactions; live power-loss
acceptance is recorded in the build log.

BusyBox init runs `chef-storage shutdown` before its global termination of
processes. The hook finds userdata mounts even if `/data` was detached, stops
filesystem users and syncs. It re-reads the userdata alias before each of its
TERM and KILL sweeps, and a sweep whose path stopped being a userdata mount
while `fuser -m` ran is redone on the next alias (at most 3 tries per signal):
on a detached path `fuser -m` names every process on the root filesystem
(seen 2026-10-10 when two shutdowns overlapped, see
[SSH and install](../../logs/ssh-and-install-2026-10-10.txt)). Only the
milliseconds between that check and the kills stay unguarded. If everything
is detached meanwhile it returns 0. A bounded filesystem-wide read-only remount must
succeed before any binds are detached; it protects surviving aliases too.
The hook then removes service binds and `/data`, and checks mountinfo before
claiming full unmount. Failed detaches leave read-only aliases and a log entry.
Consumer setup rollback uses the same read-only transition; if it fails, all
aliases must be detached or an explicit rollback failure is logged. Normal `reboot`,
`poweroff`, buttond and powerd take this path. Forced syscall/sysrq reboots
and the PMIC hard reset bypass it by design.

The return to fastboot (`scripts/phone-boot.sh`, the fastboot test and
install workflow) runs `chef-reboot bootloader` detached (since 2026-10-10,
[log](../../logs/reboot-bootloader-2026-10-10.txt)). `btprobe restart
bootloader` is a raw `reboot(2)` that skips init's `::shutdown` lines, so the
helper does their work first: it sends init SIGTSTP (busybox init then
reaps but respawns nothing), runs `chef-state shutdown` and `chef-state
log-save` (bounded as in inittab), `openrc shutdown` (every other service,
[services](services.md#shutdown-reboot-and-the-return-to-fastboot); 23.5 s
live against 14.2 s at a power-off, since a paused init reaps each
stopped supervisor only once a second) and `chef-stop-supervisors` (any
supervisor that step left), then the plain `chef-storage shutdown` hook,
`sync`, and `btprobe restart bootloader`. Every step runs in the background
with a bound (chef-state 8 s, log-save 5 s, openrc 45 s, supervisors 12 s,
chef-storage 100 s, sync 20 s) and is abandoned when it runs over, so a step stuck in D state
does not block the reboot; a failure or overrun is logged to kmsg and
`/run/chef/log/chef-reboot.log` and the helper goes on to the bootloader,
which the recovery path depends on. btprobe calls `sync(2)` itself before
rebooting, so a wedged eMMC can still hold it (then only the PMIC reset helps). An exit trap
resumes init with SIGCONT on every path that does not reboot (btprobe
returning, INT/TERM), a detached 205-second watchdog recovers a killed
helper, SIGHUP is ignored (a closing telnet session) and a lock in
`/run/chef/lock` keeps a second run from resuming init under the first. Init is paused once,
by the helper only: chef-storage's own `shutdown --pause-init` mode (an exit
trap that sends SIGCONT and a 120-second watchdog of its own) would resume
init before the reboot. That mode stays for images built before the helper,
where `scripts/phone-boot.sh` falls back to `chef-storage shutdown
--pause-init; sync; sleep 2; btprobe restart bootloader`, which refuses to
restart after a shutdown failure and does not run `chef-state shutdown`. Use
the default shutdown hook for orderly init poweroff/reboot; sending SIGTSTP
from that hook would stall init.

## Persistent state

[Acceptance audit](../../logs/state-persistence-2026-10-06.txt) distinguishes the prior installed image from subsequent fixes and the live checks then pending. The magnetometer bias seed (part D) was accepted live on 2026-10-10 ([log](../../logs/mag-seed-live-2026-10-10.txt)).

Since 2026-10-06 (`chef-state`, `tools/rtc-edge.c`; [handoff](../archive/state-persistence-handoff.md)) these also survive a reboot:

| State | Directory | Written by | Read back by |
|---|---|---|---|
| Crash records (pstore) and boot history | `/data/v1/crash` | `chef-state boot` in `/init` | you (`cat`, `ls`) |
| Wall-clock offset to the RTC | `/data/v1/time/rtc-offset` | `chef-state timekeeper` (the `chef-timekeeper` service) and `chef-state shutdown` | `chef-state boot` in `/init` |
| chrony drift | `/data/v1/chrony` bound on `/var/lib/chrony` | chronyd (about hourly, and on exit) | chronyd at start |
| Power log | `/data/v1/power/log.csv(.1)` | `powerd -l`, batched through `/run/chef/power/log.pending` ([battery](battery-and-charging.md)) | you |
| Magnetometer hard-iron bias (REG2 group 2980) | `/data/v1/sensors/mag-group-2980` | sensord via `chef-state mag-save` (after a registry write-back, at most once a minute) and `chef-state shutdown` | `sensors-up registry` via `chef-state mag-restore` ([sensors](sensors.md#magnetometer-calibration)) |
| GPS switch (gps-manager `enable`/`disable`) | `/data/v1/gps/enabled` (`gps` 0700, created on the first save) | `gps-manager` via `chef-state gps-save 0\|1` | `gps-manager` via `chef-state gps-load` at start; `/run/chef/state/gps-enabled` when `/data` is not ours ([GPS](gps.md#use-gnss-gps-manager)) |
| BLE sensor records (address, kinds, paired date, name, pending Forget) | `/data/v1/blesens/sensors` (`blesens` 0700, file 0600, created on first save) | `blesensd` via `chef-state blesens-save`, sealed atomic writes | `blesensd` via `chef-state blesens-load`; `/run/chef/state/blesens` without owned `/data` ([Bluetooth](bluetooth.md#ble-sensors)) |
| Wi-Fi switch and NM radio state (`WirelessEnabled`, `NetworkingEnabled`, `WWANEnabled`) | `/data/v1/networkmanager/NetworkManager.state` (in the 0700 `networkmanager` directory, written by NM as an atomic temp file plus rename) | NetworkManager, started by `wifi-nm` with `--state-file` | NetworkManager at start; the tmpfs `/var/lib/NetworkManager` default when `/data` is not ours ([Wi-Fi](wifi-ui.md)) |
| This boot's syslog ring ([logging](logging.md)), at every orderly shutdown | `/data/v1/log/syslog-<seq>` (`log` 0700, files 0600: Wi-Fi names and Bluetooth addresses are in there; `<seq>` is the boot's number in `crash/boots.log`, `syslog-noseq-<boot_id>` without one); the newest 8 kept, older ones removed while the directory holds more than 8 MiB | `chef-state log-save`, inittab's `::shutdown` line between `chef-state shutdown` and `chef-storage shutdown`, and `chef-reboot` | you: `ls -l /data/v1/log`, `cat /data/v1/log/syslog-<seq>` (the previous boot's is the highest number below this boot's, `tail -n 2 /data/v1/crash/boots.log`) |
| UI settings (brightness, theme, auto-sleep, time zone, alert volume, haptics, alert sounds, quiet when paused, 24-hour clock) | `/data/v1/ui/settings` (`ui` 0700, file 0600, created on the first save) | `chef-settings` 2 s after a change and at exit, same checksum and atomic-write rules, on its persistence worker | `chef-settings` at start; `/run/chef/ui/settings` when `/data` is not ours, adopted when `/data` appears ([system UI](system-ui.md#settings)) |

`chef-storage boot` creates `crash`, `time`, `power`, `sensors` and `ssh` (the [SSH](ssh.md) host key; root, 0700) and `chrony` (`chrony:chrony`, 0700) after the BlueZ and NetworkManager binds, and binds `chrony` on `/var/lib/chrony`. A failure there is logged and leaves the other binds in place. A failed chrony bind leaves its tmpfs directory; a missing or foreign `/data` gives all consumers their RAM paths. On an owned but unwritable `/data`, state saves are skipped or fail with a log line; powerd falls back if creating its log directory fails, and keeps its pending rows on `/run` (bounded, oldest dropped) while flushing them fails.

Common rules, all in `rootfs-overlay/usr/bin/chef-state`:

- **Ours only.** Nothing is read from or written to `/data` unless `chef-state ours` holds: `/data` is a mount point and `/data/chef-layout` reads `chef-cyclo-data-v1`. Otherwise every consumer runs from `/run` as before, with one log line, and `/run/chef/state/time` says `source=none reason=ram-only`.
- **Atomic, checked files.** `rtc-offset` and `mag-group-2980` are short text files whose last line is `sha256` of the lines before. They are written as a temp file in the same directory, fsynced, renamed and the directory fsynced, under a per-kind lock in `/run/chef/state`. On read, the version, line layout, checksum and ranges are checked; a file that fails is ignored with a `chef-state: ... ignoring ...` log line (kmsg at boot and shutdown, syslog otherwise), never trusted and never fatal.
- **Bounded.** `/init` runs `timeout -k 2 25 chef-state boot`. busybox init runs `timeout -k 1 5 chef-state shutdown` as the first `::shutdown` line (and in `::restart`), before `chef-storage shutdown`. It stops powerd, sensord and the timekeeper through OpenRC at entry (`rc-service <name> stop`, all at once in the background, so their exit work overlaps the RTC save; a supervised daemon signalled by anything but its supervisor would be respawned, so it never signals one itself). The timekeeper's service TERMs its whole process group (supervise-daemon `setsid()`s it), which also stops a save in flight with its subshell and chronyc/rtc-edge children. A save killed before its rename leaves only a temp file, which the next save removes. The shutdown save then waits at most about 1 s for the save lock (a `flock -n` retry loop: BusyBox `flock` has no `-w`) and is skipped with a `time: save lock still busy` kmsg line if a save that is not the timekeeper's still holds it; its own work is bounded by chronyc 1 s and one RTC edge 1.5 s, and the chronyc and rtc-edge calls never pass the lock descriptor to BusyBox `timeout`'s watchdog daemon, which can outlive them. chronyd stays alive for that query and is then stopped (through OpenRC) while `/data` is still writable. After the stop wait, rows a stopped powerd left in `/run/chef/power/log.pending` are appended with `powerd ... flush` (only when no powerd is alive). The process-stop wait is at least 0.3 s even when a slow time save used up the 1.8 s budget. Nothing respawns them: a stopped service's supervisor is gone, and init does not respawn during the `::shutdown` lines. A shared approximate 1.8 s process-stop budget leaves time for the final bias snapshot and sync near the normal 2 s target; proc reads and subprocess overhead mean this is not a hard wall-clock bound. The check snapshots process start times before TERM, counts dead/zombie processes as exited, detects reused PIDs, and treats unreadable or malformed live process state conservatively. Shutdown logs return codes and whether those processes exited; exit alone does not prove a drift write succeeded, and a timeout cannot confirm it. Drift readback/startup-frequency evidence is recorded separately. The final installed implementation measured 0.23 to 1.11 s over 20 readiness-gated reboots; this is normal-path evidence, not a worst-case guarantee. A blocked hook is bounded by the outer 5 s TERM timeout plus 1 s KILL grace (the timeout targets the hook PID and does not guarantee every descendant exits), so the failure path can exceed the approximate 2 s normal target. It works from `/`, so `chef-storage shutdown`'s `fuser` never finds it.
- **Small write load.** The power log reaches `/data` in batches at least every 10 minutes, and at once for plug/unplug, status, alert and throttle changes and the shutdown row. Every orderly shutdown (any path through init's `::shutdown` lines) appends and fsyncs the pending rows: powerd's exit flush on TERM, or `chef-state shutdown`'s fallback once powerd is confirmed gone. The return to fastboot through `chef-reboot bootloader` (`scripts/phone-boot.sh` since 2026-10-10) runs `chef-state shutdown` first and keeps them too. A hardware reset, power loss, panic, `reboot -f`, the 5 s hook bound expiring or a bare raw reboot to the bootloader (`btprobe restart bootloader` typed directly, or `scripts/phone-boot.sh`'s fallback chain on an image built before the helper: `chef-storage shutdown --pause-init` does not run `chef-state shutdown` and does not stop powerd) can lose up to one interval of ordinary rows ([battery](battery-and-charging.md)). The offset file is rewritten only when the offset moved by 0.1 s or more, or 6 hours of RTC time passed; the bias only when its bytes change and at most once per minute during supervision (an exit/shutdown save may bypass this interval); `boots.log` gets one line per boot; the syslog ring is copied once per orderly shutdown (at most 1 MiB), never while the system runs.

**Clock.** The PM660 RTC is write-disabled and counts whole seconds from battery connect (`since_epoch`), so the kernel boots the clock at 1970. `rtc-edge sample` waits for the RTC's next second tick (2 ms polls, at most 1.1 s) and returns the wall time at that instant, so the saved `offset_ns` (wall minus `since_epoch`) is good to a few ms. The timekeeper saves it only while chrony is synchronised to a real source (leap status `Normal`, reference not `00000000`, `7F7F0101` or `LOCL`; the GPS refclock counts): every minute until the first save of the boot, then every 15 minutes, and once at shutdown. At boot, right after `chef-storage boot`, `rtc-edge set` sets the clock at the next RTC tick to `since_epoch + offset`. A `since_epoch` below the saved one (the battery was disconnected), more than 5 years of RTC time since the save, a result outside 2026 to 2100, or no RTC tick falls back to the saved wall time as a floor, applied only if the clock is behind it. `/run/chef/state/time` records `source=rtc-offset|floor|none`, a `reason` and `saved_age_s` (RTC seconds since the save). A reset is only detected while the new RTC counter is below the saved counter; if enough time passes after battery disconnect for it to catch up, this file cannot distinguish the reset from continuous operation. The time stays unverified until chrony syncs; `makestep 1.0 -1` steps any later error. The ext4 check at boot runs before the clock is restored, so it still sees a 1970 clock, as before.

**Crash records.** At boot pstore is mounted read-only on `/sys/fs/pstore`; every non-empty record whose SHA-256 is not already saved is copied into `/data/v1/crash/<seq>-<bootreason>/` with an `info` file (this boot's id, the previous boot's id, `androidboot.bootreason`, `since_epoch`, wall time if the clock was restored, and each file's size and SHA-256), then pstore is unmounted. Nothing is removed from pstore. One kmsg line names the directory. The newest 32 record directories are kept, and older ones are removed while the directory holds more than 16 MiB. `boots.log` gets `seq=N boot_id=... bootreason=... since_epoch=... wall=...|na pstore=N` per boot, appended and fsynced (a torn last line is skipped when the next sequence number is read), rotating to `boots.log.1` at 256 KiB. What survives in pstore: a kernel panic's `dmesg-ramoops-0` and `console-ramoops-0`, for one boot (the warm PON after the panic). An unplugged buttond poweroff followed by Power on on 2026-10-06 also kept only the empty annotate record: no previous console survived. An ordinary `reboot`, the bootloader path and a `fastboot boot` keep nothing but an empty `annotate-ramoops-0` (checked 2026-10-06), so the previous boot's console log is not available after a clean reboot.

The system UI's Storage page reads these three category paths (maps, rides, routes) read-only
and bounded ([system UI](system-ui.md#device-pages-m4)): allocated blocks, never following
symlinks or leaving the `/data` mount, absent directories count as zero and are never created.

Reserved follow-up locations, still volatile until their consumers migrate:

| State | Planned directory |
|---|---|
| Rides | `/data/v1/rides` |
| Map tiles | `/data/v1/maps` |
| Planned routes | `/data/v1/routes` |
| GNSS RAM shadow | `/data/v1/gnss` |

Since phase 3 the root filesystem is the read-only `system_a`
([installed layout](../building.md#installed-layout-phase-3)); `/var` and
`/root` are tmpfs seeded at boot, so state outside `/data` is lost at every
reboot, as it was with the RAM root. The full RAM image (`out/boot-ram.img`)
uses the same `/data` handling. Recovery uses fastboot in the unchanged
bootloader, reached by holding VolDown through a Power-held reset. Flash a
known-good `boot_a` image with an explicit `_a` target; never use
`fastboot set_active`. See the [recovery guide](../device.md#stock-backups-and-recovery).

On the final 2026-10-06 pair, a settled 300.40 s idle interval wrote 237,568
bytes to all userdata (65.16 MiB/day extrapolated, including journal and
other consumers); the power log grew 2,804 bytes in 16 rows (0.77 MiB/day
payload extrapolated). These are short-interval estimates, not long-term
guarantees. GPS leases and sensors were off; powerd, chrony, the timekeeper,
NetworkManager and BlueZ were active. Protected partition write counters
stayed zero. [Exact images and raw evidence mapping](../../logs/state-persistence-2026-10-06.txt).
