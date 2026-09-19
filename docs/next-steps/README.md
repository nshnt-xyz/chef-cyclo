# Next steps

[Project overview](../../README.md) · [Current feature guides](../features/README.md) · [Build log](../build-log.md)

This directory contains unfinished work. Completed bring-up and operating recipes live in the feature guides; dated results remain in the build log. The grouping below preserves the existing roadmap without treating every dependency as a strict serial schedule.

## Work areas

| Plan | Remaining work and dependencies |
|---|---|
| [GPS and time](gps-and-time.md) | Demand-driven manager and clients next; chrony needs SHM support or another handoff. Assistance persistence needs storage, downloads need connectivity. |
| [Storage and boot](storage-and-boot.md) | Storage enables recordings, maps, credentials, and retained state. Standalone boot needs a verified recovery plan. Android retirement comes later. |
| [UI and ride app](ui-and-ride-app.md) | Choose a UI stack, integrate buttons/GPS/sensors, build pages and recording, then offline maps/routes. Durable recordings require storage. GPU is optional. |
| [Connectivity and sensors](connectivity-and-sensors.md) | Real BLE sensors, Wi-Fi, audio, Strava, and optional SLPI. Wi-Fi requires fixing shell exposure; Strava needs recording, storage, and connectivity. |
| [Power and reliability](power-and-reliability.md) | Validate battery/charging, measure idle/suspend, add low-battery handling and recovery. Crash recovery depends on standalone boot and persistent evidence. |

## How to update a plan

Keep implementation choices, prerequisites, unresolved decisions, and acceptance checks with their work area. Link to another plan instead of referring to numbered backlog items. When work lands, move the current operating contract into its feature guide and append verification evidence to the build log; leave only remaining work here.

Firmware redistribution considerations are tracked in [building](../building.md#firmware-and-distribution).
