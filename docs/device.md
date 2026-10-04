# Device reference

[Project overview](../README.md)

- Bootloader already unlocked; A/B, active slot `_a` (verified 2026-10-04; inspect the runtime slot before slot-specific work); `_b` is marked unbootable. No dtbo partition (DTB appended to kernel).
- Partitions used (find them by `PARTNAME` in `/sys/class/block/*/uevent`; there are no by-name links): `boot_a` `mmcblk0p44` (64 MiB) holds our boot image since 2026-10-04; `userdata` `mmcblk0p69` (107986911 sectors, 55289298432 bytes) was provisioned as ext4 `chefdata` for writable `/data` in phase 2. `system_a` `mmcblk0p67` (5242880 sectors, 2684354560 bytes, `partition-size:system_a` 0xa0000000) holds our read-only root (ext4 `chefroot`, no journal) since phase 3 of the [install layout](next-steps/install-layout-handoff.md); the stage-1 `/init` in `boot_a` mounts it read-only after `blockdev --setro` (see [installed layout](building.md#installed-layout-phase-3)). The bootloader passes `root=/dev/mmcblk0p67`; our kernel does not use it while the ramdisk has an `/init`. eMMC logical sectors are 512 bytes; the GPT has 69 entries, primary at LBA 1, backup at the last LBA.
- A/B slot state lives in the GPT attributes of `boot_a`/`boot_b`. `fastboot flash boot_a` resets `boot_a` to unsuccessful with 7 retries and each boot of it (also `fastboot boot`) takes one until it is marked successful; our image does that with `abslot` 30 s into each boot ([A/B slot flags](next-steps/storage-and-boot.md#ab-slot-flags)).
- Panel: Tianma 1080x2246 DSI video mode (`mdss_dsi_mot_tianma_nt_618_fhd_vid_v0`, DDIC IDs 0xDA/DB/DC = 02/61/21, `panel_ver=0x00216102`), selected by the bootloader (XBL reads the DDIC IDs, passes `mdss_mdp.panel=` on the cmdline; the DT's `qcom,dsi-pref-prim-pan` is only a no-cfg fallback). Touch: Novatek TDDI on I2C 3-0062 (driver `NVT-ts`, `NVTCapacitiveTouchScreen` = event1, IRQ gpio 67, reset gpio 66). The replacement panel fitted 2026-09-18 reports the same panel identity as the original but a different touch IC: NT36525 (trim id `0B .. .. 25 65 03`, fw 226, 18x32 nodes, raw range **720x1600**) where the original was NT36672A (`0A .. .. 72 66 03`, PID `6005`). Both are in the driver's trim table; touch coordinates must be scaled by the evdev ABS range, never assumed to be panel pixels. The trim id is the stable identifier: the driver's `PID` (a live two-byte I2C read of the firmware event buffer, `nvt_read_pid`) came back `3AD9` on the first live boot with this panel and `32D8` on the next, and the `buildid` sysfs (fw version plus three raw "date" bytes from the same buffer) changed likewise — neither is fused identity.
- Wi-Fi/BT MACs passed on cmdline (`androidboot.wifimacaddr`, `androidboot.btmacaddr`).
- The bootloader appends `skip_initramfs` at runtime; stripping the boot image header cannot remove it. Our kernel accepts and ignores it so `/init` in the initramfs runs. If the stage-1 ramdisk ever failed to unpack, the kernel would mount `root=/dev/mmcblk0p67 ro` itself and run `system_a`'s `/init`, which should work (not tested; the stage-2 `/init` mounts everything it needs). The bootloader also rewrites console/debug parameters, so the header cmdline is not authoritative.
- No ANT+ (not enabled by Motorola). Plan: BLE via BlueZ; USB ANT stick over OTG as fallback.

## Stock backups and recovery

`stock/partitions/` contains the 2026-09-13 raw backup of every partition except userdata from stock QPTS30.61-18-16-19 (Android 10), with `SHA256SUMS` verified against the device. Device-unique `persist`, `modemst1/2`, `fsg_*`, `utags`, `cid`, and `hw` must be retained.

Since 2026-10-04 `boot_a` holds our image and a normal boot no longer starts Android. From 2026-09-18 to then it held a Magisk-rooted stock image; it and the unrooted `boot_a` backup are listed with hashes under [images to restore](next-steps/storage-and-boot.md#images-to-restore), with the flash route back. The GPT as it was before the first flash is saved as `stock/partitions/gpt-primary-20261004.bin` and `gpt-backup-tail-20261004.bin` (`GPT-SHA256SUMS`), next to a copy of the Magisk image; like the rest of `stock/partitions/` they are gitignored and only on the build PC, so keep them in the off-machine backup.

The verified `stock/twrp-3.7.0_9-0-chef.img` can be temporarily booted with `fastboot boot` for a root adb shell. A normal reboot boots whatever `boot_a` holds (our image since 2026-10-04). Holding power for about 8.7 seconds causes a hardware reset; holding VolDown through that reset enters the bootloader (fastboot), from which `fastboot flash boot_a` restores any known-good image.

Recovery with the installed layout (`boot_a` stage 1 + `system_a` root):

- A `system_a` that stage 1 refuses (missing, wrong size or label, journal, stamp not matching the boot image, mount failure) leaves the phone in the stage-1 rescue shell on USB networking (`telnet 172.16.42.1`, reason in `/run/rescue-reason`); `scripts/phone-boot.sh` reaches fastboot from there. If stage 1 itself dies, the kernel panics and reboots after 5 s (`PANIC_TIMEOUT=5`); after 7 unmarked boots `abl` stops in fastboot (`boot_b` is unbootable). A hang without a panic needs the button reset above.
- From fastboot: `fastboot flash system_a` the last good root image together with `fastboot flash boot_a` its matching stage-1 image (the stamps must match), or `fastboot flash boot_a` a full RAM image (`out/boot-ram.img`, `out/boot.img`), which never reads `system_a`.
- Back to factory Android: stock `boot_a.img` and `system_a.img` from `stock/partitions/` and a `userdata` format.

- **TWRP downloads** from dl.twrp.me get silently truncated; verify sha256 and resume with `curl -C -`.

- **TWRP shell scripting:** toybox `dd` wants `bs=4194304` not `4M`; `adb shell` inside `while read` eats the loop's stdin.

## Hardware-specific behavior

See [display and touch](features/display-and-touch.md) for panel lifetime and coordinate scaling, [GPS](features/gps.md) for active-slot EFS handling, and [buttons and power-off](features/buttons-and-power-off.md) for the measured PMIC reset and power-on policy.

The [build log](build-log.md) retains the original hardware survey and identification evidence. [Standalone boot](next-steps/storage-and-boot.md#standalone-boot) has the recovery route for `boot_a`; persistent storage is still a plan.
