# chef-cyclo

A non-Android Linux bike-computer OS for the Motorola One Power (`chef`, XT1942-2, SDM636).

The goal is a standalone bike computer with a Linux UI, GPS ride recording, and BLE sensors, using the phone's 5000 mAh battery and available kernel source.

## Current status

As of 2026-09-19, the phone boots an Alpine-based Linux userspace through `fastboot boot`. USB networking, Bluetooth LE scanning and sleep, display and touch, GPS fixes through gpsd, side-button screen control and shutdown, and speaker playback through the ADSP have been verified on-device.

GPS is started manually in the baseline image. A separate temporary ride image starts GPS logging automatically. The bike-computer application, persistent storage, Wi-Fi, and standalone boot are still planned; connecting real BLE sensors remains to be tested.

## Start here

- [Build and boot](docs/building.md) — prerequisites, commands, rebuilding, and build troubleshooting.
- [Feature guides](docs/features/README.md) — use, modify, and test the working subsystems.
- [Next steps](docs/next-steps/README.md) — remaining work and dependencies.
- [Device reference](docs/device.md) — hardware, backups, and boot behavior.
- [Build log](docs/build-log.md) — dated experiments, fixes, measurements, and verification evidence.

## Operating constraints

The current workflow boots images temporarily; nothing is flashed. Stock Android remains the recovery path. Keep the verified stock partition backups, especially device-unique data.

Ride recordings live in RAM and disappear on shutdown, reboot, or battery loss. [Download them over USB](docs/features/ride-logging.md) before stopping the phone. Modem EFS writes stay in RAM shadows; real EFS and `persist` must not be written by this workflow.

The USB shell is passwordless root at `telnet 172.16.42.1`. Its current listener is not restricted to USB; network exposure must be addressed before enabling Wi-Fi.

## Repository

- `kernel/`, `kernel-config/` — Motorola Linux 4.4.192 tree and project configuration.
- `initramfs/`, `initramfs-ride/` — baseline and temporary ride overlays.
- `tools/`, `scripts/` — device helpers, host tests, and image builds.
- `stock/`, `logs/` — stock backups and development evidence; some artifacts are local-only.
- `docs/` — guides, plans, and history.

See the [full repository layout](docs/repository-layout.md) for individual components.
