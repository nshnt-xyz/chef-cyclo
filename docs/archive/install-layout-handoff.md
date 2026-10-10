# Install layout: retire Android (handoff)

**Archived 2026-10-11.** All three phases completed 2026-10-04/05; remaining user cold-boot, speaker and touch checks accepted 2026-10-06. Later pairs supersede the hashes below; see [boot and recovery](../features/boot.md), [storage](../features/storage.md) and the [build log](../build-log.md). The original design and process instructions below are historical, not active assignments.

[Archive index](README.md) · [Storage and boot plan](../next-steps/storage-and-boot.md) · [Device reference](../device.md) · [Building](../building.md) · [Live testing](../live-testing.md)

**Status:** handed to Herdr agents `gnss_impl` (implementation, live runs)
and `gnss_review` (review) on 2026-10-04. Phase 1 done 2026-10-04
(phase 1 installed `out/boot-abslot.img`; slot flags answered and handled by
`abslot`, see [standalone boot](../features/boot.md#standalone-boot)).
Phase 2 resumed on 2026-10-05 with Herdr agents `storage_impl` and
`storage_review`, coordinated by `storage_research`. Reviewed provisioning
of `userdata` is complete and reviewed `out/boot-data.img` (`df856fc3`) is
installed in `boot_a` and promoted to the baseline `out/boot.img`; real
consumer persistence, one PMIC reset, three sysrq write-loss cycles and
device regressions passed. The final shutdown
fix makes the entire data filesystem read-only before detaching aliases,
including when `/data` is absent. Research notes, draft design and the phase 3
timing baseline: [phase 2 notes](../../logs/install-layout-phase2-notes-2026-10-04.md).
Phase 3 started 2026-10-05 with Herdr agents `rootfs_impl` and
`rootfs_review`, coordinated by `rootfs_research`; see
[phase 3 research and plan](#phase-3-research-and-plan-rootfs_research-2026-10-05).
Its live steps 2 to 6 passed the same night (see
[phase 3 result](#phase-3-result-rootfs_impl-2026-10-05)): `system_a` holds
the read-only root `9c47c6b8` and `boot_a` the stage-1 image `5c509eb2`;
promotion of `out/boot.img` and the commit are the coordinator's call.
After the [shutdown hang](shutdown-hang-handoff.md) fix (kernel #22,
2026-10-05) the installed pair was `system_a` `e72a3039` (stamp `13f336e5`) +
`boot_a` `81a5c5ef`; the phase 3 pair is kept in `out/pre-btfix/`. The
[state persistence](state-persistence-handoff.md) work replaced it on
2026-10-06 with `799577b6` + `e5b57eff`, and its
[follow-up](state-persistence-followup-handoff.md) on 2026-10-08 with
`system_a` `84a0baa4` (stamp `7ff6f75a`) + `boot_a` `6062f115` (commit
8201c79). The [SSH install](ssh-and-install-handoff.md) on 2026-10-11 put
in the current pair `system_a` `e73c8c54` (stamp `1e289bf2`) + `boot_a`
`034db212` (commit 1fae453, kernel #22 unchanged); `84a0baa4` + `6062f115`
and `799577b6` + `e5b57eff` stay in `out/` as fallbacks.

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

Run the existing [standalone boot procedure](../features/boot.md#standalone-boot)
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

Start from the [phase 2 notes](../../logs/install-layout-phase2-notes-2026-10-04.md)
(draft design, reviewer reminders, `CONFIG_EXT4_USERDATA_BLKNUM` to check).

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

#### Phase 3 research and plan (rootfs_research, 2026-10-05)

Started 2026-10-05 with Herdr agents `rootfs_impl` (implementation, live
runs) and `rootfs_review` (review), coordinated by `rootfs_research`.
**The user is asleep and unavailable**: no unplug, no buttons, no hard
reset, no cold-boot check that needs them. Everything below must be
recoverable over USB alone. The phone sits indoors next to a window.

Facts checked on 2026-10-05:

- Phone: running the installed `boot_a` image `df856fc3` (= baseline
  `out/boot.img`), USB plugged, `/data` mounted. The bootloader cmdline
  ends with `root=/dev/mmcblk0p67 ... skip_initramfs rootwait ro init=/init`.
- Kernel (`out/kernel/.config`): `RD_GZIP=y`, `RD_LZMA=y`, `RD_BZIP2=y`,
  no `RD_LZ4`/`RD_XZ`; **no `OVERLAY_FS`, no `SQUASHFS`**; `EXT4_FS=y`,
  `DM_VERITY=y`, `MODULES=y` (no signing), `PANIC_TIMEOUT=5` (a panic
  reboots after 5 s), `QCOM_WATCHDOG_V2=y`, no hung-task or lockup
  detector. Keep the kernel unchanged in this phase: `wlan.ko` is built
  against it and the stamp ties them together.
- Only module today: `/lib/modules/wlan.ko`. Root tree today: 848 files,
  about 107 MiB (`out/initramfs-root`); `system_a` is 2684354560 bytes.
- Host: `/usr/sbin/mke2fs` is e2fsprogs 1.47.2, but `PATH` finds the
  Android SDK `mke2fs` first, so call it by full path with a pinned
  `MKE2FS_CONFIG` (as `chef-storage` does for `userdata`). `fakeroot` is
  installed (root ownership for `mke2fs -d`); no `img2simg`. fastboot
  37.0.1 resparses a raw image larger than `max-download-size` itself;
  read `max-download-size` and `partition-size:system_a` before flashing.
- tftp RFS (`/usr/share/rfs/msm/mpss`) is a read-only seed copied into RAM
  by `tftpserv` (`tools/tftp/ramfs.h`), so modem writes never reach root.

**Runtime writes to `/` today.** Full-tree md5 diff of the live RAM root
against `out/initramfs-root` after 10 min of uptime (Wi-Fi up, `/data`
mounted): new files `/etc/resolv.conf` (NetworkManager), `/root/.ash_history`,
`/var/lib/NetworkManager/{NetworkManager-intern.conf,*.lease,seen-bssids,timestamps}`,
`/var/lib/dbus/machine-id`; no existing file changed content. `/init` also
writes to `/`: `busybox --install -s` (applet links in `/bin`, `/sbin`,
`/usr/bin`, `/usr/sbin`), `sed -i /etc/udhcpd.conf`, `mkdir /var/lib/dbus`,
`dbus-uuidgen --ensure`, and `chown`/`chmod 4750` of
`/usr/libexec/dbus-daemon-launch-helper` (the cpio is all root-owned).
Each one needs a build-time equivalent or a tmpfs/`/run` target. Repeat
the diff on the new image with the root mounted read-only and grep every
service log and kmsg for `Read-only file system`/`EROFS`.

Recommended design (deviations need a reason in the review):

1. **`system_a` image**: ext4 **without a journal** (`-O ^has_journal`:
   never mounted writable, so nothing to replay), label `chefroot`, a
   pinned feature set the 4.4 kernel supports, built from the staged root
   tree with `fakeroot /usr/sbin/mke2fs -d`, deterministic where
   practical (fixed UUID/hash seed, `SOURCE_DATE_EPOCH`). The
   build-time fixes above go into the tree before packing (applet links,
   launch-helper `root:messagebus 4750`, `/etc/resolv.conf` and
   `/var/lib/dbus/machine-id` as links into `/run`, NetworkManager told
   not to manage `/etc/resolv.conf` directly). A stamp file
   `/etc/chef/build-stamp` carries the kernel release, the SHA-256 of
   `out/kernel/Module.symvers` and the kernel image, and a manifest hash
   of the tree.
2. **Stage-1 initramfs** (gzip, small: busybox plus what it needs, e.g.
   musl loader + `busybox`/`busybox-extras`, or a static build): mount
   proc/sys/devtmpfs; require `androidboot.slot_suffix=_a`; find exactly
   one block device with `PARTNAME=system_a` through
   `/sys/class/block/*/uevent` and the exact live byte size; check the
   ext4 magic and label from the raw superblock; `blockdev --setro`
   **before** mounting; mount `ro`; compare the image stamp with the
   stamp baked into stage-1 byte for byte; move `/dev`, `/proc`, `/sys`
   (or let stage 2 remount them); `switch_root` to the real `/init`.
   Every step bounded (no unbounded `rootwait`-style loops). Normal path
   leaves the USB gadget to stage 2 as today. Any failure: log to kmsg,
   bring up the NCM gadget on 172.16.42.1 with udhcpd and telnetd, write
   the reason to `/run/rescue-reason`, and stay there. The rescue shell
   must include a way to reboot straight into fastboot (raw `reboot(2)`
   `RESTART2 "bootloader"`, as `btprobe restart bootloader` does) so
   `scripts/phone-boot.sh` can still recover it. If stage-1 `/init`
   itself dies, the kernel panics and reboots in 5 s into `boot_a`.
3. **Stage 2** (today's `/init` on `system_a`): no writes to `/`. tmpfs
   for `/run`, `/tmp` and the volatile `/var` state (NetworkManager,
   dbus, chrony, logs, cache, `/root` history or `HOME` elsewhere); the
   existing `/data` binds unchanged. Behaviour must match today's RAM OS
   except that root is read-only.
4. **Build**: one command builds kernel image + stage-1 boot image +
   `system_a` image with matching stamps, refusing a mismatch. Keep
   building the full RAM image too: `out/boot-data.img` is the universal
   fallback for `boot_a` because it never touches `system_a`. Test
   images under other names; `out/boot.img` promotion is the
   coordinator's call.
5. **Timing**: record `Trying to unpack rootfs`, `Freeing initrd memory`,
   `Freeing unused kernel memory`, `switch_root` and `exec init` times
   against the LZMA baseline (0.387 s, 5.987 s, 9.07 s), and the new boot
   image size and loader margin.
6. `dm-verity` (`DM_VERITY=y` exists) is a possible later follow-up, not
   this phase.

Live order (each live step only after `rootfs_review` clears the exact
image hashes for it):

1. Host tests: every stage-1 refusal and rescue decision against fake
   sysfs and loop images (wrong slot, missing or duplicate `system_a`,
   wrong size, wrong magic or label, stamp mismatch, mount failure,
   missing `/init`), the build refusing mismatched stamps, plus the
   existing suites; `make -C tools test` green.
2. From fastboot, after `current-slot` is `a`: `fastboot flash system_a`
   only. `boot_a` (`df856fc3`) does not read `system_a`, so it stays a
   working fallback. Record `getvar` before and after.
3. Boot the installed `boot_a`; from the RAM OS, `blockdev --setro`,
   mount `system_a` read-only, verify label, stamp, manifest and a
   chroot smoke test; `diskstats` write sectors on `mmcblk0p67` stay 0.
4. **Rescue test first**: `fastboot boot` a stage-1 test image with a
   deliberately wrong expected stamp; it must land in the rescue shell on
   172.16.42.1 and its fastboot reboot must work.
5. `fastboot boot` the real stage-1 image; full regression from the list
   below with root read-only, the EROFS grep, the runtime-write diff,
   `/data` binds, Wi-Fi autoconnect, an ordinary `reboot` (which lands in
   the old `boot_a`, as expected), and at least two such boots. GPS: try a
   `gps-manager` lease for up to 15 min next to the window; record SVs and
   any fix, and a missing fix indoors is not a blocker.
6. Only after step 5 passes and the review clears it: `fastboot flash
   boot_a` with that exact image (fresh `current-slot`/hash check first).
   After the flash `boot_a` is back to 7 retries until `abslot` marks it;
   a build that keeps panicking ends in fastboot after 7 boots
   (`boot_b` is unbootable), which USB can recover, but a hang without a
   panic would need the user, so step 5 must have proven the image.
   Then two ordinary reboots, `abslot` successful, regression again,
   `mmcblk0p67` hash still equals the flashed image and write sectors 0.
7. Report to `rootfs_research` with the evidence directory
   (`~/chef-cyclo-evidence/rootfs-phase3-20261005/`, private), a credential-free
   summary in `logs/`, docs updated, the diff staged but **not
   committed**.

#### Phase 3 result (rootfs_impl, 2026-10-05)

Built with `SKIP_KERNEL=1 scripts/mkinstall.sh` (kernel unchanged, `da2e6c61`,
the one inside `df856fc3`); see [installed layout](../building.md#installed-layout-phase-3).
Installed: `system_a` = `out/system_a.img` `9c47c6b8…` (stamp `f52cfe21…`),
`boot_a` = `out/boot-stage1.img` `5c509eb2…`. Not installed: `out/boot-ram.img`
`7255afa3…` (full RAM image from the same tree, short regression passed).
`out/boot.img` and `out/boot-data.img` are still `df856fc3`.

- Step 1: host tests (`tools/tests/test_stage1.py`, `scripts/tests/test_chef_stamp.py`, existing suites) green.
- Steps 2/3: guarded `fastboot flash system_a` (fastboot resparsed it); the
  flash also reset slot `_a` to unsuccessful/retry 7, `abslot` re-marked it.
  Full read-back hash exact, device listing equal to the manifest, write
  sectors 0 on `system_a`, `persist` and EFS. Done twice (`fe03231a`, then
  `9c47c6b8` after review fixes).
- RAM image check (A): no runtime writer outside tmpfs/`/data`.
- Step 4: bad-stamp image landed in the rescue shell with the reason, no
  `system_a` mount, writes 0; `phone-boot.sh` reached fastboot from it. No
  firmware requests happen in the stage-1 window.
- Step 5: the first stage-1 boot landed in rescue ("cannot unmount /dev
  before switch_root"): busybox `timeout` leaves a daemonized watchdog with
  stdio on `/dev/null` for up to a second. Fixed (plain `umount` with a
  bounded retry, about 1 s in practice); then two boots passed the full
  regression with the root read-only: runtime-write diff 0, no EROFS, Wi-Fi
  autoconnect and HTTPS, BT, chrony, sensors, ADSP/card up, GPS fix with
  12 SVs, crash counts 0, protected write sectors 0.
- Step 6: guarded `fastboot flash boot_a`; first boot marked the slot, two
  ordinary reboots and the regression passed again; `boot_a` and `system_a`
  read back exactly; final `slot-successful:a` yes, retry 6.
- Timing: `exec init` at about 5.4 s (9.7 s before), boot image 13 MB with a
  46 MB loader margin.
- Deferred to the user, done 2026-10-06 on the fixed pair (`e72a3039` +
  `81a5c5ef`): the audible speaker tone and a cold boot without USB (see the
  [build log](../build-log.md#2026-10-06-user-checks-on-the-installed-layout)).

Evidence: `~/chef-cyclo-evidence/rootfs-phase3-20261005/` (private),
summary in [logs](../../logs/install-layout-phase3-2026-10-05.txt).

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
