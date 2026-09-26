# Battery and charging

[Feature index](README.md) · [Build instructions](../building.md) · [Plan and stock research](../next-steps/battery-and-charging-plan.md)

## Current behavior

Charging is done by the kernel, the same way stock Android does it: `qpnp-smb2` (PM660 charger: source detection, input and charge current limits, termination), the Motorola `mmi` heartbeat inside `smb-lib.c` (battery-temperature zones, step charging) and `qpnp-fg-gen3` (fuel gauge, battery profile picked by the battery serial). No userspace helper is needed for any of that.

`tools/powerd.c` adds the small policy layer stock userspace puts on top. It runs from both the baseline and the ride `inittab` (`::respawn:`):

- **Low battery** (a clone of Android `BatteryService`). With no power attached, it warns once at 15 % and again at 5 % (critical, plus a 600 ms vibrator pulse on the baseline image), as `powerd:` kmsg lines that fblog shows on the panel. Both warnings re-arm when power is attached or the capacity is back at 20 %.
- **Clean shutdown** when unpowered, `battery/present == 1`, `bms/soc_reporting_ready == 1` and either the capacity is 0 % or `voltage_now` is below 3300 mV with the capacity at or below 5 %. Also when the battery is **above 68.0 °C**, whether or not a charger is attached: this is the only shutdown allowed while powered. Every condition must hold on 3 samples at least 5 s apart. An unreadable or `-22` value counts as unknown and restarts the count. The shutdown is `sync` then busybox `poweroff`, the same orderly path as `buttond`.
- **"Unpowered"** means `battery/status` reads exactly `Discharging` **and** `usb/online`, `pc_port/online` and `dc/online` each read 0 or their supply does not exist. An SDP (PC USB 2) port reads `usb/online = 0` and reports the input on `pc_port/online`, and an attached charger that is not charging reads `Not charging`, so neither counts as unplugged. Anything unreadable counts as powered. The sign of `current_now` is never used (positive = discharging, negative = charging on this fuel gauge).
- **Charge thermal throttle** (a clone of thermal-engine's `SS-BATT-BATT` rule). While a supply is online it checks every 5 s: battery ≥ 44.0 °C raises `battery/system_temp_level` by one step (up to level 7), ≤ 42.0 °C lowers it. The level indexes the DT `qcom,thermal-mitigation` FCC table (3000, 2500, 2000, 1500, 1200, 900, 700, 300 mA). It is written back to 0 at start, on unplug and on a clean exit.
- **Logging** (a clone of `batt_health`, to RAM): `/run/power/log.csv` gets one row per uevent and poll, bounded at 1 MiB with one rotation to `log.csv.1`. `/run/power/state` holds the current values as `key=value` lines, rewritten atomically.

Nothing else is written: no input/charge current overrides, no `input_suspend`, ship mode or demo mode, and nothing in persist or EFS. The kernel's own charging stays in charge. The charging LED stays off, as on stock.

## Inspect

In the phone shell:

```sh
powerd status                 # the state file
cat /run/power/log.csv        # history since boot (RAM only)
dmesg | grep powerd:          # transitions: plug/unplug, status, warnings, throttle, shutdown
```

State keys include `status`, `source` (`usb`, `dc`, `battery` or `unknown`), `usb_type` (`USB_CDP`, `USB_DCP`, `USB` for SDP, ...), `capacity`, `voltage_mv`, `current_ma`, `temp_c`, `charge_counter_uah`, `usb_input_ma`, `usb_voltage_mv`, `input_settled_ma`, `throttle_level`, `alert` (`none`, `low`, `critical`, `confirming`, `shutdown`) and `overrides` (empty in production).

`profile_fcc_ma` is `battery/constant_charge_current_max`, which is only the battery-profile FCC vote. The throttle's vote does not show there. The effective charge current limit is the programmed FCC register, read directly from `main/constant_charge_current_max` (3000000 at level 0, 1500000 on stock at level 3), and it is also in the kernel heartbeat line `EFFECTIVE: FV = …, FCC = …, USBICL = …` in dmesg. powerd does not log the `main` value yet.

The ride logger adds `power=<status>,<source>,<mV>mV,<mA>mA,<°C>C` to its heartbeat and `meta.txt` from the state file, falling back to sysfs.

## Test options

Each of these is logged as `TEST OVERRIDE` in kmsg and listed in the state file's `overrides=`:
`--warn`, `--critical`, `--rearm`, `--empty-pct`, `--empty-mv`, `--overtemp`, `--throttle-set`, `--throttle-clear`, `--no-throttle`, `--confirm`, `--confirm-gap-ms`, `--step-ms`, `--poll-s`, `-x CMD` (run `sh -c CMD` instead of powering off, with no fallback) and `--shutdown-when-online` (run the low-battery policy as if unpowered, to test the shutdown with the USB control link attached). `powerd` refuses `critical > warn` or `rearm <= warn`.

## Modify and verify

`make -C tools test` runs `tools/tests/test_powerd.c` (policy, SDP/unknown handling, throttle, outputs and cadence against a fake sysfs tree with injected hooks) and `tools/tests/test_powerd.sh` (the host binary end to end, shutting down through `-x` only). `tools/tests/test_ride-logger.sh` covers the heartbeat field. Then rebuild both images.

## Live verification (2026-09-26, QPTS kernel `cbd2b3acf`, `boot.img` `066279cc…`)

See the [build-log entry](../build-log.md) and `logs/powerd-live-test-2026-09-26-*`.

- **The kernel charges on its own.** On a wall charger (`USB_DCP`, then `USB_PD` after negotiation, input settled at 2575 mA), `current_now` peaked at −1.98 A, and `bms/charge_counter` rose by 302 mAh in 14.2 min (about 1.28 A mean, including 4 min throttled by the L3 test). Net from before the plug to after the unplug it rose 274 mAh, which matches 80 → 85 % of 5004 mAh. Unplugged, it fell at about +310 mA. The counter also steps at plug events (+5 mAh while still `Discharging` at plug-in, −26 mAh just before the unplug), so drain maths must never span a plug event. The profile, capacity and `charge_full` match stock.
- **Host ports are weak sources.** The PC's CDP port advertised 1500 mA but AICL settled the input at 125 to 175 mA, so the phone net-discharged while reporting `Charging`, exactly as on stock. Use a wall charger or power bank to actually charge.
- Plug/unplug transitions were logged within one uevent. The wall charger bounced once during detection (`Not charging` → `Discharging` for about 1.2 s); the 3-sample confirmation ignores that.
- The throttle stepped `system_temp_level` 0 → 7 with a test set point. The effective FCC (heartbeat) went to 300 mA, the charge current fell from 1.6 A to 0.27 A, and the level reset to 0 on unplug. Only the step-up was exercised live.
- The low-battery shutdown with test thresholds went LOW → CRITICAL (buzz) → 3 confirmations → SHUTDOWN in 11 s, then power-off into Android's off-mode charger. The evidence for the power-off is the host kmsg stream closing right after the `SHUTDOWN` line plus the user seeing the charger screen; no init shutdown or power-down line was captured.
- **Heat:** on the wall charger the battery went from 32 °C to 42 °C in about 7 min at room temperature, so the 44 °C throttle will engage in real use (sun, handlebars, power bank).
- **Open observation, not parity:** stock was read at `system_temp_level=3` (`main/constant_charge_current_max` 1500000) with the battery at 37 °C, 30 s after boot (`logs/powerd-live-test-2026-09-26-stock-before-after.txt`). Stock can therefore throttle charging below 44 °C, probably through the `SS-CHG-BATT` rule on `msm_therm` or as a boot transient (one sample). powerd's battery-only 44 °C rule is less conservative than stock.
- Not yet seen live: `Full` and the recharge restart (Q4), a real 68 °C shutdown, a real throttle at 44 °C, an SDP port, and a real empty-battery shutdown or ride drain (plan step L5).

## Limits

- Ride data lives in `/run` (RAM), so a low-battery shutdown on the ride image loses the ride log. The warnings exist so the rider can pull logs first.
- Off-mode charging under our own image needs a standalone boot. Today a power-off with USB attached comes back in Android's own charger mode. Stock's charger writes `/mnt/vendor/persist/chargeonly/cooldown` itself; our image never writes persist.
- Cycle count and battery age are not persisted (no writable storage yet). The fuel gauge keeps the cycle count while the battery stays connected.
- The ride image has no critical buzz (`powerd -V ''`), because the vibrator carries the ride logger's patterns.
