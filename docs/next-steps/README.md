# Next steps

[Project overview](../../README.md) · [Current feature guides](../features/README.md) · [Completed plans](../archive/README.md) · [Build log](../build-log.md)

Reviewed against the code and recorded acceptance through 2026-10-11. This directory contains remaining work. Completed operating recipes live in feature guides, original implementation handoffs in the archive, and investigations in [research](../research/README.md).

## Work areas

| Plan | Remaining work and dependencies |
|---|---|
| [GPS and time](gps-and-time.md) | UI/recorder clients, modem-restart validation, GNSS shadow attribution/persistence and assistance injection; optional USB-host time source. GPS manager, GPS time, retained RTC offset and chrony drift already work. |
| [Storage and boot](storage-and-boot.md) | Charger mode, test-image slot marking, durable ride/map/GNSS consumers and optional root integrity. Standalone boot, `/data`, read-only `system_a`, state persistence and orderly return to fastboot are implemented and installed. |
| [UI and ride app](ui-and-ride-app.md) | System screens from the round-1 Claude Design (settings, Wi-Fi, Bluetooth, GPS, status bar), data pages, GPS/sensor integration, ride recording, screen policy, offline maps/routes and alerts. The LVGL/fbdev platform and SDL host build are complete. Storage is available. |
| [Connectivity and sensors](connectivity-and-sensors.md) | Real BLE/classic/mesh devices, PAN DHCP/NAP guards, Wi-Fi UI/power measurements, audio alerts and Strava. Bluetooth keys and Wi-Fi profiles already persist; SSH is installed. |
| [On-board sensors](sensors.md) | Boot/power integration, wake/tap, optional compass refinements and desktop facade. Raw sensors, stopped-map compass and retained magnetometer bias already work. |
| [Power and reliability](power-and-reliability.md) | Charging/drain validation, idle/suspend and shared ADSP lifecycle, watchdog/reboot supervision and recorder recovery. Persistent crash evidence and batched power logs are implemented; the Bluetooth shutdown hang is fixed. |

## How to update a plan

Keep implementation choices, prerequisites, unresolved decisions, and acceptance checks with their work area. Link to another plan instead of duplicating its task. When work lands, move the current operating contract into its feature guide and append verification evidence to the build log; leave only remaining work here. Preserve completed plans under `docs/archive/` and investigations under `docs/research/`, clearly labelled historical. Give each task one roadmap owner; feature guides may state limitations and link to that task.

Firmware redistribution considerations are tracked in [building](../building.md#firmware-and-distribution).
