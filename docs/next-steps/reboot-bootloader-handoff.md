# Reboot to the bootloader without losing state

User decision 2026-10-10: add a helper so our test/flash loop's return to
fastboot runs the same state work as an orderly shutdown. Follows
8201c79 / 03f40ce (see `logs/state-persistence-followup-2026-10-08.txt`
section 4, "loss cases", and `docs/next-steps/state-persistence-followup-handoff.md`).

Roles: `persist_impl` implements, `persist_review` reviews, `persist_coord`
coordinates and talks to the user. Report with `herdr agent prompt
persist_coord "..."`. No em dashes anywhere.

## Problem

`scripts/phone-boot.sh` (and the install workflow that copies its
pattern) returns to fastboot with, on the phone:

    chef-storage shutdown --pause-init; sync; sleep 2; btprobe restart bootloader

`btprobe restart bootloader` is a raw `reboot(2)` RESTART2, so init's
`::shutdown` lines never run. `chef-storage shutdown` does make `/data`
read-only, but `chef-state shutdown` never runs and powerd is not stopped
(it holds no `/data` descriptor between flushes, so `fuser -m` misses
it). Lost each time: the pending power rows in `/run/power/log.pending`,
the last RTC offset save, and chronyd's drift write.

## Goal

One on-phone helper (name it; `chef-reboot bootloader` is a suggestion,
installed in `/usr/bin` on both the RAM image and system_a, and usable
from stage-1 if stage-1 ever needs it) that does, in order:

1. Pause init first (`kill -TSTP 1`, as `chef-storage shutdown
   --pause-init` does today, with its CONT watchdog), so init does not
   respawn chronyd, powerd or the timekeeper after chef-state stops them.
   Check how `--pause-init` and its watchdog/traps compose when
   chef-state runs inside the paused window; do not pause twice or leave
   init stopped on any failure path.
2. `chef-state shutdown` (bounded as in inittab: `timeout -k 1 5`), which
   saves the offset, stops chronyd (drift write) and sensors-up, TERMs
   powerd (its fsynced exit flush) and runs the powerd `flush` fallback.
3. `chef-storage shutdown` (read-only + detach), then `sync`.
4. `btprobe restart bootloader`.

A failure in 2 or 3 must not prevent reaching the bootloader (the user's
recovery path depends on it), but must be logged to kmsg and the helper's
log. Then switch `scripts/phone-boot.sh` and any other caller (grep for
`restart bootloader`; the install workflow, docs that show the command)
to the helper, keeping a fallback for older images that lack it (the
current `command -v chef-storage` pattern).

Optional, only if trivial and safe: `chef-reboot` without an argument for
a plain reboot through init (`reboot`) is not needed; init already
handles it.

## Acceptance

- Host tests in the style of `tools/tests/test_chef_state.py` /
  `test_storage.py` with stubs: order of the steps, init paused before
  chef-state and resumed on every failure path, bootloader reached when
  chef-state or chef-storage fail or hang, no double pause.
  `make -C tools test` passes.
- Live on a RAM image: return to fastboot with the helper and show, after
  the next boot, the pending power rows on `/data`, a fresh
  `rtc-offset` save (when synced) and a drift file mtime at the
  shutdown, userdata cleanly unmounted (no journal recovery at the next
  mount), protected write counters 0. Then the normal fastboot loop still
  works end to end (`scripts/phone-boot.sh` boots the next image).
- Update the loss-case text added in 03f40ce (log section 4,
  `docs/features/battery-and-charging.md`, `docs/features/storage.md`)
  to say this path is now covered, with the evidence.

## Process

- The coordinator is using the phone first for a magnetometer test. Do
  host-side work and host tests now; do not touch the phone until
  `persist_coord` says it is free.
- RAM images and reboots are fine once the phone is released. An install
  to boot_a/system_a needs reviewer clearance against the exact diff and
  then the coordinator's go (the user decides). The helper only matters
  for the dev loop, so a RAM-image-only acceptance is fine if the
  installed pair does not need it yet; say which you recommend.
- Record results in `logs/reboot-bootloader-2026-10-10.txt` and the
  build log. Commit only after the reviewer approves.
