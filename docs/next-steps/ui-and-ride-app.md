# UI and ride application

[Next-steps index](README.md) · [Current features](../features/README.md)

Remaining integration and validation work, updated 2026-10-17 with ride app design rounds 1 and 2. Linked feature guides describe implemented behavior; candidate approaches must be checked when implementing.

## Button integration

Use [buttond](../features/buttons-and-power-off.md) rather than opening the key evdev nodes in the app. Proposed mapping: recorder claims `power.long` to finish a ride, UI claims `power.double` for pause and volume gestures for page/zoom. Decide whether the recorder offers power-off after finishing: its long-press claim disables the default shutdown while held. Use `power+volup` for a software power menu and retain the hardware escape through Power+VolDown.

Acceptance: claims release on process death, UI restarts do not interrupt the recorder, and the user can still finish a ride and shut down cleanly.

## UI stack

Decided 2026-10-03: **LVGL directly on the existing MDSS fbdev**, with touch read from evdev and the side buttons through `buttond`. No kernel change, no compositor and no third-party applications. Fullscreen applications (ride UI, later settings) hand the screen over through the fb0 lock instead of compositing. A [prototype](../research/lvgl-fbdev-prototype.md) verified colours, tear-free double buffering, touch mapping, rapid taps, brightness and screen off/on through buttond live on the baseline kernel: 0.2% CPU on a static page, 5.8% for small 60 fps animations.

This replaces the 2026-10-02 direction of DRM/KMS, a Wayland compositor and libinput. That direction would have needed SDE 3.2/14 nm PHY/panel enablement on 4.4 or a mainline board port, and would only have added GPU acceleration (mainline plus Mesa) and composition the project does not need. The [display modernization](../research/display-modernization.md), [DRM bring-up](../research/drm-bringup.md) and [loader budget](../research/chef-loader-kernel-budget.md) records remain as historical evidence. Kernel commit `64fa801` (DRM helper namespace separation) only affects DRM builds and is harmless to the baseline.

The shared platform layer is implemented and live accepted: display backend, own multitouch reader, buttond client, screen-state/handoff protocol, host SDL simulator and a demo application. Use the [UI platform guide](../features/ui-platform.md) for the application contract; the [archived design](../archive/ui-platform.md) retains acceptance checks. Touch requirements from the earlier plan carry over there: discovery by capability, scaling from the MT ABS ranges, slot lifecycle, dropped-event recovery, sleep/wake and consistent rotation. Physical gloves/rain behavior is still unmeasured.

The menus' screen policy is implemented in the [system UI](../features/system-ui.md) (it owns screen power and brightness, auto-sleeps). Still open for the ride application: its own policy on ride pages, and a UI restart must not interrupt recording.

Provide a **sunlight and gloves** mode: maximum backlight on request, a high-contrast, big-digit layout, and every riding action reachable from the side buttons because touch through gloves or rain is unreliable. Define always-on versus button-wake policy and the manual brightness override against Auto brightness (a drag while Auto is on is an offset today). Verify sleep/wake, process death, fallback, and clean shutdown before adding the full ride application.

## System UI design

Round 1 of the system screens was designed in Claude Design on 2026-10-08, in the project **Bike Computer OS Design** (`f6f205a3-9d48-47e0-a4b1-ece4685fa800`), file `Bike OS System Screens.dc.html`. It uses the **Industry** design system (`45761091-7d93-4f50-9583-93d6f266c339`) for its palette and Barlow type. The project's `github.md` maps screens to this repo: status bar and safe area to the [UI platform](../features/ui-platform.md), Wi-Fi to [Wi-Fi UI](../features/wifi-ui.md), and Bluetooth and GPS to this plan. The design is HTML/CSS for review only. Nothing is generated for LVGL, so every screen is rebuilt by hand on chefui.

Screens: 18 portrait screens (P01 settings hub, P02 display, P03 sound and haptics, P04 time and date, P05 storage, P06 about, P07–P10 Bluetooth off/list/pairing/sensor detail, P11–P14 GPS off/acquiring/locked/satellites, P15–P18 Wi-Fi off/list/password/details) and 4 landscape screens (L01 hub, L02 GPS locked, L03 Bluetooth, L04 Wi-Fi). Each comes in dark and light, plus the status bar states.

Design contract, in device pixels:

- Canvas 1080×2246 portrait, 2246×1080 landscape, the real panel size. Content starts at the 96 px [safe top](../features/ui-platform.md#content-clearance-and-full-panel-access); landscape has no inset.
- Status bar: 96 px high, drawn in the two ears beside the 556 px notch (about 238 px each, contents kept to about 190 px, at least 40 px clear of the notch). The left ear shows GPS, Bluetooth, Wi-Fi and temperature (temperature dropped on 2026-10-11, see below). The right ear shows the battery, with its percentage drawn inside the outline, and the time. A muted icon means searching or off. The warning colour is used only for low battery (≤ 15 % and not charging). The sensor count shows on the hub tile instead of the bar.
- Gloves: the smallest target is 192 px (about 12 mm). List rows are 216 px, master toggles 240 px, switches 204×108 px.
- Type for `lv_font_conv`: Barlow Condensed 600 at 40/48/72/96/150/280, Barlow 400–500 at 34/42/48/56/64, with tabular numerals.
- Shapes: 36–40 px corners on cards and buttons, 24 px on keys, pill-shaped switches and satellite chips, 3 px lines, "+" registration marks on key cards. Accent fill is used only for the active state.
- Palette: dark uses bg `#111213`, surface `#1d1f20`, fg `#f2f2f3`, muted `#98989b`, lines `#2b2b2d`/`#5d5d60`, accent `#94bce3` (text `#b5d9fd`, on-accent `#1d2d3d`), warn `oklch(0.78 0.11 60)`. Light uses bg `#f2f2f3`, surface `#e7e7ea`, fg `#1d1f20`, muted `#5d5d60`, lines `#d4d4d7`/`#98989b`, accent `#5980a6` (text `#416180`, on-accent `#f5f5f8`), warn `oklch(0.52 0.13 50)`. Convert the oklch values to sRGB hex for LVGL.
- Icons are Lucide (`lucide-static@0.460.0`): locate/locate-fixed/locate-off, bluetooth/-connected/-off, wifi/wifi-off and zap. Ship them as an `lv_font_conv` icon font built from the Lucide TTF, or as pre-rendered A8 images.

Pulling the design: in Claude Code, `DesignSync` `list_files`/`get_file` on the project id reads the `.dc.html` sources (`Bike OS System Screens.dc.html` includes `Screens.dc.html` and `StatusBar.dc.html`). After each design round, re-read `github.md` and the changed screens rather than copying the whole project.

Slice 1 (2026-10-11, [handoff](system-ui-handoff.md), [guide](../features/system-ui.md)) built the foundation: the token/theme kit with runtime switching, the generated Barlow and Lucide fonts, `chefui_snapshot()` and `tools/chefui/compare-ref.py` against 1:1 reference renders (kept outside git in `~/chef-cyclo-evidence/system-ui-r1-20261011/ref-png/`), the live status bar, the hub (P01), the Display page (P02), the settings file and start at boot.

Slice 2 M1 to M3 added Wi-Fi (P15 to P18) on NetworkManager, GPS (P11 to P14) on gps-manager and gpsd, and Bluetooth sensors (P07 to P10) through blesensd. M4 added Sound and haptics (P03), Time and date (P04), Storage (P05) and About (P06). M5 made Auto brightness and theme Auto work from the ambient light sensor, started `sensors-up` at boot and added the on-screen guided prompt card. The hub tiles and rows open the implemented pages. Host tests, reference comparisons and RAM-image checks are recorded in the [slice 2 log](../../logs/system-ui-slice2-2026-10-11.txt).

Remaining translation work: **landscape** (L01 to L04), after a design pass that respects the notch on the short edge. It needs the design pass first (the short edge puts content under the notch, so the safe areas and the status bar ears change), then a rotation-aware layout in the kit. Blocked on that design round. Left-out pieces of the finished pages (Wi-Fi Sync, internet check and dBm, sound playback, the zone picker and GPS power modes) are in the plans that own them: [connectivity](connectivity-and-sensors.md) and [GPS and time](gps-and-time.md).

Final guided batch of slice 2: done on 2026-10-16 ([log](../../logs/system-ui-slice2-2026-10-11.txt)). Passed: the Wi-Fi join (wrong then right passphrase, Forget), the haptic feel, the 12-hour corner (after a fix) and the GPS satellites page (after a fix); BLE idle current measured (absent sensor +7 mA while retrying, +0.5 mA after; the connected cost stays open, see [connectivity](connectivity-and-sensors.md)). Found and fixed in the batch: the 12-hour clock cut by the panel corner, the satellite count reading 0, the GPS page counter restarting at 0:00 on a quick return. Still open: a look at the GPS page counter on a return while the manager is still searching (covered by a host test, not seen live).

Decided 2026-10-11 (implemented in slice 1 unless noted):

- **Power short press**: Back on every page except the hub, where it turns the screen off. Menus also auto-sleep after the Display page's timeout, and Power wakes. In menus, the volume rocker and `power.double` stay unclaimed (they are reserved for ride pages).
- **Dark and light**: a manual Dark/Light/Auto setting. Auto switches to Light in bright sun from the ALS with hysteresis, so Light doubles as the sunlight theme (implemented in slice 2 M5).
- **Status bar**: no temperature (there is no ambient sensor).
- **Local time**: a POSIX `TZ` setting, default `IST-5:30`. Since 2026-10-20 the image ships tzdata with `/etc/localtime` linked to Asia/Kolkata, and the UI uses that zone file; the setting is the fallback when the file is missing ([system UI](../features/system-ui.md#settings)).
- **Landscape** waits for a design pass that respects the notch on the short edge. L01 to L04 put content under it (not yet implemented).
- The system UI app starts at boot and owns the panel.

## Ride application design

Rounds 1 and 2 of the ride app were designed in the same Claude Design project (synced 2026-10-17): `Ride Pre-ride.dc.html` (built from `PreRide.dc.html`) and `Ride In-ride.dc.html` (built from `RideScreen.dc.html`). They reuse the system UI contract above: 1080×2246 portrait, the 96 px status bar, Barlow, the Industry palette, Lucide icons and the glove target sizes. Each screen is drawn in dark, light and a new **sunlight and gloves** palette. As with the system screens, nothing is generated; every screen is rebuilt by hand on chefui.

Pre-ride (R01 to R03, S01 to S03):

- A 192 px "Ride" header with a Ready / Not ready tag, then three 216 px readiness rows: GPS (fix accuracy such as ±4 m or the search timer, satellites in use, or in use / seen while searching), heart rate (strap state, bpm and zone) and speed and cadence (connected check).
- A battery card showing **riding time left** (for example 6 h 40 min) beside the percentage, "with GPS, 2 sensors and the screen at the current brightness". This needs an estimator built on powerd's measured draw and is new work.
- **Hold to start**: a ring with a 28 px track that fills clockwise over the hold time (1.5 s default, tunable 0.5 to 3 s) and resets if released early. On completion: one haptic pulse, the disc fills with the accent, and recording starts. Touch only. While GPS has no fix the ring is a dashed outline that ignores touch, and it unlocks on a gpsd fix. The notes say 540 px but the component draws 510 px; settle this when building. Use `lv_arc`.

Riding (round 2):

- **Overview presets**, chosen by the rider in ride settings: Grid (six fields at 150 px: speed, distance, ride time, heart rate, cadence, climb), Hero (280 px speed over the ridden track, then four fields) or Stacked (three 280 px fields: speed, heart rate, ride time). Each field has a 40 to 48 px caps label and a secondary value such as the average or the HR zone. The ride settings page itself is not designed yet.
- **Pages**: Overview, Laps, Climb and Map, stepped with the volume rocker or a horizontal swipe. A 240 px bottom bar shows the page name, page dots and two 192 px touch buttons, Lap (248 px wide) and Pause (at least 360 px). When paused, Pause becomes Resume with the accent fill and the values turn muted.
- **Laps**: a current-lap card (280 px lap time, plus distance, average speed and average HR) above a table of earlier laps.
- **Climb**: 280 px gradient, an elevation profile of the last 10 km, and climbed, descended and altitude. Its footnote takes elevation from the map's terrain data, so this page depends on the DEM in the map plan below.
- **Map**: track only, with no tiles yet. Track-up rotation, a heading marker, a scale bar, a "No map tiles yet" pill, and speed and distance along the bottom.
- **Banners**: 192 px, sliding over the top of the content while the data stays visible. Auto-paused ("Resumes when you move"), Paused ("Timer stopped · alerts quiet"), Lap (shown for 3 s with the previous lap's summary), HR zone (for example Z5, above the rider's Z4 ceiling) and low battery (14 %, the only warn-coloured banner, with the time left).
- **Finish**: hold Power. A 540 px ring fills over the overlay ("Keep holding Power / Let go to keep riding"), and releasing cancels.
- **Summary**: "Ride saved" with a "Waiting to upload" tag (the Wi-Fi Sync count), a track thumbnail, distance, ride time, average speed, time in each HR zone (Z1 to Z5 bars) and a Done button.

Sunlight and gloves (S01 to S03, S10 to S13): the same states with no header or subtitles, 96 px labels, 280 px values, 6 px lines, a 640 px ring, no accent, and Lap and Pause 224 px tall. Only the Stacked overview is offered, with speed, heart rate and time. Banners are 240 px and inverted (fg fill), with short caps text such as PAUSED, LAP 4 or BATT 14%. The palette is near black on white: bg `#fbfbfc`, surface `#e7e7ea`, fg and accent `#000000`, muted `#2b2b2d`, lines `#d4d4d7`/`#1d1f20`, on-accent `#fbfbfc`, warn `oklch(0.45 0.14 45)`.

Additions to the kit: Barlow Condensed 600 at 32 and 120 px; Lucide `heart-pulse`, `bike`, `check`, `play`, `pause`, `square`, `flag`, `mountain`, `navigation`, `navigation-2`, `battery-low` and `plus` (registration marks); a sunlight token set beside dark and light.

Ride slice 1 follows the whole [standard interfaces plan](standard-interfaces.md#order) (user decision 2026-10-18). Its buttond part, the hold-progress events, is added to buttond's JSON protocol in slice 1 itself. Decisions needed before implementation:

1. **Hold Power to finish needs press and release from buttond.** Today buttond only reports completed gestures, so a client cannot draw a ring that cancels on release. Options are a new hold-progress stream for a claimed `power.long`, or raw `power.down`/`power.up` events. While the recorder claims `power.long` the 3 s software power-off is suppressed. The finish hold must complete well before the PM660 S1 timer (6.7 s, hard reset at about 8.7 s).
2. **Gloves versus touch-only controls.** The design makes start, Lap and Pause touch only, but the gloves requirement above wants every riding action on the side buttons. The earlier proposal (`power.double` for pause) is not in the design. Choose button paths for start, Lap and Pause, and decide what a short Power press does on ride pages (menus use it for Back or screen off).
3. **Sunlight mode versus the Light theme.** The system UI decided that Light doubles as the sunlight theme under ALS Auto. The ride design adds a separate sunlight and gloves palette and layout. Decide whether it is a manual ride mode, an ALS step beyond Light, or both, and whether it also applies to the system pages.
4. **Climb before the DEM ships.** Hide the Climb page and the climb field until map terrain data exists, or show smoothed GPS altitude labelled as such.
5. **HR zones** need a max HR or zone table setting (for the zone label, the zone alert and the summary bars). It is not designed yet, and neither is ride settings (preset choice, auto-pause, alerts).

## Bike-computer application

Inputs: gpsd ([GPS manager](../features/gps.md#use-gnss-gps-manager), [client integration](gps-and-time.md#client-integration)) for position/speed/track/altitude, BLE sensors through [blesensd](../features/bluetooth.md#ble-sensors) ([remaining validation](connectivity-and-sensors.md#ble-sensors): HR strap, speed/cadence, power), battery (`power_supply/battery`), side buttons ([button integration](ui-and-ride-app.md#button-integration)), touch ([UI stack](ui-and-ride-app.md#ui-stack)). Outputs: the panel through the chosen [UI stack](#ui-stack), vibrator/speaker ([audio](connectivity-and-sensors.md#audio)) for alerts.

Consumers of the left-out system pieces (the pieces themselves are in [connectivity](connectivity-and-sensors.md)): **alerts** play through the audio path honoring the Sound and haptics preferences stored by P03 (alert volume, alert sounds, haptics) and "quiet when paused", which the ride application must set when a ride is paused so alerts stay silent; **Wi-Fi Sync** needs the recorder to keep finished rides as files with a "waiting to upload" count the Wi-Fi page and the Storage page can show, and to mark a ride uploaded only after the server confirms it.

Core: ride recording (GPX/FIT to [persistent storage](../features/storage.md)), live data pages (speed, distance, time, HR, cadence, climb), lap/auto-pause, route following later. Housekeeping: screen on/off, GPS leases and idle power, clean shutdown, upload over Wi-Fi ([Wi-Fi](connectivity-and-sensors.md#wi-fi)).

Opening the map acquires a temporary `map` lease from the GPS manager; leaving it releases that lease. Starting a ride starts or hands off to a recorder that owns a durable `ride` lease until the ride is explicitly ended, independent of the visible page and UI process lifetime. The modem shuts down after the manager's grace period only when neither lease remains.

Order: (a) **UI first** — the data pages on the panel with touch/buttons on the chosen UI stack, driven by gpsd and the sensors, ride recording underneath; (b) **then a map source** — offline tiles or vector data on the writable storage ([persistent storage](../features/storage.md): OSM extracts, e.g. MBTiles/PMTiles or a raster tile cache pre-fetched over Wi-Fi/USB), rendered under the position with track-up rotation, then route following on top; no online map dependency on a ride; GPS altitude is noisy and there is no barometer, so take **elevation** from a DEM shipped with the map data (climb totals, gradient). The map design is settled in [offline maps](#offline-maps) below.

**Time zone**: chrony ([time synchronization](gps-and-time.md#time-synchronization)) gives UTC; the UI needs local time: the system zone (tzdata, `/etc/localtime`), with the settings' POSIX `TZ` as the fallback, as the system UI does. Written as an ordinary Linux app against gpsd/BlueZ and the chosen toolkit/input stack so it is testable on the host with replayed `nmea.log` from the ride runs.

## Offline maps

Decided 2026-10-17 and 2026-10-18 (evidence and sizes in the [offline maps research](../research/offline-maps.md)):

- **Data**: OpenStreetMap as the Protomaps v4 vector basemap (PMTiles, MVT), used as-is. India has no cycle route network, and `surface` is tagged on only about 1 in 10 road kilometres, so our own tile build is not worth it. Elevation from Mapterhorn (Copernicus GLO-30) at z11.
- **Regions**: the rider downloads Indian states and union territories only, from OSM `admin_level=4` outlines grown by a 10 km margin, always at full street detail (z15). One file per region under `/data/v1/maps`. Goa first: 13 MB map, 4.3 MB DEM.
- **Download**: the phone cuts its own region from the newest Protomaps build over HTTP range requests (Goa: 57 requests, 14 MB). The official 54 MB `pmtiles` binary from `/data` first; our own resumable C extractor before large regions matter.
- **Refresh**: on every Wi-Fi connect, if a newer daily build exists, and only when no ride is recording, the map is closed, the clock is valid, and battery and free space allow it. Each refresh is a whole-region download, swapped in only after verification. The DEM is fetched once per region.
- **Rendering**: on the phone, in C: a PMTiles reader, an MVT decoder and LVGL vector drawing (ThorVG, not yet enabled in `lv_conf.h`). Track-up rotation with upright labels. Theming does not matter.

**Next: the Goa render prototype.** Before any production code:

1. Cut Goa (z15) and its z11 DEM on the host and copy them to the phone.
2. A throwaway chefui program that reads the archive, decodes the MVT tiles for the densest view (Panaji at z15 and overzoomed to z17), and draws roads, water and land use with ThorVG, rotated for track-up.
3. Measure on the phone: decode time, draw time and peak RAM per full-screen frame, with and without ThorVG. Compare with LVGL's line and triangle drawing if ThorVG is slow.
4. Pass: a full redraw well under one second at 1 Hz GPS, with idle cost near zero between position updates. The result chooses the drawing path; implementation then goes to the herdr impl and review pair.

Still to design: the Settings **Maps page** (browse states, sizes, download progress, update, delete) needs a round in the Claude Design project.

## Sensor integration

Ambient light values are starting points to tune on real use: the curve (about 30 % at 250 to 320 lux), Light at 2500 lux held 5 s and Dark at 800 lux held 20 s, the smoothing (tau 1 s brightening, 5 s dimming) and the 5 s wake grace that protects the theme from a hand over the sensor right after a wake. Needs: outdoor and riding data (sun, shade, tunnels, a sleeved hand on the bars) and the thresholds' runtime options (`--als-*`) for trials; blocked on getting that data on a bike. Also open: sunlight mode against Auto brightness (above), and the system UI test hygiene item that `test-wifi`'s check count varies between runs (212 or 213, timing dependent): make it deterministic.

Use the [sensord contract](../features/sensors.md) for raw sensors and magnetic heading. The UI owns ALS-driven brightness, including manual override and sunlight mode; the display owner applies backlight changes through the chosen display backend, preserving the framebuffer commit contract while the fbdev path remains in use.

For maps, show calibration/disturbance state and use the accepted compass behavior when stopped. Blend with GPS course over ground when moving, and compute declination from GPS/WMM on the consumer side before setting true heading. Validate switching at low speeds, disturbed or uncalibrated input, and missing GPS. Daemon accuracy refinements remain in the [sensor roadmap](sensors.md#optional-compass-refinements).

GPU acceleration and the shelved DRM/Wayland route are historical investigations, retained in [display research](../research/display-modernization.md) and [DRM bring-up](../research/drm-bringup.md). They are outside the current LVGL application roadmap.
