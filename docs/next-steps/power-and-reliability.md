# Power and reliability

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Battery and charging

The [feature guide](../features/battery-and-charging.md) owns the implemented charging and `powerd` policy. Remaining validation and integration:

- **Ride drain:** run the image with GPS (and later the recorder) active, unplugged for at least 1 h, then retrieve `/run/power/log.csv` for %/h and mean current. Do not span plug events when calculating drain.
- **Full and recharge:** observe a long charge to 100% and confirm recharge restarts correctly.
- **Thermal and source coverage:** verify throttle step-down and normal 44/42 °C behavior, and an SDP source. The 68 °C and empty-battery shutdown paths were not observed at real thresholds; retain that verification limitation without deliberately overheating the phone. Investigate stock's below-44 °C throttle before claiming thermal-policy parity.
- **Effective FCC logging:** add `main/constant_charge_current_max` to power logs; the battery-profile vote does not show the throttle's effective limit.
- **Off-mode charging:** implement our charger mode after [standalone boot](storage-and-boot.md#standalone-boot). Today shutdown with USB returns to Android's charger.
- **Persistent cycle/age and power logs:** integrate after [writable storage](storage-and-boot.md#persistent-storage). Anything kept in RAM is still lost at low-battery shutdown.

Acceptance details and original completed test steps are retained in the [research record](../research/battery-and-charging.md).

## Suspend and idle power

`lpm_levels.sleep_disabled=1` is still on the cmdline; measure idle current with the panel off, then with the modem up vs. off (GPS keeps it powered), BT asleep vs. off, Wi-Fi off vs. associated, and decide what a ride's power budget allows. Use the verified current sign convention in the [battery guide](../features/battery-and-charging.md), and retain the clean-shutdown path.

## ADSP lifecycle and power

Audio and sensors share the ADSP. Measure idle draw with it off, audio brought up, sensors running without claims, and sensors streaming. The kernel accepts `0` on `/sys/kernel/boot_adsp/boot`, but unload/reload is unverified.

On an ephemeral boot, test unload with no audio or sensor claims, ALSA card removal, APR/service cleanup, and a second load with both audio and sensor recovery. Establish ownership of shared firmware mounts and services before changing production teardown. Reboot remains the verified reset. Use these results for [sensor boot integration](sensors.md#boot-integration-and-power) and [alert-player lifecycle](connectivity-and-sensors.md#audio).

## Crash recovery

Only meaningful once flashed ([standalone boot](storage-and-boot.md#standalone-boot)): `panic=10` (or so) on the cmdline so a kernel panic reboots instead of hanging, the PMIC/APPS watchdog enabled and kicked by init, and whole-system supervision around the GPS manager and UI so a mid-ride crash recovers in seconds and leaves evidence on writable storage ([persistent storage](storage-and-boot.md#persistent-storage)). The [GPS manager](gps-and-time.md#gps-manager) owns recovery of `gps-up`/LOC/broker/gpsd within a modem lifetime; the ride recorder owns the durable `ride` lease so a UI restart does not stop recording. Whole-system reboot recovery also needs persisted recorder state; a socket lease does not survive reboot. Verify the effective runtime cmdline because the bootloader rewrites it.
