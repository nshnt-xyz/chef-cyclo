# On-board sensors

[Feature index](README.md) · [Build instructions](../building.md) · [Plan and stock research](../research/sensors.md)

## Current behavior

The accelerometer, gyroscope, magnetometer, ambient light and proximity sensors work under our image, live-verified on 2026-09-26 (evidence under `logs/sensors-live-test-2026-09-26-*`). They are **manual opt-in**: run `sensors-up` from the shell. Nothing starts them from inittab yet.

| Channel | Chip | Unit | Fields | Max rate |
|---|---|---|---|---|
| `accel` | Bosch BMI160 | m/s² | `x` `y` `z` | 200 Hz |
| `anglvel` | Bosch BMI160 | rad/s | `x` `y` `z` | 200 Hz |
| `magn` | AKM AK09918 | gauss | `x` `y` `z` | 50 Hz |
| `illuminance` | Eminent EPL259x | lux | `illuminance` | 5 Hz |
| `proximity` | Eminent EPL259x | none | `near` (0/1), `proximity` (raw) | 5 Hz |
| `heading` | computed in `sensord` ([compass](#compass)) | degrees | `heading` `pitch` `roll` `calibrated` `disturbed` `accuracy` `cal_source` `mag_bias` | 50 Hz |
| `rotvec` | the ADSP's rotation vector (`-V` only, diagnostic) | quaternion | `x` `y` `z` `w` `accuracy` `coord` `heading` `pitch` `roll` | 20 Hz |

The rates, ranges and names of the first five are what the DSP reported live (`sensord list`). `heading` and `rotvec` are host-tested only so far (live check planned: [compass](#compass)).

There is no separate sensor DSP (SLPI) on SDM636. The sensor core (Qualcomm's SMGR, its drivers and fusion algorithms) runs inside the ADSP, the same subsystem `audio-up` boots for sound. It speaks the Sensors1 QMI protocol over the IPC router. On stock Android two apps-side pieces talk to it, and `sensord` replaces both:

1. **Registry server** (QMI REG2, service 0x10f instance 0x0002; `sensors.qti` on stock). The ADSP reads its configuration from it: driver UUIDs, I2C bus and address, axis orientation, calibration. Live, the DSP read 75 groups and 1 item within about 0.5 s of the server appearing, then registered SMGR.
2. **SMGR client** (QMI service 0x100 instance 0x3201 on the ADSP; `libsensor1` and the sensors HAL on stock). It enumerates the sensors, adds a report on the DSP for each claimed channel at the highest rate any client asked for, deletes it when the last claim goes, and converts the samples.

The registry is served from a **RAM copy** of persist's `/sensors/sns.reg` (27426 bytes). Which bytes belong to which REG2 item or group is not stored in the file; it is compiled into the stock `sensors.qti`. `tools/sns-reg-map.py` extracts that layout (2561 items, 108 groups) into `/usr/share/sensord/sns_reg.map` at image build time. That map is vendor data and is not committed. Registry writes from the DSP change the RAM copy and are logged; 3 s after the last one (and when `sensord` exits) `sensord` writes the RAM copy back over `/run/sensors/sns.reg` (temp file, fsync, rename; only ever onto tmpfs/ramfs, never persist), so the `/run` copy holds what the DSP wrote this boot. Restarting `sensord` does not restart the ADSP, which keeps its calibration state in its own memory; the `/run` copy matters when the ADSP restarts or re-reads the registry, and as the source for the narrow across-boot saved group. A seeded group is applied from the first samples (live 2026-10-10, [across boots](#across-boots)). Live, the DSP wrote group 20 (126 bytes) once at start-up, and group 2980 (the magnetometer hard-iron bias it learns, see [below](#magnetometer-calibration)) from time to time.

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

1. Copies `sns.reg` from persist to `/run/sensors/sns.reg` (mode 0400), under the [read-only contract](#persist-read-only-contract) below, and checks its size against the map. It records the boot id (`/proc/sys/kernel/random/boot_id`) in `sns.reg.boot_id` and keeps the copy as read in `sns.reg.persist`. If a copy made in this boot is already there with the right size (`sensord` was restarted), it reuses it without touching persist at all (no `setro`, no mount) and logs `registry reused from this boot: ... (DSP writes kept: N bytes differ from persist's; persist not touched)`. `SENSORS_FRESH=1` forces a fresh copy. A fresh copy gets the magnetometer bias saved on `/data` overlaid before `sensord` serves it ([across boots](#across-boots)); `SENSORS_FRESH=1` skips that too.
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
| `declination [DEG\|off]` | `{"ok":"declination","declination":1.50}` (or `null`): sets (-180..180, east positive), clears or reads the declination for every heading client; `{"err":"bad declination"}` |

Sample lines, with `t` in CLOCK_MONOTONIC nanoseconds (the accel line is a live sample; the others show the shape):

```
{"sensor":"accel","t":667817617363,"x":8.42253,"y":0.173035,"z":5.33611}
{"sensor":"anglvel","t":NS,"x":X,"y":Y,"z":Z}
{"sensor":"magn","t":NS,"x":X,"y":Y,"z":Z}
{"sensor":"magn","t":NS,"x":X,"y":Y,"z":Z,"bias":[BX,BY,BZ],"bias_raw":[R0,R1,R2],"accuracy":A}   (-Q only)
{"sensor":"illuminance","t":NS,"illuminance":LUX}
{"sensor":"proximity","t":NS,"near":1,"proximity":RAW}
{"sensor":"heading","t":NS,"heading":88.13,"pitch":2.90,"roll":-5.81,"calibrated":true,"disturbed":false,"accuracy":3.0,"cal_source":"live","mag_bias":0.528}
{"sensor":"heading","t":NS,...,"mag_bias":0.528,"rotvec_accuracy":3}   (-V: the ADSP rotation vector's accuracy, 0..3)
{"sensor":"heading","t":NS,...,"mag_bias":0.528,"true_heading":89.63,"declination":1.50}   (declination set)
{"sensor":"rotvec","t":NS,"x":0,"y":0,"z":-0.707107,"w":0.707107,"accuracy":2,"coord":1,"heading":90.00,"pitch":0.00,"roll":0.00}   (-V)
```

`status` also carries the compass (`"compass":"off|init|on"`, `compass_clients`, `mount`, `calibrated`, `cal_source` (`live`, `registry` or `none`), `mag_bias` (the live bias, factory minus full, device frame, gauss, or `null` before it is known), `mag_bias_pairs`, `mag_bias_reg` (registry items 3903..3905 as gauss, SMGR frame, or `null`), `disturbed`, `accuracy`, `rotvec_accuracy`, `declination`, the filter's counters `compass_lines`, `compass_dropped`, `compass_gaps`, `compass_resets`, `compass_gyro_lost`, `compass_disturbances`, `compass_reacquired`, and its `gyro_bias` in rad/s) and the rotation vector client (`rotvec`, `rotvec_instance`, `rotvec_enables`, `rotvec_inds`, `rotvec_errors`, `rotvec_last_error`, like the `qmag` fields, which stay last). `list` has an entry for each: heading with `"virtual":true`, its inputs, `mount` and `calibrated`; rotvec with `present` (true with `-V`) and `state`.

Errors are `{"err":"unknown sensor","sensor":"x"}`, `{"err":"sensor not present",...}`, `{"err":"bad rate",...}`, `{"err":"unknown command",...}`, `{"err":"line too long"}` and `{"err":"smgr refused report",...}`.

Rules:

- A sensor is powered (a report exists on the DSP) only while at least one connection claims it. A claim lives on its connection: disconnecting releases everything that client claimed.
- Claiming `heading` claims `accel` 50 Hz, `anglvel` 50 Hz and `magn` 20 Hz (calibration as `-c` says, `full` by default) inside `sensord`, plus a second magn report at calibration `factory`, 5 Hz, for the live calibrated flag (internal: report ID base + 6, not listed, not claimable; not added when `-c magn` is not `full`), and with `-V` the rotation vector (its accuracy goes on the heading lines), through the same rate maximum as client claims: another client can raise them, none lowers them while heading is claimed, and they go (with the filter) when the last heading claim goes. A report the DSP refuses for one of them is also reported to the heading's claimants as `{"err":"smgr refused report","sensor":"heading","input":"magn"}`. `rotvec` claims no SMGR report (the DSP's rotation vector gets its own data inside the DSP); without `-V` it answers `{"err":"not enabled (sensord -V)","sensor":"rotvec"}`.
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

`sensord [-r REG] [-m MAP] [-S SOCK] [-T dsps|cntvct|rx] [-b HZ] [-s CH=ID:DT]... [-c CH=full|factory|raw]... [-i BASE] [-M MOUNT] [-D DEG] [-Q] [-V] [-P] [-t] [-R] [-F] [-v]`; `sensors-up` passes `SENSORD_ARGS` through (default `-v`). Client mode: `sensord [-S SOCK] [-n COUNT] list | status | get SENSOR [HZ] | watch SENSOR [HZ] | declination [DEG|off]`.

| Flag | Meaning |
|---|---|
| `-r`, `-m`, `-S` | Registry copy (`/run/sensors/sns.reg`), map (`/usr/share/sensord/sns_reg.map`), socket (`/run/sensord.sock`). |
| `-T dsps` | Default: timestamps through `/dev/sensors`. Falls back to `rx` if the device cannot be opened. `-T cntvct` reads the QTimer directly; `-T rx` pins the newest sample of each indication to its receive time. A mismatched counter logs `timestamp skew` once a minute. |
| `-b HZ` | Cap the report rate below the sample rate so samples arrive batched. Default 0: one report per sample, as the stock HAL does. |
| `-s CH=ID:DT` | Map a channel to another SMGR sensor ID and data type, e.g. `illuminance=40:1`. |
| `-c CH=CAL` | Diagnostic. Calibration SMGR applies to that channel (the BUFFERING item's calibration field): `full` (0, default: factory plus any dynamic calibration), `factory` (1, what the stock HAL asks for its uncalibrated sensors) or `raw` (2). Repeatable. Logged on every report add/change (`add report 3: magn 20 Hz, calibration factory`). No effect with `-P`. |
| `-i BASE` | SMGR report IDs are BASE + channel + 1 (default 0). For an extra SMGR client beside the main sensord (`-R -S OTHER.sock`), so the two never share a report ID. |
| `-Q` | Diagnostic only: QMAG_CAL client (SAM 0x140 instance 0x3201). Live it was accepted but sent no indications and changed nothing; the hard-iron calibration that matters runs in SMGR without it (see [magnetometer calibration](#magnetometer-calibration)). While `magn` is in use (claimed by a client, or internally by the `heading` channel): reads the algorithm attributes (0x24, once), enables it (0x02) and stores the instance, logs every report (0x05: bias raw and as gauss, accuracy; all to stderr, the first, accuracy changes and one per 5 s to kmsg) and error (0x06) indication, adds the latest bias to magn lines and `status`. Disables it (0x03) when magn is no longer used, while SMGR is lost, and on exit. ENABLE is sent empty (the IDL gives its optional report-period TLV a max length of 0 and a u32 at offset 1 of a 2-byte struct, so no stock client can send it; sensord has no way to), exactly once with a 10 s timeout (each ENABLE creates an instance, so it is never resent), a late ENABLE response carrying a foreign instance is disabled at once, and on exit an ENABLE still in flight is waited for up to 1.5 s and its instance disabled. |
| `-M MOUNT` | How the phone is mounted, for the `heading` channel: `portrait` (default), `landscape-left` (top edge to the left), `landscape-right`, `flat` (+y only), `upright` (-z only). See [forward vector](#forward-vector). |
| `-D DEG` | Declination (east positive, -180..180): heading lines add `true_heading`. The `declination` request changes it at run time. |
| `-V` | Diagnostic only: rotation vector client (SAM 0x112 instance 0x3201, the ADSP's 9-axis fusion) behind the `rotvec` channel, for comparison with `heading`. Same rules as `-Q`: instance-0 lookup filtered to 0x3201, attributes (0x24) once, ENABLE sent exactly once with a 10 s timeout, a stray instance disabled at once, disabled on the last release, while SMGR is lost and on exit (an ENABLE in flight at exit is waited for up to 1.5 s and its instance disabled), reports decoded with bounds checks and the result TLV checked for its exact 18 bytes (the codec would accept a short one). Enable TLVs exactly as the stock HAL's default ("synchronous") request: report period 0, sample rate 20 Hz, suspend notification {apps, no indications in suspend}, no coordinate-system TLV (see [compass](#rotation-vector-comparison)). |
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

`sensord` exits the same way on `TERM`, `INT` and `HUP` (a closed telnet session): it deletes its SMGR reports and, with `-Q`/`-V`, disables QMAG_CAL and the rotation vector. A restart of `sensord` is clean: after `TERM`, `sensors-up` logs `sensord exited (0)`. Running it again in the same boot reuses the `/run` registry copy (with the DSP's writes) and reaches `smgr ready`; after a reboot it copies from persist again. The ADSP itself is not restarted and keeps its calibration in its own memory either way. Live 2026-09-27 (`logs/regreuse-live-test-2026-09-27-notes.txt`): the rerun logged `registry reused from this boot ... (DSP writes kept: 0 bytes differ from persist's; persist not touched)` with no `blockdev` or mount, and persist diskstats stayed at 9 reads, 0 writes. If the SMGR port disappears (DSP restart), `sensord` notices within 5 s, looks the service up again and re-adds the reports that clients still hold.

## Modify and verify

`make -C tools/sensord test` runs fifteen suites: wire-format tests pinned to the stock IDL (`tests/test_sns_msgs.c`, plus an ASan/UBSan build with truncated-message cases), registry tests on a synthetic fixture, daemon unit tests (also under ASan), the compass filter on synthetic motion (`tests/test_compass.c`, also under ASan), an end-to-end test of the real daemon against a fake SMGR and QMAG_CAL over a fake transport that follows the kernel's lookup rule (`tests/test_e2e.sh`), the heading and rotation vector end to end with the daemon under ASan/UBSan (`tests/test_compass_e2e.sh`: the heading claim adding and removing the three reports, rate max with other clients, the calibrated flag from a fake group 2980 write, declination, the rotation vector client through release, TERM, HUP, SMGR and DSP restarts, slow and in-flight ENABLEs), `tests/test_sensors-up.sh` (setro before the only mount, refusal paths, same-boot reuse without blockdev or mount), `tests/test_magcal_run.sh` and `tests/test_compass_run.sh` (the guided scripts against the fake), `tests/test_sns_tools.py`, `tests/test_mag_cal_check.py` (the fit tool on synthetic phone motion with a known offset and soft-iron matrix) and `tests/test_compass_check.py` (the compass acceptance tool on synthetic runs with known errors, and the replay of synthetic raw captures through `compass-replay`; with `COMPASS_EVIDENCE=DIR` it also replays a recorded magcal capture directory). The QMI codec `tools/qrtr/qmi.c` is shared with servreg-locator, so also run `make -C tools/servreg-locator test` after changing it.

Protocol references: `logs/sns-idl-dump-2026-09-26.txt`, `logs/sns-idl-dump-2026-09-27-qmag.txt` (QMAG_CAL 0x140, MAG_CAL 0x110), `logs/sns-idl-dump-2026-09-27-rotvec.txt` (ROTATION_VECTOR 0x112, with the stock HAL's use of it; regenerate with `tools/sns-idl-dump.py` against the stock `libsensor1.so`), and `tools/sensord/sns_msgs.h`, which carries our reading of each message.

Sources: `tools/sensord/` (`sensord.c`, `compass.{c,h}`, `sns_msgs.{c,h}`, `sns_reg.{c,h}`, `compass-replay.c`), `initramfs/usr/bin/sensors-up`, `initramfs/usr/bin/sensors-compass-run`, `tools/compass-check.py`, `tools/sns-idl-dump.py`, `tools/sns-reg-map.py`, the map step in `scripts/mkinitramfs.sh`.

## Magnetometer calibration check

Tooling for the experiment in [section 7 of the plan](../research/sensors.md#7-magnetometer-calibration-check-research-2026-09-27). With `sensors-up` running:

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
- **Without a saved overlay it starts at zero, and learning is condition-dependent.** persist (and the stock backup) hold zeros in group 2980, and we never write persist, so a fresh boot without an accepted saved group begins uncorrected (at rest |B| read 0.74 G, y +0.71 G, before learning). The DSP does not reliably learn quickly: in the first session the first nonzero 2980 write came at 609 s, about 35 to 50 s into the slow face-by-face holds; in the reuse session a fast 30 s figure-8 in the air produced a 2980 write with a zero bias, and `full` stayed equal to `factory` (still phone: full (+0.341, +0.586, +0.215) G, factory (+0.341, +0.587, +0.214) G). Learning appears to need slow, varied orientations with holds. Until it happens `magn` is uncorrected, so a UI or compass must show "not calibrated" until `full` differs from `factory` (or group 2980 is nonzero). Restarting `sensord` does not restart the ADSP, which keeps its calibration state in its own memory; the `/run` copy matters when the ADSP restarts or re-reads the registry, and as the source for the narrow across-boot saved group. A seeded group is applied from the first samples (live 2026-10-10, [across boots](#across-boots)).
- **Calibrated output**, `full`, as delivered, after learning: residual sphere-fit offset (-0.0072, -0.0164, -0.0165) / (-0.0091, -0.0244, -0.0210) G (6 / 5 gross outliers dropped); face up + face down z -0.0127 / -0.0191 G (acceptance < 0.03: pass); heading against gyro yaw over a 30 s flat turn: max 13.8 / 9.5 deg, rms 6.8 / 4.0 deg (the 5 deg max is not met); |B| mean 0.370 / 0.372 G, std 0.030 / 0.037 G (8 / 10 %), spread 20.7 / 26.4 %; stationary |B| per resting face 0.350..0.386 / 0.363..0.403 G. The 5 % |B| spread criterion is not met indoors (field gradients over the movement volume and some soft iron). The ellipsoid fit does not help: for a it made the face sum worse, for q it was squashed (W eigenvalues 0.62..1.30) and 0.36 G off the sphere centre, so the tool keeps the sphere. Dip after the fit +3.0 / +3.0 deg.
- `factory` and `raw` still carry the whole offset: sphere fit (+0.389, +0.218, +0.240) G (a factory) to (+0.392, +0.204, +0.233) G (q factory), face up + face down z +0.489 to +0.502 G, heading off by up to 179.5 deg. They differ from each other by under 3 mG. Use them only as diagnostics (`-c`).
- <a id="across-boots"></a>**Across boots (2026-10-06).** Only group 2980 is kept, never the whole registry (the rest is factory data and keeps coming from persist). Each time `sensord` writes the `/run` copy back (it renames a new file over it, so `sensors-up` sees the inode change within a second) and when `sensord` exits, `chef-state mag-save` saves the group to `/data/v1/sensors/mag-group-2980` if its bias (items 3903..3905, Q16 gauss) is nonzero and within 2 G on each axis; an all-zero group never replaces a saved bias, and equal bytes are not rewritten. Changed writes are coalesced to at most one save per minute during supervision; exit/shutdown takes a final snapshot. The file carries the SHA-256 of `sns.reg.persist` and of `sns_reg.map`, and a checksum line. On a fresh copy `chef-state mag-restore` overlays it (offset and length from the map at run time) only if both hashes match, the length matches the map and the bias is plausible; the overlay goes into a scratch copy that replaces the temp copy only after its read-back matches, and if anything fails `sensors-up` serves persist's bytes. `chef-state shutdown` stops `sensors-up` so the last write-back is saved before `/data` goes read-only. `sensord`'s own write-back still goes only to tmpfs/ramfs. Accepted live on 2026-10-10 on the installed pair ([log](../../logs/mag-seed-live-2026-10-10.txt)): the bias learned in one boot (second round of guided slow holds, calibrated about 84 s into it; saved as -0.3392 -0.2132 0.0425 G in the SMGR frame) was seeded after an ordinary reboot, and with the phone left still the first live comparison, about 3.3 s after the heading claim, was calibrated with a live bias within 5 mG of the learned one (0.2172 0.3401 0.0431 G in the device frame). Learning is needed once per hard-iron change, not once per boot. A stale bias after a hard-iron change (new mount, a magnet nearby) is still seeded and reported as calibrated until the ADSP re-learns; `SENSORS_FRESH=1` skips the overlay.
- **QMAG_CAL (0x140) is not needed.** Enabling it was accepted (instance returned, attributes answered) but it sent no indications and changed nothing: the `-Q` capture was identical to plain `full`. `-Q` stays a diagnostic.

## Compass

The `heading` channel: a tilt-compensated heading for the bike UI when stopped or slow, with an honest "not calibrated" / "disturbed" state ([design and experiment record](../research/sensors.md#8-tilt-compensated-compass-plan-2026-09-27)). Linux's name for the same quantity is IIO's `in_rot_from_north_magnetic_tilt_comp` (iio-sensor-proxy's `CompassHeading`). First live run 2026-10-01 (`logs/compass-live-test-2026-10-01-*`): the filter tracks the gyro to about 4 deg rms in hand-held motion and agrees with the ADSP's rotation vector within about 0.5 deg in calibrated still steps, but the calibrated flag of that build (registry only) was wrong. Run 2 live-verified the factory-minus-full flag below; see [limits and open items](#limits-and-open-items).

```sh
sensord -n 20 watch heading 10        # 10 Hz; claims accel 50, anglvel 50, magn 20 (+ factory 5) inside sensord
sensord get heading
sensord declination 1.5               # true_heading on every heading line; "declination off" clears
```

### Filter

`tools/sensord/compass.c`, pure C with no I/O, shared with the host replay tool. A Mahony-style complementary filter on a quaternion (body = the device frame, world = East-North-Up, the convention of an Android rotation vector):

- the gyro propagates the attitude (midpoint rate, exact rotation per step, minus a learned bias);
- each accelerometer sample corrects tilt only (gain 1 rad/s per rad), weighted 1 while |a| is within 5 % of g, 0 beyond 15 % (bumps, braking);
- each magnetometer sample corrects yaw only (gain 0.5 rad/s per rad, about a 2 s time constant) against the attitude at the sample's own time;
- a small integral term learns the gyro bias while the phone turns slowly, outside the fast-convergence windows below and only from errors under 10 deg (a calibration jump of 100 deg is not gyro bias: integrating one learned about 1 deg/s of false bias);
- start: the first attitude from 5 accelerometer samples and one magnetometer sample (TRIAD); nothing is output before that. After start, a calibration change, a gap or a reacquired field the gains are 5x for 2 s.
- `pitch` = asin(up.y) (the top edge above the horizontal), `roll` = asin(-up.x) (the right edge below it): both well defined flat and in the upright handlebar pose, where Android's atan2 roll reads about +-90;
- timing: gyro samples must be strictly newer than the last; accel/magn more than 200 ms before or 2 s after the last gyro sample are dropped; a gyro gap over 250 ms is not integrated, over 2 s re-initialises. If anglvel stops (10 accel/magn samples in a row more than 2 s past the last gyro sample) the filter re-initialises once and runs on accel + magn alone, with 4x gains, until the gyro is back; `sensord` logs `heading: anglvel stopped ...` and counts `compass_gyro_lost`.

The magnetometer gate ("disturbed"): a sample is skipped when |B| is more than 25 % off its running value, the inclination more than 12 deg off, or the measured north more than 25 deg from the predicted one (a magnet can turn the field without changing |B|); three in a row mark the field disturbed and the gyro alone carries the heading. It clears after 1 s inside 12 % / 6 deg / 10 deg (0.5 s smoothed). A field that stays disturbed for 10 s but is steady for 5 s becomes the new reference (the phone moved to a new place or onto the bike). `accuracy` is sqrt(3^2 + rms innovation^2) plus drift while not corrected (0.1 deg/s plus 3 % of the angle turned); 180 when not calibrated or when the mount has no forward direction. The first innovation after start or a calibration change seeds it, so the line right after the bias is learned shows the jump (about 90 deg for a 100 deg jump) instead of 3 deg.

#### Forward vector

The heading is the azimuth of a body "forward" direction. `portrait` (default) is **up x (body +x)**: horizontal and perpendicular to the phone's x axis. For any pitch that is exactly the horizontal part of (+y - z) the plan describes (the top edge when flat, the back of the phone when upright, a handlebar mount at any pitch), but it does not move when the phone rolls about the forward axis. The literal horizontal part of (+y - z) does: upright and rolled 30 deg it is 27 deg off, which would fail the plan's tilt-invariance check (heading within +-5 deg under +-30 deg roll). The rule holds for rolls under 90 deg (face down the portrait heading is the top edge's plus 180). `landscape-left` / `landscape-right` use up x (-y) / up x (+y) (+x forward when flat with the top edge to the left); `flat` and `upright` are the horizontal parts of +y and -z. On its edge (x vertical) a portrait phone has no heading: the last one is held and `accuracy` is 180.

#### Calibrated flag

The flag in use and where it comes from (`cal_source` on every heading line and in `status`):

- **live** (primary): while heading is claimed `sensord` also runs a 5 Hz magn report at calibration `factory`. `factory` minus `full` of the same field is exactly the hard-iron bias SMGR subtracts right now (`compass_magcal` in `compass.c`: each factory sample paired with the full samples around it, interpolated, or the nearest within 30 ms). Once that difference has stayed within 0.02 G of its running mean for 3 s the state is known, and calibrated when |bias| > 0.05 G. Why 0.05 G: before learning `full` and `factory` differ by under 3 mG (2026-09-27: 1 mG at rest, factory vs raw under 3 mG); the learned bias was 0.53 to 0.55 G (both live sessions). `mag_bias` on heading lines is |bias|, `status` has the vector. A bias that moves by more than 0.05 G while calibrated makes the filter re-learn the field. Replayed on the 2026-09-27 full + factory captures (`compass-replay -F A-magn-factory.jsonl ...`): calibrated 3.0 s into each phase with (0.395, 0.241, 0.255) / (0.401, 0.229, 0.254) G, the notes' factory minus full, and no flips through the figure-8s.
- **registry** (before the live state is known, e.g. the first 3 s of a claim): group 2980's bias items (3903..3905) nonzero, checked at load (a `/run` copy reused in the same boot carries the DSP's earlier writes) and after every DSP write. It lags: live on 2026-10-01 the ADSP applied its bias about 75 s into the holds but wrote group 2980 only when the magn report was deleted, 8 minutes later, so a registry-only flag read "not calibrated" for the whole run.
- **none**: neither (no registry with `-R`).

A known live state wins over the registry (it is what SMGR applies; after an ADSP restart the registry copy may claim a bias the DSP has forgotten). The live state is forgotten when SMGR is lost. Never calibrated when `magn` is not delivered at `full` (`-c`). `sensord` logs each change: `magnetometer calibrated (source live, live): live bias 0.4010 0.2290 0.2540 gauss (factory - full, device frame, known)`. If the DSP refused the factory report (two magn reports with different calibration selects from one client; separate clients worked in the magnetometer check) the registry stays the only source.

### Replay on the 2026-09-27 captures

`tools/sensord/compass-replay` runs the same filter over recorded captures (`make -C tools/sensord compass-replay`; `compass-replay -s A-accel.jsonl A-anglvel.jsonl A-magn-full.jsonl`), and `tools/compass-check.py DIR` replays a capture directory that has no heading channel. On the magcal capture of 2026-09-27 (`~/chef-cyclo-evidence/magcal-live-20260927T084431Z/...`, `full` calibration after the DSP had learned its bias; 0 samples dropped, 0 gaps):

| Window | Heading change vs gyro yaw | max / rms error | Raw magnetometer heading (mag-cal-check) |
|---|---|---|---|
| a.turn (28.7 s, 1.3 turns) | +470.3 vs +480.6 deg | 10.6 / 6.1 deg | 13.8 / 6.8 deg |
| q.turn (28.7 s, 1.7 turns) | +620.3 vs +625.6 deg | 5.3 / 2.7 deg | 9.5 / 4.0 deg |

Still faces: heading standard deviation 0.0 to 1.2 deg per 10 s hold. On its left or right edge a portrait phone has no heading (accuracy 180). The figure-8s near the laptop were 83 % / 98 % disturbed. The q captures start during that figure-8, so the filter's first field reference was a hand-held one near the laptop: q was disturbed from its start until one reacquisition 5.2 s into the `q.up` hold (a 27 deg correction, `accuracy` 180, then 21.7, then 4.4 over 5 s); 2 disturbances and 1 reacquisition in q, 2 and 0 in a. What is left in the turns is the magnetometer's own orientation-dependent error indoors (|B| varied +-18 % around the circle): the filter smooths it but cannot remove it.

### Rotation vector comparison

With `-V`, `rotvec` carries the ADSP's own 9-axis rotation vector. The stock HAL (`sensors.ssc.so`, `RotationVector::enable`/`processInd`, disassembled) sends by default report period 0 and a sample rate in Hz << 16 plus a suspend-notification TLV, never the coordinate-system TLV, and copies the four report words unchanged into an Android rotation vector event, so they are floats x, y, z, w. `sensord` sends the same, at 20 Hz, and derives heading, pitch and roll from the quaternion with the same mount rule, reading it as body to ENU. Live 2026-10-01: `coord` 0, |q| 1.0000, and its heading agreed with ours within about 0.5 deg (mean) in calibrated still steps, so the default frame reads as ENU. Its accuracy byte tracked the ADSP's calibration (0 before, 1 to 2 once the bias was applied, 3 later); with `-V` a heading claim enables it and puts it on the heading lines as `rotvec_accuracy`, for comparison only (nothing depends on it).

### Live check

`sensors-compass-run` (like the magnetometer check; `--list` for the steps): slow face-by-face calibration holds until `sensord` reports calibrated (at most 3 rounds, with a clear message if it never does; skipped if already calibrated), four flat 90 deg clockwise turns from a reference mark, a tilt sweep (flat, up to upright and back, handlebar pose, roll left/right) pointing the same way, 10 s of shaking, and a quick 90 deg turn. It records `heading` at 25 Hz, the raw inputs, `status` every 5 s and, with `ROTVEC=1`, a `-R -i 40 -V` helper's `rotvec`, under `/run/sensortest/compass-<stamp>/`. The calibration loop waits for `sensord`'s flag (live once known) and logs its source and bias. During tilt.up, tilt.bar and tilt.roll the panel shows the live pose every 2 s next to the target (`pitch 29, aim 60`; for the roll `roll 12, aim +-30 left and right; pitch 85, aim 90 (upright)`); the roll step reads "hold it UPRIGHT in front of you like a steering wheel and tilt it left/right about 30 deg" (live 2026-10-01 the handlebar pose was held at 29 deg and the roll done at 23 deg pitch).

```sh
sensors-compass-run --list
setsid sensors-compass-run > /run/compass.log 2>&1 &     # ROTVEC=1 for the rotation vector too
tail -n 1 /run/compass.log                                 # one-line summary when done
```

On the host, `python3 tools/compass-check.py DIR --ref BEARING [--replay] [--calibrated-from STEP]` prints per-step statistics, the heading against the gyro over turns, the rotation vector difference, and the original acceptance checks: turns 90 +- 5 deg apart (with the gyro's own angle next to it, to separate the person's turning from the compass), and |turn.0 - BEARING| <= 10 deg. Tilt, shake and the quick turn are scored as heading change minus the change of a gyro-propagated reference (the attitude of the window's first heading line, propagated by the gyro, its heading taken with the same mount rule), because a hand-held phone really yaws while it tilts or shakes (live the roll step yawed 95 deg): tilt within +-5 deg in each of tilt.up, tilt.bar, tilt.roll, each scored only if the pose was reached (tilt.up pitch to >= 70, tilt.bar median pitch 45..75, tilt.roll median pitch >= 60 with |roll| to >= 20; n/a otherwise, with the pose printed); shake: standard deviation under 3 deg; quick: within 2 deg of its final value no later than 1 s after the gyro says the turn ended. `--calibrated-from STEP` treats lines from that step on as calibrated, to re-score a run whose flag was wrong (`logs/compass-live-test-2026-10-01-check.txt`: `--calibrated-from cal2.up`: turns FAIL (steps 86.7 95.4 94.4 82.0 against gyro 88.8 91.7 91.7 88.8), tilt FAIL (tilt.up -7.9 deg at pitch up to 90; tilt.bar and tilt.roll n/a, pose not reached), shake PASS (std 0.89 deg), quick FAIL (settled in 1.78 s)). A criterion whose scored windows contain heading lines flagged `"calibrated":false` is reported `NOT CALIBRATED` (a failure, the acceptance holds after the ADSP has learned its bias), with the calibrated fraction; the per-step table shows that fraction too. In the script, still steps are announced `HOLD N s:` and movement steps (tilt.up, tilt.roll, shake, quick) `DO N s:`; the long buzz starts either kind (the quick turn starts at it).

## Limits and open items

- **Magnetometer bias across reboots.** Since 2026-10-06 the learned group 2980 is saved on `/data` and seeded into the next boot's registry copy ([across boots](#across-boots)); persist is still never written. The save/restore paths have host coverage, and learn, save, reboot and seed were accepted live on 2026-10-10: the seeded bias applied from the first samples without moving the phone ([log](../../logs/mag-seed-live-2026-10-10.txt)). A seeded bias goes stale when the hard iron changes (new mount, a magnet nearby); it is then reported as calibrated until the ADSP re-learns (`SENSORS_FRESH=1` skips the overlay). Until the DSP has a bias (condition-dependent: slow, varied orientations with holds worked; a fast figure-8 alone did not), `magn` carries the uncorrected offset (about 0.4 to 0.7 G) and consumers must treat it as not calibrated.
- **No smoothing of `magn`.** The stock HAL smooths the magnetometer with an 8-sample moving average before Android sees it. `sensord` publishes raw samples; use the `heading` channel (filtered with the gyro) for a direction.
- **Compass is usable, not survey-grade (live 2026-10-01, stopped there by choice).** Run 2 (image `d1c4e041`, `sensord` `0494d086`): the data-driven `calibrated` flag came on after 6 holds from the live factory-minus-full bias; shake PASS (heading minus gyro reference std 2.2 deg) and quick turn PASS (final +0.3 deg). Known weaknesses, left open:
  - Right after the calibration flag flips, the filter stayed `disturbed` and up to 36 deg off the ADSP rotation vector through the next step (about 20 s), so the four turns FAIL (61.1 / 105.4 / 84.9 / 82.9 against gyro 92.9 / 89.7 / 89.8 / 93.0). The `|B|`/dip reference should re-seed on a calibration change.
  - About 45 % of heading lines are flagged `disturbed` indoors; the gates are conservative.
  - tilt.up scored +30 deg against the gyro reference while the ADSP rotation vector agreed with our heading (mean -0.2 deg); not diagnosed whether the reference or both fusions are off during large pitch changes.
  - The handlebar-pose and lean (roll) invariance were never exercised: held poses were 29 to 48 deg pitch, against targets of 60 and 90.
  - In still, calibrated steps our heading and the ADSP rotation vector agree within about 0.5 to 2 deg; use the rotation vector (`-V`) as a second opinion if the heading looks wrong.
  Evidence: `logs/compass-live-test-2026-10-01-*`.
- **Not in inittab yet.** Start by hand. Adding it needs an idle and streaming power measurement first.
- **No D-Bus facade.** There is no `net.hadess.SensorProxy` (iio-sensor-proxy) interface; add one only when a consumer needs it.
- The DSP's fusion and gesture algorithms (SAM: motion detect, tap, step counter) are not exposed; its rotation vector is, as a diagnostic (`-V`).
- Registry writes from the DSP other than group 2980 are lost on reboot (the `/run` copy only, by design; persist is never written).

Outstanding daemon work and optional accuracy refinements are owned by the [sensor roadmap](../next-steps/sensors.md); UI policies are owned by [UI integration](../next-steps/ui-and-ride-app.md#sensor-integration).
