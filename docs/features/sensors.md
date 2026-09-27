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

The registry is served from a **RAM copy** of persist's `/sensors/sns.reg` (27426 bytes). Which bytes belong to which REG2 item or group is not stored in the file; it is compiled into the stock `sensors.qti`. `tools/sns-reg-map.py` extracts that layout (2561 items, 108 groups) into `/usr/share/sensord/sns_reg.map` at image build time. That map is vendor data and is not committed. Registry writes from the DSP change the RAM copy and are logged; 3 s after the last one (and when `sensord` exits) `sensord` writes the RAM copy back over `/run/sensors/sns.reg` (temp file, fsync, rename; only ever onto tmpfs/ramfs, never persist), so the `/run` copy holds what the DSP wrote this boot. Restarting `sensord` does not restart the ADSP, which keeps its calibration state in its own memory; the `/run` copy matters when the ADSP restarts or re-reads the registry, and as the source for a future across-boot copy. Live, the DSP wrote group 20 (126 bytes) once at start-up, and group 2980 (the magnetometer hard-iron bias it learns, see [below](#magnetometer-calibration)) from time to time.

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

1. Copies `sns.reg` from persist to `/run/sensors/sns.reg` (mode 0400), under the [read-only contract](#persist-read-only-contract) below, and checks its size against the map. It records the boot id (`/proc/sys/kernel/random/boot_id`) in `sns.reg.boot_id` and keeps the copy as read in `sns.reg.persist`. If a copy made in this boot is already there with the right size (`sensord` was restarted), it reuses it without touching persist at all (no `setro`, no mount) and logs `registry reused from this boot: ... (DSP writes kept: N bytes differ from persist's; persist not touched)`. `SENSORS_FRESH=1` forces a fresh copy.
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
| `list` | `{"ok":"list","smgr":"ready","sensors":[{"sensor":"accel","unit":"m/s^2","smgr_id":0,"data_type":0,"present":true,"name":"BMI160 Accelerometer","vendor":"BOSCH","max_hz":200,"range":78.45,"resolution":0.002396,"rate":0,"claims":0,"calibration":"full"},...]}` |
| `status` | `{"ok":"status","proto":1,"smgr":"ready","smgr_node":5,"smgr_port":87,"smgr_resets":0,"api":"buffering","reg2":true,"reg2_reads":...,"reg2_writes":...,"reg2_misses":...,"reg2_saves":...,"reg2_save_errors":...,"time2":false,"time2_reqs":0,"unknown_reqs":0,"ts":"dsps","clients":1,"qmag":"off","qmag_instance":-1,"qmag_enables":0,"qmag_inds":0,"qmag_errors":0,"qmag_last_error":-1}` (with `-Q` and a QMAG_CAL report received, also `"bias"`, `"bias_raw"`, `"accuracy"` as on magn lines) |
| `claim SENSOR [HZ]` | `{"ok":"claim","sensor":"accel","rate":50}`, then sample lines |
| `release SENSOR` | `{"ok":"release","sensor":"accel"}` |
| `get SENSOR [HZ]` | one sample line, or `{"err":"timeout","sensor":"accel"}` after 5 s |

Sample lines, with `t` in CLOCK_MONOTONIC nanoseconds (the accel line is a live sample; the others show the shape):

```
{"sensor":"accel","t":667817617363,"x":8.42253,"y":0.173035,"z":5.33611}
{"sensor":"anglvel","t":NS,"x":X,"y":Y,"z":Z}
{"sensor":"magn","t":NS,"x":X,"y":Y,"z":Z}
{"sensor":"magn","t":NS,"x":X,"y":Y,"z":Z,"bias":[BX,BY,BZ],"bias_raw":[R0,R1,R2],"accuracy":A}   (-Q only)
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

`sensord [-r REG] [-m MAP] [-S SOCK] [-T dsps|cntvct|rx] [-b HZ] [-s CH=ID:DT]... [-c CH=full|factory|raw]... [-i BASE] [-Q] [-P] [-t] [-R] [-F] [-v]`; `sensors-up` passes `SENSORD_ARGS` through (default `-v`).

| Flag | Meaning |
|---|---|
| `-r`, `-m`, `-S` | Registry copy (`/run/sensors/sns.reg`), map (`/usr/share/sensord/sns_reg.map`), socket (`/run/sensord.sock`). |
| `-T dsps` | Default: timestamps through `/dev/sensors`. Falls back to `rx` if the device cannot be opened. `-T cntvct` reads the QTimer directly; `-T rx` pins the newest sample of each indication to its receive time. A mismatched counter logs `timestamp skew` once a minute. |
| `-b HZ` | Cap the report rate below the sample rate so samples arrive batched. Default 0: one report per sample, as the stock HAL does. |
| `-s CH=ID:DT` | Map a channel to another SMGR sensor ID and data type, e.g. `illuminance=40:1`. |
| `-c CH=CAL` | Diagnostic. Calibration SMGR applies to that channel (the BUFFERING item's calibration field): `full` (0, default: factory plus any dynamic calibration), `factory` (1, what the stock HAL asks for its uncalibrated sensors) or `raw` (2). Repeatable. Logged on every report add/change (`add report 3: magn 20 Hz, calibration factory`). No effect with `-P`. |
| `-i BASE` | SMGR report IDs are BASE + channel + 1 (default 0). For an extra SMGR client beside the main sensord (`-R -S OTHER.sock`), so the two never share a report ID. |
| `-Q` | Diagnostic only: QMAG_CAL client (SAM 0x140 instance 0x3201). Live it was accepted but sent no indications and changed nothing; the hard-iron calibration that matters runs in SMGR without it (see [magnetometer calibration](#magnetometer-calibration)). While `magn` is claimed: reads the algorithm attributes (0x24, once), enables it (0x02) and stores the instance, logs every report (0x05: bias raw and as gauss, accuracy; all to stderr, the first, accuracy changes and one per 5 s to kmsg) and error (0x06) indication, adds the latest bias to magn lines and `status`. Disables it (0x03) when the last magn claim goes, while SMGR is lost, and on exit. ENABLE is sent empty (the IDL gives its optional report-period TLV a max length of 0 and a u32 at offset 1 of a 2-byte struct, so no stock client can send it; sensord has no way to), exactly once with a 10 s timeout (each ENABLE creates an instance, so it is never resent), a late ENABLE response carrying a foreign instance is disabled at once, and on exit an ENABLE still in flight is waited for up to 1.5 s and its instance disabled. |
| `-P` | Use the older periodic REPORT request (0x02) instead of BUFFERING (0x21). Not needed live. |
| `-t` | Also serve TIME2 (0x118 instance 0x3202) on the apps node. Not needed live: nothing asked for it. |
| `-R` | Do not serve REG2 (tests SMGR without it). |
| `-F` | Allow claims on channels the sensor info did not list. |
| `-v` | Log every REG2 request and SMGR exchange to stderr. |

## Persist read-only contract

persist (`mmcblk0p38`) is device-unique and irreplaceable. `sensors-up` never writes it:

- Before mounting, it runs `blockdev --setro` on the persist block device and refuses to continue unless `blockdev --getro` reads back 1; it logs `/dev/mmcblk0p38 set read-only at the block layer (blockdev --getro = 1)`. A mount with `-o ro,noload` alone is not enough: ext4 still runs orphan cleanup on a read-only mount and writes the device if the superblock has an orphan list (`ext4_orphan_cleanup()` only skips when the block device itself is read-only). The flag is never cleared; nothing on this image writes persist.
- It then mounts `-t ext4 -o ro,noload` (`noload` is still needed: a read-only device with a journal to replay fails to mount without it), copies the one file, and unmounts before doing anything else. If persist is already mounted somewhere, it copies from there and leaves that mount alone.
- `sensord` loads the copy once and writes only that `/run` copy back (the DSP's registry writes; atomic rename; refused with `EXDEV` unless the file is on tmpfs or ramfs). A `sensors-up` rerun in the same boot reuses that copy and does not touch persist at all.

Live proof: `mmcblk0p38` diskstats showed 24 reads and **0 writes** across the whole session, `getro` = 1 before each mount, persist not mounted at the end, and after rebooting to stock the `sns.reg` SHA-256 matched the pre-session stock read.

## Coexistence with audio

Sensors and audio share the ADSP. `sensors-up` reuses `audio-up` unchanged to boot it, and either order works. `audio-up` sets `restart_level=related`, so a DSP crash would restart the ADSP instead of panicking the kernel. Live S5: 1500 accel samples at 50 Hz while `speaker-test-tone` played (maximum gap 39.7 ms, none out of order), the tone completed, no subsystem restart.

`sensord` exits the same way on `TERM`, `INT` and `HUP` (a closed telnet session): it deletes its SMGR reports and, with `-Q`, disables QMAG_CAL. A restart of `sensord` is clean: after `TERM`, `sensors-up` logs `sensord exited (0)`. Running it again in the same boot reuses the `/run` registry copy (with the DSP's writes) and reaches `smgr ready`; after a reboot it copies from persist again. The ADSP itself is not restarted and keeps its calibration in its own memory either way. Live 2026-09-27 (`logs/regreuse-live-test-2026-09-27-notes.txt`): the rerun logged `registry reused from this boot ... (DSP writes kept: 0 bytes differ from persist's; persist not touched)` with no `blockdev` or mount, and persist diskstats stayed at 9 reads, 0 writes. If the SMGR port disappears (DSP restart), `sensord` notices within 5 s, looks the service up again and re-adds the reports that clients still hold.

## Modify and verify

`make -C tools/sensord test` runs ten suites: wire-format tests pinned to the stock IDL (`tests/test_sns_msgs.c`, plus an ASan/UBSan build with truncated-message cases), registry tests on a synthetic fixture, daemon unit tests (also under ASan), an end-to-end test of the real daemon against a fake SMGR and QMAG_CAL over a fake transport that follows the kernel's lookup rule (`tests/test_e2e.sh`), `tests/test_sensors-up.sh` (setro before the only mount, refusal paths, same-boot reuse without blockdev or mount), `tests/test_magcal_run.sh` (the guided calibration script against the fake), `tests/test_sns_tools.py` and `tests/test_mag_cal_check.py` (the fit tool on synthetic phone motion with a known offset and soft-iron matrix). The QMI codec `tools/qrtr/qmi.c` is shared with servreg-locator, so also run `make -C tools/servreg-locator test` after changing it.

Protocol references: `logs/sns-idl-dump-2026-09-26.txt`, `logs/sns-idl-dump-2026-09-27-qmag.txt` (QMAG_CAL 0x140, MAG_CAL 0x110; regenerate with `tools/sns-idl-dump.py` against the stock `libsensor1.so`), and `tools/sensord/sns_msgs.h`, which carries our reading of each message.

Sources: `tools/sensord/` (`sensord.c`, `sns_msgs.{c,h}`, `sns_reg.{c,h}`), `initramfs/usr/bin/sensors-up`, `tools/sns-idl-dump.py`, `tools/sns-reg-map.py`, the map step in `scripts/mkinitramfs.sh`.

## Magnetometer calibration check

Tooling for the experiment in [section 7 of the plan](../next-steps/sensors-plan.md#7-magnetometer-calibration-check-research-2026-09-27). With `sensors-up` running:

```sh
sensors-magcal-run --list                              # the step list, to read to the person holding the phone
setsid sensors-magcal-run > /run/magcal.log 2>&1 &    # MODE=sequential for one calibration select at a time
tail -n 1 /run/magcal.log                              # one-line summary when done
```

It prints the step list, then announces each step on the panel (kmsg: `---- STEP k/N: TEXT`, `move the phone now (8 s)` (11 s after face down), `HOLD N s: TEXT`) and on the vibrator (one long buzz: hold still now; two short: step over, move on), and records under `/run/sensortest/magcal-<stamp>/`. TERM, INT or HUP aborts it cleanly: captures stopped, extra instances stopped (QMAG_CAL disabled), no claims left on the main sensord. Phase a: magn with calibration full (main sensord), factory and raw (two extra `sensord -R -i N -c magn=...` instances on their own sockets) at the same time, through six faces (10 s each), a 30 s figure-8 and a slow 30 s flat turn. Phase q: a third extra instance with `-Q` enables QMAG_CAL on its magn claim; 60 s figure-8, six faces, flat turn, all four magn captures running. accel and anglvel at 50 Hz from the main sensord throughout. About 7 minutes (16 steps, 404 s of holds and moves; the live run took 418 s). `MODE=sequential` (about 11 minutes) runs phase a once per calibration select and captures only the QMAG instance's magn in phase q, for when the DSP refuses concurrent magn reports. The extra instances are stopped at the end (their reports deleted, QMAG_CAL disabled); the main sensord is left as it was.

On the host: `python3 tools/mag-cal-check.py DIR` drops gross outliers (|B - centre| outside median +-50 %, counted), fits a sphere and an ellipsoid per magn capture (hard-iron offset, soft-iron matrix; the ellipsoid replaces the sphere only if its W eigenvalues stay within 0.8..1.25, its centre within 0.05 G of the sphere's, and it makes neither the |B| spread nor the face sum worse, else the reasons are printed), reports |B| statistics (without and with the outliers), the stationary |B| per resting face and its range, face up vs face down z, the magnetic dip, heading against gyro-integrated yaw over the flat turn, the QMAG_CAL bias and accuracy history, and the median difference between captures recorded together (full minus factory is the bias SMGR applied), with a PASS/FAIL verdict for the data as delivered and after the fit, against the acceptance numbers in the plan, and a note line when the |B| spread is the only failure.

## Magnetometer calibration

Measured live on 2026-09-27 (`logs/magcal-live-test-2026-09-27-*`; every number below the first bullet is from `logs/magcal-live-test-2026-09-27-check.txt`, captures a (setup phase) / q (QMAG phase)):

- **The ADSP calibrates the magnetometer itself.** With calibration select `full` (sensord's default) SMGR subtracts a hard-iron bias that the DSP learns on its own, with no client enabling anything. It keeps it in registry group 2980 (items from about 3834), which it writes over REG2: all zero at 75 s after boot, (-0.241, -0.395, +0.255) G in the SMGR frame by 609 s, (-0.229, -0.401, +0.254) G later. In the device frame that is the correction added to the factory-calibrated field: live, factory minus full was (+0.401, +0.229, +0.254) G, the registry value after the axis map with the opposite sign.
- **It starts at zero every boot, and learning is condition-dependent.** persist (and the stock backup) hold zeros in group 2980, and we never write persist, so each boot begins uncorrected (at rest |B| read 0.74 G, y +0.71 G, before learning). The DSP does not reliably learn quickly: in the first session the first nonzero 2980 write came at 609 s, about 35 to 50 s into the slow face-by-face holds; in the reuse session a fast 30 s figure-8 in the air produced a 2980 write with a zero bias, and `full` stayed equal to `factory` (still phone: full (+0.341, +0.586, +0.215) G, factory (+0.341, +0.587, +0.214) G). Learning appears to need slow, varied orientations with holds. Until it happens `magn` is uncorrected, so a UI or compass must show "not calibrated" until `full` differs from `factory` (or group 2980 is nonzero). Restarting `sensord` does not restart the ADSP, which keeps its calibration state in its own memory; the `/run` copy matters when the ADSP restarts or re-reads the registry, and as the source for a future across-boot copy.
- **Calibrated output**, `full`, as delivered, after learning: residual sphere-fit offset (-0.0072, -0.0164, -0.0165) / (-0.0091, -0.0244, -0.0210) G (6 / 5 gross outliers dropped); face up + face down z -0.0127 / -0.0191 G (acceptance < 0.03: pass); heading against gyro yaw over a 30 s flat turn: max 13.8 / 9.5 deg, rms 6.8 / 4.0 deg (the 5 deg max is not met); |B| mean 0.370 / 0.372 G, std 0.030 / 0.037 G (8 / 10 %), spread 20.7 / 26.4 %; stationary |B| per resting face 0.350..0.386 / 0.363..0.403 G. The 5 % |B| spread criterion is not met indoors (field gradients over the movement volume and some soft iron). The ellipsoid fit does not help: for a it made the face sum worse, for q it was squashed (W eigenvalues 0.62..1.30) and 0.36 G off the sphere centre, so the tool keeps the sphere. Dip after the fit +3.0 / +3.0 deg.
- `factory` and `raw` still carry the whole offset: sphere fit (+0.389, +0.218, +0.240) G (a factory) to (+0.392, +0.204, +0.233) G (q factory), face up + face down z +0.489 to +0.502 G, heading off by up to 179.5 deg. They differ from each other by under 3 mG. Use them only as diagnostics (`-c`).
- **QMAG_CAL (0x140) is not needed.** Enabling it was accepted (instance returned, attributes answered) but it sent no indications and changed nothing: the `-Q` capture was identical to plain `full`. `-Q` stays a diagnostic.

## Limits and open items

- **Magnetometer bias relearned every boot.** The DSP's learned hard-iron bias lives in the ADSP's memory and in the `/run` registry copy, not across reboots, because persist is never written. Until the DSP has learned it (condition-dependent: slow, varied orientations with holds worked; a fast figure-8 alone did not), `magn` carries the uncorrected offset (about 0.4 to 0.7 G) and consumers must treat it as not calibrated. Keeping it across reboots needs writable storage of our own.
- **No smoothing.** The stock HAL smooths the magnetometer with an 8-sample moving average before Android sees it. `sensord` publishes raw samples, so a compass consumer must filter.
- **Not in inittab yet.** Start by hand. Adding it needs an idle and streaming power measurement first.
- **No D-Bus facade.** There is no `net.hadess.SensorProxy` (iio-sensor-proxy) interface; add one only when a consumer needs it.
- The DSP's fusion and gesture algorithms (SAM: motion detect, tap, rotation vector, step counter) are not exposed.
- Registry writes from the DSP are lost on reboot (the `/run` copy only, by design; persist is never written).

Remaining work is in the [sensors plan](../next-steps/sensors-plan.md#remaining-work).
