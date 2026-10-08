# State persistence follow-up: batched power log, bounded shutdown lock

Two fixes on top of 71163bd ("Persist crash, clock, power and sensor state
on data"), from the coordinator's review of that commit on 2026-10-08.
Read `docs/next-steps/state-persistence-handoff.md`,
`docs/features/storage.md` ("Persistent state") and
`logs/state-persistence-2026-10-06.txt` first: everything there still
holds unless this file changes it.

Roles: `persist_impl` implements, `persist_review` reviews, `persist_coord`
(the coordinator) only researches and relays to the user. Report to the
coordinator with `herdr agent prompt persist_coord "..."` when done or
blocked. No em dashes anywhere (code, docs, messages, commits).

## 1. Power log on /data: batch the appends

### Problem

On the final 2026-10-06 pair a settled 300.40 s idle window wrote 237,568
bytes to userdata (65.16 MiB/day extrapolated) while the power log grew
2,804 bytes in 16 rows (0.77 MiB/day). That is about 85 times the payload.
The likely cause (inferred, not yet measured in isolation) is
`csv_append` in `tools/powerd.c`: with `-l /data/v1/power` it opens,
appends one row and closes the file for every row (about one row per
19 s idle), and each dirty row costs an ext4 journal commit (commit=5):
data block, inode, journal descriptor and commit blocks. Endurance is not
the worry at this rate; the eMMC waking every ~19 s and the amplification
are.

First step: confirm the attribution. On the RAM image, measure userdata
write sectors (`/sys/block/<userdata>/stat` or diskstats, as the earlier
evidence did) over the same kind of settled idle window with powerd
logging to /data and then with powerd stopped or on `/run`. Record both
in the log. If powerd is not the dominant writer, stop and report to the
coordinator before changing powerd.

### Design (adjust if you find a better one, say why)

Only the `-l LOGDIR` case changes. Without `-l` (log on `/run/power`,
tmpfs) keep today's per-row append.

- Rows are still formatted per sample exactly as now, but with `-l` they
  are appended to a pending file on tmpfs, `RUNDIR/log.pending`
  (`/run/power/log.pending`), which is cheap and survives a powerd crash
  and respawn within the boot.
- A flush appends the whole pending file to `LOGDIR/log.csv` in one
  `write` (or as few as possible), applying the existing rules: header on
  a new file, rotation to `log.csv.1` if the batch would pass `log_max`
  or the header differs. Then the pending file is truncated. A failed
  flush keeps the pending rows (bounded, see below) and is logged once,
  like the current `log_failed` handling.
- Flush triggers:
  - pending rows older than the flush interval, default 600 s, settable
    with a new option (test-overridable; document it in `usage()` and the
    header comment);
  - immediately for rows that matter after a crash or power loss: the
    start row, a supply plug/unplug, a battery status change, warn or
    critical alert changes, a throttle level change, and the shutdown row
    (`shutdown_now`), which must reach `LOGDIR` before the shutdown
    command runs (fsync that one: it is the last thing before poweroff);
  - on SIGTERM/SIGINT before `main` returns. `chef-state shutdown` TERMs
    powerd and allows it about 1.8 s shared with the RTC save, so the exit
    flush must be one small append, never a wait;
  - at start: leftover `log.pending` from a previous powerd in this boot
    is flushed first.
- User requirement (2026-10-08): pending rows must reach the disk
  whenever the device shuts down, on every path, before `/data` goes
  read-only: powerd's own low-battery or overtemp poweroff, a buttond
  poweroff, `reboot`/`poweroff` from a shell, and the inittab
  `::restart` path. The exit flush is fsynced (file and directory), not
  only written. If powerd has not exited inside chef-state's stop budget,
  consider a fallback in `chef-state shutdown`: once powerd is confirmed
  gone, append any leftover `/run/power/log.pending` to the log
  directory (same rotation rules) before `chef-storage shutdown`; never
  while powerd may still be writing. Only a hardware reset (the 8.7 s
  PMIC hold) or sudden power loss may lose pending rows.
- Bound the pending file (for example 256 KiB). If a flush keeps failing
  and it is full, drop the oldest pending rows with one kmsg line rather
  than growing tmpfs without limit.
- `powerd status` and the state file are unchanged.

Trade-off to document in `docs/features/battery-and-charging.md` and
`docs/features/storage.md`: on abrupt power loss up to one flush interval
of ordinary rows can be lost (today it is up to the writeback delay); the
important rows above are flushed at once.

### Acceptance

- Host: extend `tools/tests/test_powerd.c` / `test_powerd.sh` for the
  batching: rows reach `LOGDIR` only on the triggers, the immediate
  triggers, TERM flush, leftover pending flush at start, rotation and
  header-mismatch rotation on a batch, failing `LOGDIR` keeps rows and
  logs once, the pending bound. `make -C tools test` passes.
- Live on a RAM image (`fastboot boot`): the same settled idle diskstats
  measurement as 2026-10-06 (at least 300 s, longer is better) with the
  new powerd, compared against the attribution baseline above; an
  ordinary reboot whose `log.csv` on /data ends with the rows up to the
  shutdown; the powerd threshold-test poweroff still lands its shutdown
  row on /data. For each shutdown path above, the rows logged in the
  last minute before shutdown are in `/data/v1/power/log.csv` after the
  next boot (host tests for each path too).

## 3. chef-state shutdown: do not queue behind the timekeeper

### Problem

`do_shutdown` (`initramfs/usr/bin/chef-state`) TERMs the timekeeper's main
shell, but the timekeeper's save runs in the `locked` subshell (a separate
process) together with its chronyc and rtc-edge children, and TERM to the
parent does not stop them. `do_shutdown` then calls `time_save`, which
blocks in `flock 8` until that save finishes: up to `CHRONYC_SECS` 3 s for
chronyc plus up to 1.1 s for `rtc-edge sample`, then its own chronyc (1 s)
and rtc-edge (1.1 s). With a slow or hung chronyd that can use up the
outer `timeout -k 1 5` hook bound, so chef-state is killed before it TERMs
chronyd (drift file not written while /data is writable) and before the
final magnetometer save. The outer timeout kills only the hook PID, so the
timekeeper's subshell can also outlive the hook into `chef-storage
shutdown` while it may hold a descriptor on `/data/v1/time`.

### Design

- Stop the whole timekeeper, not just its shell: busybox init `setsid()`s
  each child, so the timekeeper should be its own process group. Verify
  that on the device (pgrp field of `/proc/PID/stat` equals the PID) and
  in code before using it; if it holds, `kill -TERM -- -$keeper`, else
  fall back to finding the subshell and children by parent PID. Keep the
  existing cmdline check that the PID file still names a `chef-state
  timekeeper`. A save killed mid-`put` leaves only a `rtc-offset.tmp.*`,
  which the next save already removes under the lock; the rename is the
  commit point, so the saved file is never torn.
- Bound the lock wait in the shutdown path to about 1 s (or the remaining
  part of the 1.8 s budget). The device's busybox `flock` has no `-w`
  (only `-s -x -n -u`; host util-linux flock hides this in tests), so use
  a `flock -n` retry loop. If the lock is not obtained,
  skip the shutdown time save with a kmsg line (`time_save_rc` must say
  skipped, as now) and go on to TERM chronyd and the bias save.
- Keep the shutdown time save's own work bounded so that the chronyd TERM
  and `mag_save` always run well inside the 5 s hook bound, even when
  chronyc hangs (chronyc 1 s is already there; give the shutdown
  `rtc-edge sample` a timeout that fits the budget).
- No change to the timekeeper's normal cadence or the save rules.

### Acceptance

- Host: `tools/tests/test_chef_state.py` cases for a timekeeper blocked
  inside its locked save (stub chronyc that sleeps 3 s and a stub
  rtc-edge that sleeps): shutdown still TERMs chronyd and runs the final
  mag save, finishes in about 2 s, reports the time save as skipped, and
  no timekeeper subshell or child is left running afterwards. Existing
  shutdown tests (`test_shutdown_*`, the outer hook bound test) still
  pass. `make -C tools test` passes.
- Live on the RAM image: the 20-cycle style readiness-gated reboot check
  can be shortened to a few cycles for this change, plus one cycle with
  the timekeeper made to block (for example a temporary chronyc wrapper on
  `/run`, never on system_a), showing chronyd stopped and the drift file
  read back after the reboot.

## Process

- Same project rules as before: no writes to persist, EFS, or any
  partition other than userdata through the existing layout; rollback
  images stay in `out/`. Phone live-test drivers and gotchas are in the
  earlier logs (`logs/state-persistence-2026-10-06.txt`) and
  `docs/next-steps/state-persistence-handoff.md`.
- RAM images via `fastboot boot` and reboots of the phone are fine for
  testing. Installing to `boot_a`/`system_a` needs the reviewer's
  clearance against the exact staged diff and then a go from the
  coordinator, who will ask the user. Ask before any step that needs the
  user to touch the phone.
- Record results in `logs/state-persistence-followup-2026-10-08.txt`
  (exact image hashes, evidence paths) and update `docs/build-log.md`,
  `docs/features/storage.md`, `docs/features/battery-and-charging.md` as
  needed. Commit only after the reviewer approves; one commit per fix is
  fine.
