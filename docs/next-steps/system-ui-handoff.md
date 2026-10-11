# System UI slice 1: kit, status bar, hub, Display page, boot

User request 2026-10-11: start the [UI plan](ui-and-ride-app.md) with
the system UI foundation from the round-1 Claude Design. This slice
builds the shared UI kit (tokens, fonts, icons, widgets), the live
status bar, the settings hub (P01), the Display page (P02), the screen
policy, and starts the app at boot.

Roles: `ui_impl` (Claude) implements, `ui_review` (Codex) reviews,
`ui_coord` (the coordinator) researches and talks to the user. Report
with `herdr agent prompt ui_coord "..."`. No em dashes anywhere (code,
docs, UI strings, messages, commits). Read first:
[UI platform guide](../features/ui-platform.md) (the binding app
contract), [display and touch](../features/display-and-touch.md)
(fb0 lock, fblog), [buttons](../features/buttons-and-power-off.md),
[storage](../features/storage.md#persistent-state) (`/data/v1` rules),
`scripts/phone.py` and `scripts/phone-boot.sh` before touching the phone.

## User decisions (2026-10-11)

- **First slice** is the system UI foundation (this document).
- **Power short press**: Back on every page except the hub; on the hub
  it turns the screen off. Menus also auto-sleep after the Display
  page's timeout (30 s, 1 min default, 5 min, Never). Power wakes.
- **Theme**: a manual setting Dark / Light / Auto; Auto switches to
  Light in bright sun from the ambient light sensor, with hysteresis.
- **Boot**: the app starts at boot (inittab respawn) and owns the panel;
  `fblog` is the fallback when it dies or is stopped.
- **Temperature**: dropped from the status bar (no ambient sensor; the
  thermistors read device heat). The left ear holds GPS, Bluetooth and
  Wi-Fi only.
- **Time zone**: a POSIX `TZ` string in the UI settings, default
  `IST-5:30`, no tzdata. A picker can come later.
- **Portrait only.** The landscape designs (L01 to L04) ignore the
  notch, which sits on a short edge at 90 degrees (80 px deep, 556 px
  long) and would cover content. Landscape waits for a design pass.
- Review: the reviewer is a Codex agent this time.

Not in this slice: the ALS-driven parts. `sensord` is manual opt-in
(nothing starts `sensors-up` at boot), so Auto brightness and theme
Auto are shown but disabled (muted, not tappable) with their
explanatory lines. They land with sensors boot integration. Pages other
than P01/P02 (Bluetooth, GPS, Wi-Fi, Sound, Time, Storage, About) are
later slices.

## Research results (coordinator, 2026-10-11)

Evidence: `~/chef-cyclo-evidence/system-ui-r1-20261011/` (outside git):
`design/` (the round-1 `.dc.html` sources, the design runtime and a
headless render script), `ref-png/` (all 18 portrait screens, dark and
light, 1:1 at 1080x2246), `proto/` (the hub prototype, its renders,
side-by-side comparisons, the tnum script and the D-Bus probe),
`fonts-src/` (Barlow TTFs with OFL, Lucide TTF with its licence and
codepoint map).

- **D-Bus: static basu works.** Alpine v3.24 community ships
  `basu-static` 0.2.1-r2 (sd-bus API, LGPL-2.1-or-later, built on musl
  1.2.6, the same version as our buildroot toolchain). A static aarch64
  probe linked with `libbasu.a -pthread -lrt -lm` (no libcap needed,
  614 KB unstripped) ran on the phone over SSH: system bus open 0.4 ms,
  NM properties 2.6 ms, BlueZ `GetManagedObjects` 1.5 ms. So chefui apps
  stay static; the bus fd joins `chefui_watch_fd()` (with
  `sd_bus_get_events`/`sd_bus_get_timeout`). NM `PrimaryConnection` is
  type `o`: read it with `sd_bus_get_property` and `"o"`, not the string
  helper (the probe got -ENXIO that way). On the host, use libsystemd's
  sd-bus (same API, `<systemd/sd-bus.h>`) or the fake state below.
  Static LGPL linking: the project ships its source, which satisfies
  relinking; record it in [building](../building.md#firmware-and-distribution).
- **Fonts.** Barlow's default figures are proportional (Barlow Medium
  digit advances 352 to 570 units); `tnum` is a GSUB feature and
  `lv_font_conv` ignores GSUB. `proto/freeze_tnum.py` (fontTools) remaps
  the cmap to the tnum glyphs before conversion: all digits then advance
  533 (Medium) / 475 (Condensed SemiBold). Use `lv_font_conv@1.5.3`
  with `--no-compress --bpp 4 --format lvgl`: compressed output renders
  nothing because `LV_USE_FONT_COMPRESSED` is off (log: `Couldn't get
  the bitmap of a glyph`). Sizes: Condensed 600 at 280 px is 135 KB for
  full ASCII compressed, 24 KB digits-only, so subset the display sizes.
  The prototype's 14 fonts were 1.3 MB of C source.
- **Icons.** `lucide-static@0.460.0` `font/lucide.ttf` through the same
  pipeline renders cleanly (codepoints in `font/info.json`, for example
  `locate-fixed` U+E1DB, `bluetooth-connected` U+E1B8, `wifi` U+E1AE,
  `chevron-right` U+E073). Lucide is ISC.
- **Prototype.** `proto/proto.c` builds the status bar and P01 on chefui
  (SDL offscreen, frame dumped from the DIRECT-mode draw buffer).
  `proto/cmp_p01_*.png` puts it next to the design render: layout,
  colours, type and icons match; the remaining difference is a few
  pixels of vertical drift (about 8 px by the last row) because LVGL
  label boxes use the font's ascent+descent, not CSS line-height. Match
  CSS line boxes explicitly (see below).
- **LVGL 9.6 deprecations.** `lv_obj_add_flag`/`remove_flag`/`has_flag`
  log a deprecation warning at runtime; use the dedicated
  `lv_obj_set_<flag>()` setters and `lv_obj_is_<flag>()` getters.
- **Warn colours** (oklch converted): dark `#eba66d`, light `#a24e10`.

## Design (adjust with reasons)

Code layout (suggested): `tools/chefui/kit/` for the shared kit
(library `libchefkit` for device, host and test), `tools/chefui/sys/`
for system state sources, `tools/chef-settings/` (or
`tools/chefui/apps/settings.c`) for the app, installed as
`/usr/bin/chef-settings`.

**Tokens** (`kit/theme.[ch]`): both palettes exactly as in the
[design contract](ui-and-ride-app.md#system-ui-design) (bg, surf, fg,
mut, line, line2, acc, acct, onacc, warn), spacing, radii (cards and
buttons 36 to 40, keys 24, pills), 3 px lines, target sizes (192 px
minimum, rows 216, master toggles 240, switches 204x108). Theme switch
at runtime: widgets take colours from shared `lv_style_t`s that the
theme updates and reports with `lv_obj_report_style_change`, so a switch
restyles without rebuilding screens.

**Fonts** (`kit/fonts/`): commit the source TTFs with their licences, a
generator script (fontTools tnum freeze, then pinned
`npx lv_font_conv@1.5.3`), and the generated `.c` files plus a header.
Body fonts: `0x20-0x7E,0xA0-0xFF,0x2013,0x2018-0x201D,0x2026,0x2212`.
Display sizes (150, 280): digits and the few symbols they need. Generate
only sizes that built screens use. The design uses about 30 distinct
sizes; snap a design size within 2 px to an existing one rather than
adding a font. Icon font sizes as used (24, 36, 56, 60, 64, 72, 84, 96
for this slice; check against the screens).

**Line boxes.** CSS places text in a line box of `font-size x
line-height` (`normal` for Barlow is about 1.2). Give text a container
of that height with the label vertically centred, or set the label
height and `text_line_space` explicitly, so stacked text matches the
reference to within a pixel or two.

**Widgets** (kit): status bar; page frame (192 px header with title,
168 px back target with `chevron-left` on non-hub pages); section label
(Condensed 600 40, letter spacing 0.12 em, upper case strings); hub
tile; list row (216 px, icon, title + subtitle, chevron); switch
(204x108 pill); segmented control (192 px cells, accent fill for the
active cell only); brightness slider (144 px track as in P02). Disabled
state: muted colours, no input.

**Status bar** on `chefui_root()` across the 96 px safe top, drawn in
the two ears beside the 556 px notch (contents within about 190 px of
each edge, at least 40 px clear of the notch). Left ear: GPS, Bluetooth,
Wi-Fi icons (36 px, gap 12). Right ear: battery outline (38 px high,
min 78 px, 3 px border, radius 12, fill at 22 % opacity, percentage
inside, `zap` 24 px when charging) and the time (Condensed 600 44).
Muted icon = searching or off. Warn colour only for battery at or below
15 % and not charging.

**State sources** (`sys/`), all on the chefui loop, none blocking:

- Battery: `/sys/class/power_supply/battery/{capacity,status}` and the
  online flags as powerd defines "unpowered"
  ([battery](../features/battery-and-charging.md)); refresh on
  power_supply uevents (netlink fd) with a slow timer as fallback.
- Time: wall clock with `TZ` from settings, a timer aligned to the
  minute; also redraw after a clock step (chrony steps once after the
  1970 boot clock).
- GPS: `/run/gps-manager.sock` `watch` (never `lease`: the settings app
  takes no lease). `OFF`/`FAILED` = off, `STARTING`/`ACQUIRING` =
  searching, `RUNNING` = fix. Tile detail: while `RUNNING`, read gpsd
  (`127.0.0.1:2947`, `?WATCH`) TPV `mode` and `eph` for "3D fix,
  +-N m" (use the design's `·` and `±` glyphs); disconnect gpsd when not
  `RUNNING`. Reconnect the manager socket with backoff.
- Wi-Fi: NM over basu: `WirelessEnabled`, wlan0 device state and the
  primary/active connection `Id`; `PropertiesChanged` signals, not
  polling.
- Bluetooth: BlueZ over basu: adapter `Powered`, count of `Device1`
  with `Connected`; `InterfacesAdded/Removed` and `PropertiesChanged`.
- Storage free: `statvfs("/data")` when `chef-state ours`, refreshed on
  hub entry. Hub row subtitles without a backing page yet (Sound, Time
  source, About) may show real values where cheap (`/run/chef-time`,
  the image stamp) or a static placeholder; say which in the guide.
- A fake source (`--fake-state FILE` or env) that sets every field,
  for host runs, tests and reference comparisons (design values: 14:16,
  GPS fix, BT 2, Wi-Fi HomeNet, 78 %, not charging).

**Settings** persist in `/data/v1/ui/settings` when `chef-state ours`
(otherwise `/run/ui/settings`, one kmsg line): brightness, theme mode,
auto-sleep, TZ. Same rules as `chef-state` files: short text,
versioned, last line sha256 of the lines before, temp file + fsync +
rename + directory fsync, a bad file ignored with a kmsg line. Writes
are debounced (for example 2 s after the last change, and at exit),
never per slider event. Add the path to the storage guide's table.

**Navigation and screen policy.** A page stack: hub at the root. Hub
tiles and rows other than Display open a placeholder page with the
standard header and back (so Back is exercised), until their slice.
Power short (via `chefui_on_button`): screen off -> return false
(buttond default wakes); on the hub -> return false (default turns it
off); on any other page -> pop and return true. Power long stays with
buttond (power off). Power double and the volume keys are not claimed
in menus. Auto-sleep: any touch resets an inactivity timer; at the
timeout call `chefui_screen_set(false)`; Never disables it. Wake shows
the page that was open.

**Boot.** Add an inittab `::respawn:` line for `chef-settings` after
the services it reads (it must tolerate NM, BlueZ, gps-manager and
`/data` not being ready yet and pick them up when they appear). fblog
stays resident and yields through the fb0 lock. If `chefui_init()`
fails, sleep about 5 s before exiting so respawn does not spin. A boot
that starts with `/run/fblog.off` present must stay dark (the platform
already does this). Document how to see the log screen (stop the app
over SSH; respawn brings it back) in the guide.

**Snapshots.** Add a platform capture that does not read the
write-combined fb mapping: dump the RAM shadow (device) or the DIRECT
draw buffer (host) as PNG or PPM, for example `chefui_snapshot(path)`
plus an app option `--snapshot PATH --snapshot-after MS`. Use it for
the comparison set: per screen and theme, reference PNG vs host SDL vs
device capture, side by side, with a small script under
`tools/chefui/` that writes the composites and a per-row offset report.

## Acceptance

- Host: `make -C tools/chefui test host host-test` plus new unit tests
  for the settings file (round trip, checksum failure, debounce),
  screen policy (power short on hub / page / screen off, auto-sleep,
  Never), page stack, the state sources against fakes (gps-manager
  socket lines, a fake NM/BlueZ on a private bus or a mocked layer,
  sysfs fixtures, uevent), theme switch restyling, and the fonts
  (generated files reproducible from the script, tnum advances equal).
- Comparisons: P01 and P02, dark and light, host and device, within a
  few pixels of the references in `ref-png/`; differences explained in
  the log.
- Device build is static aarch64 musl; image size growth recorded.
- Live on a RAM image (`scripts/phone-boot.sh`): app comes up at boot
  over fblog; status bar shows real battery, time in IST, Wi-Fi SSID,
  BT state, GPS off (and searching/fix while
  `gps-manager hold map` runs from SSH); changes appear without
  restarting (Wi-Fi down/up with nmcli, BT power off/on, plug/unplug
  USB); Display page brightness drag is smooth and persists across an
  app restart and a reboot; theme Dark/Light switches live and persists;
  auto-sleep at 30 s turns the screen off and Power wakes on the same
  page; power short on P02 goes back, on the hub turns the screen off;
  `kill -9` of the app returns the panel to fblog and respawn takes it
  back; boot with the screen-off flag stays dark; clean shutdown and
  power-off via a long press still work. CPU idle on the static hub
  near the platform baseline (about 0.1 %, two wakeups/s) with the
  D-Bus and gps-manager fds attached.
- Guided user steps (touching, watching the panel, plugging USB) go
  through the coordinator, using the numbered STEP format.

## Process

- RAM images and reboots are fine; no install in this slice unless the
  user asks. Ask the coordinator before any step that needs the user to
  touch or watch the phone.
- Never delete anything under `/data/v1` you did not create.
- Fetching packages (basu, fonts, lv_font_conv) is fine; pin versions
  and record sha256 in the setup script.
- Update the [UI platform guide](../features/ui-platform.md) (snapshot
  API), a new feature guide for the system UI app, the storage table,
  the [UI plan](ui-and-ride-app.md) (move done work out), the build log
  and a `logs/system-ui-slice1-<date>.txt` evidence log.
- Commit only after the reviewer approves. Do not push.
