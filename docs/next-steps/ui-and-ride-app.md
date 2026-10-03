# UI and ride application

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Button integration

Use [buttond](../features/buttons-and-power-off.md) rather than opening the key evdev nodes in the app. Proposed mapping: recorder claims `power.long` to finish a ride, UI claims `power.double` for pause and volume gestures for page/zoom. Decide whether the recorder offers power-off after finishing: its long-press claim disables the default shutdown while held. Use `power+volup` for a software power menu and retain the hardware escape through Power+VolDown.

Acceptance: claims release on process death, UI restarts do not interrupt the recorder, and the user can still finish a ride and shut down cleanly.

## UI stack

Decided 2026-10-03: **LVGL directly on the existing MDSS fbdev**, with touch read from evdev and the side buttons through `buttond`. No kernel change, no compositor and no third-party applications. Fullscreen applications (ride UI, later settings) hand the screen over through the fb0 lock instead of compositing. A [prototype](../research/lvgl-fbdev-prototype.md) verified colours, tear-free double buffering, touch mapping, rapid taps, brightness and screen off/on through buttond live on the baseline kernel: 0.2% CPU on a static page, 5.8% for small 60 fps animations.

This replaces the 2026-10-02 direction of DRM/KMS, a Wayland compositor and libinput. That direction would have needed SDE 3.2/14 nm PHY/panel enablement on 4.4 or a mainline board port, and would only have added GPU acceleration (mainline plus Mesa) and composition the project does not need. The [display modernization](../research/display-modernization.md), [DRM bring-up](../research/drm-bringup.md) and [loader budget](../research/chef-loader-kernel-budget.md) records remain as historical evidence. Kernel commit `64fa801` (DRM helper namespace separation) only affects DRM builds and is harmless to the baseline.

The current phase is the shared platform layer: display backend, own multitouch reader, buttond client, screen-state/handoff protocol, host SDL simulator and a demo application. See [UI platform](ui-platform.md) for the specification and acceptance checks. Touch requirements from the earlier plan carry over there: discovery by capability, scaling from the MT ABS ranges, slot lifecycle, dropped-event recovery, sleep/wake and consistent rotation. Physical gloves/rain behavior is still unmeasured.

Application work on top of the platform: define one normal screen policy (the foreground application owns screen power and brightness requests; `fblog` remains the diagnostic fallback and must not wake a screen deliberately left off). A UI restart must not interrupt recording.

Provide a **sunlight and gloves** mode: maximum backlight on request, a high-contrast, big-digit layout, and every riding action reachable from the side buttons because touch through gloves or rain is unreliable. Define always-on versus button-wake policy, manual brightness override, and ALS-driven brightness. Verify sleep/wake, process death, fallback, and clean shutdown before adding the full ride application.

## Bike-computer application

Inputs: gpsd ([GPS manager](gps-and-time.md#gps-manager)) for position/speed/track/altitude, BLE sensors through BlueZ ([BLE sensors](connectivity-and-sensors.md#ble-sensors): HR strap, speed/cadence, power), battery (`power_supply/battery`), side buttons ([button integration](ui-and-ride-app.md#button-integration)), touch ([UI stack](ui-and-ride-app.md#ui-stack)). Outputs: the panel through the chosen [UI stack](#ui-stack), vibrator/speaker ([audio](connectivity-and-sensors.md#audio)) for alerts.

Core: ride recording (GPX/FIT to [persistent storage](storage-and-boot.md#persistent-storage)), live data pages (speed, distance, time, HR, cadence, climb), lap/auto-pause, route following later. Housekeeping: screen on/off, GPS leases and idle power, clean shutdown, upload over Wi-Fi ([Wi-Fi](connectivity-and-sensors.md#wi-fi)).

Opening the map acquires a temporary `map` lease from the GPS manager; leaving it releases that lease. Starting a ride starts or hands off to a recorder that owns a durable `ride` lease until the ride is explicitly ended, independent of the visible page and UI process lifetime. The modem shuts down after the manager's grace period only when neither lease remains.

Order: (a) **UI first** — the data pages on the panel with touch/buttons on the chosen UI stack, driven by gpsd and the sensors, ride recording underneath; (b) **then a map source** — offline tiles or vector data on the writable storage ([persistent storage](storage-and-boot.md#persistent-storage): OSM extracts, e.g. MBTiles/PMTiles or a raster tile cache pre-fetched over Wi-Fi/USB), rendered under the position with track-up rotation, then route following on top; no online map dependency on a ride; GPS altitude is noisy and there is no barometer, so take **elevation** from a DEM shipped with the map data (climb totals, gradient).

**Time zone**: chrony ([time synchronization](gps-and-time.md#time-synchronization)) gives UTC; the UI needs local time (tzdata or a fixed `TZ`). Written as an ordinary Linux app against gpsd/BlueZ and the chosen toolkit/input stack so it is testable on the host with replayed `nmea.log` from the ride runs.

## Sensor integration

Use the [sensord contract](../features/sensors.md) for raw sensors and magnetic heading. The UI owns ALS-driven brightness, including manual override and sunlight mode; the display owner applies backlight changes through the chosen display backend, preserving the framebuffer commit contract while the fbdev path remains in use.

For maps, show calibration/disturbance state and use the accepted compass behavior when stopped. Blend with GPS course over ground when moving, and compute declination from GPS/WMM on the consumer side before setting true heading. Validate switching at low speeds, disturbed or uncalibrated input, and missing GPS. Daemon accuracy refinements remain in the [sensor roadmap](sensors.md#optional-compass-refinements).

## GPU

Not planned for the LVGL platform, which renders on the CPU. Record kept for a possible later mainline route. The Adreno 509 currently uses the downstream `kgsl` driver and is unused by our display clients. As part of the kernel investigation, establish the exact GPU revision, DRM `msm`/Mesa freedreno compatibility, required firmware, and buffer-sharing support with the display driver. Do not assume KMS scanout also enables GPU rendering, or that Mesa can use the existing KGSL interface unchanged.

Once software-rendered DRM/KMS works, bring up Mesa/EGL acceleration and verify actual hardware rendering, compositor buffer import/presentation, and recovery across screen cycles. Compare CPU usage, frame latency and power on data pages and rotating maps. Record software-rendering fallback behavior. Libhybris over stock Android GLES blobs may be investigated if the native route is blocked, but is a separate compatibility approach, not proof of native DRM/Mesa support.
