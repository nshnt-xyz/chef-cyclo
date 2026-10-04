# Storage and standalone boot

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Persistent storage

Everything is in RAM today and gates most of what follows: ride files and map tiles ([bike-computer application](ui-and-ride-app.md#bike-computer-application)), Bluetooth pairing keys, classic and LE (`/var/lib/bluetooth`, [BLE sensors](connectivity-and-sensors.md#ble-sensors)), Wi-Fi credentials ([Wi-Fi](connectivity-and-sensors.md#wi-fi)), the wall−RTC offset ([time synchronization](gps-and-time.md#time-synchronization)), sensor registry calibration ([sensor persistence](sensors.md#calibration-across-boots)), power logs/cycle state ([power](power-and-reliability.md#battery-and-charging)), the GNSS shadow ([warm starts](gps-and-time.md#warm-starts-and-assistance)). `userdata` is Android's FBE-encrypted data — reformatting it means Android is gone from this phone, so either accept that ([Android retirement](storage-and-boot.md#retire-android)) or, while Android is still the escape hatch, carve out space elsewhere: an unused/spare partition, or shrink and split `userdata`. Whatever it is: mounted `noatime`, ext4 or f2fs, tested for power-loss (the battery can die mid-ride), and the no-writes-to-EFS/`persist` rule stays absolute. Move the Alpine root there afterwards (it is already the seed).

## Standalone boot

Plan and validate installation to `boot_a` so a PC is no longer required at startup. Do not assume `_b` is a tested Android fallback simply because the device is A/B. Flashing is future work, outside the current temporary-boot workflow; the procedure below was written on 2026-10-04 and has **not** been run.

### Why flashing `boot_a` is recoverable

- **Fastboot does not live in `boot_a`.** It is the `abl` bootloader. With `boot_a` unbootable, holding VolDown through the Power-held hardware reset (about 8.7 s) still enters fastboot ([device reference](../device.md#stock-backups-and-recovery)), and a single `fastboot flash boot_a` restores Android.
- **Only `boot_a` is written.** Our image runs from RAM and never mounts `userdata`, so Android's data is untouched while our image is installed. `abl`, `xbl`, `vbmeta`, `boot_b`, `persist`, `modemst*`, `fsg*` and the rest are never written by this procedure; those are the partitions whose damage would not be fixable from fastboot.
- **A flashed boot loads like `fastboot boot`.** The same `abl` loader applies the same [loader-space budget](../research/chef-loader-kernel-budget.md) (about 24 MB margin after the 2026-10-04 libinput removal) and appends `skip_initramfs`, which our kernel already ignores. The `boot_a` partition is 64 MiB; the current image is about 34 MiB.
- **One system at a time.** Android also boots from `boot_a`. While our image is installed, Android does not start; switching is a flash from the PC in either direction.

### Images to restore

| Image | SHA-256 | State |
| --- | --- | --- |
| `~/chef-cyclo-evidence/persistent-root-20260917-235600/exec/magisk_patched-v30.7-boot_a.img` | `c6ab9f3a72cb9ccafeafb3deeec0265e8bd3d120d43477c3628435dbbb4ebc3f` | What `boot_a` holds today: stock Android 10 with Magisk v30.7 root (since 2026-09-18). |
| `stock/partitions/boot_a.img` | `c77eb87d128e8251e04da904555a9bc67228eedf0e91957f5fffbe5a5abf51f2` | Unrooted stock (2026-09-13 backup, also in `stock/partitions/SHA256SUMS`). |

Both files were present with matching hashes on 2026-10-04. The Magisk image is outside the repository: copy it next to the stock backups (and into any off-machine backup) before the first flash.

`userdata` is not backed up and does not need to be for this step. A raw copy is a poor safety net anyway: it is FBE-encrypted with keys bound to the phone's TEE and is large. Anything worth keeping on the Android side should be backed up from Android itself. `userdata` only matters for [persistent storage](#persistent-storage), where reformatting it ends Android on this phone.

### Procedure (not yet run)

1. Install only an image that has passed a temporary boot (`scripts/phone-boot.sh out/boot-<name>.img`, [live testing](../live-testing.md)). Check the restore images with `sha256sum` against the table above.
2. Put the phone in fastboot and confirm the slot: `fastboot getvar current-slot` must say `a`. If it does not, stop.
3. Flash with the explicit slot suffix, never an unsuffixed `fastboot flash boot`: `fastboot flash boot_a out/boot-<name>.img`.
4. `fastboot reboot`, then check the USB shell as in a temporary boot.
5. Unplug USB, power off (Power held 3 s, released), power on with Power, and confirm the system comes up without the PC: panel log, touch, buttons, then a GPS fix.
6. Back to Android: enter fastboot (VolDown through the Power-held reset), `fastboot flash boot_a <Magisk image>`, `fastboot reboot`. Use the stock image instead only if unrooted Android is wanted.

Acceptance: cold boot without USB to a working system and GPS fix, plus a verified route back to stock (step 6 actually performed and Android booting with its data). This precedes [crash recovery](power-and-reliability.md#crash-recovery).

## Retire Android

Once persistent storage and standalone boot are proven and the Android escape hatch is no longer needed, decide whether to reclaim its slot and userdata. Reformatting Android userdata destroys its encrypted data; this is a separate future decision, not part of documentation or routine image builds.
