# chef-cyclo

A non-Android Linux bike-computer OS for the Motorola One Power (`chef`, XT1942-2, SDM636).

The goal is a standalone bike computer with a Linux UI, GPS ride recording, and BLE sensors, using the phone's 5000 mAh battery and available kernel source.

## Current status

As of 2026-10-02, the phone boots an Alpine-based Linux userspace through `fastboot boot`. USB networking, Bluetooth LE scanning and sleep, display and touch, GPS fixes through gpsd, side-button screen control and shutdown, speaker playback through the ADSP, and standard NetworkManager WPA2 Wi-Fi with Internet/DNS connectivity have been verified on-device.

The baseline starts one shared resident modem support stack and uses standard
[NetworkManager Wi-Fi tools](docs/features/wifi-ui.md). Clean baseline startup,
automatic chrony time synchronization, verified HTTPS and signed apk tool
installation are verified. Profiles and credentials stay in RAM. The temporary
ride variant is excluded from this final validation at the user's request and
retained pending planned removal. The bike-computer application, persistent
storage and standalone boot remain planned; real BLE sensors remain untested.

## Start here

- [Build and boot](docs/building.md) — prerequisites, commands, rebuilding, and build troubleshooting.
- [Feature guides](docs/features/README.md) — use, modify, and test the working subsystems.
- [Next steps](docs/next-steps/README.md) — remaining work and dependencies.
- [Research records](docs/research/README.md) — completed investigations, design decisions, and experiment findings.
- [Device reference](docs/device.md) — hardware, backups, and boot behavior.
- [Build log](docs/build-log.md) — dated experiments, fixes, measurements, and verification evidence.

## Operating constraints

The current workflow boots images temporarily; nothing is flashed. Stock Android remains the recovery path. Keep the verified stock partition backups, especially device-unique data.

Ride recordings live in RAM and disappear on shutdown, reboot, or battery loss. [Download them over USB](docs/features/ride-logging.md) before stopping the phone. Modem EFS writes stay in RAM shadows; real EFS and `persist` must not be written by this workflow.

The USB shell is passwordless root at `telnet 172.16.42.1`. The listener binds only to the USB address 172.16.42.1.

## Repository

- `kernel/`, `kernel-config/` — Motorola Linux 4.4.192 tree and project configuration.
- `initramfs/`, `initramfs-ride/` — baseline and temporary ride overlays.
- `tools/`, `scripts/` — device helpers, host tests, and image builds.
- `stock/`, `logs/` — stock backups and development evidence; some artifacts are local-only.
- `docs/` — guides, plans, and history.

See the [full repository layout](docs/repository-layout.md) for individual components.
