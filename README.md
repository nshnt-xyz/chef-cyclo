# chef-cyclo

A non-Android Linux bike-computer OS for the Motorola One Power (`chef`, XT1942-2, SDM636).

The goal is a standalone bike computer with a Linux UI, GPS ride recording, and BLE sensors, using the phone's 5000 mAh battery and available kernel source.

## Current status

As of 2026-10-11, the phone boots standalone into a read-only Alpine root on `system_a`, with writable `/data` on `userdata` and a small stage-1 image in `boot_a`. Android is retired. Temporary `fastboot boot` images remain available for testing and rescue.

USB networking, standard NetworkManager WPA2 Wi-Fi, Bluetooth tooling, GPS fixes and on-demand GPS leases, GPS/chrony time, display/touch, side-button control and shutdown, battery policy, on-board sensors/compass and speaker playback have been verified on-device. The [LVGL/fbdev UI platform](docs/features/ui-platform.md) and SDL host build are complete; the bike-computer application, ride recorder, offline maps and real BLE sensor integration remain to be built.

[Persistent data](docs/features/storage.md) retains Bluetooth keys, Wi-Fi profiles, crash records/boot history, clock state, batched power logs and magnetometer bias. [Key-only SSH](docs/features/ssh.md) is installed for cable-free access; the latest installed pair and verification limits are recorded in the [build log](docs/build-log.md#2026-10-10-ssh-with-key-login).

## Start here

- [Build and boot](docs/building.md) — prerequisites, commands, rebuilding, and build troubleshooting.
- [Feature guides](docs/features/README.md) — use, modify, and test the working subsystems.
- [Next steps](docs/next-steps/README.md) — remaining work and dependencies.
- [Completed plans](docs/archive/README.md) — historical implementation handoffs and acceptance checklists.
- [Research records](docs/research/README.md) — completed investigations, design decisions, and experiment findings.
- [Live testing](docs/live-testing.md) — boot an image and drive the phone from scripts (`scripts/phone-boot.sh`, `scripts/phone.py`).
- [Device reference](docs/device.md) — hardware, backups, and boot behavior.
- [Build log](docs/build-log.md) — dated experiments, fixes, measurements, and verification evidence.

## Operating constraints

The installed root is read-only; updates use a matching system/boot pair ([building](docs/building.md#installed-layout-phase-3)). Keep the verified stock partition backups, especially device-unique data, and the known-good recovery images ([boot and recovery](docs/features/boot.md)). Android must never boot against our repurposed `system_a` and `userdata`.

Durable service state lives on `/data`; `/run` and other RAM-only evidence disappear on reboot, so retrieve test captures before stopping the phone. Modem EFS writes stay in RAM shadows; real EFS and `persist` must not be written.

The USB shell is passwordless root at `telnet 172.16.42.1`; scripts drive it through `scripts/phone.py` ([live testing](docs/live-testing.md)). The listener binds only to the USB address 172.16.42.1.

## Repository

- `kernel/`, `kernel-config/` — Motorola Linux 4.4.192 tree and project configuration.
- `initramfs/` — the image's overlay (init, inittab, helper scripts, config).
- `third_party/lvgl` — LVGL v9.6.0 submodule for the UI platform (`tools/chefui/`).
- `tools/`, `scripts/` — device helpers, host tests, and image builds.
- `stock/`, `logs/` — stock backups and development evidence; some artifacts are local-only.
- `docs/` — guides, plans, and history.

See the [full repository layout](docs/repository-layout.md) for individual components.
