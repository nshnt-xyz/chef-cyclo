# UI and ride application

[Next-steps index](README.md) · [Current features](../features/README.md)

Remaining integration and validation work, reviewed 2026-10-11. Linked feature guides describe implemented behavior; candidate approaches must be checked when implementing.

## Button integration

Use [buttond](../features/buttons-and-power-off.md) rather than opening the key evdev nodes in the app. Proposed mapping: recorder claims `power.long` to finish a ride, UI claims `power.double` for pause and volume gestures for page/zoom. Decide whether the recorder offers power-off after finishing: its long-press claim disables the default shutdown while held. Use `power+volup` for a software power menu and retain the hardware escape through Power+VolDown.

Acceptance: claims release on process death, UI restarts do not interrupt the recorder, and the user can still finish a ride and shut down cleanly.

## UI stack

Decided 2026-10-03: **LVGL directly on the existing MDSS fbdev**, with touch read from evdev and the side buttons through `buttond`. No kernel change, no compositor and no third-party applications. Fullscreen applications (ride UI, later settings) hand the screen over through the fb0 lock instead of compositing. A [prototype](../research/lvgl-fbdev-prototype.md) verified colours, tear-free double buffering, touch mapping, rapid taps, brightness and screen off/on through buttond live on the baseline kernel: 0.2% CPU on a static page, 5.8% for small 60 fps animations.

This replaces the 2026-10-02 direction of DRM/KMS, a Wayland compositor and libinput. That direction would have needed SDE 3.2/14 nm PHY/panel enablement on 4.4 or a mainline board port, and would only have added GPU acceleration (mainline plus Mesa) and composition the project does not need. The [display modernization](../research/display-modernization.md), [DRM bring-up](../research/drm-bringup.md) and [loader budget](../research/chef-loader-kernel-budget.md) records remain as historical evidence. Kernel commit `64fa801` (DRM helper namespace separation) only affects DRM builds and is harmless to the baseline.

The shared platform layer is implemented and live accepted: display backend, own multitouch reader, buttond client, screen-state/handoff protocol, host SDL simulator and a demo application. Use the [UI platform guide](../features/ui-platform.md) for the application contract; the [archived design](../archive/ui-platform.md) retains acceptance checks. Touch requirements from the earlier plan carry over there: discovery by capability, scaling from the MT ABS ranges, slot lifecycle, dropped-event recovery, sleep/wake and consistent rotation. Physical gloves/rain behavior is still unmeasured.

Application work on top of the platform: define one normal screen policy (the foreground application owns screen power and brightness requests; `fblog` remains the diagnostic fallback and must not wake a screen deliberately left off). A UI restart must not interrupt recording.

Provide a **sunlight and gloves** mode: maximum backlight on request, a high-contrast, big-digit layout, and every riding action reachable from the side buttons because touch through gloves or rain is unreliable. Define always-on versus button-wake policy, manual brightness override, and ALS-driven brightness. Verify sleep/wake, process death, fallback, and clean shutdown before adding the full ride application.

## System UI design

Round 1 of the system screens was designed in Claude Design on 2026-10-08, in the project **Bike Computer OS Design** (`f6f205a3-9d48-47e0-a4b1-ece4685fa800`), file `Bike OS System Screens.dc.html`. It uses the **Industry** design system (`45761091-7d93-4f50-9583-93d6f266c339`) for its palette and Barlow type. The project's `github.md` maps screens to this repo: status bar and safe area to the [UI platform](../features/ui-platform.md), Wi-Fi to [Wi-Fi UI](../features/wifi-ui.md), and Bluetooth and GPS to this plan. The design is HTML/CSS for review only. Nothing is generated for LVGL, so every screen is rebuilt by hand on chefui.

Screens: 18 portrait screens (P01 settings hub, P02 display, P03 sound and haptics, P04 time and date, P05 storage, P06 about, P07–P10 Bluetooth off/list/pairing/sensor detail, P11–P14 GPS off/acquiring/locked/satellites, P15–P18 Wi-Fi off/list/password/details) and 4 landscape screens (L01 hub, L02 GPS locked, L03 Bluetooth, L04 Wi-Fi). Each comes in dark and light, plus the status bar states.

Design contract, in device pixels:

- Canvas 1080×2246 portrait, 2246×1080 landscape, the real panel size. Content starts at the 96 px [safe top](../features/ui-platform.md#content-clearance-and-full-panel-access); landscape has no inset.
- Status bar: 96 px high, drawn in the two ears beside the 556 px notch (about 238 px each, contents kept to about 190 px, at least 40 px clear of the notch). The left ear shows GPS, Bluetooth, Wi-Fi and temperature. The right ear shows the battery, with its percentage drawn inside the outline, and the time. A muted icon means searching or off. The warning colour is used only for low battery (≤ 15 % and not charging). The sensor count shows on the hub tile instead of the bar.
- Gloves: the smallest target is 192 px (about 12 mm). List rows are 216 px, master toggles 240 px, switches 204×108 px.
- Type for `lv_font_conv`: Barlow Condensed 600 at 40/48/72/96/150/280, Barlow 400–500 at 34/42/48/56/64, with tabular numerals.
- Shapes: 36–40 px corners on cards and buttons, 24 px on keys, pill-shaped switches and satellite chips, 3 px lines, "+" registration marks on key cards. Accent fill is used only for the active state.
- Palette: dark uses bg `#111213`, surface `#1d1f20`, fg `#f2f2f3`, muted `#98989b`, lines `#2b2b2d`/`#5d5d60`, accent `#94bce3` (text `#b5d9fd`, on-accent `#1d2d3d`), warn `oklch(0.78 0.11 60)`. Light uses bg `#f2f2f3`, surface `#e7e7ea`, fg `#1d1f20`, muted `#5d5d60`, lines `#d4d4d7`/`#98989b`, accent `#5980a6` (text `#416180`, on-accent `#f5f5f8`), warn `oklch(0.52 0.13 50)`. Convert the oklch values to sRGB hex for LVGL.
- Icons are Lucide (`lucide-static@0.460.0`): locate/locate-fixed/locate-off, bluetooth/-connected/-off, wifi/wifi-off and zap. Ship them as an `lv_font_conv` icon font built from the Lucide TTF, or as pre-rendered A8 images.

Pulling the design: in Claude Code, `DesignSync` `list_files`/`get_file` on the project id reads the `.dc.html` sources (`Bike OS System Screens.dc.html` includes `Screens.dc.html` and `StatusBar.dc.html`). After each design round, re-read `github.md` and the changed screens rather than copying the whole project.

Translation plan:

1. A token header for chefui: dark and light palettes as `lv_color_hex` constants, spacing, radii, line widths and target sizes, with the theme selectable at runtime.
2. Barlow and Barlow Condensed fonts at the listed sizes, plus the icon font, generated by `lv_font_conv` and committed with the generation command.
3. Reference renders: each screen rendered headless at 1:1 (1080×2246) to a PNG, kept outside git or in a small reference directory, and compared side by side with an fb0 capture of the LVGL build and with the SDL host build.
4. The shared status bar widget first (it sits on every page), then the hub, then the Wi-Fi, Bluetooth and GPS screens backed by the existing NM, BlueZ and GPS manager interfaces, then the remaining settings pages.

Open decisions:

- **Power short press.** The design makes a short Power press go Back on every page except the hub. Today `power.short` toggles the screen (buttond default, or a chefui claim). Decide how screen off works in menus (for example, short press on the hub or a timeout) and update the [button integration](#button-integration) mapping to match.
- **Dark and light.** Decide whether light is the [sunlight mode](#ui-stack), chosen manually, or ALS-driven.
- The design keeps the volume rocker for ride pages only. Confirm that against the button mapping above.

## Bike-computer application

Inputs: gpsd ([GPS manager](../features/gps.md#use-gnss-gps-manager), [client integration](gps-and-time.md#client-integration)) for position/speed/track/altitude, BLE sensors through BlueZ ([BLE sensors](connectivity-and-sensors.md#ble-sensors): HR strap, speed/cadence, power), battery (`power_supply/battery`), side buttons ([button integration](ui-and-ride-app.md#button-integration)), touch ([UI stack](ui-and-ride-app.md#ui-stack)). Outputs: the panel through the chosen [UI stack](#ui-stack), vibrator/speaker ([audio](connectivity-and-sensors.md#audio)) for alerts.

Core: ride recording (GPX/FIT to [persistent storage](../features/storage.md)), live data pages (speed, distance, time, HR, cadence, climb), lap/auto-pause, route following later. Housekeeping: screen on/off, GPS leases and idle power, clean shutdown, upload over Wi-Fi ([Wi-Fi](connectivity-and-sensors.md#wi-fi)).

Opening the map acquires a temporary `map` lease from the GPS manager; leaving it releases that lease. Starting a ride starts or hands off to a recorder that owns a durable `ride` lease until the ride is explicitly ended, independent of the visible page and UI process lifetime. The modem shuts down after the manager's grace period only when neither lease remains.

Order: (a) **UI first** — the data pages on the panel with touch/buttons on the chosen UI stack, driven by gpsd and the sensors, ride recording underneath; (b) **then a map source** — offline tiles or vector data on the writable storage ([persistent storage](../features/storage.md): OSM extracts, e.g. MBTiles/PMTiles or a raster tile cache pre-fetched over Wi-Fi/USB), rendered under the position with track-up rotation, then route following on top; no online map dependency on a ride; GPS altitude is noisy and there is no barometer, so take **elevation** from a DEM shipped with the map data (climb totals, gradient).

**Time zone**: chrony ([time synchronization](gps-and-time.md#time-synchronization)) gives UTC; the UI needs local time (tzdata or a fixed `TZ`). Written as an ordinary Linux app against gpsd/BlueZ and the chosen toolkit/input stack so it is testable on the host with replayed `nmea.log` from the ride runs.

## Sensor integration

Use the [sensord contract](../features/sensors.md) for raw sensors and magnetic heading. The UI owns ALS-driven brightness, including manual override and sunlight mode; the display owner applies backlight changes through the chosen display backend, preserving the framebuffer commit contract while the fbdev path remains in use.

For maps, show calibration/disturbance state and use the accepted compass behavior when stopped. Blend with GPS course over ground when moving, and compute declination from GPS/WMM on the consumer side before setting true heading. Validate switching at low speeds, disturbed or uncalibrated input, and missing GPS. Daemon accuracy refinements remain in the [sensor roadmap](sensors.md#optional-compass-refinements).

GPU acceleration and the shelved DRM/Wayland route are historical investigations, retained in [display research](../research/display-modernization.md) and [DRM bring-up](../research/drm-bringup.md). They are outside the current LVGL application roadmap.
