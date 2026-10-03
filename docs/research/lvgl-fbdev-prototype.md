# LVGL on the MDSS framebuffer: prototype

[Research index](README.md) · [UI platform plan](../next-steps/ui-platform.md) · [Framebuffer contract](../features/display-and-touch.md#framebuffer-and-touch-contract)

2026-10-03. Research prototype for the UI platform decision recorded in the [UI roadmap](../next-steps/ui-and-ride-app.md#ui-stack): LVGL rendering directly on the existing 4.4 MDSS fbdev, touch read from evdev, side buttons through `buttond`, with no kernel change. The prototype `lvproto` is research code. Its final source is retained as [evidence](../../logs/lvgl-proto-2026-10-03-lvproto.c), and the production platform is specified separately.

Phone: baseline kernel #20 (`4.4.192-cyclo+`), temporary boot, fblog/buttond/powerd resident. The binary was pushed to `/tmp` over the USB network; nothing was flashed. LVGL v9.6.0 came from the release tarball; [hashes](../../logs/lvgl-proto-2026-10-03-hashes.txt) identify it and every prototype binary that ran. Configuration changes from `lv_conf_template.h` are in the [config delta](../../logs/lvgl-proto-2026-10-03-lv_conf-delta.txt).

## Why not DRM/Wayland

The October 2–3 [DRM bring-up](drm-bringup.md) established DRM core startup but no card: native scanout needs SDE 3.2 catalog/format support, a 14 nm DSI PHY implementation, panel and DT translation on 4.4, or a mainline board port, and GPU acceleration needs mainline plus Mesa in either case. The goal was a modern, light userspace display stack with no third-party applications. LVGL on fbdev meets that without kernel work; a Wayland compositor would only add composition copies and a second process. Fullscreen applications hand fb0 over through the existing lock protocol instead of compositing. The DRM records remain historical evidence.

## Findings

### Pixel format: render BGRA, swizzle into RGBA

MDSS registers fb0 as `MDP_RGBA_8888` (bytes R,G,B,A; `fbdev.h` notes this). LVGL 9.6's 32-bit formats are `ARGB8888`/`XRGB8888` only (bytes B,G,R,A/X); it has no ABGR/RGBA render format.

`mdss_fb_set_par()` does accept a 32-bit BGRA var (red 16, blue 0, alpha 24 → `MDP_BGRA_8888`), but a format change calls `mdss_fb_blank_sub(FB_BLANK_POWERDOWN)` then `UNBLANK` directly, bypassing the fb notifier chain. That is the documented path that powers the NT36525 TDDI down while its driver believes it is awake (dead touch). It would also leave fblog drawing with a stale var. **Do not change fb0's format.** Swap R and B while copying instead (below); the live colour swatches read red, green, blue.

### The fb mapping is write-combined: never read it

The first prototype let LVGL render straight into the two fb pages (DIRECT mode, two buffers) and swizzled in place. A full-screen in-place swizzle took **232 ms** for 2.4 Mpx (~97 ns/px) and a once-a-second clock label cost 20–30 ms of handler time ([direct-fb smoke](../../logs/lvgl-proto-2026-10-03-direct-fb-smoke.txt)). Reads from the MDSS mapping are uncached; blending and in-place swizzles read it.

The kept design renders into a cached RAM shadow (DIRECT mode, one 1080x2246 XRGB8888 buffer, stride 4320) and copies only with writes. Full-screen copy+swizzle into a page then takes **~10 ms** (≈4 ns/px, ≈1.3 GB/s). See [shadow smoke](../../logs/lvgl-proto-2026-10-03-shadow-smoke.txt).

### Double buffering by pan offset works

`virtual_size` is 1080x4492 (two pages, stride 4352). `FBIOPAN_DISPLAY` with `yoffset = page * 2246` is accepted (`mdss_fb_pan_display_sub` bounds-checks it). The kernel's `pan_display` always waits for the frame (`wait_for_finish = true`), so the ioctl blocks until vsync (~16 ms) and is the flip.

Per frame: copy+swizzle this frame's dirty areas **and the previous frame's** from the shadow into the back page, then pan to it. The previous frame's areas bring the back page up to date, since it last held the frame before. Skip the separate copy when a full-screen area is present. No tearing was visible on a 60 fps sliding bar (user-observed). fblog always commits page 0 (`fb_commit` sets yoffset 0), so leaving page 1 displayed at handoff is harmless.

### Measured cost

[CPU/cycle evidence](../../logs/lvgl-proto-2026-10-03-cpu-cycles-kill9.txt), process CPU time via `CLOCK_PROCESS_CPUTIME_ID`:

| Load | Frames/s | CPU (one core) | Wakeups/s |
| --- | ---: | ---: | ---: |
| Static page, clock label once per second | 1 | 0.2% | 2 |
| Spinner + 200x120 sliding bar (~3.7 Mpx/s copied) | 60 | 5.6–5.8% | 60 |
| Full-screen invalidation every frame | 60 | 66.7% | 60 |

The full-screen case is the worst case (e.g. a future map pan): render plus ~7.6 ms copy per frame. Single-threaded rendering (`LV_OS_NONE`) was used throughout. The static stripped binary with two large Montserrat fonts is 834 KiB.

The main loop sleeps in `poll()` on the touch fd and the buttond socket with the timeout from `lv_timer_handler()`, touch in `LV_INDEV_MODE_EVENT`; while a contact is down it reads every 16 ms so long-press, scroll and release work. This is what keeps a static page at 2 wakeups/s.

### Screen off/on and touch power

`FBIOBLANK POWERDOWN` took ~315 ms and `UNBLANK` ~235 ms, both blocking in the ioctl. Every cycle went through the notifier: `nvt_ts_suspend` before panel off, and on unblank panel on (`Pwr_mode 0x9c`) then `nvt_ts_resume` with bootloader reset. No `CTP_I2C`, `BUS ERROR` or `Touch is already resume` after a powered-down panel occurred across six cycles, and touch worked immediately after wake. After `UNBLANK`, write the backlight and force a full redraw; the commit applies the level (contract quirk 1).

A `power.short` claim on `/run/buttond.sock` toggled the screen live in both directions.

### Handoff and process death

The prototype takes `LOCK_EX` on `/run/fb0.lock` before opening fb0; fblog paused within milliseconds. On clean exit (POWERDOWN, close, unlock) fblog resumed with UNBLANK and a touch reset. After `kill -9` with the screen on, fblog resumed ~80 ms later; the panel stayed powered because fblog still held fb0, which is the expected `Touch is already resume` case.

Gap found: when the app exits while it has deliberately turned the screen off, fblog resumes and turns it back on. fblog reads `/run/fblog.off` only at startup. Screen-off must become shared state that fblog checks before resuming after a borrow.

### Touch: LVGL's evdev driver is not usable as-is

Three live runs ([raw](../../logs/lvgl-proto-2026-10-03-live1.txt), [gestures on](../../logs/lvgl-proto-2026-10-03-live2.txt), [final](../../logs/lvgl-proto-2026-10-03-live3.txt)):

1. **No scaling.** `lv_evdev` takes its calibration from `EVIOCGABS(ABS_X/ABS_Y)` only. NT36525 advertises only `ABS_MT_POSITION_X/Y` (0..720, 0..1600); the single-touch query succeeds with an all-zero range, so raw coordinates passed through. Taps landed about two thirds of the way toward the top-left (a slider tap hit the button above it).
2. **Zeroed slots.** With `LV_USE_GESTURE_RECOGNITION`, `lv_evdev` zeroes a slot's coordinates on release, but the input core suppresses unchanged per-slot `ABS_MT` values. Rapid taps at nearly the same place reuse slot 0 with an unchanged X or Y, which is never resent, so the pointer reads 0 on that axis (`0,0`, `549,0`, `0,467` in live run 2). Rapid taps missed the button.
3. **No `SYN_DROPPED` handling**, and it drains all queued events per read, so a press and release delivered in one batch can collapse into one state.

With explicit MT calibration and gesture recognition off, the final run mapped corners to (82,53), (1001,57), (1005,2177), (100,2226) inside 90 px targets, and five rapid taps 80–170 ms apart gave five clicks. The production platform needs its own small MT protocol-B reader; multi-touch gestures then come from feeding LVGL's gesture recognizer from that reader's slot state. Gesture recognition was not accepted in this prototype.

## Not established

- Gestures (pinch, rotate, two-finger swipe) through a correct slot reader; `SYN_DROPPED` recovery.
- Power draw: the CPU figures are not battery measurements.
- Multi-threaded rendering, NEON-specific copy loops, asynchronous commits (`MSMFB_DISPLAY_COMMIT` without waiting).
- Panel-on time between two applications during a handoff (each side measured separately).
- The host SDL simulator (SDL2 development headers were not installed).
