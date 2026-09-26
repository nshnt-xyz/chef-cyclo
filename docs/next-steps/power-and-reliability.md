# Power and reliability

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Battery and charging

Done 2026-09-26 ([feature guide](../features/battery-and-charging.md), [plan](battery-and-charging-plan.md)): the kernel charges on its own under our image, the fuel gauge is sane with the right profile and no helper, status flips correctly, and `powerd` provides the low-battery warn/shutdown, the charge throttle and a `/run/power` log. Remaining:

- **Ride drain (plan step L5):** ride image with GPS on, unplugged for at least 1 h, then pull `/run/power/log.csv` for %/h and mean current.
- **`Full` and recharge restart (Q4):** needs a long charge to 100 %.
- **Off-mode charging under our image:** needs a standalone boot. Today power-off with USB attached returns to Android's charger.
- **Persistent cycle/age and power logs:** need writable storage. Until then a low-battery shutdown on the ride image loses the RAM ride log.

## Suspend and idle power

`lpm_levels.sleep_disabled=1` is still on the cmdline; measure idle current with the panel off, then with the modem up vs. off (GPS keeps it powered), BT asleep vs. off, Wi-Fi off vs. associated, and decide what a ride's power budget allows. First validate current readings under [battery and charging](#battery-and-charging), and retain the clean-shutdown path.

## Crash recovery

Only meaningful once flashed ([standalone boot](storage-and-boot.md#standalone-boot)): `panic=10` (or so) on the cmdline so a kernel panic reboots instead of hanging, the PMIC/APPS watchdog enabled and kicked by init, and whole-system supervision around the GPS manager and UI so a mid-ride crash recovers in seconds and leaves evidence on writable storage ([persistent storage](storage-and-boot.md#persistent-storage)). The [GPS manager](gps-and-time.md#gps-manager) owns recovery of `gps-up`/LOC/broker/gpsd within a modem lifetime; the ride recorder owns the durable `ride` lease so a UI restart does not stop recording. Whole-system reboot recovery also needs persisted recorder state; a socket lease does not survive reboot. Verify the effective runtime cmdline because the bootloader rewrites it.
