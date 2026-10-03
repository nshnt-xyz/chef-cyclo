# UI platform live acceptance (2026-10-03)

Completed guided acceptance with the user; temporary boots only, nothing flashed.

## Final image and package

- Final `out/boot-chefui.img`: 45,395,968 bytes, SHA256 `a029c4851efb751977e633d91d4096c1d03a15c5c235024842bdb2b441c352d7`.
- Final LZMA ramdisk: 32,940,834 bytes, SHA256 `6cc430e1a8bcf73598417e133d76a37a2034d4a0e363df2ad410dd624ec20da7`.
- Loader margin: 14,508,032 bytes. Protected baseline kernel, `out/boot.img` and `out/initramfs.cpio.gz` unchanged.
- Demo: 896,376 bytes stripped, SHA256 `47938d394b48bf4f8f9a9a3c343d4369887711dbeda7e21e8846528e84faaf1d`.
- Buttond: 154,168 bytes, SHA256 `a858f31f45481afda0f8d0072034dc92b37701db01805192cbcfd8afac2a78ea`.
- Independent archive extraction verified both packaged binaries against approved live builds. Final fresh boot reverified their installed hashes and 60 fps animated default; clean timed exit succeeded.
- Final boot: /init starts at kernel time 11.360 s, USB up at 11.383 s, exec init at 11.710 s, input coldplug ready at 11.734 s. Initial image exec init took 26.806 s; these are observations, not a controlled compression benchmark.
- Readiness markers, fblog and NetworkManager WLAN device present. WLAN association was not tested as part of UI acceptance.

## User-confirmed checks

- RGB swatches, five rapid taps, corner contact mapping, three simultaneous contact dots.
- Power off/on five times, touch after wake, brightness range, horizontal90 operation.
- Pinch, two-finger swipe and deliberate rotation all classify correctly in both directions after explicit rotation-threshold fix.
- Content clearance calibrated to 96 physical pixels with a visible gap. This is usable content clearance, not measured hardware notch depth.
- Smooth 60 fps animation and smooth brightness dragging after coalescing fix.
- No tearing in the 30/60 fps animation windows.
- Normal switch to peer and back completed. Dark handoffs in both directions stayed dark until power, with touch working after start-dark wake.
- After dark SIGKILL, corrected buttond default wake lights once and stays on without the original extra blink.

## Lifecycle evidence

- SIGTERM while on: POWERDOWN, release, fblog resumed. SIGKILL while on: fblog resumed.
- SIGKILL while off: flag retained, fblog POWERDOWN/closed fb0 and idled. Clean SIGTERM while off: flag retained and fblog had no fb0 fd. Buttond's default woke it.
- Start-dark: lock acquired, sysfs sizing accepted, no fb0 fd and no rendering before power wake. Both dark handoffs inherited lock fd 5; peer remained dark until power.
- Normal handoffs inherited lock fd 5. Suspend-start to peer screen-ready intervals: 563 ms and 561 ms. Software timing proxy, not an optical blink measurement.
- Restarted buttond under init: demo detected disconnect, reconnected and reclaimed three gestures.
- Screen-off 304–319 ms, screen-on about 235–242 ms. No CTP_I2C/BUS ERROR in collected acceptance evidence. Already-resume messages occur on opens while already on; none after logged POWERDOWN cycles. Known powered-down brightness-command timeout appears on redundant fblog shutdown, without touch I2C errors.

## Steady-state costs

| Workload | Measured fps | Process CPU | Wakeups/s |
| --- | --- | --- | --- |
| Static 1 Hz page | 1 | 0.1% | 2 |
| Animation, 30 target | 29–31 | about 2.4% | 31–61 |
| Animation, 60 target | 59–60 | about 4.3% | 60 |
| Full redraw, 30 target | 30 | about 37.5% | 31 |

Startup samples excluded. Full redraw copies about 72.77 Mpx/s at 30 fps, taking about 272 ms/s in copy; incremental animation at 60 fps copies about 3.24 Mpx/s.

## Issues found and resolved during acceptance

1. LVGL rotation threshold defaulted to zero, stealing pinch/swipe: explicitly set 0.2 rad; noisy gesture regressions and live retest pass.
2. Display16/animation33 limited animation to31fps: synchronize timers at init/runtime, preserve30 override; blocking-pan regressions and live60 pass.
3. Slider per-input synchronous pans reduced rendered fps to8–9: coalesce desired brightness into scheduled frame, share pan, preserve static/off-state behavior; bursts and live dragging pass.
4. Fblog inotify wake raced buttond's unlink-then-SIGTERM: wake now only removes flag, off still creates flag before signal; same-daemon inotify/fallback tests and live single-wake pass.

Evidence is in `ui-platform-2026-10-03-*` logs: final-image-boot, final-package, final-costs, normal-handoff-final, dark-handoff, clean-off, crash-off-fixed, wake-blink, buttond-restart, notch-calibration, slider-fixed and independent review logs. Prototype evidence remains `lvgl-proto-2026-10-03-*`.
