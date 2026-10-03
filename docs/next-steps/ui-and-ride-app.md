# UI and ride application

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Button integration

Use [buttond](../features/buttons-and-power-off.md) rather than opening the key evdev nodes in the app. Proposed mapping: recorder claims `power.long` to finish a ride, UI claims `power.double` for pause and volume gestures for page/zoom. Decide whether the recorder offers power-off after finishing: its long-press claim disables the default shutdown while held. Use `power+volup` for a software power menu and retain the hardware escape through Power+VolDown.

Acceptance: claims release on process death, UI restarts do not interrupt the recorder, and the user can still finish a ride and shut down cleanly. Decide how the future recorder supersedes the temporary ride image, which currently has no button daemon.

## Display and touch modernization

Direction agreed on 2026-10-02: modernize both the display and touch paths. Target a supported DRM/KMS display driver, a current Wayland compositor, and libinput for touchscreen handling. A polished framebuffer application alone does not complete this work. This is a hardware enablement project; support for this phone's exact panel and replacement touchscreen must be established before choosing a kernel or compositor.

Initial support findings and evidence are in the [display modernization investigation](../research/display-modernization.md). The [DRM bring-up record](../research/drm-bringup.md) includes successful isolated built-in compilation and runtime isolation. The [actual bootloader budget investigation](../research/chef-loader-kernel-budget.md) identified a space constraint strongly supported by successful DRM-core and full DRM boots after reencoding the unchanged userspace to smaller LZMA archives. DRM initializes, but no DRM card or native scanout exists yet. Next establish SDE3.2/14nm PHY/panel support and the touch power lifecycle before transferring display ownership. Standard touch discovery already works on the current kernel.

### Display investigation and bring-up

The current downstream 4.4 kernel exposes the panel through `mdss_fb` and the GPU through `kgsl`; the working userspace path is [fbdev.h](../../tools/fbdev.h). Investigate a newer kernel with DRM `msm` support for SDM636 and the board, versus a scoped DRM/KMS port to the existing kernel. Inventory DSI host/PHY, panel initialization and timings, regulators, clocks, reset GPIOs, WLED/backlight, boot splash handoff, and the touchscreen's shared panel rails. Record available upstream support, missing drivers/board descriptions, and effects on working modem, GPS, Wi-Fi, Bluetooth, audio and power features. Choose the kernel approach from that evidence, rather than assuming that a generic SoC driver enables the complete phone.

Bring up software-rendered KMS scanout first: identify the connector/mode, allocate a supported scanout buffer, show a test pattern, and verify frame updates/page flips. DRM/KMS display support and GPU acceleration are separate milestones. Check atomic modesetting support and compositor requirements explicitly. Use temporary boot images and retain the verified baseline and stock recovery path.

Acceptance: correct 1080x2246 output and pixel format, stable repeated updates, clean boot handoff, brightness changes, repeated screen-off/on and reopen, and no panel or touch I2C faults. Measure presentation latency, CPU usage and power for static data pages and representative map redraws. Compare with the existing baseline before replacing it.

### Standard Linux touch path

Initial discovery is verified on the 2026-10-02 temporary baseline: input-only coldplug supplies standard classification, libinput 1.31.3 recognizes the ten-contact touchscreen, and cached inventory works with the panel off. See [results and evidence](../research/display-modernization.md#initial-image-results). Physical touch mapping, dropped-event recovery and compositor integration remain pending.

The NT36xxx driver already exposes evdev multitouch; [fbtouch.c](../../tools/fbtouch.c) is the working reference. Integrate libinput with correct eudev touchscreen classification and discovery by identity/capabilities, without hardcoding `event1`. Validate the driver's ABS ranges, multitouch slots, contact lifecycle, and recovery after dropped events and panel sleep. Keep power/volume input owned by `buttond` and its socket contract.

The touch surface reports 720x1600 while the panel is 1080x2246. Normalize from the reported ABS ranges and map to the intended output; the resolution difference alone does not require calibration. Add a calibration matrix only if measured alignment or orientation needs correction. Keep display rotation and touch mapping consistent. The [ArchWiki touchscreen guide](https://wiki.archlinux.org/title/Touchscreen) is a setup reference; verify behavior against the chosen libinput/compositor versions.

Acceptance: taps at all corners and center, strokes, three simultaneous contacts, release/cancel without stuck touches, correct portrait/landscape mapping, and repeated sleep/wake. Test with libinput diagnostics and then through the actual compositor/toolkit; raw evdev success alone is insufficient.

## UI stack

After DRM/KMS and libinput bring-up, spike a current Wayland compositor and a maintained application toolkit. Validate software rendering first, then accelerated rendering when available. Choose based on startup reliability, touch behavior, memory, rendering latency, power, host development/replay support, and the later offline-map requirements. Pin tested versions and document device permissions and compositor/session supervision.

Define one normal display owner: the compositor/session coordinates presentation, screen power and backlight; the UI supplies screen policy and brightness requests. Replace `buttond`'s current `fblog.off`/restart coupling with a display-owner interface. Preserve a diagnostic fallback, but ensure it cannot draw concurrently or wake a screen deliberately left off. A UI restart must not interrupt recording; a compositor restart must restore display and input predictably.

During investigation, retain `fblog`/`fbtouch` and the binding [framebuffer contract](../features/display-and-touch.md#framebuffer-and-touch-contract) for the existing fbdev path. Any temporary Xorg/fbdev or direct-toolkit spike must adapt and verify locking, frame commits, backlight application, and blank-before-close behavior; generic fbdev support does not prove compatibility. Those experiments are interim options, not the modernization target. A DRM path needs its own verified power/touch lifecycle rather than blindly copying fbdev ioctls.

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

The Adreno 509 currently uses the downstream `kgsl` driver and is unused by our display clients. As part of the kernel investigation, establish the exact GPU revision, DRM `msm`/Mesa freedreno compatibility, required firmware, and buffer-sharing support with the display driver. Do not assume KMS scanout also enables GPU rendering, or that Mesa can use the existing KGSL interface unchanged.

Once software-rendered DRM/KMS works, bring up Mesa/EGL acceleration and verify actual hardware rendering, compositor buffer import/presentation, and recovery across screen cycles. Compare CPU usage, frame latency and power on data pages and rotating maps. Record software-rendering fallback behavior. Libhybris over stock Android GLES blobs may be investigated if the native route is blocked, but is a separate compatibility approach, not proof of native DRM/Mesa support.
