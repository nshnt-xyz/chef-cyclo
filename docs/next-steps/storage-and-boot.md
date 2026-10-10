# Storage and boot follow-ups

[Next-steps index](README.md) · [Boot and recovery](../features/boot.md) · [Persistent data](../features/storage.md) · [Original install plan](../archive/install-layout-handoff.md)

Standalone boot, writable `/data` on `userdata` and the read-only root on `system_a` are complete. The installed layout passed the user's unplugged cold-boot, panel, buttons/touch and speaker checks on 2026-10-06. Bluetooth keys, NetworkManager profiles, crash records/boot history, RTC offset, chrony drift, batched power logs and magnetometer bias persist; `chef-reboot bootloader` saves state before returning to fastboot. The latest pair was installed 2026-10-11 ([build log](../build-log.md#2026-10-10-ssh-with-key-login)).

## Remaining

- **Charger mode.** USB attached at power-off re-powers the phone into our full OS with `androidboot.mode=charger`. Decide and implement an off-mode charging screen or an appropriate wait-for-key policy; Android's charger no longer runs.
- **Test images mark `boot_a`.** After 30 s, a temporary `fastboot boot` image also marks the installed slot successful. Establish reliable image provenance before gating `abslot`, so a temporary test cannot mark a broken installed image good. See the [current slot contract](../features/boot.md#ab-slot-flags).
- **Durable consumers.** Build ride recording and map storage on the existing `/data` layout ([UI/ride roadmap](ui-and-ride-app.md#bike-computer-application)). GNSS shadow persistence first needs attribution and acceptance research ([GPS roadmap](gps-and-time.md#warm-starts-and-assistance)). Each consumer must handle missing or read-only storage safely.
- **Optional uncompressed boot.** The image was built and measured; flashed it saved about 0.2 s. Gzip remains installed. Changing that is an optional installation choice, not unfinished implementation ([experiment](../archive/boot-compression-handoff.md), [build options](../building.md)).
- **Optional root integrity.** Evaluate `dm-verity` for `system_a` if required (`CONFIG_DM_VERITY=y` exists).

Software updates to the installed read-only root use a rebuilt matching system/boot pair; runtime `apk add` is available on the RAM image. See [building](../building.md#installed-layout-phase-3).
