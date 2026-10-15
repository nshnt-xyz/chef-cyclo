# Power and reliability

[Next-steps index](README.md) · [Current features](../features/README.md)

Remaining integration and validation work, reviewed 2026-10-11. Linked feature guides describe implemented behavior; candidate approaches must be checked when implementing.

## Battery and charging

The [feature guide](../features/battery-and-charging.md) owns the implemented charging and `powerd` policy. Remaining validation and integration:

- **Ride drain:** run the image with GPS (and later the recorder) active, unplugged for at least 1 h, then retrieve `/data/v1/power/log.csv` on the installed layout (or `/run/power/log.csv` on a RAM-only boot) for %/h and mean current. Do not span plug events when calculating drain.
- **Full and recharge:** observe a long charge to 100% and confirm recharge restarts correctly.
- **Thermal and source coverage:** verify throttle step-down and normal 44/42 °C behavior, and an SDP source. The 68 °C and empty-battery shutdown paths were not observed at real thresholds; retain that verification limitation without deliberately overheating the phone. Investigate stock's below-44 °C throttle before claiming thermal-policy parity.
- **Effective FCC logging:** add `main/constant_charge_current_max` to power logs; the battery-profile vote does not show the throttle's effective limit.
- **Off-mode charging:** owned by the [boot roadmap](storage-and-boot.md#remaining); shutdown with USB currently reboots into our full OS in charger mode.
- **Cycle/age tracking:** decide whether to retain battery lifetime counters. Power logs already persist on `/data` in batches and flush at orderly shutdown ([current policy](../features/battery-and-charging.md)); sudden power loss can lose pending ordinary rows.

Acceptance details and original completed test steps are retained in the [research record](../research/battery-and-charging.md).

## Suspend and idle power

`lpm_levels.sleep_disabled=1` is still on the cmdline; measure idle current with the panel off, then with the modem up vs. off (GPS keeps it powered), BT asleep vs. off, Wi-Fi off vs. associated, and decide what a ride's power budget allows. Cellular RF is settled: the modem's boot default draws the same as DMS `low-power` and explicit `online` costs about +2.8 mA on block means (+0.9 mA on medians), so nothing is applied at boot ([GPS guide](../features/gps.md#cellular-rf)). A GNSS session (gps-manager lease: LOC, gpsd, NMEA pipeline) adds about +31 mA over `OFF` at about 95 mA ([GPS guide](../features/gps.md#use-gnss-gps-manager)). Use the verified current sign convention in the [battery guide](../features/battery-and-charging.md), and retain the clean-shutdown path.

**Idle draw regression to explain.** On 2026-10-16 the Phase A base blocks (panel off, wled 0, Wi-Fi radio off, BT as booted, ADSP off, image of 390b0e0 with `chef-settings`, `blesensd` and `gps-manager` `OFF`) averaged about 198 mA (block means 203.4, 197.5 and 193.6 mA while the battery cooled from 40 to 36 C at 29 %), against about 95 mA for `gps-manager` `OFF` on 2026-10-04. It needs an A/B at a cool battery above 50 % that stops one resident at a time (`chef-settings`, `blesensd`, `bluetoothd`/`bt-up`, `gps-manager`, `sensors-up`) and powers BT off, to find the cause before any suspend work. Evidence: [log](../../logs/system-ui-slice2-2026-10-11.txt) (M5 Phase A).

## ADSP lifecycle and power

Audio and sensors share the ADSP. Measured 2026-10-16 (panel off, Wi-Fi radio off, BT as booted): the ADSP booted with `sensord` running and no claims costs 0 to +2 mA over the ADSP off, an `illuminance` claim at 5 Hz +0.4 mA, below the 11 to 22 mA block noise ([evidence](../../logs/system-ui-slice2-2026-10-11.txt)); it is booted at start now, so audio no longer pays a boot. Still open: sensors streaming at ride rates. The kernel accepts `0` on `/sys/kernel/boot_adsp/boot`, but unload/reload is unverified.

On an ephemeral boot, test unload with no audio or sensor claims, ALSA card removal, APR/service cleanup, and a second load with both audio and sensor recovery. Establish ownership of shared firmware mounts and services before changing production teardown. Reboot remains the verified reset. Use these results for [sensor boot integration](sensors.md#boot-integration-and-power) and [alert-player lifecycle](connectivity-and-sensors.md#audio).

## Crash recovery

Persistent crash records and boot history already exist ([storage contract](../features/storage.md#persistent-state)); panic/reboot retention was live verified. Remaining work: `panic=10` (or so) on the cmdline so a kernel panic reboots instead of hanging, the PMIC/APPS watchdog enabled and kicked by init, and whole-system supervision around the GPS manager and UI so a mid-ride crash recovers in seconds and leaves evidence on writable storage ([persistent storage](../features/storage.md)). The [GPS manager](../features/gps.md#use-gnss-gps-manager) owns recovery of LOC/broker/gpsd within a modem lifetime (never `gps-up`); the ride recorder owns the durable `ride` lease so a UI restart does not stop recording. Whole-system reboot recovery also needs persisted recorder state; a socket lease does not survive reboot. Verify the effective runtime cmdline because the bootloader rewrites it.
