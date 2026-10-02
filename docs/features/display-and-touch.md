# Display, touch, and log screen

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior and source

Display/touch and `fblog` handoff were live-verified on 2026-09-18. `tools/fbdev.h` owns the shared framebuffer contract; `tools/fbtouch.c` is the reference foreground client; `tools/fblog/fblog.c` is the background log viewer. The generated `font9x15.h` uses the public-domain X11 misc-fixed font and can be regenerated with `tools/fblog/mkfont.py`.

The baseline and ride images start `fblog` from inittab. It displays userspace `/dev/kmsg` messages and kernel errors (`KERN_ERR` or worse), newest at the bottom, with kernel release, uptime, and battery percentage. It opens no input devices and does not respond to touch.

## Use

Run these in the phone shell:

```sh
echo "probe: step 3" > /dev/kmsg
fbtouch info
fbtouch show -t 60
fbtouch bl 96
```

`fbtouch show` borrows the display, draws a test pattern, and paints/logs touch contacts. On exit or death, `fblog` resumes, unblanks, and restores its backlight. `fbtouch bl` logs a marker that causes a commit; the 1 Hz heartbeat is the fallback for brightness writers that do not log.

To idle the screen from the shell:

```sh
touch /run/fblog.off
kill $(pidof fblog)
```

To wake it:

```sh
rm /run/fblog.off
kill $(pidof fblog)
```

In the baseline image, a short power-button press performs that toggle. The ride image has no `buttond`; use its [HTTP screen control](ride-logging.md).

Edit `fblog` arguments in the applicable `etc/inittab`: `-b` brightness (default 96), `-s` glyph scale (default 2, 59×68 cells), `-k` maximum kernel log level, or `-a` all kernel messages.

## Read-only inventory and libinput diagnostics

The baseline overlay includes `display-touch-inventory` (also inherited by the
ride image). It reports every framebuffer's cached name, modes, virtual geometry,
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

The rootfs packages `libinput` and `libinput-tools`. Boot coldplugs network devices
first, keeping `/run/udev-ready` solely for NetworkManager. Input-only coldplug
then runs in a background task (trigger TERM after 4 seconds, KILL after one more;
settle timeout 5 seconds), without delaying the handoff to init. Its independent
`/run/input-udev-ready` marker means classification completed. Settle watches the
global udev queue, so unrelated events can cause input readiness to fail. Failure
is logged and does not remove network readiness. Generic eudev `input_id`
classifies this touchscreen; no device-specific rule or calibration is installed.

After input readiness, manual diagnostics include:

```sh
udevadm info --query=property --path=/sys/class/input/event1
libinput list-devices
libinput debug-events --device /dev/input/event1
```

Select the event node by the inventory identity/capabilities; event numbers can
change. These libinput commands are separate manual diagnostics that open input
devices; `debug-events` consumes live events until interrupted. They are not
started automatically and do not replace the display client or button daemon.

Host checks: `make -C tools test-display-inventory` exercises saved metadata
fixtures with hazardous paths represented by FIFOs, and mocks the actual ABS
command's open/ioctl/close calls including failure cleanup. `python3
tools/tests/test_wifi_nm.py` checks independent net/input failure paths. Device
validation remains necessary: compare boot-to-shell duration with the baseline,
check input coldplug start/ready log timestamps and both readiness markers,
verify `ID_INPUT_TOUCHSCREEN=1`, then compare inventory ranges with libinput
classification and live touch behavior. Host checks cannot establish boot timing
or physical controller health.

## Framebuffer and touch contract

- **The display lives only while something holds `/dev/fb0` open.** mdss_fb's first `open()` unblanks, its last `close()` sets the backlight to 0 and powers the panel down (`mdss_fb_release_all`), and there is no `fb_write`: `cat > /dev/fb0`, busybox `fbsplash` or any open/draw/close tool shows nothing and then turns the screen off. The contract is open → `FBIOBLANK UNBLANK` → `mmap` → draw → `FBIOPAN_DISPLAY` → stay resident (`fbtouch.c`). No fbcon: mdss_fb has no fillrect/copyarea/imageblit.

- **A backlight write only reaches the WLED at the next frame commit.** Write `/sys/class/leds/lcd-backlight/brightness` (the mdss LED, 0–255 → 1–4095; never `leds/wled` directly), then commit a frame. After a reopen the level is otherwise parked forever: the last close saved 0 as the level to restore, the unblank restored it and blocked updates "until the first kickoff", and `mdss_fb_update_backlight()` returns early with nothing to apply, so the block is never lifted. Symptom: DSI up, panel answers `0x9C`, screen black (`leds/wled/brightness` reads 0). Android's HWC commits continuously and never notices. Live-verified 2026-09-18.

- **The touch IC is a TDDI on the panel's rails and is slaved to fb blank events**: `FBIOBLANK POWERDOWN`/`UNBLANK` through the fb core suspend/resume the NT36xxx driver (deep sleep, then bootloader reset + fw-reset wait); the last-close power-down does *not* go through the notifier, so the IC loses power while the driver still thinks it is awake, I2C fails (`CTP_I2C_READ error, ret=-3`, `/proc/nvt_fw_version` → `EAGAIN`) and the next open logs `Touch is already resume`, skips the reset, and delivers no events. Always `FBIOBLANK POWERDOWN` before the last close (`fbtouch` does). Never write `/proc/NVTflash` or any `doreflash`/`forcereflash` node: that programs the touch IC's flash.

- **Two fb clients must not draw at once, and the second open does not unblank.** mdss_fb refcounts opens, so with `fblog` resident nothing else's `close()` is ever the last close — a foreground client's `FBIOBLANK POWERDOWN` still turns the panel off through the notifier, and the resident client has to `UNBLANK` again afterwards. `tools/fbdev.h` makes this a protocol: foreground (`fbtouch show`) takes `flock(LOCK_EX)` on `/run/fb0.lock` before opening fb0 (bounded 10 s wait, then a `lock: … held by another screen client` failure); background (`fblog`) takes `LOCK_SH|LOCK_NB` only for the milliseconds it draws and commits, treats `EWOULDBLOCK` as "screen borrowed" (stop drawing, munmap, retry every 250 ms) and resumes with UNBLANK + remap + redraw + backlight commit. Locks die with their holder, so a killed client hands the screen back by itself. Host-tested end to end in `tools/tests/test_fblog.c` and live-verified 2026-09-18 (fblog, live): pause 3 ms after the lock, resume 11 ms after the close with panel-on + touch resume, second mmap fine.

- **Don't read `/proc/nvt_fw_version` (or the `buildid`/`ic_ver` sysfs) while the panel is off** — i.e. whenever nothing holds `/dev/fb0` open, or after a `POWERDOWN`. The proc `open()` does live I2C reads (`nvt_get_fw_info()`), and with the TDDI on the powered-down panel rails that is four `i2c-msm-v2 … check core_clk` timeouts, 46 `BUS ERROR` lines, ten `CTP_I2C_*: error, ret=-3`, four `FW info is broken`, a fallback to `abs_x_max=1080, abs_y_max=2246` (only a more permissive IRQ-side clamp; the evdev range set at probe stays 720x1600), `PID=0000`, the NVT ESD check switched off until the next touch report, and `EAGAIN` to userspace. So `EAGAIN` there is the *normal* answer from a correctly suspended IC and by itself does not indicate the un-suspended power-loss case above; the sign of that case is `Touch is already resume` on the next open of a panel that had gone dark. (`Touch is already resume` on the very first open after boot is expected instead: the driver has been awake since probe and the cont-splash panel was never blanked.) Live-verified 2026-09-18, second boot.

The panel is 1080×2246, but the replacement NT36525 touch controller reports 720×1600. Scale from evdev ABS ranges, never assume raw coordinates are panel pixels. Trim IDs identify the IC; live PID/buildid reads are not stable identity. See the [device reference](../device.md).

## Modify and verify

Use `fbdev.h` for new clients and preserve its locking, commit, and blank-before-close rules. Run `make -C tools test`; `tools/tests/test_fbtouch.c` covers packing, patterns, multitouch, and coordinate mapping, and `test_fblog.c` covers parsing, rendering, and handoff behavior. Rebuild the initramfs and boot image.

Live checks: initial boot display, foreground borrow/return, client death, blank/unblank and reopen, brightness, then touches in all corners, a stroke, and three fingers. Check for panel-dead or I2C faults. The [2026-09-18 build-log entries](../build-log.md) retain evidence and expected diagnostic distinctions. The [UI plan](../next-steps/ui-and-ride-app.md) covers the future display stack.
