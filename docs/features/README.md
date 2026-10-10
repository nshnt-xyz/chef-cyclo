# Feature guides

[Project overview](../../README.md) · [Build and boot](../building.md) · [Next steps](../next-steps/README.md)

These guides describe current behavior and how to work on it. Reviewed against recorded device tests through 2026-10-11; planned behavior belongs in next steps.

| Feature | Current state |
|---|---|
| [USB networking](usb-networking.md) | NCM, DHCP, and recovery shell work; unplug/replug verified. |
| [Wi-Fi](wifi.md) | Standard baseline NM/WPA2, automatic chrony/HTTPS/apk and static-overlap rejection verified. |
| [Bluetooth](bluetooth.md) | Dual-mode BlueZ with standard tooling: LE scan, restart and IBS sleep verified; classic pairing, rfcomm, session-bus obexd, HID (uhid and HIDP), PAN (incl. NetworkManager) and A2DP via PipeWire, meshd and bonded idle current verified 2026-10-02; real sensor pairing still pending. |
| [GPS](gps.md) | Modem/LOC and gpsd fixes, demand-driven manager leases and offline chrony SHM time verified; shared modem support stays resident. |
| [UI platform](ui-platform.md) | LVGL/fbdev + SDL platform, calibrated content clearance, 60 fps pacing and coalesced brightness; host-reviewed and live accepted; [final evidence](../../logs/ui-platform-2026-10-03-live-summary.md). |
| [Display and touch](display-and-touch.md) | Log screen, framebuffer handoff, backlight, and multitouch verified. |
| [Buttons and power-off](buttons-and-power-off.md) | Baseline screen toggle, gesture claims, and clean shutdown verified. |
| [Battery and charging](battery-and-charging.md) | Kernel charging proven live (wall charger +302 mAh in 14 min, 1.98 A peak); `powerd` low-battery warn/shutdown, 44/42 °C charge throttle and `/run/power` log live-verified 2026-09-26; `Full` and a real drain not yet seen. |
| [On-board sensors](sensors.md) | Accelerometer, gyroscope, magnetometer, light and proximity through `sensord` (`/run/sensord.sock`, IIO units) live-verified 2026-09-26, including audio coexistence and 0 writes to persist; manual opt-in (`sensors-up`), magnetometer calibration verified September 27; compass accepted October 1 for orienting a stopped map, with documented accuracy limits; across-boot bias seeding accepted October 10. |
| [Boot and recovery](boot.md) | Standalone stage-1 boot, checked read-only `system_a`, A/B slot handling and USB rescue; installed cold boot accepted. |
| [Persistent data](storage.md) | `/data` on userdata; Bluetooth/Wi-Fi, crash/clock/power state and magnetometer bias persist; orderly shutdown and return to fastboot verified. |
| [SSH](ssh.md) | dropbear on port 22 of every interface, key login only, keys baked into the image, host key on `/data`; telnet over USB stays the recovery shell. Added 2026-10-10, installed 2026-10-11. |
| [Audio](audio.md) | `audio-up` ADSP boot/card and `speaker-test-tone` mmap playback live-verified 2026-09-19; manual opt-in, speaker protection abandoned September 26 after live experiments; conservative playback limits are permanent. |

When changing a feature, inspect the listed source files, run its host tests, rebuild the appropriate image, and distinguish build/host results from live verification. Update the guide when behavior changes and append dated evidence to the [build log](../build-log.md). Keep pending integration work in the linked plan.
