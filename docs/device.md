# Device reference

[Project overview](../README.md)

- Bootloader already unlocked; A/B, last verified active slot `_a` (2026-09-19; inspect the runtime slot before slot-specific work); no dtbo partition (DTB appended to kernel).
- Panel: Tianma 1080x2246 DSI video mode (`mdss_dsi_mot_tianma_nt_618_fhd_vid_v0`, DDIC IDs 0xDA/DB/DC = 02/61/21, `panel_ver=0x00216102`), selected by the bootloader (XBL reads the DDIC IDs, passes `mdss_mdp.panel=` on the cmdline; the DT's `qcom,dsi-pref-prim-pan` is only a no-cfg fallback). Touch: Novatek TDDI on I2C 3-0062 (driver `NVT-ts`, `NVTCapacitiveTouchScreen` = event1, IRQ gpio 67, reset gpio 66). The replacement panel fitted 2026-09-18 reports the same panel identity as the original but a different touch IC: NT36525 (trim id `0B .. .. 25 65 03`, fw 226, 18x32 nodes, raw range **720x1600**) where the original was NT36672A (`0A .. .. 72 66 03`, PID `6005`). Both are in the driver's trim table; touch coordinates must be scaled by the evdev ABS range, never assumed to be panel pixels. The trim id is the stable identifier: the driver's `PID` (a live two-byte I2C read of the firmware event buffer, `nvt_read_pid`) came back `3AD9` on the first live boot with this panel and `32D8` on the next, and the `buildid` sysfs (fw version plus three raw "date" bytes from the same buffer) changed likewise — neither is fused identity.
- Wi-Fi/BT MACs passed on cmdline (`androidboot.wifimacaddr`, `androidboot.btmacaddr`).
- The bootloader appends `skip_initramfs` at runtime; stripping the boot image header cannot remove it. Our kernel accepts and ignores it so `/init` in the initramfs runs. The bootloader also rewrites console/debug parameters, so the header cmdline is not authoritative.
- No ANT+ (not enabled by Motorola). Plan: BLE via BlueZ; USB ANT stick over OTG as fallback.

## Stock backups and recovery

`stock/partitions/` contains the 2026-09-13 raw backup of every partition except userdata from stock QPTS30.61-18-16-19 (Android 10), with `SHA256SUMS` verified against the device. Device-unique `persist`, `modemst1/2`, `fsg_*`, `utags`, `cid`, and `hw` must be retained.

The verified `stock/twrp-3.7.0_9-0-chef.img` can be temporarily booted with `fastboot boot` for a root adb shell. Nothing needs to be flashed for the current development workflow. A normal reboot returns to the flashed system (stock Android in the recorded tests). Holding power for about 8.7 seconds causes a hardware reset; holding VolDown through that reset enters the bootloader.

- **TWRP downloads** from dl.twrp.me get silently truncated; verify sha256 and resume with `curl -C -`.

- **TWRP shell scripting:** toybox `dd` wants `bs=4194304` not `4M`; `adb shell` inside `while read` eats the loop's stdin.

## Hardware-specific behavior

See [display and touch](features/display-and-touch.md) for panel lifetime and coordinate scaling, [GPS](features/gps.md) for active-slot EFS handling, and [buttons and power-off](features/buttons-and-power-off.md) for the measured PMIC reset and power-on policy.

The [build log](build-log.md) retains the original hardware survey and identification evidence. Plans for [persistent storage and standalone boot](next-steps/storage-and-boot.md) are not implemented recovery instructions.
