# On-board sensors

[Feature index](README.md) · [Build instructions](../building.md) · [Plan and stock research](../next-steps/sensors-plan.md)

## Current behavior

The accelerometer, gyroscope, magnetometer, ambient light and proximity sensors work under our image, live-verified on 2026-09-26 (evidence under `logs/sensors-live-test-2026-09-26-*`). They are **manual opt-in**: run `sensors-up` from the shell. Nothing starts them from inittab yet.

| Channel | Chip | Unit | Fields | Max rate |
|---|---|---|---|---|
| `accel` | Bosch BMI160 | m/s² | `x` `y` `z` | 200 Hz |
| `anglvel` | Bosch BMI160 | rad/s | `x` `y` `z` | 200 Hz |
| `magn` | AKM AK09918 | gauss | `x` `y` `z` | 50 Hz |
| `illuminance` | Eminent EPL259x | lux | `illuminance` | 5 Hz |
| `proximity` | Eminent EPL259x | none | `near` (0/1), `proximity` (raw) | 5 Hz |

The rates, ranges and names are what the DSP reported live (`sensord list`).

There is no separate sensor DSP (SLPI) on SDM636. The sensor core (Qualcomm's SMGR, its drivers and fusion algorithms) runs inside the ADSP, the same subsystem `audio-up` boots for sound. It speaks the Sensors1 QMI protocol over the IPC router. On stock Android two apps-side pieces talk to it, and `sensord` replaces both:

1. **Registry server** (QMI REG2, service 0x10f instance 0x0002; `sensors.qti` on stock). The ADSP reads its configuration from it: driver UUIDs, I2C bus and address, axis orientation, calibration. Live, the DSP read 75 groups and 1 item within about 0.5 s of the server appearing, then registered SMGR.
2. **SMGR client** (QMI service 0x100 instance 0x3201 on the ADSP; `libsensor1` and the sensors HAL on stock). It enumerates the sensors, adds a report on the DSP for each claimed channel at the highest rate any client asked for, deletes it when the last claim goes, and converts the samples.

The registry is served from a **RAM copy** of persist's `/sensors/sns.reg` (27426 bytes). Which bytes belong to which REG2 item or group is not stored in the file; it is compiled into the stock `sensors.qti`. `tools/sns-reg-map.py` extracts that layout (2561 items, 108 groups) into `/usr/share/sensord/sns_reg.map` at image build time. That map is vendor data and is not committed. Registry writes from the DSP change only the RAM copy and are logged; live, the DSP wrote group 20 (126 bytes) once at start-up.

## Run

In the phone shell:

```sh
setsid sensors-up > /run/sensors-up.log 2>&1 &     # SENSORD_ARGS=-v for per-request REG2 logging
sensord list                   # channels, chip names, rates, claims; "smgr":"ready" once up
sensord status                 # daemon state and counters
sensord get accel              # one sample, then exit
sensord watch accel 50         # stream at 50 Hz until ^C
sensord -n 250 watch magn 20   # stop after 250 samples
dmesg | grep -E 'sensord|sensors-up'
```

`sensors-up` does, in order:

1. Copies `sns.reg` from persist to `/run/sensors/sns.reg` (mode 0400), under the [read-only contract](#persist-read-only-contract) below, and checks its size against the map.
2. Runs `irsc`, starts `sensord`, and waits for REG2 (0x10f) in `dump_servers`.
3. If the ADSP is not `ONLINE` and `audio-up` is not running, starts `audio-up` to boot it. If the ADSP is already up (audio-up ran first), it logs `adsp already ONLINE` and the DSP reads the registry late. That order worked live.
4. Waits up to 60 s for SMGR (0x100) and logs how long it took.
5. Stays resident supervising `sensord`. `TERM` stops `sensord` only. The ADSP stays up, and an `audio-up` it started keeps running.

Run it with `setsid` so closing the telnet session does not hang it up. A second `sensors-up` refuses while `/run/sensors-up.lock` exists, and a second `sensord` refuses while another one answers on the socket.

Without a REG2 server the ADSP boots (audio works) but SMGR never registers: live S0 saw only 5 sensor-range services in 70 s, then 50 once REG2 was served.

## Socket protocol

`/run/sensord.sock`, a Unix stream socket. Requests are plain text lines. Every line `sensord` sends is one JSON object.

| Request | Reply |
|---|---|
| (connect) | `{"hello":"sensord","proto":1}` |
| `list` | `{"ok":"list","smgr":"ready","sensors":[{"sensor":"accel","unit":"m/s^2","smgr_id":0,"data_type":0,"present":true,"name":"BMI160 Accelerometer","vendor":"BOSCH","max_hz":200,"range":78.45,"resolution":0.002396,"rate":0,"claims":0},...]}` |
| `status` | `{"ok":"status","proto":1,"smgr":"ready","smgr_node":5,"smgr_port":87,"smgr_resets":0,"api":"buffering","reg2":true,"reg2_reads":...,"reg2_writes":...,"reg2_misses":...,"time2":false,"time2_reqs":0,"unknown_reqs":0,"ts":"dsps","clients":1}` |
| `claim SENSOR [HZ]` | `{"ok":"claim","sensor":"accel","rate":50}`, then sample lines |
| `release SENSOR` | `{"ok":"release","sensor":"accel"}` |
| `get SENSOR [HZ]` | one sample line, or `{"err":"timeout","sensor":"accel"}` after 5 s |

Sample lines, with `t` in CLOCK_MONOTONIC nanoseconds (the accel line is a live sample; the others show the shape):

```
{"sensor":"accel","t":667817617363,"x":8.42253,"y":0.173035,"z":5.33611}
{"sensor":"anglvel","t":NS,"x":X,"y":Y,"z":Z}
{"sensor":"magn","t":NS,"x":X,"y":Y,"z":Z}
{"sensor":"illuminance","t":NS,"illuminance":LUX}
{"sensor":"proximity","t":NS,"near":1,"proximity":RAW}
```

Errors are `{"err":"unknown sensor","sensor":"x"}`, `{"err":"sensor not present",...}`, `{"err":"bad rate",...}`, `{"err":"unknown command",...}`, `{"err":"line too long"}` and `{"err":"smgr refused report",...}`.

Rules:

- A sensor is powered (a report exists on the DSP) only while at least one connection claims it. A claim lives on its connection: disconnecting releases everything that client claimed.
- The DSP runs at the highest claimed rate, clamped to the sensor's maximum. The default rate is 10 Hz for `accel`/`anglvel`/`magn` and 5 Hz for `illuminance`/`proximity`. A client that claimed a lower rate gets samples decimated to its own rate.
- Each client has a bounded output queue (16 KiB, plus a 16 KiB socket buffer). A slow reader loses samples instead of stalling the daemon, and before its next sample line it gets `{"dropped":N}`. A client that lets a reply overflow is disconnected.

## Units and axes

SMGR reports Q16 fixed point in its own axis frame. `sensord` maps it the way the stock HAL does (disassembly of `sensors.ssc.so`): `x = d[1]`, `y = d[0]`, `z = -d[2]` for accel, gyro and magnetometer, each divided by 65536. The result is the Android/IIO device frame: x to the right, y towards the top edge, z out of the screen. Units are IIO's: m/s², rad/s, gauss (the HAL multiplies by 100 for µT), lux.

Verified live (2026-09-26, guided run):

- Face up: accel z = +10.09 m/s². Face down: z = −9.50. Portrait, top edge up: y = +9.11. Landscape, right edge down: x negative. At rest |a| = 9.97 m/s².
- Gyro and accel agree: integrating the gyro predicts the measured gravity direction within 0 to 2° across the face up → face down → portrait steps.
- Magnetometer: during a flat spin the heading follows the gyro in the opposite sense (gyro z −111°, magnetometer heading +100 to +116° per 1.5 s); horizontal field about 0.32 gauss. See [limits](#limits-and-open-items) for the z-axis question.
- Light: 250 to 320 lux in the room, 0 when covered or face down.
- Proximity: `near` = 1 when covered or face down, 0 otherwise. `near` is true when `d[0] / 65536` truncates to a non-zero value, the HAL's rule. The `proximity` field is `d[1]` as the DSP sends it, unscaled (the stock HAL only logs it); live it stayed constant (762, later 748) whatever was in front of the sensor, so use `near`.
- Timing: accel and gyro at 50 Hz, 3595 samples, mean interval 20.00 ms, maximum 39.9 ms, none out of order; magnetometer at 20 Hz, mean 50.02 ms.

Timestamps: SMGR stamps each sample with the DSP's 32768 Hz tick counter. For each indication `sensord` reads the same counter through `/dev/sensors` (`DSPS_IOCTL_READ_SLOW_TIMER`, QTimer × 16 / 9375) back to back with CLOCK_MONOTONIC and converts. Live, the last sample's `t` was within 16 ms of `/proc/uptime`.

## Flags

`sensord [-r REG] [-m MAP] [-S SOCK] [-T dsps|cntvct|rx] [-b HZ] [-s CH=ID:DT]... [-P] [-t] [-R] [-F] [-v]`; `sensors-up` passes `SENSORD_ARGS` through (default `-v`).

| Flag | Meaning |
|---|---|
| `-r`, `-m`, `-S` | Registry copy (`/run/sensors/sns.reg`), map (`/usr/share/sensord/sns_reg.map`), socket (`/run/sensord.sock`). |
| `-T dsps` | Default: timestamps through `/dev/sensors`. Falls back to `rx` if the device cannot be opened. `-T cntvct` reads the QTimer directly; `-T rx` pins the newest sample of each indication to its receive time. A mismatched counter logs `timestamp skew` once a minute. |
| `-b HZ` | Cap the report rate below the sample rate so samples arrive batched. Default 0: one report per sample, as the stock HAL does. |
| `-s CH=ID:DT` | Map a channel to another SMGR sensor ID and data type, e.g. `illuminance=40:1`. |
| `-P` | Use the older periodic REPORT request (0x02) instead of BUFFERING (0x21). Not needed live. |
| `-t` | Also serve TIME2 (0x118 instance 0x3202) on the apps node. Not needed live: nothing asked for it. |
| `-R` | Do not serve REG2 (tests SMGR without it). |
| `-F` | Allow claims on channels the sensor info did not list. |
| `-v` | Log every REG2 request and SMGR exchange to stderr. |

## Persist read-only contract

persist (`mmcblk0p38`) is device-unique and irreplaceable. `sensors-up` never writes it:

- Before mounting, it runs `blockdev --setro` on the persist block device and refuses to continue unless `blockdev --getro` reads back 1; it logs `/dev/mmcblk0p38 set read-only at the block layer (blockdev --getro = 1)`. A mount with `-o ro,noload` alone is not enough: ext4 still runs orphan cleanup on a read-only mount and writes the device if the superblock has an orphan list (`ext4_orphan_cleanup()` only skips when the block device itself is read-only). The flag is never cleared; nothing on this image writes persist.
- It then mounts `-t ext4 -o ro,noload` (`noload` is still needed: a read-only device with a journal to replay fails to mount without it), copies the one file, and unmounts before doing anything else. If persist is already mounted somewhere, it copies from there and leaves that mount alone.
- `sensord` opens the copy `O_RDONLY` once and never writes it.

Live proof: `mmcblk0p38` diskstats showed 24 reads and **0 writes** across the whole session, `getro` = 1 before each mount, persist not mounted at the end, and after rebooting to stock the `sns.reg` SHA-256 matched the pre-session stock read.

## Coexistence with audio

Sensors and audio share the ADSP. `sensors-up` reuses `audio-up` unchanged to boot it, and either order works. `audio-up` sets `restart_level=related`, so a DSP crash would restart the ADSP instead of panicking the kernel. Live S5: 1500 accel samples at 50 Hz while `speaker-test-tone` played (maximum gap 39.7 ms, none out of order), the tone completed, no subsystem restart.

A restart of `sensord` is clean: after `TERM`, `sensors-up` logs `sensord exited (0)`. Running it again re-copies the registry (with `setro` again) and reaches `smgr ready`. If the SMGR port disappears (DSP restart), `sensord` notices within 5 s, looks the service up again and re-adds the reports that clients still hold.

## Modify and verify

`make -C tools/sensord test` runs eight suites: wire-format tests pinned to the stock IDL (`tests/test_sns_msgs.c`, plus an ASan/UBSan build with truncated-message cases), registry tests on a synthetic fixture, daemon unit tests (also under ASan), an end-to-end test of the real daemon against a fake SMGR over a fake transport that follows the kernel's lookup rule (`tests/test_e2e.sh`), `tests/test_sensors-up.sh` (setro before the only mount, refusal paths) and `tests/test_sns_tools.py`. The QMI codec `tools/qrtr/qmi.c` is shared with servreg-locator, so also run `make -C tools/servreg-locator test` after changing it.

Protocol references: `logs/sns-idl-dump-2026-09-26.txt` (regenerate with `tools/sns-idl-dump.py` against the stock `libsensor1.so`), and `tools/sensord/sns_msgs.h`, which carries our reading of each message.

Sources: `tools/sensord/` (`sensord.c`, `sns_msgs.{c,h}`, `sns_reg.{c,h}`), `initramfs/usr/bin/sensors-up`, `tools/sns-idl-dump.py`, `tools/sns-reg-map.py`, the map step in `scripts/mkinitramfs.sh`.

## Limits and open items

- **Magnetometer calibration.** Face up the magnetometer z read +0.20 gauss and face down +0.31 gauss; it should change sign. That points to a z offset or an uncalibrated state (sensord requests calibration 0, which should be full calibration). Check the calibration before building a tilt-compensated compass on it.
- **No smoothing.** The stock HAL smooths the magnetometer with an 8-sample moving average before Android sees it. `sensord` publishes raw samples, so a compass consumer must filter.
- **Not in inittab yet.** Start by hand. Adding it needs an idle and streaming power measurement first.
- **No D-Bus facade.** There is no `net.hadess.SensorProxy` (iio-sensor-proxy) interface; add one only when a consumer needs it.
- The DSP's fusion and gesture algorithms (SAM: motion detect, tap, rotation vector, step counter) are not exposed.
- Registry writes from the DSP are lost when `sensord` restarts (RAM only, by design).

Remaining work is in the [sensors plan](../next-steps/sensors-plan.md#remaining-work).
