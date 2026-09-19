# UI and ride application

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## Button integration

Use [buttond](../features/buttons-and-power-off.md) rather than opening the key evdev nodes in the app. Proposed mapping: recorder claims `power.long` to finish a ride, UI claims `power.double` for pause and volume gestures for page/zoom. Decide whether the recorder offers power-off after finishing: its long-press claim disables the default shutdown while held. Use `power+volup` for a software power menu and retain the hardware escape through Power+VolDown.

Acceptance: claims release on process death, UI restarts do not interrupt the recorder, and the user can still finish a ride and shut down cleanly. Decide how the future recorder supersedes the temporary ride image, which currently has no button daemon.

## UI stack

Decide the UI stack. The 4.4 kernel exposes the panel as fbdev (`mdss_fb`, the `tools/fbdev.h` contract) and the GPU as `kgsl`, no DRM/KMS — current Weston/wlroots need DRM (fbdev backend was dropped in Weston 10), so Wayland means either Weston ≤9 built from source with the fbdev backend + pixman, or libhybris. If not Wayland then X11: Xorg with the `fbdev` DDX (`xf86-video-fbdev`, still maintained and packaged in Alpine) runs on `mdss_fb` today with no kernel work, `xf86-input-libinput` or `evdev` for touch, and the app can be any X toolkit — the pragmatic middle ground. Last resort: draw straight to `fb0` (LVGL has fbdev + evdev backends) and skip the display server entirely. These candidates have not been integrated; spike and choose one.

Either way the touch path needs doing: the NT36xxx multitouch protocol is decoded in `tools/fbtouch.c` (reference client; the [framebuffer contract](../features/display-and-touch.md#framebuffer-and-touch-contract) is binding); under Wayland that becomes libinput reading `event1` (check it handles the panel's raw 720x1600 coordinates vs. the 1080x2246 framebuffer — needs a calibration matrix or `LIBINPUT_CALIBRATION_MATRIX`), under fbdev/LVGL it is the evdev backend with the same scaling. Test: taps in the corners, a stroke, three fingers, as in the [2026-09-18 display verification](../build-log.md).

Also, whatever the stack: a **sunlight and gloves** mode — maximum backlight and a high-contrast, big-digit layout by default, and everything reachable from the side buttons ([button integration](ui-and-ride-app.md#button-integration)) because touch through gloves or rain is unreliable; a screen-on policy (always on while riding vs. wake on button) lives here too.

## Bike-computer application

Inputs: gpsd ([GPS manager](gps-and-time.md#gps-manager)) for position/speed/track/altitude, BLE sensors through BlueZ ([BLE sensors](connectivity-and-sensors.md#ble-sensors): HR strap, speed/cadence, power), battery (`power_supply/battery`), side buttons ([button integration](ui-and-ride-app.md#button-integration)), touch ([UI stack](ui-and-ride-app.md#ui-stack)). Outputs: the panel through the chosen [UI stack](#ui-stack), vibrator/speaker ([audio](connectivity-and-sensors.md#audio)) for alerts.

Core: ride recording (GPX/FIT to [persistent storage](storage-and-boot.md#persistent-storage)), live data pages (speed, distance, time, HR, cadence, climb), lap/auto-pause, route following later. Housekeeping: screen on/off, GPS leases and idle power, clean shutdown, upload over Wi-Fi ([Wi-Fi](connectivity-and-sensors.md#wi-fi)).

Opening the map acquires a temporary `map` lease from the GPS manager; leaving it releases that lease. Starting a ride starts or hands off to a recorder that owns a durable `ride` lease until the ride is explicitly ended, independent of the visible page and UI process lifetime. The modem shuts down after the manager's grace period only when neither lease remains.

Order: (a) **UI first** — the data pages on the panel with touch/buttons on the chosen UI stack, driven by gpsd and the sensors, ride recording underneath; (b) **then a map source** — offline tiles or vector data on the writable storage ([persistent storage](storage-and-boot.md#persistent-storage): OSM extracts, e.g. MBTiles/PMTiles or a raster tile cache pre-fetched over Wi-Fi/USB), rendered under the position with track-up rotation, then route following on top; no online map dependency on a ride; GPS altitude is noisy and there is no barometer, so take **elevation** from a DEM shipped with the map data (climb totals, gradient).

**Time zone**: chrony ([time synchronization](gps-and-time.md#time-synchronization)) gives UTC; the UI needs local time (tzdata or a fixed `TZ`). Written as an ordinary Linux app against gpsd/BlueZ/evdev so it is testable on the host with replayed `nmea.log` from the ride runs.

## GPU

Adreno 509 behind the downstream `kgsl` driver (`kgsl-hyp` messages appear at boot), no DRM. Using it means either freedreno/Mesa on a DRM `msm` driver this 4.4 kernel does not have, or libhybris over the stock Android GLES blobs. Software rendering on `fb0` is likely enough for a bike computer UI at 1080x2246; only revisit if the chosen [UI stack](#ui-stack) turns out to be too slow or too power-hungry without it.
