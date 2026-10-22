# On-board sensor follow-ups

[Next-steps index](README.md) · [Current sensors](../features/sensors.md) · [Research](../research/sensors.md)

Raw sensors, within-boot registry reuse, magnetometer calibration, across-boot bias persistence and the tilt-compensated compass are implemented. The compass was accepted on 2026-10-01 as good enough to orient a map at a stop; the original stricter accuracy checks did not all pass.

## Boot integration and power

Done 2026-10-16: `sensors-up` ran from inittab until 2026-10-21 (respawn through a 10 s wrapper, `flock` lock, orphaned sensord handling); since then the OpenRC services `sensord` and `sensors-up` start the sensors ([sensors](../features/sensors.md#run), [services](../features/services.md)). Phase A measured with the panel off, Wi-Fi radio off and a cooling battery: ADSP booted plus `sensord` without claims adds 0 to +2 mA (idle 195.3 mA against 193.6 mA in the last base block), and an `illuminance` claim at 5 Hz adds +0.4 mA, below the block noise of 11 to 22 mA ([evidence](../../logs/system-ui-slice2-2026-10-11.txt)). The UI reconnects after a daemon restart (live: claim back 0.54 s after the restart). Still to measure when a ride consumer exists: accelerometer streaming at ride rates. Coordinate ADSP lifecycle with [power measurements](power-and-reliability.md#adsp-lifecycle-and-power).

The ambient light report is on-change only (at steady light gaps reached 55 s at a 5 Hz claim; the 100 s seen once was the age of the cached first line of a claim), so a consumer holds the last value and checks liveness with `status`, never by waiting for samples; the system UI does exactly that ([wake grace and probe](../features/system-ui.md#ambient-light-m5)). The sensord pid-file window (a `kill -9` of `sensors-up` just after it started sensord left an orphan its pid file did not name) is gone since 2026-10-21: sensord is its own supervised OpenRC service, so supervise-daemon holds its pid from the fork, and `sensors-up` keeps no pid file and no orphan handling ([services](../features/services.md)). Open: the older guided runs (magnetometer calibration and compass scripts) still pace by buzz cues and chat; move them to the on-screen prompt card (`/run/chef/chef-settings/prompt`, see [guided prompts](../features/system-ui.md#guided-prompts-testing)).

## Calibration across boots

Completed: `chef-state` saves only magnetometer hard-iron group 2980 to `/data`; `sensors-up` overlays an accepted saved group onto the RAM registry copy without writing stock persist. Learning, saving, rebooting and applying the bias from the first samples were accepted live on 2026-10-10 ([evidence](../../logs/mag-seed-live-2026-10-10.txt), [current contract](../features/storage.md#persistent-state)). A changed magnetic environment still needs re-learning. No full-registry persistence task remains planned.

## Wake-on-motion and tap

Expose DSP SAM AMD (0x104) and TAP (0x11a) as claimable event channels; their IDL is already recovered. Verify event delivery, release on disconnect, and wake behavior before using them for screen policy.

## Optional compass refinements

These are optional accuracy improvements, not blockers for the accepted stopped-map use:

- Re-seed the magnetic-field magnitude/dip reference and gates when calibration changes.
- Retune disturbance gates (45% of run-2 lines were disturbed indoors).
- Diagnose the tilt.up discrepancy against the gyro reference.
- Test lean about the forward horizontal axis at a 30–50° handlebar pitch.

Use the [recorded acceptance checks and results](../research/sensors.md#8-tilt-compensated-compass-plan-2026-09-27) when revisiting accuracy. GPS-course blending and declination belong to [UI integration](ui-and-ride-app.md#sensor-integration).

## Optional desktop interface

Add a `net.hadess.SensorProxy` D-Bus facade only when an off-the-shelf consumer needs it. Keep existing socket clients and IIO names/units compatible.
