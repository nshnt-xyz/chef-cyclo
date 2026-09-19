# Power and reliability

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Battery and charging

`power_supply/battery` is already readable (the ride image logs capacity % and status in its heartbeat and `meta.txt`), but nothing is verified beyond that: does charging actually run under our kernel with no Android charger daemon (current limits, USB vs. wall, the PMIC's default input limit), does `status` flip to Charging/Discharging/Full correctly, what does a full ride drain look like (the terrace run is the only data point), and low-battery behaviour — warn on the panel, then a clean shutdown before the PMIC cuts power. Also the fuel gauge: check whether capacity is sane without Android's `qcom,qpnp-fg` userspace helper, and whether the battery-swap on 2026-09-15 needs a profile reload.

## Suspend and idle power

`lpm_levels.sleep_disabled=1` is still on the cmdline; measure idle current with the panel off, then with the modem up vs. off (GPS keeps it powered), BT asleep vs. off, Wi-Fi off vs. associated, and decide what a ride's power budget allows. First validate current readings under [battery and charging](#battery-and-charging), and retain the clean-shutdown path.

## Crash recovery

Only meaningful once flashed ([standalone boot](storage-and-boot.md#standalone-boot)): `panic=10` (or so) on the cmdline so a kernel panic reboots instead of hanging, the PMIC/APPS watchdog enabled and kicked by init, and whole-system supervision around the GPS manager and UI so a mid-ride crash recovers in seconds and leaves evidence on writable storage ([persistent storage](storage-and-boot.md#persistent-storage)). The [GPS manager](gps-and-time.md#gps-manager) owns recovery of `gps-up`/LOC/broker/gpsd within a modem lifetime; the ride recorder owns the durable `ride` lease so a UI restart does not stop recording. Whole-system reboot recovery also needs persisted recorder state; a socket lease does not survive reboot. Verify the effective runtime cmdline because the bootloader rewrites it.
