# Storage and standalone boot

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Persistent storage

Everything is in RAM today and gates most of what follows: ride files and map tiles ([bike-computer application](ui-and-ride-app.md#bike-computer-application)), BLE pairing keys (`/var/lib/bluetooth`, [BLE sensors](connectivity-and-sensors.md#ble-sensors)), Wi-Fi credentials ([Wi-Fi](connectivity-and-sensors.md#wi-fi)), the wall−RTC offset ([time synchronization](gps-and-time.md#time-synchronization)), the GNSS shadow ([warm starts](gps-and-time.md#warm-starts-and-assistance)). `userdata` is Android's FBE-encrypted data — reformatting it means Android is gone from this phone, so either accept that ([Android retirement](storage-and-boot.md#retire-android)) or, while Android is still the escape hatch, carve out space elsewhere: an unused/spare partition, or shrink and split `userdata`. Whatever it is: mounted `noatime`, ext4 or f2fs, tested for power-loss (the battery can die mid-ride), and the no-writes-to-EFS/`persist` rule stays absolute. Move the Alpine root there afterwards (it is already the seed).

## Standalone boot

Plan and validate installation to `boot_a` so a PC is no longer required at startup. Before any flashing, inspect both slots, verify recoverable stock images, and write an exact restoration procedure using the actual backup filenames. Do not assume `_b` is a tested Android fallback simply because the device is A/B. Flashing is future work, outside the current temporary-boot workflow.

Acceptance: cold boot without USB to a working system and GPS fix, plus a verified route back to stock. This precedes [crash recovery](power-and-reliability.md#crash-recovery).

## Retire Android

Once persistent storage and standalone boot are proven and the Android escape hatch is no longer needed, decide whether to reclaim its slot and userdata. Reformatting Android userdata destroys its encrypted data; this is a separate future decision, not part of documentation or routine image builds.
