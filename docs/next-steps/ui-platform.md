# UI platform (LVGL on fbdev)

[Current operating/app contract](../features/ui-platform.md) · [Next-steps index](README.md) · [UI roadmap](ui-and-ride-app.md) · [Prototype research](../research/lvgl-fbdev-prototype.md) · [Framebuffer contract](../features/display-and-touch.md#framebuffer-and-touch-contract)

Implemented, independently reviewed and live accepted on 2026-10-03; results are recorded below.
The [feature guide](../features/ui-platform.md) is the current operating and
application contract; this document retains design decisions and the acceptance
checklist. Decided 2026-10-03: the UI stack is LVGL rendering directly on the existing MDSS fbdev, with no DRM, Wayland or kernel change and no third-party applications. This phase builds only the **platform layer** that every fullscreen application links, plus a demo application to verify it. The ride UI, settings UI, maps and recorder come later on top of it.

Every number and hazard below was measured or observed in the [prototype](../research/lvgl-fbdev-prototype.md); follow its findings rather than LVGL's stock Linux drivers.

## Decisions

| Topic | Decision |
| --- | --- |
| Toolkit | LVGL **v9.6.0** as a git submodule at `third_party/lvgl`, pinned to the tag. |
| Location | `tools/chefui/`: library `libchefui.a`, demo `chefui-demo`, own `Makefile` and `tests/`, following `tools/sensord/`. |
| Device build | Static aarch64 musl (`toolchain/aarch64-musl`), built by `scripts/mkinitramfs.sh` like `sensord`. |
| Rendering | Single-threaded (`LV_OS_NONE`), NEON blend (`LV_DRAW_SW_ASM_NEON`), C stdlib malloc/string/sprintf. |
| Frame target | Default 16 ms (~60 fps target); 33 ms (~30 fps) and explicit application periods remain available. Actual throughput depends on rendering, copy and pan. |
| Pixel path | Cached RAM shadow, XRGB8888, DIRECT mode; copy+swizzle dirty areas into the fb back page; pan to flip. |
| App switching | Simple handoff: POWERDOWN, close, exec the next app with the screen lock held. A ~0.55 s black blink per switch is accepted. |
| Touch | Own multitouch protocol-B evdev reader, **not** `lv_evdev` (LVGL's evdev driver). |
| Host | SDL2 backend for running the same application code on the PC (`libsdl2-dev` required). |

## Components

### 1. Display backend (device)

- **Ownership:** take `LOCK_EX` on `/run/fb0.lock` with `fb_lock_exclusive()` (10 s), or adopt an inherited lock fd (see [handoff](#4-screen-state-handoff-and-exit)). Then open `/dev/fb0`, `FBIOBLANK UNBLANK`, and stay resident. Reuse `tools/fbdev.h`; extend it rather than duplicating its contract.
- **Layout check:** require 32 bpp, red/green/blue offsets 0/8/16 and `yres_virtual >= 2 * yres`; otherwise fail with a clear message. **Never** `FBIOPUT_VSCREENINFO`: a format change power-cycles the panel outside the fb notifier and kills touch.
- **Shadow:** one 64-byte-aligned XRGB8888 buffer at the logical resolution, set with `lv_display_set_buffers_with_stride(..., LV_DISPLAY_RENDER_MODE_DIRECT)`. LVGL never touches the fb mapping. The mapping is write-combined, so code must **never read it**.
- **Flush:** collect the frame's areas. On `lv_display_flush_is_last()`, write the union of this frame's and the previous frame's areas from the shadow into the back page, then `FBIOPAN_DISPLAY` with `yoffset = page * yres` (blocks until vsync), and swap pages. Skip areas fully contained in another, take a single full-screen copy when any area covers the screen, and fall back to a full copy if the area list overflows. Copy loops write destination rows sequentially, swap R and B, and force alpha to 0xff.
- **Rotation:** support 0/90/180/270 at init. The shadow uses the logical (rotated) size; the copy transforms coordinates while still writing destination rows sequentially. Touch uses the same transform. 0 is the default; the bike-mount orientation is a later application decision.
- **Backlight:** `chefui_brightness_set(1..255)` queues the latest requested level. Calls never block on vsync. While on, a one-pixel invalidation schedules a frame even on a static page; immediately before that frame's normal pan, write `/sys/class/leds/lcd-backlight/brightness` (never `leds/wled`). All intervening requests coalesce into one write and share the rendering commit. Deferred write errors are logged. While off, store the level; every screen-on writes it before the first frame. The getter returns the requested value.
- **Stats:** when `CHEFUI_STATS=1`, log one line per second to stderr: fps, process CPU % (`CLOCK_PROCESS_CPUTIME_ID`), Mpx copied, copy ms, pan average/max, wakeups.

### 2. Touch reader

- **Discovery:** scan `/dev/input/event*` for `INPUT_PROP_DIRECT` plus `ABS_MT_POSITION_X/Y`, never a hardcoded node. Open it nonblocking; do not grab. On `ENODEV` or read error, close it and rescan once a second.
- **Scaling:** ranges come from `EVIOCGABS(ABS_MT_POSITION_X/Y)` (NT36525: 0..720, 0..1600) scaled to the panel, then the rotation transform, then clamped.
- **Slot state:** track up to 10 slots (tracking id, x, y). **Never clear a slot's coordinates on release**: the input core does not resend unchanged per-slot values.
- **Frames:** apply events up to each `SYN_REPORT` as one frame. One LVGL read consumes one frame; if more complete frames are queued, set `data->continue_reading` so a press and release that arrive together still reach LVGL as two states.
- **Primary pointer:** the first contact down drives the LVGL pointer until it lifts, which is reported as a release even if other contacts remain. A new primary only starts after all contacts are up.
- **Gestures:** enable `LV_USE_GESTURE_RECOGNITION` (needs `LV_USE_FLOAT`). Feed LVGL's recognizers from the slot state the way `lv_evdev.c` does, using correct coordinates.
- **`SYN_DROPPED`:** drop events until the next `SYN_REPORT`, then resync slots with `EVIOCGMTSLOTS` (tracking id, X, Y). If resync fails, release all contacts.
- **Screen off:** release all contacts and stop reading. On screen on, drain stale events, then resync.
- **Main loop:** use `LV_INDEV_MODE_EVENT`, and read when the fd is readable. While a contact is down, also read every refresh period, so long-press, scrolling and release timing work.

### 3. Buttons (buttond client)

- Connect to `/run/buttond.sock` and send `claim G` for each configured gesture; `power.short` is always claimed by default.
- Parse `ok …`, `err …` and `event G` lines across partial reads, and deliver `event G` to the application's callback.
- If buttond restarts, reconnect with 1 s backoff and claim again.
- Default `power.short` toggles the screen unless the application's callback reports it handled the event.
- Do not claim `power.long` by default; buttond's shutdown default must keep working. Power+VolDown is reserved.

### 4. Screen state, handoff and exit

`/run/fblog.off` becomes the **system-wide "screen deliberately off" flag**. Its path is kept so buttond and existing scripts keep working; the documentation must describe the new meaning.

- **Screen off:** `FBIOBLANK POWERDOWN` (the touch IC suspends through the notifier, ~315 ms), then create the flag. Pause the display refresh timer (`lv_display_get_refr_timer`) but keep application timers and fds running.
- **Screen on:** remove the flag, `FBIOBLANK UNBLANK` (~235 ms), write the backlight, resync touch, then invalidate the whole screen and mark the previous frame full.
- **Starting while the flag exists:** take the lock but do not open fb0, since the first open would light the panel. Open it at the first screen-on request.
- **Clean exit and fatal signals** (SIGTERM, SIGINT, SIGHUP): POWERDOWN, close fb0, release the lock. Leave the flag as it is: fblog stays dark when the screen was off, and resumes otherwise.
- **Handoff, `chefui_handoff_exec(path, argv)`:** POWERDOWN, close fb0, keep the lock fd open without `CLOEXEC`, export `CHEFUI_LOCK_FD=<n>`, and exec. The new process adopts that fd, so fblog cannot resume in between. Remove the flag first unless the application asks to hand off dark.
- **Process death** (SIGKILL, crash): the lock dies with the process. fblog resumes, but must honour the flag (next item).
- **fblog change** (`tools/fblog/fblog.c`): before resuming after a borrow, check the flag. If present, POWERDOWN (harmless when already off), close fb0 and idle exactly as at startup. Add the case to `tests/test_fblog.c`. buttond's integrated default uses create flag + SIGTERM for off, and unlink only for on; resident fblog wakes through inotify/fallback, or init's next spawn sees the current flag when absent. This protocol change passed independent review and coordinator live wake retest on 2026-10-03.

### 5. Application API and loop

`chefui.h` exposes, at minimum:

- `chefui_init(const struct chefui_config *)`: application name, refresh period (0 selects 16 ms; explicit override retained), rotation, brightness, button claims, backend auto/fbdev/sdl. Returns the LVGL display. Calls `lv_init` and the tick callback (`CLOCK_MONOTONIC`).
- `chefui_run()` / `chefui_quit(code)`: a `poll()` loop with the timeout from `lv_timer_handler()`. A static page must stay at about 2 wakeups/s; the prototype measured 0.2% CPU.
- `chefui_watch_fd(fd, events, cb, user)` / `chefui_unwatch_fd(fd)`: application sockets (gpsd, D-Bus, sensord) join the same loop.
- `chefui_screen_set(bool)`, `chefui_screen_is_on()`, `chefui_brightness_set(int)`, `chefui_on_button(cb, user)`, `chefui_handoff_exec(path, argv, dark)`.
- Lifecycle notes go to `/dev/kmsg` with a `chefui[<app>]:` prefix (start, lock acquired, screen on/off, handoff, exit, errors). Per-frame detail goes only to `CHEFUI_STATS`.

### Safe content and notch calibration

Ordinary controls use `chefui_content_root()` as their LVGL parent. It is a
transparent, non-scrolling child inside `chefui_safe_area()` (inclusive logical
bounds). `chefui_root()` returns the full-panel active screen for backgrounds
and diagnostics. Content roots are owned by their screen; request the current
content root again after switching screens. The library reuses it on returning
to a screen. Applications own the layout of children within that rectangle.

`chefui_config.safe_top_px` specifies **physical panel pixels**, excluding a
strip across the physical top. Zero selects the **visually calibrated 96 px** default;
-1 disables the inset. Positive values override it; values at or above physical
height are rejected. `chefui_safe_top_set()` adjusts it at runtime and moves the
current screen's existing content root; `chefui_safe_top_get()` reports the
resolved physical inset. A screen's content root is updated when requested
again after a screen switch. This shared implementation applies to SDL and fbdev.
The physical-top strip maps to logical top/left/bottom/right at 0/90/180/270°,
using the existing copy transform. Framebuffer dimensions and touch coordinates
stay unchanged. Backgrounds fill the panel, including the notch region.

**Default clearance is 96 physical pixels, visually calibrated by the user on
2026-10-03.** The device reported `notch calibration: --safe-top 96 physical
pixels`. This is a measured content clearance, including the desired gap,
rather than a claim about the notch hardware depth. To recalibrate, run
`chefui-demo --calibrate-notch --safe-top 96`. The translucent
purple band reserves clearance; its green border is the boundary. The guide
text starts 4 px inside the content rectangle. Volume Up/Down (host Up/Down)
adjust physical clearance by 4 px; the screen logs the resolved value as
`--safe-top N`. Brightness stays available through the slider. Lower clearance
until the guide approaches the notch's lowest physical obstruction, then raise
it until the entire text and boundary clear the obstruction with a small
comfortable gap. Check landscape orientations too. Record the resulting
physical value with evidence; calibration does not save it automatically.
`--safe-top 0` disables the margin for diagnostics. Corner targets and contact
dots remain full-panel. The demo preserves the chosen inset and calibration
mode across peer handoffs. The user calibration above establishes the current default. Explicit overrides
and disabling the margin remain available for other panels or diagnostics.

### Gesture threshold workaround

LVGL v9.6.0's rotation recognizer allocates a zeroed configuration without
initializing its rotation threshold. Tiny angular noise can therefore recognize
rotation before pinch or two-finger swipe reaches its threshold; the first
recognized gesture owns the contact sequence. The device backend explicitly
sets the configured LVGL default (200 milliradians = 0.2 radians, about 11.5°)
through the public setter. Pinch/swipe thresholds and recognizer precedence
remain LVGL's defaults. Host input-frame regressions exercise noisy pinch,
noisy translated swipe and deliberate rotation with exclusive classifications.
Live retest on 2026-10-03: the user confirmed pinch, two-finger swipe and
deliberate rotation classify correctly in either direction after the threshold
fix. Notch clearance was visually calibrated to 96 physical pixels in the same
acceptance session. The user also confirmed horizontal90° operation. Lifecycle/off-state, dark handoff, buttond restart, animation tearing and device
cost checks subsequently passed; see the final acceptance record below. On user
request, the platform/demo default refresh period is now 16 ms (60 fps target).

A live `-f 60 -a` run initially measured 31 fps: the display timer was 16 ms,
but LVGL's animation timer retained its 33 ms configured default. Init and
`chefui_refresh_period_set()` now synchronize the animation timer with the
requested display period. LVGL timestamps timers before executing callbacks
and subtracts elapsed callback time from the next wait; the blocking pan itself
does not require a second full-period wait. A host regression using real LVGL
animation and a mocked 16 ms blocking pan measures the effective frame rate:
16 ms cadence exceeds 48 fps, explicit 33 ms cadence remains about 30 fps,
and forcing the old 33 ms animation tick reproduces about 31 fps. Wakeups are
bounded, and the static 1 Hz label test still checks about two wakeups/second.
These host observations do not establish device throughput; the coordinator
subsequently retested the reviewed binary live as recorded below.

Live pacing retest on 2026-10-03 reached 60 fps, about 4.2% CPU and 16 ms pan;
the user reported smooth animation. Dragging the brightness slider then exposed
per-input synchronous pans (stats dropped to about 9 fps). Brightness requests
now coalesce before the next rendered frame, including a minimal invalidation
for brightness-only changes. Host tests exercise slider value-event bursts with
16 ms blocking pans, assert no synchronous request pans, latest level at commit,
continued animation throughput, and static/off-to-on behavior. The user confirmed smooth slider dragging on the reviewed fix on 2026-10-03.
The integrated wake protocol passed independent review and live retest on
2026-10-03. Final packaging contains the approved, live-tested binaries.

### 6. Host backend (SDL2)

`make -C tools/chefui host` builds the same applications against LVGL's SDL driver:

- **Display:** the window shows the logical resolution, scaled (default zoom 0.4, configurable). The mouse acts as the single touch pointer.
- **Buttons:** keyboard keys stand in for buttond gestures: `P`/`Shift+P`/`Ctrl+P` for `power.short`/`.long`/`.double`, `Up`/`Down` for `volup.short`/`voldown.short`, plus a modifier for `.long`.
- **Screen off:** blanks the window and pauses refresh.
- **No device paths:** lock, fb and buttond are not used.

Application code must not need `#ifdef` for host versus device.

### 7. Demo application `chefui-demo`

This is the platform's verification vehicle, not a product. It includes:

- R/G/B swatches and corner targets
- a tap counter and a brightness slider
- one dot per active contact (proves multitouch slots)
- a gesture label, a screen-state line, and the stats line when enabled
- a "switch" button that hands off to a second instance (`--peer`), so handoff blinks and lock inheritance can be checked

Options:

| Option | Effect |
| --- | --- |
| `-t secs` | auto-exit |
| `-a` | animation (spinner plus moving bar) |
| `-F` | full-screen invalidation every frame |
| `-r 0/90/180/270` | rotation |
| `-f fps` | frame target (30 or 60; default 60), not a guaranteed measured rate |
| `--dark` | handoff test leaves the screen dark |

`mkinitramfs.sh` installs it as `/usr/bin/chefui-demo`; inittab does not start it. Check the [loader budget](../research/chef-loader-kernel-budget.md) with `scripts/check-chef-loader-budget.py` after the ramdisk grows.

## Host tests (`make -C tools/chefui test`)

Use mocks or wrapped syscalls like the existing `tools/tests` (no device, no real fb):

- **Copy:** swizzle and alpha, stride handling, and pixel placement for every rotation.
- **Dirty-area plan:**
  - previous ∪ current areas
  - containment pruning
  - full-screen shortcut
  - overflow forcing a full copy
  - page alternation and pan offsets
- **Touch reader**, from injected `input_event` streams:
  - scaling and rotation
  - rapid taps reusing slot 0 with one unchanged axis (the prototype's `0,0` bug)
  - press and release in one batch giving two LVGL states
  - multi-slot primary tracking
  - `SYN_DROPPED` resync through a wrapped `EVIOCGMTSLOTS`, and release-all on screen off
- **buttond client:**
  - partial-line parsing and claims sent on connect
  - reconnect and re-claim after the server closes
  - default `power.short` toggle versus an application override
- **Screen state machine**, with mocked fb ioctls and a temporary flag path:
  - ordering of POWERDOWN, flag write and UNBLANK
  - start-dark
  - clean exit with the screen on and with it off
  - signal exit
  - handoff lock inheritance, including no unlocked gap
- **fblog:** resuming after a borrow with the flag present stays idle and closes fb0.
- **Regression:** `make -C tools test` and the other component suites still pass.

## Live acceptance (coordinator, with the user present)

Resolved live issue (2026-10-03): after screen-off SIGKILL, default wake
unlinked the flag, woke fblog, then SIGTERMed that newly opened daemon; the
panel blinked off until init respawn about two seconds later. Evidence:
[default wake blink](../../logs/ui-platform-2026-10-03-wake-blink.txt). The
implemented correction wakes by unlink only and retains create-then-signal
for off. Host integrated inotify/fallback and absent-daemon/next-respawn tests
pass; independent review approved the correction, and the user confirmed
a single wake stays on without blinking in the coordinator live retest.

Post the step table in chat first, announce the window right after the first committed frame, and number the steps. Record evidence under `logs/`.

1. **Visual:**
   - colours correct
   - no tearing at 30 and 60 fps
   - corner taps inside 90 px targets
   - five rapid taps give five clicks
   - three simultaneous contacts show three dots
   - pinch and two-finger swipe are recognized
2. **Screen:**
   - `power.short` off/on ×5 with touch working after each
   - brightness 5..255
   - no `CTP_I2C`/`BUS ERROR`, and no `Touch is already resume` after a powered-down panel
3. **Lifecycle:**
   - clean exit hands back to fblog
   - `kill -9` with the screen on: fblog resumes
   - `kill -9` and clean exit with the screen off: fblog stays dark, and buttond's toggle wakes it
   - start with the flag present: stays dark until `power.short`
   - handoff to the peer in both directions, normal and `--dark`; measure blink duration
   - buttond restart: claims re-established
4. **Rotation:** 90° with touch mapping checked at the corners.
5. **Cost:**
   - 1 Hz static page: CPU and wakeups
   - animation at 30 fps vs 60 fps
   - full-screen redraw at 30 fps
   - rendered binary size

## Final acceptance record (2026-10-03)

The guided device session passed the checklist above: colours, rapid taps,
three contacts, gestures in both directions, five power/touch cycles,
brightness including smooth slider dragging, rotation 90, clean and SIGKILL
exit on/off, start-dark, normal and dark handoffs both ways, and buttond restart.
The user reported no tearing at 30 or 60 fps. Notch-safe content clearance was
visually calibrated to 96 physical pixels. Normal handoffs measured 563 and
561 ms from touch suspend start to the peer's screen-ready log; this is a
software timing proxy for the blink, not an optical measurement.

| Workload | Measured fps | Steady process CPU | Wakeups/s |
| --- | --- | --- | --- |
| Static 1 Hz page | 1 | 0.1% | 2 |
| Animation, 30 target | 29–31 | about 2.4% | 31–61 |
| Animation, 60 target | 59–60 | about 4.3% | 60 |
| Full redraw, 30 target | 30 | about 37.5% | 31 |

Device demo: 896,376 bytes stripped. Final LZMA ramdisk: 32,940,834 bytes,
14,508,032-byte loader margin. Archive extraction independently verified that
chefui-demo and buttond match the reviewed live binaries. A fresh temporary
boot of the final image verified the installed hashes and 60 fps animation. Protected baseline
kernel, boot image and gzip ramdisk are unchanged. WLAN readiness/device
presence passed; network association was not part of this UI acceptance.
See the [dated evidence summary](../../logs/ui-platform-2026-10-03-live-summary.md)
and [current operating guide](../features/ui-platform.md).

## Out of scope this phase

- Ride UI, settings UI, recorder, maps.
- GPU acceleration and multi-threaded rendering.
- Asynchronous commits (non-blocking pan).
- Renaming `/run/fblog.off`.
- Removing `libinput`/`libinput-tools` from the rootfs. **Done 2026-10-04** (user decision 2026-10-03), see the [build log](../build-log.md). They were only needed for the Wayland plan and manual diagnostics, and `libinput-tools` pulled in Python. The input coldplug and the libinput diagnostics in the display guide went with them.

## Working rules for implementation and review

The coordinator owns device access, live tests and commits. Implementation and review sessions work only on the host:

- no phone access, no kernel or DT changes
- no flashing, no commits
- keep `out/` baseline artifacts unchanged

Review checks this spec, the framebuffer contract and the prototype findings, runs all host tests, and reports blocking issues before any live run.
