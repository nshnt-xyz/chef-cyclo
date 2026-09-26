# Battery and charging: stock research and implementation plan

[Power and reliability](power-and-reliability.md#battery-and-charging) · [Next-steps index](README.md)

Status: implemented and live-verified L0 to L4 on 2026-09-26 (see the [feature guide](../features/battery-and-charging.md) and the dated [build-log entry](../build-log.md)); L5 (ride drain) and Q4 (`Full`) remain. Written 2026-09-26. Raw stock evidence: [`logs/stock-power-survey-2026-09-26.txt`](../../logs/stock-power-survey-2026-09-26.txt).

The goal is to copy how stock Android manages power on this phone into the Alpine image. The research shows that most of the work already happens in the kernel we share with stock. Stock userspace adds only a small, clonable policy layer.

## 1. How stock Android does it

### 1.1 Kernel (same source and DT as ours)

| Piece | What it does | Evidence it already runs under our kernel |
|---|---|---|
| `qpnp-smb2` + `smb-lib.c` (PM660 charger) | APSD/Type-C source detection (SDP/CDP/DCP/HVDCP), input current limit (ICL), fast-charge current (FCC), float voltage, termination, HW JEITA, AICL. | dmesg 2026-09-26: `QPNP SMB2 probed successfully`, `APSD=CDP`, ICL raised 500 → 1500 mA at 19 s. |
| Motorola `mmi` logic inside `smb-lib.c` (`mmi_heartbeat_work`) | Battery-temperature zones from DT `qcom,mmi-temp-zones` (chef: <0 °C 620 mA to 4.2 V; 0 to 10 °C 1550 mA; 10 to 15 °C 1550 mA; 15 to 45 °C 3000 mA, tapering to 2000 mA above 4.2 V; 45 to 60 °C 1550 mA to 4.2 V; >60 °C stop). Step charging (FV 4.4 V then 4.2 V), taper detection, charge-full. | `PMI: mmi_heartbeat_work: ... EFFECTIVE: FV = 4200000, FCC = 3000000, USBICL = 1500000` in our dmesg. |
| `qpnp-fg-gen3` (fuel gauge, `bms` psy) | SOC, OCV, ESR, capacity learning, cycle counter (FG SRAM), battery profile from DT picked by battery serial (`fg_get_mmi_battid`). Cutoff 3200 mV (SOC reads 0 below this), empty 3000 mV, `qcom,fg-force-load-profile`. | Our dmesg: `Battsn = SB18C29743` → `Battery Match Found using qcom,jk50_atl_india_5000mah`, the same as stock. |
| `system_temp_level` on `battery` psy | Thermal mitigation index into DT `qcom,thermal-mitigation` (chef: 3000, 2500, 2000, 1500, 1200, 900, 700, 300 mA; 8 levels). Written by userspace only. | Attribute exists in the kernel; nobody writes it on our image. |
| `force_demo_mode` (battery/device) | Moto "demo" charge cap, 35 to 80 %. Suspends USB input at the cap, with hysteresis. | Kernel feature, unused on stock in normal mode. |
| `enable-charging-limit` 75/60 % | Only active with `factory_image_mode`; inert on retail. | n/a |

Static verification (2026-09-26): no project kernel commit touches `drivers/power` or the PMIC DT (all 8 are Bluetooth, modem PIL or `skip_initramfs`). Every stock userspace write to charger sysfs only restricts charging (thermal-engine `system_temp_level`) or logs (`batt_health` `age`/`cycle_count`), plus the factory-only `factory_image_mode`; the one enabling write (`charging_enabled 1`) is commented out in the stock rc. Existing evidence under our kernel: the ride image read `battery/status=Charging` at 90 to 92 % on 2026-09-18, and the mmi heartbeat applied FCC 3000 mA / ICL 1500 mA. The `ODEBUG ... stub_timer` warnings inside `smb2_probe` are an upstream init-order bug in `battery.c` (`qcom_batt_init` votes `PL_DISABLE`, whose callback cancels work items initialised a few lines later). The struct is zero-filled, so this is harmless, and only our debug-objects config reports it. Coulomb-counter proof that charge actually flows in is still owed: live gate L0b. Two refinements from review: (1) the vendor rc also chowns `force_chg_{usb_suspend,ibatt,iusb,idc,auto_enable,itrick,fail_clear,usb_otg_ctl}` to `batt_health`, but smb-lib only creates those nodes in `mmi.factory_mode`, so they are absent on retail (consistent with the survey's `battery/device` listing). (2) Our kernel is built from Motorola's `sdm660_defconfig` (debug) where a retail kernel normally uses `sdm660-perf_defconfig`; on power-relevant keys the difference is only `DEBUG_OBJECTS*`, `PM_DEBUG` and `WQ_WATCHDOG`, which is also why only we print the ODEBUG warning. `status=Charging` proves charge *enable*, not charge *flow*: stock showed Charging while net-discharging (section 1.3). **Kernel update (2026-09-26, commits 9d396c5/bce8dd1):** the eight project patches now sit on Motorola `MMI-QPTS30.61-18-10` (`41aeca87a`, fork `cbd2b3acf`), built with stock's `sdm660-perf_defconfig`. `git diff e9225a64f 41aeca87a` is empty for `drivers/power`, `drivers/thermal`, `include/linux/power_supply.h`, the PM660/chef/moto-common DT and the battery-data dtsi, so every source claim above still holds, including line references. The perf config keeps `QPNP_SMB2`/`QPNP_FG_GEN3` and drops `DEBUG_OBJECTS`, so the ODEBUG warnings should disappear, and the debug-versus-retail config caveat no longer applies.

Conclusion: charging itself does not need an Android daemon. Detection, current limits, temperature zones, step charging and termination are all in-kernel and already probe identically under our image.

### 1.2 Stock userspace, piece by piece

| Stock component | What it does on this phone | Clone? |
|---|---|---|
| `thermal-engine` (`/vendor/etc/thermal-engine-chef.conf`) | Two battery rules. `SS-BATT-BATT`: steady-state controller on `batt_therm`, set point 44 °C, clear 42 °C, device `battery`, so it raises `system_temp_level` to hold the battery at ≤44 °C while charging. `SS-CHG-BATT`: monitor on `msm_therm` (thresholds 36/37/39/42/44 °C, clear 1 °C lower) → `battery` action levels `7 5 4 2 1`. Also `Mon-GPU-BATT`, CPU clusters and hotplug rules, and virtual `front_temp`/`back_temp` zones. | **Yes, the SS-BATT-BATT part.** The kernel temp zones already keep the battery safe. This rule adds the stock "stay under 44 °C" comfort throttle, which matters for a phone in the sun on handlebars while charging from a power bank. The `SS-CHG-BATT` level ordering (hotter → *lower* level) looks inverted and `msm_therm` reads a flat `30` on stock, so do not copy it blindly (open question Q3). CPU throttling belongs to the separate suspend/idle work. |
| `batt_health` (`vendor_pwric`) | Listens to `power_supply` uevents and logs them to gzip'd CSV under `/data/vendor/power_supply_logger`. Persists `age` and `cycles` in `/mnt/vendor/persist/batt_health/` and restores `cycle_count` to sysfs at boot. | **Logging part: yes** (to RAM, `/run/power`). **Persist part: no.** Writing `persist` is forbidden, and there is no writable storage yet. FG SRAM keeps cycle count while the battery stays connected. |
| `android.hardware.health@2.0-service` + framework `BatteryService` | Reads psy sysfs and broadcasts it. AOSP policy: shut down when `level == 0` and not powered; shut down when battery temp ≥ 68.0 °C (`config_shutdownBatteryTemperature` default); low-battery warning 15 %, critical 5 % (AOSP defaults; the Moto overlay was not decoded). | **Yes**: this is the low-battery warn + clean shutdown the next-steps page asks for. |
| `motorola.hardware.health@1.0-service` | Moto Mods external battery (`gb_battery`, `gb_ptp`, `eb_*` params). | No: no mods. |
| `hvdcp_opti` | QC3/PPS negotiation daemon. Only referenced from `on charger`, and no service definition exists. Not running in normal boot. | No. |
| `charge_only_mode` + `on charger` / `moto-charger` init triggers | Off-mode charging UI when the bootloader boots with `androidboot.mode=charger` (USB plugged at power-off). | **Deferred** to standalone boot. Today power-off with USB returns to Android's own charger mode, which is fine. |
| Charging LED (`/sys/class/leds/charging`) | Stock leaves it **off** (brightness 0, trigger none) while charging with the screen on. The kernel offers `battery-charging` etc. triggers. | No change: match stock (off). |
| `vendor.power-hal`, `perf`, `system_suspend` | CPU/perf hints, wakelocks and autosleep. | Out of scope: [suspend and idle power](power-and-reliability.md#suspend-and-idle-power). |

### 1.3 Things read live on stock today (2026-09-26, host CDP port; the phone was net-discharging)

`battery`: status Charging, charge_type Fast, capacity 79, temp 320 (tenths of °C), `current_max` 3000000, `input_current_limited=1`, `current_now` +103 mA, `system_temp_level=0` of `num_system_temp_levels=8`, `age=100`. `bms`: `battery_type=JK50atl_india_4v4_5000mAh`, `charge_full = charge_full_design = 5004000`, `cycle_count=0`, `soc_reporting_ready=1`, `voltage_ocv` 4.10 V, `time_to_empty_avg` 113077 s. `usb`: `real_type=USB_CDP`, `current_max=1500000`, but `input_current_now` ≈145 mA, `main/input_current_settled=150000` and `usb/voltage_now` 4.355 V: AICL had collapsed the input to about 150 mA on this port. **The phone was net-discharging while reporting Charging**: `charge_counter` fell across three reads (3862139 → 3862092 → 3862039). Sign convention (qpnp-fg-gen3.c `fg_get_time_to_full` negates ibatt; DT `fg-sys-term-current = -273`; `battery/current_now` is `bms/current_now`): **positive = discharging, negative = charging.** Charging LED off.

## 2. What we build

One small C daemon, **`powerd`** (`tools/powerd.c`), in the style of `buttond`. It is started from inittab in both the baseline and ride images. It is the combined clone of `BatteryService` policy, `batt_health` logging and the `SS-BATT-BATT` thermal-engine rule. No kernel changes are expected.

### 2.1 Inputs

- `NETLINK_KOBJECT_UEVENT` socket filtered to `SUBSYSTEM=power_supply` (what `batt_health` and healthd use), plus a periodic poll: 60 s normally, 10 s once capacity ≤ the warn level or battery temp ≥ the throttle set point.
- Attributes read (tolerate missing and `-22`/`EINVAL` values): `battery/{status,capacity,temp,voltage_now,current_now,health,charge_type,system_temp_level}`, `bms/{voltage_ocv,charge_full,cycle_count,soc_reporting_ready}`, `usb/{online,real_type,current_max,input_current_now,voltage_now,typec_mode}`, `pc_port/online`, `dc/online`, `main/input_current_settled`.
- The sysfs root must be overridable (`-r DIR` or env) so host tests can use a fake tree.

### 2.2 Outputs

- **State file** `/run/power/state`: `key=value` lines, written atomically (tmp + rename) on every change and poll. Consumers (fblog header, ride-logger heartbeat, future UI) read this instead of sysfs.
- **Event log** `/run/power/log.csv`: one row per uevent/poll (uptime, wall clock, the fields above). Size-bounded with one rotation (for example 1 MiB → `log.csv.1`). This is the `batt_health` clone and the main evidence source for unplugged tests.
- **kmsg lines** (`powerd: ...`) for transitions only: plug/unplug with source type, Charging/Discharging/Full, warn/critical thresholds, throttle level changes, and shutdown. fblog then shows them on the panel for free.
- `powerd status` client mode that prints the state file, like `buttond watch`, for scripts and humans.

### 2.3 Policy (stock-derived defaults, all overridable by flags)

1. **Low battery** (BatteryService clone). "Discharging" means `battery/status` is exactly `Discharging` **and** each of `usb/online`, `pc_port/online` and `dc/online` is 0 or the psy is absent; anything unreadable counts as powered. It never uses the sign of `current_now` (see Q1). Corrected after review: for an SDP source or a PD host data role, qpnp-smb2 `smb2_usb_get_prop` forces `usb/online` to 0 and reports the input on `pc_port/online`, and `battery/status` reflects raw input presence (`Not charging` with a charger attached), so `usb`+`dc` alone would call a phone on an SDP port "discharging".
   - Warn once at capacity ≤ 15 %; re-arm only when capacity rises ≥ 20 % or power is attached.
   - Critical once at ≤ 5 %: kmsg + a short vibrator buzz if `buttond` exposes one (optional, see Q5).
   - **Shutdown** when discharging and `battery/present == 1` and `bms/soc_reporting_ready == 1` and (capacity == 0 **or** (`voltage_now` < 3300 mV **and** capacity ≤ the critical level)), confirmed on 3 consecutive samples at least 5 s apart. The voltage leg is gated on capacity because `bms/resistance` ≈ 168 mΩ lets GPS/panel load sag a healthy battery. A missing, `-22` or unreadable capacity/voltage counts as *unknown*, never as 0, and resets the confirmation count. The FG itself reports 0 below its 3200 mV cutoff; 3000 mV is its empty level. Action: log, `sync`, then run the same orderly path `buttond` uses (`poweroff` → busybox init `::shutdown`). Never act on low battery while any supply is online.
   - **Over-temperature shutdown** when battery temp > 68.0 °C (strictly greater, as AOSP), same 3-sample confirmation and the same unknown-value rule. This one applies **whether or not a supply is online** (AOSP parity): powered, the phone then comes back in Android's off-mode charger, which has far less load than our running image, and the kernel mmi zones already stop charging above 60 °C. This is the only shutdown allowed while a supply is online (outside the test flag).
   - Touch a marker `/run/power/shutdown-pending` and log the reason *before* the shutdown, so a streaming host sees why.
2. **Charge thermal throttle** (`SS-BATT-BATT` clone). Only while a supply is online. Every 5 s, if battery temp ≥ 44.0 °C raise `battery/system_temp_level` by 1 (max `num_system_temp_levels - 1`). If ≤ 42.0 °C lower it by 1 toward 0. Write 0 at startup and on unplug so no stale level survives. Log each level change.
3. **Nothing else writes charger knobs.** No ICL/FCC overrides, no `input_suspend`, no `set_ship_mode`, no `force_demo_mode`, no `persist`/EFS writes. The kernel's stock behaviour stays authoritative.

### 2.4 Integration

- inittab `::respawn:/usr/bin/powerd` in `initramfs/etc/inittab` and the ride image's alternate inittab.
- `ride-logger`: add power state (status, source, voltage, current, temp) to the heartbeat and `meta.txt`, reading `/run/power/state` and falling back to sysfs.
- `fblog` header: optionally show `+` or `-` for charging/discharging next to the %, read from the state file. Keep it small; skip it if it complicates fblog.
- Makefile targets, `tools/tests/test_powerd.c` (or a shell harness like `test_ride-logger.sh`) with a fake sysfs tree and injected uevents. The shutdown command must be injectable (`-x CMD`) so tests never power anything off.
- Test-only flags for live verification: override warn/critical/shutdown thresholds and the throttle set point, plus `--shutdown-when-online` (needed to exercise the shutdown path with the USB control link attached). Test flags must be loud in kmsg.

## 3. Live verification plan (one `fastboot boot` session, nothing flashed)

The phone is on stock Android with the host USB cable attached. Ask the user once before rebooting it. Use the existing telnet driver (see the phone live-test notes: `run-host.sh`, `remote.py`, `collect.py`). Anything that needs the cable unplugged relies on `/run/power/log.csv` being pulled after the replug.

| Step | What | Pass criteria |
|---|---|---|
| L0 | Read-only survey under our kernel: dump every `power_supply/*` attribute, `thermal_zone*/{type,temp}`, `/sys/module/qpnp_fg_gen3/parameters/*`, `battery/device/*`, and FG/PMI dmesg. Diff against the stock dump. | Same attribute set and sane values; `battery_type` = `JK50atl_india_4v4_5000mAh`; `soc_reporting_ready=1`; `charge_full` ≈ 5004000. Capacity plausible against `voltage_ocv` (and within a few % of what stock reported just before the reboot). |
| L0b | **Charging proof gate** (added 2026-09-26 after the user questioned the "kernel does it all" claim; revised after review). First **unplug and replug under our kernel**, so APSD, the ICL vote and the mmi heartbeat run from scratch and no stock/ABL-programmed SMB2 state is being measured. Then sample every 30 s for ≥ 10 min: `bms/charge_counter` (FG coulomb counter, µAh), `battery/{capacity,status,charge_type,current_now,voltage_now,constant_charge_current_max}`, `usb/{real_type,voltage_now,input_current_now}` and `main/input_current_settled`. Run it on the **wall charger (DCP)**, or on the host port only with the panel off (`fblog.off`). Then ≥ 2 min unplugged. | Pass: `charge_counter` rises and `current_now` is negative while plugged; it falls and turns positive unplugged. If it is flat or falling while plugged, check `input_current_settled`/`usb/voltage_now` for AICL collapse (as on stock, section 1.3): that is a weak source, not a kernel failure. **Fail** (stop, report to battery_lead before any powerd live testing) only if the counter falls on the wall charger with the input current settled high. |
| L1 | Host port: note `usb/real_type` (CDP on the current port; try an SDP/rear port too if the user is willing), `current_max`, `input_current_now`, `main/input_current_settled`, `usb/voltage_now` and battery `status`. | Detection is correct (SDP ICL 500 mA with gadget `MaxPower 500`, CDP 1500 mA). Record the settled input current: the host port may AICL-collapse to about 150 mA as it did on stock, which is a property of the port and cable. |
| L2 | User unplugs for ~2 min, plugs a **wall charger** for ~5 min, unplugs ~2 min, then replugs the host cable. Pull `log.csv`. | Status flips Charging ↔ Discharging within one uevent; wall charger detected as DCP (or HVDCP) with a higher ICL; kmsg transitions present; `current_now` sign convention recorded (Q1). |
| L3 | Thermal throttle path with a test set point (for example 30 °C). **On the wall charger**: `system_temp_level` is an FCC vote (smb-lib.c `smblib_set_prop_system_temp_level`), not ICL, so on an input-limited host port no current change is visible. | `system_temp_level` steps up; the FCC vote follows (`battery/constant_charge_current_max` or the dmesg heartbeat `FCC =`) and the charge current (`current_now`, negative) shrinks in magnitude; it steps back to 0 after the set point is restored or on unplug. |
| L4 | Low-battery path with test thresholds (for example warn = capacity + 2, critical = capacity + 1, shutdown voltage above the current `voltage_now`, plus `--shutdown-when-online`), streaming `/dev/kmsg` to the host like the buttond power-off test. | Warn → critical → shutdown logged in order, then a clean power-off. With USB attached the phone re-boots into Android charger mode, which proves a real PMIC power-down. Ask the user to watch the screen. |
| L5 (separate, long) | Drain: ride image with GPS active, unplugged for ≥ 1 h (user outdoors or on the terrace), then pull `log.csv`. | %/h and mean discharge current recorded; answers "what does a ride drain look like". |

L5 can be a separate session. L0 to L4 fit in one boot.

## 4. Open questions for the implementer to settle live

- **Q1** Confirm the sign convention under our kernel (source and stock say positive = discharge, negative = charge) and whether `battery/current_now` equals `bms/current_now`. The policy must not depend on the sign.
- **Q2** Is capacity sane without any userspace helper? The expectation is yes, because FG-gen3 is fully in-kernel. Does the 2026-09-15 battery swap need a profile reload? The expectation is no: the battery serial matches the ATL-india profile on both kernels and `qcom,fg-force-load-profile` is set. Confirm with L0 (`battery_type`, capacity vs OCV) and note that learned `charge_full` still equals design (no learning cycle yet).
- **Q3** Which thermal zones are real sensors under our kernel? On stock `msm_therm`, `chg_therm`, `xo_therm` and `quiet_therm` read `30`. These are qpnp-adc-tm zones reporting whole °C (qpnp-adc-tm.c returns `result.physical`), so 30 is probably a real 30 °C, not a dead sensor. The throttle still uses battery temp only, the `SS-BATT-BATT` input.
- **Q4** Does `status` reach `Full` and does recharge restart correctly? This needs a long charge. Record it opportunistically from `log.csv` if the phone reaches 100 % during testing; otherwise leave it as a documented gap.
- **Q5** Vibrator buzz for the critical warning: only if it can reuse whatever `buttond` uses without contention. Otherwise kmsg/panel only. The ride image runs no `buttond`, so there it is kmsg/panel only.
- **Ride image caveat**: ride data lives in `/run` (RAM), so a low-battery shutdown in the ride image destroys the ride log. The warnings exist so the rider can pull logs first; until persistent storage exists, the shutdown protects the hardware, not the data. All L4/L5 evidence must stream to the host or be pulled before any shutdown.

## 5. Out of scope here

Off-mode charging under our own image (needs standalone boot), persistent cycle/age storage (needs persistent storage), suspend/idle measurements, CPU thermal throttling, and a charge limit for storage (`force_demo_mode` exists if wanted later).

## 6. Documentation to update when done

`docs/features/battery-and-charging.md` (new feature guide), `docs/features/README.md` table, `docs/next-steps/power-and-reliability.md` (trim what is done), `docs/repository-layout.md` (`tools/powerd.c`), and a dated `docs/build-log.md` entry with evidence under `logs/`.
