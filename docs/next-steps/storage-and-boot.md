# Storage and standalone boot

[Next-steps index](README.md) · [Current features](../features/README.md)

Standalone boot is done (2026-10-04); the rest are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Persistent storage

Decided 2026-10-04 (user): Android is retired. Writable state goes on `userdata` as ext4 mounted at `/data` (phase 2 of the [install layout handoff](install-layout-handoff.md)); the Alpine root moves to `system_a`, read-only (phase 3). Everything is in RAM today and this gates most of what follows: ride files and map tiles ([bike-computer application](ui-and-ride-app.md#bike-computer-application)), Bluetooth pairing keys, classic and LE (`/var/lib/bluetooth`, [BLE sensors](connectivity-and-sensors.md#ble-sensors)), Wi-Fi credentials ([Wi-Fi](connectivity-and-sensors.md#wi-fi)), the wall-RTC offset ([time synchronization](gps-and-time.md#time-synchronization)), sensor registry calibration ([sensor persistence](sensors.md#calibration-across-boots)), power logs/cycle state ([power](power-and-reliability.md#battery-and-charging)), the GNSS shadow ([warm starts](gps-and-time.md#warm-starts-and-assistance)). `/data` is mounted `noatime` and must survive power loss mid-write (the battery can die mid-ride). The no-writes-to-EFS/`persist` rule stays absolute.

## Standalone boot

Done on 2026-10-04 as phase 1 of the [install layout handoff](install-layout-handoff.md): phase 1 installed our image (`out/boot-abslot.img`, the `343fc3f7` baseline plus [`abslot`](#ab-slot-flags)); phase 2 now has the reviewed `out/boot-data.img` (`df856fc3`) in `boot_a`, promoted to the baseline `out/boot.img`. The phone cold-boots without a PC to a working system, and a normal reboot no longer starts Android. Evidence is in the [build log](../build-log.md#2026-10-04-our-image-in-boot_a). Phase 2 (`/data` on `userdata`) is provisioned and acceptance tested; phase 3 (root on `system_a`) remains untouched.

### Why flashing `boot_a` is recoverable

- **Fastboot does not live in `boot_a`.** It is the `abl` bootloader. With `boot_a` unbootable, holding VolDown through the Power-held hardware reset (about 8.7 s) still enters fastboot ([device reference](../device.md#stock-backups-and-recovery)), and a single `fastboot flash boot_a` restores any known-good image.
- **Only `boot_a` and its GPT slot attributes are written.** `abl`, `xbl`, `vbmeta`, `boot_b`, `persist`, `modemst*`, `fsg*` and the rest are never written; those are the partitions whose damage would not be fixable from fastboot. The attribute bits are written by `abl` itself on every flash and by `abslot` once per flash (below); both partition tables were backed up first.
- **A flashed boot loads like `fastboot boot`.** The same `abl` loader applies the same [loader-space budget](../research/chef-loader-kernel-budget.md) (about 24 MB margin) and appends `skip_initramfs`, which our kernel ignores. The `boot_a` partition is 64 MiB; the image is about 34 MiB.

### A/B slot flags

`abl` keeps the slot state in the GPT entry attributes of `boot_a` and `boot_b` (bits 48-49 priority, 50 active, 51-53 retry count, 54 successful, 55 unbootable; read them with `abslot status` on the phone, `fastboot getvar slot-successful:a` / `slot-retry-count:a` in fastboot, or `scripts/gpt-slots.py` on a dump). Measured 2026-10-04:

- Before: `boot_a` successful with retry 6 (Android's boot_control HAL had marked it), `boot_b` unbootable.
- `fastboot flash boot_a` sets `boot_a` to not successful with retry 7. Every later boot of an unsuccessful `_a`, including `fastboot boot` of any image, takes one retry (7 to 6 to 5 observed). With `_b` unbootable, an exhausted `_a` would leave no bootable slot.
- A successful slot is not counted down: retry stayed at 5 over five boots (three `reboot`, one `fastboot reboot`, one cold boot).

`abslot mark-successful` (`tools/abslot.c`) does the HAL's job: inittab runs it 30 s into every boot. It sets only bit 54 of the `boot_a` entry, in both GPT copies, after validating both copies, the booted slot, the kernel's view of `boot_a` and the slot state, and does nothing when the bit is already set. Live, the first mark wrote exactly 4 sectors (`mmcblk0` delta 4, every partition 0) and the result was byte-identical to the GPT before the first flash. Consequences:

- Every `fastboot flash boot_a` costs the new image's first boot one retry before it marks itself; an image that dies within 30 s of boot keeps counting down and should end in fastboot after seven attempts rather than boot-looping.
- `fastboot boot` of a test image runs the same inittab, so it also marks `boot_a` successful, whatever `boot_a` holds. Harmless while `_b` is unbootable and fastboot stays reachable; phase 3 may gate the mark on the kernel having come from `boot_a`.
- The retry count left after a mark (5 now) stays as it is; `abl` ignores it while the slot is successful.
- If both copies ever disagree: `abslot status` reports an interrupted mark as `half-marked` and `mark-successful` completes it; anything else is refused. The pre-flash GPT dumps are `stock/partitions/gpt-primary-20261004.bin` (LBAs 0-33) and `gpt-backup-tail-20261004.bin` (the last 33 LBAs), hashes in `GPT-SHA256SUMS` (gitignored, on the build PC only: keep them in the off-machine backup); writing them back is a last resort (for example from a TWRP `fastboot boot`), since a re-flash of `boot_a` already resets the slot.

### Images to restore

| Image | SHA-256 | State |
| --- | --- | --- |
| `stock/partitions/magisk_patched-v30.7-boot_a.img` (gitignored, build PC only; copy of `~/chef-cyclo-evidence/persistent-root-20260917-235600/exec/magisk_patched-v30.7-boot_a.img`) | `c6ab9f3a72cb9ccafeafb3deeec0265e8bd3d120d43477c3628435dbbb4ebc3f` | What `boot_a` held until 2026-10-04: stock Android 10 with Magisk v30.7 root. |
| `stock/partitions/boot_a.img` | `c77eb87d128e8251e04da904555a9bc67228eedf0e91957f5fffbe5a5abf51f2` | Unrooted stock (2026-09-13 backup, also in `stock/partitions/SHA256SUMS`). |

`stock/partitions/` is gitignored: these images and the GPT dumps exist only on the build PC and belong in the off-machine backup. Re-flashing either is no longer needed now that Android is retired, but the route stays: enter fastboot (VolDown through the Power-held reset), `fastboot getvar current-slot` must say `a`, `fastboot flash boot_a <image>`, `fastboot reboot`. Android only comes back with its matching `system_a` and a `userdata` it accepts (see [recovery](install-layout-handoff.md#recovery-story-to-document)); phase 2 has replaced Android userdata with `chefdata`, so Android must never boot against it. `system_a` remains Android's old root until phase 3.

### Procedure (as run)

1. Install only an image that has passed a temporary boot (`scripts/phone-boot.sh out/boot-<name>.img`, [live testing](../live-testing.md)).
2. In fastboot, `fastboot getvar current-slot` must say `a`; stop otherwise.
3. `fastboot flash boot_a out/boot-<name>.img` (always the explicit `_a` suffix, never an unsuffixed `fastboot flash boot`), then `fastboot reboot`.
4. After 30 s, `abslot status` must show `successful=1`.
5. Unplug USB, power off (Power held 3 s, released), power on with Power, and check panel log, touch, buttons and a GPS fix.

### Remaining

- **Charger mode.** `poweroff` with USB attached re-powers the phone (PMIC USB trigger) in about 24 s with `androidboot.mode=charger` and `bootreason=charger`, and our full OS boots. An off-mode charging screen, or powering off again until the key is pressed, is a follow-up ([buttons and power-off](../features/buttons-and-power-off.md)).
- **Test images mark `boot_a`.** A `fastboot boot` test image runs the same inittab, so after 30 s it marks `boot_a` successful whatever `boot_a` holds; a freshly flashed but broken `boot_a` could then look good. Gate the mark on the running kernel having come from `boot_a` (for example its version banner against the one in the `boot_a` image), at the latest with the stage-1 initramfs of phase 3.

## Retire Android

Decided by the user on 2026-10-04: Android is retired. `userdata` becomes `/data` and `system_a` our read-only root in phases 2 and 3 of the [install layout handoff](install-layout-handoff.md); `system_b` keeps Android's old slot-`_b` image.

## Persistent storage

Phase 2 implements `/data` on the exact `userdata` partition, an explicit
confirmed one-time format, checked early mounts and orderly shutdown, and
persistent BlueZ pairing state plus root-only NetworkManager keyfiles. The
[storage guide](../features/storage.md) is the operating reference. It records
the versioned layout, conservative ext4 profile and RAM-only fallback policy.
Provisioning and live consumer/durability/regression acceptance passed:
actual Wi-Fi autoconnect and BlueZ bonding persist, one PMIC reset and three
sysrq write-loss cycles recover cleanly, and audio/sensors/HTTPS/chrony/GNSS
regressions pass. See the [phase 2 build log](../build-log.md#2026-10-05-phase-2-persistent-data).

Remaining storage work is phase 3: place the root on read-only `system_a` and
boot with a small stage-1 initramfs and USB rescue fallback. Stage-1 must check
a matching kernel/root build stamp before `switch_root`. Do not write
`system_a` as part of phase 2. Later consumer migrations include rides, map
tiles, calibration, power logs, RTC offset, chrony drift and GNSS shadow; the
first phase 2 services do not make those follow-ups persistent.
