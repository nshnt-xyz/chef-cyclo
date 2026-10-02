# Next steps

[Project overview](../../README.md) · [Current feature guides](../features/README.md) · [Build log](../build-log.md)

This directory contains unfinished work. Completed bring-up and operating recipes live in the feature guides; completed investigations live in [research](../research/README.md), and dated results remain in the build log. The grouping below preserves the existing roadmap without treating every dependency as a strict serial schedule.

## Work areas

| Plan | Remaining work and dependencies |
|---|---|
| [GPS and time](gps-and-time.md) | Demand-driven manager and clients next; chrony needs SHM support or another handoff. Assistance persistence needs storage, downloads need connectivity. |
| [Storage and boot](storage-and-boot.md) | Storage enables recordings, maps, credentials, and retained state. Standalone boot needs a verified recovery plan. Android retirement comes later. |
| [UI and ride app](ui-and-ride-app.md) | Bring up native DRM/KMS and Wayland; validate standard libinput touch (initial discovery verified), then integrate buttons/GPS/sensors, pages and recording, and offline maps/routes. Durable recordings require storage; GPU acceleration is a separate milestone. |
| [Connectivity and sensors](connectivity-and-sensors.md) | Real BLE sensors, remaining Bluetooth tooling (persistence, real classic and mesh devices, PAN DHCP/NAP), Wi-Fi lifecycle/UI/power integration, audio alerts, and Strava. Manual Wi-Fi and safe listener bindings are verified; Strava still needs recording, storage, and upload integration. Links to the separate on-board sensor roadmap. |
| [On-board sensors](sensors.md) | Boot/power integration, calibration persistence, wake/tap, optional compass accuracy improvements and desktop facade. Raw sensors and stopped-map compass already work. |
| [Power and reliability](power-and-reliability.md) | Finish charging/drain validation, measure idle/suspend and shared ADSP lifecycle, and add crash recovery. Crash recovery depends on standalone boot and persistent evidence. |

## How to update a plan

Keep implementation choices, prerequisites, unresolved decisions, and acceptance checks with their work area. Link to another plan instead of referring to numbered backlog items. When work lands, move the current operating contract into its feature guide and append verification evidence to the build log; leave only remaining work here. Preserve completed research and original experiment designs under `docs/research/`, labelled historical. Give each task one roadmap owner; feature guides may state limitations and link to that task.

Firmware redistribution considerations are tracked in [building](../building.md#firmware-and-distribution).
