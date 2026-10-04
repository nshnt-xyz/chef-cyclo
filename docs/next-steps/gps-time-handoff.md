# GPS time to chrony (handoff)

[Next steps index](README.md) · [Time synchronization plan](gps-and-time.md#time-synchronization) · [GPS guide](../features/gps.md) · [Building](../building.md) · [Live testing](../live-testing.md)

**Status:** handed to Herdr agents `gnss_impl` (implementation, live runs)
and `gnss_review` (review) on 2026-10-04; implemented and live-verified the
same day: [GPS time](../features/gps.md#gps-time), [build log](../build-log.md).

## Goal

The clock starts at 1970 on every boot (the PM660 RTC is write-disabled and
counts only from battery connect). chrony steps it from the NTP pool once
Wi-Fi is up; without network the clock stays wrong, and the broker runs
with `-n` so it never steps the clock either. Make GPS a chrony time source:
whenever the [GPS manager](../features/gps.md#use-gnss-gps-manager) has gpsd
running with a fix, chrony should be able to set the clock from it, offline,
and NTP should stay the better source when it is reachable.

## Known facts

- This kernel returns `ENOSYS` for `shmget`: `CONFIG_SYSVIPC` is off. The
  rootfs gpsd (Alpine 3.27.3-r1) has the SHM export (`ntpshm`, `TOFF`
  messages, `GPSD_SHM_KEY`). chrony 4.8 is already in the rootfs and runs
  client-only from inittab as `chronyd -d -u chrony` with
  `initramfs/etc/chrony/chrony.conf` (`pool pool.ntp.org iburst`,
  `makestep 1.0 3`, `port 0`, `cmdport 0`).
- gpsd runs only while a lease is held, started by `tools/gps-manager.c` as
  `gpsd -N -n -b udp://127.0.0.1:20175` and dropping to `nobody`. chrony must
  cope with the refclock appearing and disappearing.
- There is no PPS. Time comes from NMEA sentence arrival through
  qmicli, the broker and UDP, so expect a constant offset of tens to hundreds
  of ms plus jitter; it is a coarse source for the 1970 step and offline
  holdover, not a precision reference.
- Earlier finding: gpsd reported `TPV mode 3` even with the 1970 clock.

## Work

1. **Kernel.** Add `CONFIG_SYSVIPC=y` to `kernel-config/chef-cyclo.config`,
   rebuild (`chef_defconfig`, `kmake`, see [building](../building.md)).
   SYSVIPC changes kernel structures, so expect `Module.symvers` and module
   ABI to change: rebuild the out-of-tree WLAN module with
   `scripts/build-wifi.sh` and check every other `.ko` in the ramdisk is from
   the new build. Check the [loader budget](../research/chef-loader-kernel-budget.md)
   margin with the new kernel. Kernel source changes go in the kernel
   submodule only if a config line is not enough (it should be).
2. **Handoff choice.** Prefer gpsd SHM into `refclock SHM`. Work out the
   segment permissions (verify against the gpsd source/docs: units 0/1 are said to be root-only (0600) while gpsd drops to
   `nobody`, units 2+ world-accessible); chronyd attaches as root before
   dropping to `chrony`. Pick the unit and `perm` so it works across gpsd
   starting after chronyd, gpsd restarts and manager teardown. If SHM proves
   unworkable, evaluate gpsd's chrony socket handoff instead and record why.
3. **chrony config.** Add the refclock with `refid GPS`, a measured `offset`
   and suitable `delay`/`precision` so NTP wins when both are available (do
   not mark it `prefer`), and decide `makestep`: the plan suggested
   `makestep 1 -1`; justify whatever is chosen for an offline boot where GPS
   arrives minutes after chronyd starts and for a later NTP correction.
   Keep chrony client-only (`port 0`, `cmdport 0`). Remove the
   "No SHM refclock" comment. The broker keeps `-n`.
4. **Measure the offset.** With Wi-Fi associated and NTP selected and the
   phone at a window with a fix, log the GPS refclock offset in
   `chronyc sources`/`sourcestats` (use the root Unix socket) for long
   enough to set `offset` and see the jitter.
5. **Offline acceptance.** Fresh boot with Wi-Fi not connecting (radio off
   or no profile), clock at 1970: take a `map` lease, and chrony must select
   GPS and step the clock. Compare against host time over the USB link
   (record how). Release the lease: chrony must go unsynchronised or hold
   without errors, and a new lease must resync. Then bring Wi-Fi up and
   confirm NTP takes over without a large step.
6. **Regression.** gps-manager lease/teardown unchanged, Wi-Fi + HTTPS
   (certificate checks depend on the clock), BT, audio, sensors start as
   before on the new kernel, modem `crash_count` 0, no EFS/persist writes.

Host tests: whatever config checks fit (for example the existing
`mkinitramfs.sh` checks for `.config` options), plus `make -C tools test`.

Docs: `docs/features/gps.md` (time section: replace the ENOSYS text),
the time synchronization section of `gps-and-time.md` (keep only what
remains: RTC offset persistence, USB host chronyd), building notes if the
kernel/WLAN rebuild order changes, build log with kernel, ramdisk and image
hashes. Build test images under another name; promote to `out/boot.img`
only after sign-off (the coordinator decides).

## Starting point and gotchas

- Phone: on the baseline `out/boot.img` `58aa9de1` (gps-manager), USB
  plugged, at a window. Fastboot boots of test images need no permission;
  ask the user before any USB unplug or phone move.
- Prior evidence and drivers: `~/chef-cyclo-evidence/gpsmgr-live-20261004T150343Z/`
  and `gnss-rf-20261004T123649Z/`; the gotchas in the
  [GNSS-only RF handoff](gnss-only-rf-handoff.md) and
  [GPS manager handoff](gps-manager-handoff.md) still apply (setsid,
  no bare trailing `&` in remote commands, positions only in gitignored or
  redacted logs, manual LOC commands need their own CID).
- Messages to the user: no em dashes.
