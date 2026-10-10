# Shared evdev helper (handoff)

**Archived 2026-10-11.** Completed and live verified on the date recorded below. Current behavior belongs in the linked feature guide; application follow-ups belong in [next steps](../next-steps/README.md). The original design and process instructions below are historical, not active assignments.

[Archive index](README.md)

**Status:** done. Implemented, host-tested and live-verified 2026-10-04
(see the [build log](../build-log.md)); kept as the design record.

## Goal

All input reaches userspace as raw evdev; there is no libinput (removed in
6d11969). Three programs read evdev and each carries its own copy of the
same plumbing:

| Program | Discovery | Parsing |
|---|---|---|
| `tools/chefui/touch.c` | `cu_touch_is_touchscreen` (INPUT_PROP_DIRECT + MT X/Y), lowest-numbered node, uint8 `TEST_BIT` | MT-B slot state, frame per SYN_REPORT, SYN_DROPPED discard + `EVIOCGMTSLOTS` resync |
| `tools/fbtouch.c` | `touch_open_one`/`touch_open`: first MT node in readdir order, single-touch ABS_X/Y fallback, `has_bit` | `mt_feed`: own MT-B decoder with dirty flags emitting DOWN/MOVE/UP, no SYN_DROPPED handling, BTN_TOUCH fallback |
| `tools/buttond.c` | `keydev_open`/`keydev_scan`: EV_KEY with our keys and no EV_ABS, every match, `has_bit` | key events only (gesture logic stays in buttond) |

Replace the duplicated discovery and MT-B slot decoding with one
header-only helper, `tools/evdev.h`, in the same style as `tools/fbdev.h`
(static functions, libc + `<linux/input.h>` only, no build-system changes
needed for single-file builds such as `mkinitramfs.sh`'s
`$MUSLCC ... tools/fbtouch.c`).

Non-goals: no libinput/libevdev/mtdev, no change to buttond's gestures,
claims, socket protocol, grab policy or power-off timing, no change to
chefui's frame queue, primary-pointer rules or coordinate mapping, no
device-side input daemon.

## `tools/evdev.h` contents

1. **Bit helpers.** One `evdev_bit()` over `unsigned long` arrays plus
   sizing macros, replacing `has_bit` (fbtouch, buttond) and `TEST_BIT`
   (chefui touch.c).
2. **Classification**, each taking an open fd, no side effects:
   - `evdev_is_touchscreen(fd)`: INPUT_PROP_DIRECT and
     ABS_MT_POSITION_X/Y (chefui's current rule).
   - `evdev_key_mask(fd, codes, n)`: bitmask of which of `codes` the
     device reports, 0 if it lacks EV_KEY or has EV_ABS (buttond's rule:
     the touch panel also reports BTN_TOUCH and must not be grabbed).
3. **Scan.** `evdev_scan(dir, cb, ctx)`: visits `event*` nodes in
   ascending numeric order (not readdir order), opens each
   `O_RDONLY|O_NONBLOCK|O_CLOEXEC`, calls `cb(fd, path, ctx)`. The
   callback keeps the fd (returns > 0, scan continues or stops as the
   callback says) or rejects it (scan closes it). Returns the number kept
   or -errno. Never grabs; grabbing stays the caller's decision.
   Paths sized for the existing 300-byte `path` fields.
4. **MT-B slot decoder.** Plain state, no queue:
   `struct evdev_mt { slots[EVDEV_MT_SLOTS] {id, x, y}; cur; nslots; dropping; drops; resyncs; }`
   - `evdev_mt_init(mt)`, `evdev_mt_setup(mt, fd)` (ABS_MT_SLOT max ->
     nslots, as chefui does today),
   - `evdev_mt_feed(mt, fd, ev)` per event, returning whether a complete
     frame is now in `mt->slots` (SYN_REPORT, or the SYN_REPORT after a
     SYN_DROPPED once resynced; on resync failure all contacts are
     released),
   - `evdev_mt_resync(mt, fd)` (`EVIOCGMTSLOTS` id/X/Y +
     `EVIOCGABS(ABS_MT_SLOT)`).
   Coordinates are never cleared on release; an unchanged per-slot value
   is never resent by the input core (touch.h explains why).
   `EVDEV_MT_SLOTS` stays 10 unless a caller needs more; fbtouch's 16 can
   drop to the shared value (the NT36525 reports 10).

## Callers after the change

- **chefui `touch.c`**: `cu_touch_feed` delegates slot decoding to
  `evdev_mt_feed` and pushes a frame when it says so;
  `cu_touch_resync` wraps `evdev_mt_resync`; `cu_touch_is_touchscreen`
  and `cu_touch_open_scan` use the shared classifier and scan. The public
  `touch.h` API and behaviour stay identical; `tests/test_touch.c` must
  pass unchanged (it wraps `ioctl`, which still works because the helper
  is compiled into the same translation unit).
- **fbtouch**: `touch_open` uses `evdev_scan` + `evdev_is_touchscreen`
  (`-i DEV` still opens exactly that node); `mt_feed`'s DOWN/MOVE/UP
  events come from diffing consecutive `evdev_mt` frames. fbtouch gains
  SYN_DROPPED recovery. The single-touch BTN_TOUCH/ABS_X/ABS_Y fallback
  is dropped (the only panel is MT-B; say so in the fbtouch header and
  `docs/features/display-and-touch.md`). `input-info` output is
  unchanged. `tests/test_fbtouch.c` keeps its DOWN/MOVE/UP expectations;
  single-touch cases are removed, a SYN_DROPPED case is added.
- **buttond**: `keydev_scan` uses `evdev_scan`; `keydev_open` uses
  `evdev_key_mask`; `EVIOCGRAB`, explicit `-i`-style paths, logging and
  everything after discovery unchanged.

## Verification

Host only; no phone actions (the coordinator does the live boot):

- `make -C tools test`, `make -C tools test-display-inventory`,
  `make -C tools/chefui test`, and `make -C tools/chefui device` with
  the musl toolchain.
- A new `tools/tests/test_evdev.c` (wired into `make -C tools test`)
  covering numeric scan order, classifier rules (touch vs keys vs a
  device with both EV_KEY and EV_ABS) with wrapped `ioctl`/`open`, and the
  decoder: multi-slot frames, release keeps coordinates, SYN_DROPPED
  discard + resync, resync failure releases all.
- `scripts/mkinitramfs.sh` builds fbtouch and buttond from the new
  sources (the single-file compile lines must still work).
- Record the work in `docs/build-log.md`, marked live boot pending, and
  update the feature docs that describe discovery
  (`display-and-touch.md`, `buttons-and-power-off.md`,
  `ui-platform.md` if it mentions touch discovery).

Live check afterwards (coordinator + user): boot, chefui demo touch
smooth with multi-finger and rapid taps, `fbtouch info`, buttond
power.short toggle and `buttond status` listing both key devices.
