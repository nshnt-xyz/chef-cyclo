# Feature guides

[Project overview](../../README.md) · [Build and boot](../building.md) · [Next steps](../next-steps/README.md)

These guides describe current behavior and how to work on it. Verification status comes from the recorded device tests through 2026-09-19; planned behavior belongs in next steps.

| Feature | Current state |
|---|---|
| [USB networking](usb-networking.md) | NCM, DHCP, and recovery shell work; unplug/replug verified. |
| [Bluetooth](bluetooth.md) | BlueZ LE scan, restart, and IBS sleep verified; real sensor pairing still pending. |
| [GPS](gps.md) | Modem/LOC and gpsd 3D fixes verified; baseline lifecycle is manual. |
| [Display and touch](display-and-touch.md) | Log screen, framebuffer handoff, backlight, and multitouch verified. |
| [Buttons and power-off](buttons-and-power-off.md) | Baseline screen toggle, gesture claims, and clean shutdown verified. |
| [Temporary ride logging](ride-logging.md) | Automatic GPS capture and HTTP extraction verified; RAM-only, no button daemon. |
| [Audio](audio.md) | `audio-up` ADSP boot/card and `speaker-test-tone` mmap playback live-verified 2026-09-19; manual opt-in, speaker protection not yet usable. |

When changing a feature, inspect the listed source files, run its host tests, rebuild the appropriate image, and distinguish build/host results from live verification. Update the guide when behavior changes and append dated evidence to the [build log](../build-log.md). Keep pending integration work in the linked plan.
