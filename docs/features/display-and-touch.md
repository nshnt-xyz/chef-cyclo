# Display, touch, and log screen

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior and source

Display/touch and the `fblog` handoff (the lock protocol, still used by `fbtouch` and the chefui apps) were live-verified on 2026-09-18. `tools/fbdev.h` owns the shared framebuffer contract and `tools/evdev.h` the shared evdev discovery and multitouch slot decoding; `tools/fbtouch.c` is the reference foreground client; `tools/fblog/fblog.c` is the background log viewer (a debug tool, not in the image). The generated `font9x15.h` uses the public-domain X11 misc-fixed font and can be regenerated with `tools/fblog/mkfont.py`.

The image does not run `fblog`; the [system UI](system-ui.md) owns the panel from boot. `fblog` stays as a debug tool, see [below](#fblog-debug-tool-not-in-the-image).

## Use

Run these in the phone shell:

```sh
echo "probe: step 3" > /dev/kmsg
fbtouch info
fbtouch show -t 60
fbtouch bl 96
```

`fbtouch show` borrows the display, draws a test pattern, and paints/logs touch contacts. On exit or death a hand-run `fblog` resumes, unblanks, and restores its backlight. `fbtouch bl` logs a marker that causes a commit; the 1 Hz heartbeat is the fallback for brightness writers that do not log.

### The screen-off flag

`/run/chef/screen/off` is the system-wide **"screen deliberately off"** flag ([runtime files](runtime-files.md)). Everything that owns the screen follows it:

- Fullscreen [chefui](ui-platform.md) applications create the flag when they turn the screen off (after `POWERDOWN`) and remove it when they turn it on (before `UNBLANK`). Started with the flag present, they take the screen lock but do not open fb0 until their first screen-on. On exit they leave the flag as it is.
- `buttond`'s default short power press (used only while no chefui app has claimed `power.short`) creates or removes the flag and logs that, nothing else: it signals no process. With no chefui app running nobody holds fb0, so the panel is already off; the next screen owner honours the flag. The system UI claims the press while it runs.
- A hand-run `fblog` watches the flag itself, with an inotify watch of the flag's directory (the quiet `/run/chef/screen`; a 250 ms recheck if inotify is unavailable). Started with the flag present it idles without opening `/dev/fb0` (panel and touch IC stay powered down). While lit, the flag appearing makes it do `FBIOBLANK POWERDOWN`, close fb0 and idle; the flag's removal wakes it and it lights the panel again. When it resumes after a foreground client releases the screen lock it checks the flag first and, if the client left the screen off, does `FBIOBLANK POWERDOWN`, closes fb0 and idles.

A running chefui application does not watch the flag: for it the flag only takes effect when it starts. Create it before a restart to start dark, remove it to start lit.

```sh
touch /run/chef/screen/off    # the next screen owner starts dark (a running fblog goes dark)
rm /run/chef/screen/off       # the next screen owner starts lit (a running fblog lights up)
```

A short power-button press is claimed by the system UI, which toggles the screen itself.

### fblog (debug tool, not in the image)

`tools/fblog/fblog.c` tails `/dev/kmsg` onto the panel: userspace lines, plus kernel errors (`KERN_ERR` or worse), newest at the bottom, with kernel release, uptime and battery percentage. It opens no input devices and does not respond to touch.

The image has not started it since 2026-10-17: with the system UI holding the screen lock from boot it spun one core at 100 % (about 96 mA of battery current); the loop is fixed in the source (`next_timeout_ms()` rechecks the lock every 250 ms while paused). It is still built and host-tested by `make -C tools`. To use it on a debug boot, build it with the musl cross compiler, push it and start it detached:

```sh
toolchain/aarch64-musl/bin/aarch64-buildroot-linux-musl-gcc -Wall -Wextra -O2 -static -o out/fblog tools/fblog/fblog.c
scripts/phone.py push out/fblog
scripts/phone.py run 'setsid /run/push/fblog </dev/null >/dev/null 2>&1 & true'
```

Stop it with `kill $(pidof fblog)`. Arguments: `-b` brightness (default 96), `-s` glyph scale (default 2, 59×68 cells), `-k` maximum kernel log level, or `-a` all kernel messages. It yields the screen to any foreground client through `/run/chef/screen/fb0.lock`, so stop the system UI first (see [system-ui.md](system-ui.md)) to see its log.

## Read-only inventory

The overlay includes `display-touch-inventory`. It reports every framebuffer's cached name, modes, virtual geometry,
bits per pixel and stride, then every evdev device's cached identity, properties
and capability bitmaps. It uses BusyBox tools and the packaged `fbtouch input-info`
command for all advertised ABS ranges, including slots and tracking IDs:

```sh
display-touch-inventory
display-touch-inventory --metadata-only
fbtouch input-info -i /dev/input/event1
```

The inventory never opens `/dev/fb0`, reads vendor panel/touch attributes
(including `msm_fb_panel_status`, NVT proc nodes, `buildid` or `ic_ver`), changes
brightness, reads events or grabs input devices. Default ABS queries open evdev
read-only/nonblocking and use only `EVIOCGBIT`/`EVIOCGABS`. NT36xxx has no input
open/close callbacks; other input drivers may have them. `--metadata-only` skips
all evdev opens. Missing metadata or failed ABS queries are printed as unavailable;
the script continues through the remaining devices and exits successfully. The
standalone `fbtouch input-info` returns 1 on a failed query and 64 without `-i`.

`--root DIR` reads a saved fixture tree under `DIR/sys/class` and implies
metadata-only. Identity and capabilities come from input-core metadata, not live
firmware identity or a controller health test. Capability/property output is raw
kernel hexadecimal bitmaps: `INPUT_PROP_DIRECT` is property bit 1. ABS codes are
hexadecimal: X/Y `00/01`, MT_SLOT `2f`, MT_POSITION_X/Y `35/36`, MT_TRACKING_ID `39`.
Ranges include minima, maxima, fuzz, flat and resolution; MT X/Y alone does not
establish protocol B. Framebuffer geometry may be virtual; use the cached modes
where provided for visible dimensions. Pixel channel layout is not available
through these generic attributes, so no framebuffer ioctl is attempted.

Boot coldplugs only network devices, for NetworkManager (`/run/chef/udev-ready`); input
devices are not coldplugged and carry no udev classification. Nothing on the phone
needs it: chefui, `fbtouch` and `buttond` read evdev directly through
`tools/evdev.h`, which visits `/dev/input/event*` in numeric order (event2 before
event10) and classifies by capabilities. chefui and `fbtouch` take the
lowest-numbered touchscreen (`INPUT_PROP_DIRECT` plus `ABS_MT_POSITION_X/Y`;
`fbtouch -i DEV` opens exactly that node, which only needs the MT axes), and
`buttond` every device with power/volume keys and no ABS axes. Touch is multitouch
protocol B only: `fbtouch`'s single-touch `BTN_TOUCH`/`ABS_X`/`ABS_Y` fallback was
dropped (the only panel is MT-B), and `fbtouch show` now recovers from
`SYN_DROPPED` like chefui (discard to the next `SYN_REPORT`, resync with
`EVIOCGMTSLOTS`, release every contact if that fails; its summary then reports the
drop count). libinput was removed from the rootfs on 2026-10-04. Select the event
node by the inventory identity/capabilities; event numbers can change. For live
touch checks, use `fbtouch show` (above) or `chefui-demo` ([UI
platform](ui-platform.md)).

Host checks: `make -C tools test-display-inventory` exercises saved metadata
fixtures with hazardous paths represented by FIFOs, and mocks the actual ABS
command's open/ioctl/close calls including failure cleanup. Host checks cannot
establish physical controller health.

## Framebuffer and touch contract

- **The display lives only while something holds `/dev/fb0` open.** mdss_fb's first `open()` unblanks, its last `close()` sets the backlight to 0 and powers the panel down (`mdss_fb_release_all`), and there is no `fb_write`: `cat > /dev/fb0`, busybox `fbsplash` or any open/draw/close tool shows nothing and then turns the screen off. The contract is open → `FBIOBLANK UNBLANK` → `mmap` → draw → `FBIOPAN_DISPLAY` → stay resident (`fbtouch.c`). No fbcon: mdss_fb has no fillrect/copyarea/imageblit. Since kernel #23 the last close skips Motorola's `bl_shutdown_dcs` backlight DCS (0x51, then a 500 ms `msleep`) when the panel is already off (`mdss_fb_is_power_on()`): it used to hit a powered-down DSI link with a `mdss_dsi_cmd_dma_tx: wait for dma_comp timed out` and sometimes a `clk_branch_wait` WARN when the UI blanked and then exited. With the panel lit at the last close (a crash) the DCS is sent as before.

- **A backlight write only reaches the WLED at the next frame commit.** Write `/sys/class/leds/lcd-backlight/brightness` (the mdss LED, 0–255 → 1–4095; never `leds/wled` directly), then commit a frame. After a reopen the level is otherwise parked forever: the last close saved 0 as the level to restore, the unblank restored it and blocked updates "until the first kickoff", and `mdss_fb_update_backlight()` returns early with nothing to apply, so the block is never lifted. Symptom: DSI up, panel answers `0x9C`, screen black (`leds/wled/brightness` reads 0). Android's HWC commits continuously and never notices. Live-verified 2026-09-18.

- **The touch IC is a TDDI on the panel's rails and is slaved to fb blank events**: `FBIOBLANK POWERDOWN`/`UNBLANK` through the fb core suspend/resume the NT36xxx driver (deep sleep, then bootloader reset + fw-reset wait); the last-close power-down does *not* go through the notifier, so the IC loses power while the driver still thinks it is awake, I2C fails (`CTP_I2C_READ error, ret=-3`, `/proc/nvt_fw_version` → `EAGAIN`) and the next open logs `Touch is already resume`, skips the reset, and delivers no events. Always `FBIOBLANK POWERDOWN` before the last close (`fbtouch` does). chefui apps record a lit panel in `/run/chef/screen/fb0.lit` and power-cycle once after a dead owner (see the [UI platform guide](ui-platform.md)); `fbtouch` does not, so after a crashed chefui app run `fbtouch show -c 5` once if you use fbtouch first. Never write `/proc/NVTflash` or any `doreflash`/`forcereflash` node: that programs the touch IC's flash.

- **Two fb clients must not draw at once, and the second open does not unblank.** mdss_fb refcounts opens, so with a resident second client (such as a hand-run `fblog`) nothing else's `close()` is ever the last close: a foreground client's `FBIOBLANK POWERDOWN` still turns the panel off through the notifier, and the resident client has to `UNBLANK` again afterwards. `tools/fbdev.h` makes this a protocol: foreground (`fbtouch show`) takes `flock(LOCK_EX)` on `/run/chef/screen/fb0.lock` before opening fb0 (bounded 10 s wait, then a `lock: … held by another screen client` failure); background (`fblog`) takes `LOCK_SH|LOCK_NB` only for the milliseconds it draws and commits, treats `EWOULDBLOCK` as "screen borrowed" (stop drawing, munmap, retry every 250 ms) and resumes with UNBLANK + remap + redraw + backlight commit. Locks die with their holder, so a killed client hands the screen back by itself. Host-tested end to end in `tools/tests/test_fblog.c` and live-verified 2026-09-18 (fblog, live): pause 3 ms after the lock, resume 11 ms after the close with panel-on + touch resume, second mmap fine.

- **Don't read `/proc/nvt_fw_version` (or the `buildid`/`ic_ver` sysfs) while the panel is off**: i.e. whenever nothing holds `/dev/fb0` open, or after a `POWERDOWN`. The proc `open()` does live I2C reads (`nvt_get_fw_info()`), and with the TDDI on the powered-down panel rails that is four `i2c-msm-v2 … check core_clk` timeouts, 46 `BUS ERROR` lines, ten `CTP_I2C_*: error, ret=-3`, four `FW info is broken`, a fallback to `abs_x_max=1080, abs_y_max=2246` (only a more permissive IRQ-side clamp; the evdev range set at probe stays 720x1600), `PID=0000`, the NVT ESD check switched off until the next touch report, and `EAGAIN` to userspace. So `EAGAIN` there is the *normal* answer from a correctly suspended IC and by itself does not indicate the un-suspended power-loss case above; the sign of that case is `Touch is already resume` on the next open of a panel that had gone dark. (`Touch is already resume` on the very first open after boot is expected instead: the driver has been awake since probe and the cont-splash panel was never blanked.) Live-verified 2026-09-18, second boot.

The panel is 1080×2246, but the replacement NT36525 touch controller reports 720×1600. Scale from evdev ABS ranges, never assume raw coordinates are panel pixels. Trim IDs identify the IC; live PID/buildid reads are not stable identity. See the [device reference](../device.md).

## UI content clearance

The chefui platform preserves full-panel framebuffer and touch coordinates.
Applications place ordinary controls under `chefui_content_root()` and use
`chefui_root()` for full-panel backgrounds. A physical-top inset rotates with
the display (logical top/left/bottom/right for 0/90/180/270°). The default **96 physical pixels**
was visually calibrated by the user on **2026-10-03**, with device log
`notch calibration: --safe-top 96 physical pixels`. This measures usable content
clearance with a gap, not the hardware notch depth. It remains configurable
through `chefui_config.safe_top_px` (0 selects the default, -1 disables it). The demo's
`--calibrate-notch --safe-top N` guide adjusts clearance by 4 px with volume
keys and logs the chosen value without saving it. See the
[calibration procedure](../archive/ui-platform.md#safe-content-and-notch-calibration).
Corner targets and contact dots still cover the full panel. Chefui defaults
to a 16 ms refresh period (60 fps target); the demo retains `-f 30`, and apps
can set an explicit refresh period. Rendering/copy/pan can limit actual rate.
Brightness requests coalesce into the next scheduled frame; a minimal
invalidation guarantees a commit even on a static page. Input callbacks do not
wait for a brightness-only pan. While off, the requested level is retained and
written before the first screen-on frame.

## Modify and verify

Use `fbdev.h` for new clients and preserve its locking, commit, and blank-before-close rules; use `evdev.h` for input discovery and MT-B decoding. Run `make -C tools test`; `tools/tests/test_evdev.c` covers numeric scan order, the classifiers and the slot decoder including `SYN_DROPPED` resync, `tools/tests/test_fbtouch.c` covers packing, patterns, down/move/up tracking, and coordinate mapping, and `test_fblog.c` covers parsing, rendering, and handoff behavior. Rebuild the initramfs and boot image.

Live checks: initial boot display, foreground borrow/return, client death, blank/unblank and reopen, brightness, then touches in all corners, a stroke, and three fingers. Check for panel-dead or I2C faults. The [2026-09-18 build-log entries](../build-log.md) retain evidence and expected diagnostic distinctions. The [UI plan](../next-steps/ui-and-ride-app.md) covers the future display stack.
