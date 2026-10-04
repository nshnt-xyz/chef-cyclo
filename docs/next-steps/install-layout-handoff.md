# Install layout: retire Android (handoff)

[Next steps index](README.md) · [Storage and boot plan](storage-and-boot.md) · [Device reference](../device.md) · [Building](../building.md) · [Live testing](../live-testing.md)

**Status:** handed to Herdr agents `gnss_impl` (implementation, live runs)
and `gnss_review` (review) on 2026-10-04. Phase 1 done 2026-10-04
(`boot_a` holds `out/boot-abslot.img`; slot flags answered and handled by
`abslot`, see [standalone boot](storage-and-boot.md#standalone-boot)).
Phases 2 and 3 not started.

## Decision (user, 2026-10-04)

Android is retired on this phone. Final layout, all on slot `_a`:

| Partition | Size | Holds |
|---|---|---|
| `boot_a` (`mmcblk0p44`) | 64 MiB | our kernel + a small stage-1 initramfs |
| `system_a` (`mmcblk0p67`) | 2560 MiB | the rest of the OS: the Alpine root filesystem, read-only |
| `userdata` (`mmcblk0p69`) | 52727 MiB | everything writable, mounted at `/data` |

The user approved flashing `boot_a`, overwriting `system_a` and reformatting
`userdata` (Android's data is given up). Everything else stays untouched:
`system_b`, `vendor_*`, `boot_b`, `modem_*`, `dsp_*`, the bootloader chain,
`persist`, EFS (`modemst*`, `fsg*`, `fsc`) and the rest. The earlier
`system_b` data plan is superseded; `system_b` keeps Android's old slot-`_b`
image (its hash matched the backup on 2026-10-04,
`~/chef-cyclo-evidence/storage-20261004/`).

## Order (each phase reviewed, committed and reported before the next)

The order matters for safety. As long as `boot_a` holds Android (Magisk),
any ordinary reboot starts Android, and Android must never see a
`userdata` or `system_a` we have changed: its init would try to wipe or
"repair" `userdata`, and its kernel would mount our `system_a` as its root.
So `boot_a` gets our image first.

### Phase 1: our image in `boot_a` (standalone boot)

Run the existing [standalone boot procedure](storage-and-boot.md#standalone-boot)
with the current baseline `out/boot.img` (`343fc3f7`, full OS in the
ramdisk, nothing else changes). Check the restore images first
(Magisk `c6ab9f3a…` and stock `c77eb87d…`; copy the Magisk image next to
the stock backups as the procedure says).

Also answer the **A/B slot-flag question** before relying on it:
`fastboot getvar` `current-slot`, `slot-successful:a`, `slot-retry-count:a`,
`slot-unbootable:a` before the flash, after the flash, and after several
boots of our image. Android's boot_control HAL marks a slot successful; our
OS does not. If the retry count decrements on each boot, `abl` will
eventually mark `_a` unbootable and try slot `_b` (old Android firmware
chain and `system_b`). If so, our OS must mark the slot successful itself
(the GPT attribute bits on the `boot_a` entry, as the HAL does); design
that write narrowly, review it, and test it. Do not use
`fastboot set_active`.

Acceptance: cold boot without USB to a working system (panel log, touch,
buttons, GPS fix), slot flags stable across at least 3 boots, and the
`fastboot flash boot_a` route back documented (actually re-flashing the
Magisk image is no longer required now that Android is retired, but the
command and hashes stay in the docs).

### Phase 2: writable `/data` on `userdata`

- Add Alpine `e2fsprogs`. ext4 only (`CONFIG_EXT4_FS=y`, no F2FS).
- An explicit, one-time format command (for example `chef-storage format`)
  that refuses unless the active slot is `_a`, the target resolves by
  `PARTNAME=userdata` (through `/sys/class/block/*/uevent`; there are no
  by-name links) to exactly 52727 MiB (take the exact byte size from the
  live partition), it is not mounted, and an explicit confirmation flag is
  given. It creates ext4 with a fixed label and a marker file with a
  layout version. **Never format at boot or automatically.** The current
  content is Android's FBE ext4; record its superblock before formatting.
- Mount at boot only when label and marker match: bounded `e2fsck -p`
  first (log the result), then `noatime` at `/data`. Missing, foreign or
  corrupt storage must never block boot: stay RAM-only and log why.
  Sync and unmount (or remount read-only) on every orderly shutdown path
  (buttond/powerd poweroff, `reboot`).
- Versioned layout under `/data`, per-service directories. Move the first
  two consumers: BlueZ pairing keys (`/var/lib/bluetooth`) and
  NetworkManager connection profiles (root-only, 0600). Document where
  rides, map tiles, sensor calibration, power logs, the wall-RTC offset,
  chrony drift and the GNSS shadow will go; they are follow-up tasks.
- Power-loss: unclean stops mid-write (sysrq reboot under a write loop, and
  the PMIC hard reset with Power held about 8.7 s, which needs the user;
  announce with a countdown), several cycles, then `e2fsck -n` clean and
  both consumers intact. Record the commit interval and why.

### Phase 3: root filesystem on `system_a`

- Build an ext4 image of the Alpine root (today's `out/rootfs` plus what
  `mkinitramfs.sh` adds) that fits 2560 MiB, flash it with
  `fastboot flash system_a` (sparse via `img2simg` if needed; check
  `max-download-size`). Mount it **read-only**; `/run`, `/tmp` tmpfs,
  writable state only under `/data`.
- Stage-1 initramfs in the boot image: find `system_a` by `PARTNAME`,
  check it carries our label and a build stamp that matches the kernel in
  the boot image (kernel modules such as `wlan.ko` live on `system_a` and
  must match the kernel's `Module.symvers`), mount it read-only and
  `switch_root`. If anything is wrong, stay in stage-1 with a **rescue
  shell on USB networking** (usb0 172.16.42.1 + telnetd) so the PC can
  still reach the phone without fastboot.
- The bootloader appends `root=/dev/mmcblk0p67` and `skip_initramfs`; our
  kernel ignores `skip_initramfs` (see the [device reference](../device.md)),
  so stage-1 runs and does the mount itself.
- Test with `fastboot boot` of the new boot image first (with `boot_a`
  still holding the phase-1 image as the fallback), then flash `boot_a`.
- Record the new boot image size and loader margin; the 24 MB ramdisk
  limit no longer constrains the OS.
- Update the build scripts so one command builds the kernel image, the
  stage-1 boot image and the `system_a` image together with matching stamps.

## Each phase

Host tests for every refusal rule and boot decision (fake sysfs, loop
images) in the existing test style; `make -C tools test` green.
Regression after each phase: Wi-Fi + HTTPS, BT, audio tone, sensors, GPS
manager lease with a fix, chrony, modem `crash_count` 0, and no writes to
`persist` or EFS (diskstats).

Docs: replace the temporary-boot assumptions where they change
(`docs/live-testing.md`: a normal reboot no longer returns to Android;
`docs/device.md` partition layout and recovery; `docs/building.md` for the
new images), a feature guide `docs/features/storage.md` for `/data`,
rewrite `storage-and-boot.md` (standalone boot done, Android retired,
remaining follow-ups only), build log entries with all image hashes.
Keep test images under other names; promotion of `out/boot.img` is the
coordinator's call.

## Recovery story to document

Fastboot is in `abl` and survives everything here: VolDown held through
the Power-held reset enters it. From fastboot: `fastboot flash boot_a` any
known-good boot image; `fastboot flash system_a` the last good root image.
Back to factory Android: flash stock `boot_a.img`, `system_a.img` from
`stock/partitions/` and format `userdata`.

## Gotchas

- Phone: on baseline `343fc3f7`, USB plugged. Fastboot boots and the
  approved flashes need no further permission; ask the user before any USB
  unplug, phone move, cold-boot check that needs them, or hard reset.
- Never an unsuffixed `fastboot flash boot`/`system`; always `_a` and
  confirm `current-slot` is `a` first.
- `ext4 ro,noload` still truncates orphans on this 4.4 kernel unless
  `blockdev --setro` comes first.
- Anything that must outlive a telnet command: `setsid … &`; never end a
  remote command with a bare `&`.
- No Wi-Fi passphrases or BT keys in `logs/` or repository evidence.
- Messages to the user: no em dashes.
