# On-board sensor follow-ups

[Next-steps index](README.md) · [Current sensors](../features/sensors.md) · [Research](../research/sensors.md)

Raw sensors, within-boot registry reuse, magnetometer calibration, across-boot bias persistence and the tilt-compensated compass are implemented. The compass was accepted on 2026-10-01 as good enough to orient a map at a stop; the original stricter accuracy checks did not all pass.

## Boot integration and power

Measure idle current with `sensord` running without claims and with accelerometer streaming at ride rates, then decide whether `sensors-up` should run from inittab. Coordinate ADSP lifecycle with [power measurements](power-and-reliability.md#adsp-lifecycle-and-power), since audio and sensors share it.

Acceptance: boot integration preserves manual bring-up behavior, clients reconnect after daemon/DSP recovery, and measured idle/streaming cost is recorded.

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
