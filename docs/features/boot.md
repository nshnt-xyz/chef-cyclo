# Standalone boot and recovery

[Feature guides](README.md) · [Build and install](../building.md#installed-layout-phase-3) · [Persistent data](storage.md) · [Remaining work](../next-steps/storage-and-boot.md)

The phone boots our read-only root on `system_a` through a stage-1 image in `boot_a`. Android was retired on 2026-10-04; all three install phases were accepted by 2026-10-06. The latest installed pair is recorded in the [build log](../build-log.md#2026-10-10-ssh-with-key-login). Measurements and restore images below retain their original dates; use the build log for current image hashes.

## Standalone boot

### Why flashing `boot_a` is recoverable

- **Fastboot does not live in `boot_a`.** It is the `abl` bootloader. With `boot_a` unbootable, holding VolDown through the Power-held hardware reset (about 8.7 s) still enters fastboot ([device reference](../device.md#stock-backups-and-recovery)), and a single `fastboot flash boot_a` restores any known-good image.
- **A boot-image-only flash writes `boot_a` and its GPT slot attributes.** Installed-root updates also write `system_a`; provisioning and persistent service state write `userdata`. `abl`, `xbl`, `vbmeta`, `boot_b`, `persist`, `modemst*`, `fsg*` and the rest are never written; those are the partitions whose damage would not be fixable from fastboot. The attribute bits are written by `abl` itself on every flash and by `abslot` once per flash (below); both partition tables were backed up first.
- **A flashed boot loads like `fastboot boot`.** The same `abl` loader applies the same [loader-space budget](../research/chef-loader-kernel-budget.md) and appends `skip_initramfs`, which our kernel ignores. The `boot_a` partition is 64 MiB; the stage-1 image is about 13 MiB (46 MB loader margin), a full RAM image about 35 MiB (about 24 MB margin).
- **`system_a` is checked before use.** Stage 1 refuses a root whose label, size or build stamp does not match the boot image and stays in its rescue shell on USB instead; a full RAM image in `boot_a` never reads `system_a` at all.

### A/B slot flags

`abl` keeps the slot state in the GPT entry attributes of `boot_a` and `boot_b` (bits 48-49 priority, 50 active, 51-53 retry count, 54 successful, 55 unbootable; read them with `abslot status` on the phone, `fastboot getvar slot-successful:a` / `slot-retry-count:a` in fastboot, or `scripts/gpt-slots.py` on a dump). Measured 2026-10-04:

- Before: `boot_a` successful with retry 6 (Android's boot_control HAL had marked it), `boot_b` unbootable.
- `fastboot flash boot_a` sets `boot_a` to not successful with retry 7; so does `fastboot flash system_a` (seen 2026-10-05). Every later boot of an unsuccessful `_a`, including `fastboot boot` of any image, takes one retry (7 to 6 to 5 observed). With `_b` unbootable, an exhausted `_a` would leave no bootable slot.
- A successful slot is not counted down: retry stayed at 5 over five boots (three `reboot`, one `fastboot reboot`, one cold boot).

`abslot mark-successful` (`tools/abslot.c`) does the HAL's job: inittab runs it 30 s into every boot. It sets only bit 54 of the `boot_a` entry, in both GPT copies, after validating both copies, the booted slot, the kernel's view of `boot_a` and the slot state, and does nothing when the bit is already set. Live, the first mark wrote exactly 4 sectors (`mmcblk0` delta 4, every partition 0) and the result was byte-identical to the GPT before the first flash. Consequences:

- Every `fastboot flash boot_a` costs the new image's first boot one retry before it marks itself; an image that dies within 30 s of boot keeps counting down and should end in fastboot after seven attempts rather than boot-looping.
- `fastboot boot` of a test image runs the same inittab, so it also marks `boot_a` successful, whatever `boot_a` holds. This can incorrectly mark a broken installed image successful; gating the mark on the running image remains a [follow-up](../next-steps/storage-and-boot.md#remaining).
- The retry count left after a mark (5 in the original test) stays as it is; `abl` ignores it while the slot is successful.
- If both copies ever disagree: `abslot status` reports an interrupted mark as `half-marked` and `mark-successful` completes it; anything else is refused. The pre-flash GPT dumps are `stock/partitions/gpt-primary-20261004.bin` (LBAs 0-33) and `gpt-backup-tail-20261004.bin` (the last 33 LBAs), hashes in `GPT-SHA256SUMS` (gitignored, on the build PC only: keep them in the off-machine backup); writing them back is a last resort (for example from a TWRP `fastboot boot`), since a re-flash of `boot_a` already resets the slot.

### Images to restore

| Image | SHA-256 | State |
| --- | --- | --- |
| `stock/partitions/magisk_patched-v30.7-boot_a.img` (gitignored, build PC only; copy of `~/chef-cyclo-evidence/persistent-root-20260917-235600/exec/magisk_patched-v30.7-boot_a.img`) | `c6ab9f3a72cb9ccafeafb3deeec0265e8bd3d120d43477c3628435dbbb4ebc3f` | What `boot_a` held until 2026-10-04: stock Android 10 with Magisk v30.7 root. |
| `stock/partitions/boot_a.img` | `c77eb87d128e8251e04da904555a9bc67228eedf0e91957f5fffbe5a5abf51f2` | Unrooted stock (2026-09-13 backup, also in `stock/partitions/SHA256SUMS`). |

`stock/partitions/` is gitignored: these images and the GPT dumps exist only on the build PC and belong in the off-machine backup. Re-flashing either is no longer needed now that Android is retired, but the route stays: enter fastboot (VolDown through the Power-held reset), `fastboot getvar current-slot` must say `a`, `fastboot flash boot_a <image>`, `fastboot reboot`. Android only comes back with its matching `system_a` (stock `stock/partitions/system_a.img`) and a `userdata` it accepts (see [recovery](../archive/install-layout-handoff.md#recovery-story-to-document)); `userdata` holds `chefdata` and `system_a` our root since 2026-10-05, so Android must never boot against them.

### Procedure (as run)

This is the original boot-image-only procedure. Updating the installed root requires a matching `system_a`/stage-1 pair in the order documented in [building](../building.md#installed-layout-phase-3); replacing only its boot image can leave a stamp mismatch and the rescue shell.

1. Install only an image that has passed a temporary boot (`scripts/phone-boot.sh out/boot-<name>.img`, [live testing](../live-testing.md)).
2. In fastboot, `fastboot getvar current-slot` must say `a`; stop otherwise.
3. `fastboot flash boot_a out/boot-<name>.img` (always the explicit `_a` suffix, never an unsuffixed `fastboot flash boot`), then `fastboot reboot`.
4. After 30 s, `abslot status` must show `successful=1`.
5. Unplug USB, power off (Power held 3 s, released), power on with Power, and check panel log, touch, buttons and a GPS fix.
