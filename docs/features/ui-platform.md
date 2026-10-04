# UI platform

[Feature guides](README.md) · [Display and touch](display-and-touch.md) · [Buttons](buttons-and-power-off.md) · [Build and boot](../building.md) · [Acceptance checklist](../next-steps/ui-platform.md)

`tools/chefui/` is the shared fullscreen application platform, using pinned
LVGL v9.6.0 on the existing MDSS framebuffer and an SDL2 host backend. It owns
display access, touch, button claims, screen state, pacing and application
handoff. Applications use the same source on both backends. The public contract
is [chefui.h](../../tools/chefui/chefui.h); the demo is a verification tool and
is installed as `/usr/bin/chefui-demo`, without automatic inittab startup.

## Build and application loop

Initialize once per process, build the screen, then run the shared loop:

```c
#include "chefui.h"

int main(void)
{
    struct chefui_config cfg = { .app_name = "example" };
    if (!chefui_init(&cfg))
        return 1;
    lv_obj_t *title = lv_label_create(chefui_content_root());
    lv_label_set_text(title, "Example");
    return chefui_run();
}
```

`chefui_init()` returns the LVGL display or NULL after cleanup on failure.
`chefui_run()` sleeps in `poll()`, dispatching LVGL timers, input and watched
file descriptors. `chefui_quit(code)` requests clean shutdown; SIGTERM, SIGINT
and SIGHUP return `128 + signal`. Everything runs on one thread.
`chefui_watch_fd()`/`chefui_unwatch_fd()` add application sockets or other
nonblocking fds to the same loop (at most 16; the application owns those fds).

| Setting | Default and override |
| --- | --- |
| `refresh_ms` | 0 selects 16 ms, a 60 fps target; 33 ms targets 30 fps. Explicit periods and `chefui_refresh_period_set()` remain available. Display and animation cadence follow the requested period. |
| `rotation` | 0; 90/180/270 also supported with matching touch mapping. |
| `brightness` | 0 selects 96; requested levels clamp to 1..255. |
| `safe_top_px` | 0 selects calibrated 96 physical pixels; positive values override, -1 disables. |
| `backend` | AUTO selects the backend built into the library; FBDEV/SDL can be requested explicitly. |
| `host_zoom` | 0 selects 0.4; host `CHEFUI_ZOOM` can override it. |

```sh
make -C tools/chefui test device host host-test
```

Device output is the static musl `build/device/chefui-demo` and `libchefui.a`;
host output is `build/host/chefui-demo` and `libchefui-host.a`. SDL2 development
headers are required for host builds. Normal unit tests are independent of SDL;
`host-test` adds offscreen SDL layout, calibration and screen-resume checks.

## Content clearance and full-panel access

Use `chefui_content_root()` as parent for ordinary controls. It is transparent
and non-scrolling, covering `chefui_safe_area()` (inclusive logical bounds).
Use `chefui_root()` for full-panel backgrounds and diagnostics. Content roots
belong to their screen; request the current content root again after changing
screens. Returning to a screen reuses its root and updates its rectangle.
Applications lay out their own children within that rectangle.

The default **96 physical pixels** was visually calibrated by the user on
**2026-10-03**: the device logged `notch calibration: --safe-top 96 physical
pixels`. This measures comfortable usable content clearance, including a gap,
rather than the notch hardware depth. Physical top maps to logical
**top/left/bottom/right** at **0/90/180/270°**, using the same transform as the
pixel copy. Framebuffer geometry and touch coordinates stay full-panel. The
same content rectangle is used on the host.

`chefui_safe_top_set()` adjusts physical clearance at runtime (0 default,
-1 disabled, positive explicit; values below -1 or at/above physical height
return `-EINVAL`). It resizes the current screen's existing content root.
`chefui_safe_top_get()` returns the resolved inset. Calibration remains available:

```sh
chefui-demo --calibrate-notch --safe-top 96
```

The translucent purple band and green border mark reserved clearance. Guide
text starts 4 pixels inside the content rectangle. Volume Up/Down (host Up/Down)
adjust clearance by 4 pixels and log the resolved `--safe-top N`; the slider
continues controlling brightness. Find the lowest clearance that leaves the
whole guide readable beneath the obstruction with a comfortable gap, then
check landscape. Adjustments are not saved automatically. Demo `--safe-top 0`
disables clearance; unlike the CLI, API zero selects the default. Peer handoffs
preserve the demo's chosen inset. Corner targets and contact dots stay full-panel.

## Callback pattern and brightness

Callbacks record desired state and return promptly. Preserve discrete press,
release, click and gesture events in order: dropping a release can leave a
control stuck, and merging taps loses actions. Coalesce replaceable continuous
updates, such as brightness, drag position and map zoom: the next application
update uses the latest desired value, rather than replaying every intermediate
position. A gesture's begin/end remains discrete even when intermediate scale
or position values are replaceable.

For application position/zoom state, keep pending values and apply them to LVGL
objects in a bounded update timer at the requested cadence. LVGL invalidation
then feeds the scheduled frame. Avoid expensive work, blocking I/O and direct
display commits in input callbacks; join nonblocking fds to the shared loop.
The platform owns framebuffer copying and the blocking vsync pan at the frame
boundary. Explicit screen power transitions and handoff are synchronous
lifecycle operations, not continuous slider work.

For brightness, `chefui_brightness_set()` already implements this pattern:

```c
static void brightness_changed(lv_event_t *event)
{
    int requested = lv_slider_get_value(lv_event_get_target(event));
    chefui_brightness_set(requested); /* record/coalesce; no vsync wait */
}
```

The getter reports the requested level. While on, the backend stores the latest
level and invalidates one pixel so even a brightness-only change on a static
page gets a scheduled commit. Immediately before that frame's normal pan, it
writes the latest value to `/sys/class/leds/lcd-backlight/brightness`; animation
and brightness share the commit. Deferred write errors are logged. While off,
requests only store the desired level; screen-on writes it before the first
full frame. Never write `leds/wled` or issue an extra pan per slider callback.

This avoids monopolizing the input drain: a previous per-event write/pan path
blocked about 16 ms for every slider update and dropped rendered fps to about 9
live. Host tests now feed thousands of slider value events with a mocked 16 ms
pan while checking frame progress, latest-level commits and zero request-time
pans. The user confirmed smooth slider dragging after the fix on 2026-10-03.

## Display, touch, buttons and lifecycle

The [framebuffer contract](display-and-touch.md#framebuffer-and-touch-contract)
is binding: take `/run/fb0.lock` exclusively before opening fb0, never change
its format with `FBIOPUT_VSCREENINFO`, never read its write-combined mapping,
and POWERDOWN through the fb notifier before closing. Rendering uses cached
XRGB8888 RAM, copy/swizzle into the back page, then `FBIOPAN_DISPLAY`. Failed
pans mark both pages stale for recovery on a subsequent frame.

Touch is discovered by direct-input properties and MT axes: the lowest-numbered
such `/dev/input/event*` node. Discovery and MT-B slot decoding are the shared
`tools/evdev.h` (also used by `fbtouch` and `buttond`). The reader preserves
slot coordinates across releases, delivers complete `SYN_REPORT` frames, and
resyncs after `SYN_DROPPED`. The first contact remains the primary pointer until
it lifts; a new primary begins only after all contacts are up. Multitouch events
arrive as `LV_EVENT_GESTURE`; use `lv_event_get_gesture_type(event)` and the
matching LVGL gesture value accessor. `chefui_on_touch()` reports active contacts
in logical coordinates. The backend explicitly sets LVGL's 0.2 radian rotation
threshold, avoiding v9.6's uninitialized zero default stealing pinch/swipe.

`power.short` is always claimed from buttond. The application button callback
returns true when handled; otherwise the default toggles the screen.
`power.long` stays with buttond unless explicitly claimed; Power+VolDown is
reserved. Reconnection retries once per second and reclaims configured gestures.
Host keys are P/Shift+P/Ctrl+P for power short/long/double, Up/Down for volume
short, and Shift+Up/Down for volume long. Host mouse supplies a single contact.

`/run/fblog.off` means the screen is deliberately off system-wide. Screen-off
POWERDOWNs, creates the flag, releases touch contacts and pauses rendering.
Application timers/fds continue; animations pause best effort until screen-on.
Screen-on removes the flag, UNBLANKs, writes brightness, resyncs touch and
schedules a full redraw. Starting with the flag present takes the lock without
opening fb0. Exit leaves the flag unchanged. Fblog honors it after a borrow and
waits for removal with inotify, or a bounded polling fallback. Buttond's default
wake removes the flag only; default off creates it before signalling fblog to
blank and exit. If fblog is absent or exiting, its init respawn observes the
current flag. Wake does not spawn a second daemon; without init supervision,
fblog must be started separately.

`chefui_handoff_exec(path, argv, dark)` powers down and closes fb0, retains the
lock across exec in `CHEFUI_LOCK_FD`, and lets the peer adopt it without an
unlocked gap. Normal handoff removes the flag; dark handoff creates it. Failure
restores the screen state and reconnects buttond. Claims drop during handoff
until the peer claims, so a power press in that window uses buttond's default.
The host uses ordinary exec and does not simulate a dark handoff across exec.

## Pacing, diagnostics and verification

Default cadence targets 60 fps; actual throughput depends on rendering, copying
and pan. Both animation and display timers must match: display16/animation33
previously limited live animation to 31 fps. LVGL timestamps timer callbacks
before execution and subtracts elapsed work from its next wait, including a
blocking pan. A live reviewed run reached 60 fps, about 4.2% CPU and 16 ms pan,
with user-confirmed smooth animation; this is not a guarantee for other workloads.
A static 1 Hz label remains about two wakeups/second in the host fbdev simulation;
SDL window event handling has its own wakeups.

`CHEFUI_STATS=1` logs fps, process CPU, pixels/copy time, pan mean/max and wakeups
to stderr once per second. Device lifecycle notes also go to `/dev/kmsg` as
`chefui[app]:`; host logs only to stderr. Demo options include `-t secs`, `-a`
animation, `-F` full invalidation, `-r`, `-f 30|60`, `-b`, `--peer` and `--dark`.

As reported by the coordinator on 2026-10-03, live checks passed LZMA boot,
RGB colours, rapid taps/corners, three contacts, five power/touch cycles,
brightness, pinch/swipe/rotation in both directions, 96 px visual clearance,
horizontal90 operation, smooth 60 fps animation and smooth slider dragging.
The remaining lifecycle/off-state, dark handoff, buttond restart and tearing/cost
matrix subsequently passed. Static cost is about 0.1% CPU and two wakeups/s;
animation costs about 2.4% CPU at 30 fps and 4.3% at 60 fps; full redraw at
30 fps costs about 37.5% CPU. Normal handoff software timing is 561–563 ms.
Host suites, independent review, archive verification and guided live checks are
recorded in the [final acceptance record](../next-steps/ui-platform.md#final-acceptance-record-2026-10-03).

Resolved live issue (2026-10-03): dark SIGKILL/default wake blinked because
buttond removed the flag, waking fblog, then sent SIGTERM and blanked it until
init respawn. The integrated unlink-only wake correction above passes host
inotify/fallback and next-respawn tests and independent review. The user confirmed
a single wake stays on without blinking in the coordinator live retest. See the
[wake evidence](../../logs/ui-platform-2026-10-03-wake-blink.txt).
