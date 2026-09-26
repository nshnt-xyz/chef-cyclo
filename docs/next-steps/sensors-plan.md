# Sensors: stock research and implementation plan

[Connectivity and sensors](connectivity-and-sensors.md#on-board-sensors) · [Next-steps index](README.md) · [Feature guide](../features/sensors.md)

Status: **implemented and live-verified 2026-09-26** (S0 to S5 below all passed). How to use it: [On-board sensors](../features/sensors.md). What is left: [remaining work](#remaining-work). The research below is kept as the record of why it is built this way. Raw stock evidence: [`logs/stock-sensors-survey-2026-09-26.txt`](../../logs/stock-sensors-survey-2026-09-26.txt); live evidence: `logs/sensors-live-test-2026-09-26-*`.

Goal: accelerometer, gyroscope, magnetometer, ambient light and proximity data available to userspace programs on the Alpine image (compass for the map, auto-brightness, wake-on-motion/tap, ride logging), exposed in a way that follows Linux conventions.

## 1. What the hardware and stock software actually are

### 1.1 There is no SLPI on this SoC

SDM636/660 has no separate sensor DSP. The sensor core (SSC: Qualcomm's sensor manager "SMGR", the drivers and the "SAM" algorithms) runs **inside the ADSP**, the same subsystem we already boot for audio (`qcom,lpass@15700000`, firmware `adsp.mdt`/`b00..b21` on the modem partition). The kernel's `sensors_ssc.c` (`qcom,msm-ssc-sensors`, enabled in `sdm660-moto-common.dtsi`, `CONFIG_SENSORS_SSC=y`) provides:

- `/sys/kernel/boot_slpi/boot`: calls `subsystem_get("slpi")`. There is no `slpi` subsystem on this SoC. **Never write it.**
- `/dev/sensors` (char 230): a single ioctl, `DSPS_IOCTL_READ_SLOW_TIMER`, that returns the QTimer scaled to the DSP's 32768 Hz clock. Used for timestamp conversion only; not a data path.

The ADSP publishes the sensor QMI services itself once booted. The service registry file `/firmware/image/adsps.jsn` names the domain `msm/adsp/sensor_pd` (instance 74); our `servreg-locator` already loads it.

### 1.2 Sensor chips (stock `dumpsys sensorservice`)

| Sensor | Chip | Rates | Notes |
|---|---|---|---|
| Accelerometer | Bosch BMI160 | 1 to 200 Hz, FIFO | Also exposed as wake-up variant |
| Gyroscope | Bosch BMI160 | 1 to 200 Hz | Calibrated and uncalibrated |
| Magnetometer | AKM AK09918 | 1 to 50 Hz | Calibrated and uncalibrated |
| Proximity | Eminent EPL259x | on-change | Wake-up; I2C bus 3, addr 0x49, IRQ GPIO 71 per the conf defaults |
| Ambient light | Eminent EPL259x | on-change | |
| Fusion / gestures (in DSP) | QTI SAM + Motorola | varies | Gravity, linear accel, rotation vectors, step, significant motion, tilt, AMD/stationary, Moto chop-chop/glance/flat/stowed |

No barometer. The two SX9310 capacitive "capsense" SAR sensors are AP-side I2C (`sx9310` kernel driver, input devices) and are not part of this work.

### 1.3 How stock runs it

| Stock piece | Role | Our equivalent |
|---|---|---|
| ADSP firmware | SMGR, drivers, SAM algorithms. Hosts QMI services on node 5: **SMGR 0x100** (inst 0x3201), SMGR_INTERNAL 0x10d, SMGR_RESTRICTED 0x12c, TIME2 0x118 (inst 0xa02), DIAG_DSPS 0x108, SAM services 0x104 to 0x174. | Already booted by `audio-up`. |
| `sensors.qti` (`/vendor/bin`, class core) | **Hosts REG2 0x10f** (inst 0x2) and **TIME2 0x118** (inst 0x3202) on the apps node. The registry is backed by `/mnt/vendor/persist/sensors/sns.reg` (27426 bytes), seeded from `/vendor/etc/sensors/sensor_def_qcomdev.conf` defaults. It watches `/persist/sensors` with inotify and rewrites `sns.reg` at runtime (calibration: bytes 0x700..0x70b changed between our backup and today). | **Must be rebuilt**: a REG2 server (and probably an apps TIME2 server). |
| `sscrpcd sensorspd` | FastRPC for an SLPI sensor PD. Started only if `/dev/msm_dsps` or `/dev/sensors` exists; `/dev/sensors` does exist, but the service is **not running** on stock and sensors work. | Not needed. |
| `libsensor1.so` + `sensors.ssc.so` + sensors HAL 1.0 | Android client side: QMI client of SMGR and SAM services, converts to Android events. | Replaced by our daemon. |
| `sensorscalibrate` HAL | Factory calibration UI. | No. |

QMI service IDs read from the IDL service objects in stock `libsensor1.so` / `sensors.qti` (idl version, max message length, number of req/resp/ind messages): SMGR 0x100 v1 (1676 bytes, 15/15/7), REG2 0x10f v2 (275, 7/7/0), TIME2 0x118 v2 (48, 3/3/1), SMGR_INTERNAL 0x10d v2, SAM_AMD 0x104, SAM_TAP 0x11a, SAM_SMD 0x128, SAM_FAST_AMD 0x134, SAM_ROTATION_VECTOR 0x112, SAM_GRAVITY 0x114, SAM_ORIENTATION 0x117 and more (full table in the evidence file). The message layouts are fully described by the QMI IDL type tables in those binaries and can be recovered from them.

Answered live (S0/S1): without a REG2 server the ADSP boots but SMGR 0x100 never registers (5 sensor-range services in 70 s). Once REG2 is served, even after the ADSP is already up, the DSP reads 75 groups and 1 item within about 0.5 s, writes group 20 once, and SMGR registers (50 services).

## 2. How Linux normally exposes sensor data

| Mechanism | Shape | Who uses it | Fit here |
|---|---|---|---|
| **IIO** (kernel, `drivers/iio`) | `/sys/bus/iio/devices/iio:deviceN/in_accel_x_raw` + `_scale`/`_offset`, `sampling_frequency`; buffered streaming through `/dev/iio:deviceN` with `scan_elements` and triggers. Standard units: accel m/s², anglvel rad/s, magn gauss, illuminance lux, proximity unitless. Sensor hubs on coprocessors are exposed this way too (`hid-sensor-hub`, ChromeOS `cros_ec_sensors`). | libiio, iio-sensor-proxy, every mainline laptop/phone with AP-attached sensors. | The "real" Linux answer, but it needs a kernel driver. Here the data only exists behind QMI on the ADSP, so an IIO driver would be an in-kernel SMGR QMI client, still depending on a userspace registry server. That means vendor-4.4 kernel work with panic risk for no user-visible gain. **Not now.** |
| **iio-sensor-proxy** (userspace, D-Bus `net.hadess.SensorProxy` on the system bus) | `ClaimAccelerometer`/`ReleaseAccelerometer` etc.; properties `AccelerometerOrientation` (normal/left-up/...), `LightLevel` + `LightLevelUnit`, `ProximityNear`, and `net.hadess.SensorProxy.Compass` `CompassHeading`. Sensors are only powered while claimed. | GNOME, Phosh, KDE, Qt Sensors: auto-rotate, auto-brightness, proximity blanking. | The desktop/phone-shell contract. Coarse (no raw vectors) but standard. Worth a thin compatibility facade later if an off-the-shelf consumer appears; `dbus-daemon` already runs on our image for BlueZ. |
| **Qualcomm DSP phones on mainline** (postmarketOS) | No kernel driver. `hexagonrpcd` serves the DSP's registry/config files; `libssc` talks QMI (the newer protobuf "SEE" protocol, SDM845 onward) to the sensor DSP; iio-sensor-proxy has an SSC backend built on libssc. | Phosh/Plasma Mobile phones on SDM845 and later. | Same architecture we need (userspace QMI client in front of a standard interface), but our SoC predates SEE: it speaks the older Sensors1/SMGR QMI API, so libssc does not apply. |
| **evdev** (`/dev/input/eventN`) | Older accelerometers as input devices with `INPUT_PROP_ACCELEROMETER` and `ABS_X/Y/Z`; proximity/lid as `EV_SW`. `uinput` lets userspace create such devices. | Legacy drivers, some Android-era kernels. | Possible via `uinput`, but no "consumer opened me" signal (sensors would stream forever or never) and no standard for magnetometer/lux. Not chosen. |
| **gpsd-style socket** | Daemon owns the hardware and publishes JSON over a socket, with watch/unwatch. | gpsd (already on our image for GPS). | The closest precedent for a userspace-owned device on this image. |

### Decision

A userspace daemon, **`sensord`**, owns the SSC (the postmarketOS architecture, adapted to the older SMGR API). Its primary interface is a **Unix socket with claim/release semantics** in the style of `buttond` and gpsd: JSON lines, sensors powered only while at least one client holds a claim, **IIO channel names and IIO units** (m/s², rad/s, gauss, lux) so consumers read like IIO and a later move to a kernel IIO driver or an iio-sensor-proxy facade changes the transport, not the data model. A `net.hadess.SensorProxy` D-Bus facade is deferred until a consumer needs it.

## 3. What we build

All new code under `tools/`, reusing the existing AF_MSM_IPC transport (`msmipc.{c,h}`) and the vendored QMI codec (`qrtr/qmi.c`, `libqrtr.h`) that `servreg-locator` and `qmuxd-lite` already use.

### 3.1 P0: protocol recovery (host only): done

Done as planned: `tools/sns-idl-dump.py`, `logs/sns-idl-dump-2026-09-26.txt`, `tools/sns-reg-map.py` (the map is generated at image build time, not committed). One finding changed the design: the stock HAL uses SMGR BUFFERING (0x21/0x22), not the periodic REPORT, so `sensord` does too.

- `tools/sns-idl-dump.py`: parse the QMI IDL service objects and type tables in stock `libsensor1.so` (extract with `debugfs -R "dump /lib64/libsensor1.so ..." stock/partitions/vendor_a.img`) and print, per service, every request/response/indication: message ID, TLV type IDs, element types, array lengths. Needed at least for SMGR 0x100, REG2 0x10f, TIME2 0x118, and later SAM_AMD 0x104 / SAM_TAP 0x11a. Commit the script and a text dump under `docs/` or `logs/`; hand-write our own C message definitions from the dump (do not copy vendor headers).
- Recover the registry layout: how REG2 single-item and group reads map item/group IDs to offsets in `sns.reg`. The mapping is compiled into `sensors.qti` (`sns_reg_la.c`/`sns_reg_data.c`, strings such as `sns_reg_storage_init, grp %d (%d of %d)`). Cross-check against the item IDs in `sensor_def_qcomdev.conf` and the known values (EPL259x UUID, i2c bus 3, addr 0x49) found in the backup `sns.reg`.

### 3.2 P1: `sensord` core: done

The TIME2 apps server exists behind `-t` but was not needed live; timestamps come from `/dev/sensors`. One live bug: the SMGR lookup has to ask for instance 0 (the kernel matches `instance & lookup_mask`, and the mask is always 0) and pick 0x3201 from the list.

One daemon, three roles, one event loop:

1. **REG2 server** (0x10f, instance/version encoding identical to stock: dump_servers shows inst 0x2). Serves from a RAM copy of `sns.reg`. **`persist` is device-unique and irreplaceable: never write it.** `sensors-up` mounts it read-only with `-o ro,noload` (no journal replay) or reads the file via `debugfs -R cat` on the block device, copies `sns.reg` to `/run/sensors/sns.reg`, and unmounts. Writes from the DSP (calibration) go to the RAM copy only and are logged. A `-r FILE` option points the server at any file for host tests.
2. **TIME2 apps server** (0x118, inst 0x3202) if the P0 dump plus the first live run show the DSP calls it. Timestamp conversion uses the same maths as `sensors_ssc.c` (QTimer `cntvct` × 16 / 9375, 32 kHz ticks), via `/dev/sensors` or directly.
3. **SMGR client** (0x100): enumerate sensors (all-sensor-info / single-sensor-info), add/delete periodic reports per claimed sensor at the max rate any client asked for, decode report indications (SMGR data is Q16 fixed point), convert to IIO units, and track data-type/sensor-ID per chip.

Logging goes to stderr plus terse `sensord: ...` kmsg lines for transitions (registry served, SMGR up, sensor list, claim/release, DSP restart).

### 3.3 P2: socket API and CLI: done

- `/run/sensord.sock`, versioned protocol. Requests: `list`, `claim <sensor> [rate_hz]`, `release <sensor>`, `get <sensor>` (one sample, claims briefly). Stream lines: `{"sensor":"accel","t":<CLOCK_MONOTONIC ns>,"x":..,"y":..,"z":..}` with fields per IIO channel (`accel`, `anglvel`, `magn`, `illuminance`, `proximity`). Disconnect releases all of that client's claims. Bounded per-client queues; a slow client drops samples (count reported), never stalls the daemon.
- `sensord list|get|watch <sensor> [rate]` client mode, like `buttond watch` / `powerd status`.
- Axis convention documented against the phone body (verify live: face up on a table gives +g on one axis).

### 3.4 P3: integration: done except inittab

`sensors-up` also marks persist read-only at the block layer (`blockdev --setro`) before mounting: `ro,noload` alone does not stop ext4 orphan cleanup from writing. See the [feature guide](../features/sensors.md#persist-read-only-contract).

- `initramfs/usr/bin/sensors-up` (shape of `audio-up`): mount firmware if needed, ensure `servreg-locator`, boot the ADSP if not already up (shared with `audio-up`; either order must work), wait for SMGR 0x100 in `dump_servers`, start `sensord`. Manual opt-in first; inittab only after live verification and a power measurement.
- Makefile targets and host tests: IDL-derived encode/decode round trips, REG2 group/item reads against a fixture `sns.reg` (synthetic fixture in the repo, not the device file), Q16 conversion, claim/release refcounting and rate max, client disconnect, slow client. A fake SMGR server over a socketpair or loopback transport for end-to-end tests.
- Later, not in this round: SAM AMD/tap for wake-on-motion, a tilt-compensated compass heading, ALS-driven backlight policy, an iio-sensor-proxy D-Bus facade.

## 4. Live verification plan (one `fastboot boot` session, nothing flashed)

All six steps passed on 2026-09-26; results are in the [feature guide](../features/sensors.md) and the [build log](../build-log.md).

Driven by the sensors lead with the existing telnet driver (see the phone live-test notes). Ask the user once before rebooting.

| Step | What | Pass criteria |
|---|---|---|
| S0 | `audio-up` only (no sensord), then `dump_servers` and `dmesg` at 10 s intervals for 60 s. | Records whether SMGR 0x100 and the SAM services register without a REG2 server, and whether the DSP stays up. Either result is informative; a sensor-PD crash (SSR) is a stop-and-report. |
| S1 | `sensors-up` with `sensord -v`. | REG2 requests logged and answered from the RAM copy; SMGR 0x100 present; sensor list shows BMI160 accel/gyro, AK09918, EPL259x. |
| S2 | `sensord watch accel 50` phone flat, then on each edge. | \|a\| ≈ 9.8 m/s² at rest; the axis carrying g changes as expected; timestamps monotonic, rate ≈ requested. |
| S3 | gyro (rotate by hand), magn (turn phone through 360° flat), ALS (cover/uncover), proximity (hand over the top edge). User performs these in announced windows. | Plausible values and on-change events. |
| S4 | Release all claims; idle 60 s. | SMGR reports deleted (no indications); the DSP stays up; `sensord` idle CPU ~0. |
| S5 | Kill and restart `sensord`; then `audio-up` + `speaker-test-tone` while streaming accel. | Clean re-registration; audio and sensors coexist on the shared ADSP. |

## 5. Hazards and rules

- Never write `persist`, `/sys/kernel/boot_slpi/boot`, `msm_subsys/*` restart nodes, or the per-process `adsprpc` debugfs (it panics our kernel, see the FastRPC notes).
- The ADSP is shared with audio: a sensor-PD crash can take audio down (SSR). Keep ADSP `restart_level=related` as `audio-up` does.
- Registry values are device calibration. Serve them read-only in spirit: the RAM copy is the only writable one.
- On stock, root commands go through the base64 `su -c sh` pipe, never `su -c 'a; b'`.

## 6. Documentation

Done 2026-09-26: [feature guide](../features/sensors.md), feature index row, [On-board sensors](connectivity-and-sensors.md#on-board-sensors) in the connectivity plan, repository layout, and the build-log entry.

## Remaining work

- **Magnetometer calibration check.** Live, magnetometer z did not change sign between face up (+0.20 gauss) and face down (+0.31 gauss). Find out whether SMGR applies its calibration (calibration 0 should be full calibration), whether the registry holds a valid calibration for the AK09918, and whether a hard-iron offset has to be estimated in userspace.
- **Tilt-compensated compass** for the map, after that check: heading from accel + magn with smoothing (stock smooths the magnetometer over 8 samples; `sensord` does not).
- **Wake-on-motion and tap** through the DSP's SAM services (AMD 0x104, TAP 0x11a; their IDL is already in the dump), exposed as claimable event channels.
- **ALS backlight policy**: auto-brightness from `illuminance`, with the display owner (fblog/UI) deciding.
- **inittab and power**: measure idle current with `sensord` up and nothing claimed, and while streaming accel at ride rates, then decide whether `sensors-up` runs at boot.
- **Optional `net.hadess.SensorProxy` facade** (iio-sensor-proxy's D-Bus interface) if an off-the-shelf consumer ever needs it; `dbus-daemon` already runs for BlueZ.
