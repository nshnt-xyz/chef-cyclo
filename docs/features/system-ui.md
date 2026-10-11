# System UI

[Feature guides](README.md) · [UI platform](ui-platform.md) · [Display and touch](display-and-touch.md) · [Buttons](buttons-and-power-off.md) · [UI plan](../next-steps/ui-and-ride-app.md) · [Slice 1 handoff](../next-steps/system-ui-handoff.md)

`chef-settings` is the phone's system UI: a status bar, the settings hub
and the Display page, built on the [UI platform](ui-platform.md) from the
round-1 system design ([design contract](../next-steps/ui-and-ride-app.md#system-ui-design)).
It starts at boot and owns the panel; `fblog` shows the log screen whenever
it is not running. Portrait only. Live-verified on a RAM image on 2026-10-11
([evidence](../../logs/system-ui-slice1-2026-10-11.txt)); not installed yet. Source: `tools/chefui/apps/settings/`
(the app), `tools/chefui/kit/` (tokens, fonts, icons, widgets) and
`tools/chefui/sys/` (state sources and the settings file).

## What it shows

**Status bar**, across the 96 px safe top in the two ears beside the notch:

| Left ear (36 px icons) | Meaning |
|---|---|
| `locate-fixed` | gps-manager `RUNNING` (a fix) |
| `locate`, muted | `STARTING` or `ACQUIRING` (searching) |
| `locate-off`, muted | `OFF`, `STOPPING`, `FAILED` or no manager |
| `bluetooth-connected` | adapter powered with at least one connected device |
| `bluetooth`, muted | powered, nothing connected |
| `bluetooth-off`, muted | off, no adapter or no BlueZ |
| `wifi` | wlan0 activated with a connection |
| `wifi`, muted | Wi-Fi enabled, not connected |
| `wifi-off`, muted | disabled, no wlan0 or no NetworkManager |

The right ear holds the battery (outline with the percentage inside, fill at
22 % opacity, `zap` while a supply is online) and the local time. The battery
turns the warn colour only at 15 % or less while unpowered as
[powerd](battery-and-charging.md) defines it. There is no temperature: the
phone has no ambient sensor.

**Hub (P01)**: Bluetooth, GPS and Wi-Fi tiles (accent frame and dot while the
subsystem is on), then rows for Display, Sound and haptics, Time and date,
Storage and About. Subtitle sources:

| Row or tile | Subtitle | Source |
|---|---|---|
| Bluetooth | `2 connected`, `On`, `Off`, `No adapter`, `Unavailable` | BlueZ |
| GPS | `3D fix · ±3 m`, `Searching…`, `Off`, `Failed`, `Unavailable` | gps-manager, gpsd TPV `mode`/`eph` while `RUNNING` |
| Wi-Fi | the connection name, `Connecting…`, `Not connected`, `Off`, `No device`, `Unavailable` | NetworkManager |
| Display | theme and brightness, `Dark · 70%` | settings |
| Sound and haptics | `Haptics on` | static placeholder (buttond's buzz is the only haptic) |
| Time and date | `14:16 · IST` | clock and zone abbreviation |
| Storage | `18.2 GB free`, `RAM only` | `statvfs("/data")` on hub entry when `/data` is ours |
| About | `Linux 4.4.192-…` | `uname` release |

Tiles and rows other than Display open a placeholder page (header, Back,
"Coming in a later update.") until their slice.

**Display (P02)**: brightness slider (5 to 100 %, drawn on a 0 to 100 track,
coalesced into the next frame), theme Dark or Light, and auto-sleep 30 s,
1 min (default), 5 min or Never. Auto brightness and theme Auto are shown
muted and do not take input: they need the light sensor, and `sensord` is
not started at boot yet. A theme switch restyles every widget in place.

## Buttons and screen

| Input | Action |
|---|---|
| Power short, screen off | wake (the platform default), on the page that was open |
| Power short on the hub | screen off |
| Power short on any other page | Back |
| Power held | power-off as always (buttond, not claimed) |
| Power double, volume keys | not claimed in menus (kept for ride pages) |
| No touch for the auto-sleep time | screen off; any touch restarts the time, so does waking |

## Settings

`/data/v1/ui/settings` when `/data` is ours ([storage](storage.md#persistent-state)),
else `/run/ui/settings` with one kmsg line (lost at reboot):

```
chef-ui-settings 1
brightness 40
theme dark
sleep 60
tz IST-5:30
sha256 <hex of the lines above>
```

Defaults as shown. `tz` is a POSIX `TZ` string (no tzdata). There is no
zone picker yet and a hand edit fails the checksum, so it stays `IST-5:30`
until the Time page exists. The file follows
the chef-state rules: checked version, layout, ranges and checksum, a bad
file ignored with a kmsg line (defaults, replaced at the next save), never
fatal. Writes are debounced 2 s after the last change and flushed at exit:
the `ui` directory (0700; `/data/v1` itself is never created here) has its
parent fsynced on every save, then a 0600 temp file is written, fsynced,
renamed and the directory fsynced, under `/run/ui-settings.lock`. A failed
save is logged once per kind of error and retried at the next change or at
exit; the change stays in memory.

**Threads.** chefui applications are single-threaded; this app has one
exception, the persistence worker. Every storage access (the ownership
check, reads, writes, fsyncs and the hub's free-space `statvfs`) runs on
that thread, so a slow or hung
eMMC cannot stall touch or rendering. It gets immutable jobs and returns
results over an eventfd on the chefui loop; LVGL and all app state stay on
the UI thread. Each change has a revision and a save only marks the
revision it wrote, so an older completion never hides a newer change. The
first read waits at most 1 s at start (defaults meanwhile, the file applied
when it arrives); the exit flush waits at most 2 s per step and otherwise
logs that the file may be stale. Waits use `CLOCK_MONOTONIC`, so a clock
step cannot stretch them.

**Late `/data`.** On `/run`, the worker checks every 30 s whether `/data`
became ours. If settings were changed in this boot they win and are written
to `/data`; otherwise a valid `/data` file replaces them live (brightness,
theme, auto-sleep, zone). At shutdown init detaches `/data` before it
signals processes, so a change made in the last 2 s before a shutdown is
not saved.

## Boot and the log screen

inittab respawns `/usr/bin/chef-settings` after buttond and powerd. It takes
the panel from fblog through the fb0 lock and tolerates NetworkManager,
BlueZ, gps-manager and `/data` appearing later (each source retries with
backoff or follows D-Bus name changes). A boot that starts with
`/run/fblog.off` stays dark. If `chefui_init()` fails the app sleeps 5 s
before exiting so respawn does not spin. When it dies (`kill -9` included),
the lock is released and fblog repaints the log until respawn takes the
panel back.

To read the log screen for longer:

```sh
mkdir -p /run/chef-settings; touch /run/chef-settings/stop
kill $(pidof chef-settings)                                    # fblog shows the log
rm /run/chef-settings/stop                                     # the UI comes back within 1 s
```

## State sources

All on the chefui loop, nonblocking, none polling while idle:

- **Battery**: `/sys/class/power_supply/battery/{capacity,status}` and the
  `usb`, `pc_port`, `dc` online flags, re-read 200 ms after power_supply
  uevents (kernel netlink) and every 60 s as a fallback.
- **Clock**: a `CLOCK_REALTIME` timerfd armed for the next minute with
  `TFD_TIMER_CANCEL_ON_SET`, so a clock step (chrony's first step after the
  1970 boot clock, the chef-state restore) redraws at once.
- **GPS**: a `watch` connection to `/run/gps-manager.sock` (never a lease:
  opening the settings keeps nothing powered), reconnected with backoff
  1 s doubling to 8 s; gpsd `127.0.0.1:2947` `?WATCH` JSON only while
  `RUNNING`, for TPV `mode` and `eph`.
- **Wi-Fi and Bluetooth**: the system bus through sd-bus (static basu on the
  phone, libsystemd on a PC), asynchronous calls and matches only, every
  call with `NO_AUTO_START` so a query never starts a service through D-Bus
  activation. NetworkManager: `WirelessEnabled`, wlan0 `State` and
  `ActiveConnection`, the connection `Id`. BlueZ: `GetManagedObjects` for
  Adapter1 `Powered` and Device1 `Connected`. Re-read when a name gets an
  owner and on `PropertiesChanged` for those properties (RSSI noise is
  ignored), `DeviceAdded/Removed` and `InterfacesAdded/Removed`, coalesced
  over 100 ms. A lost bus is closed (not flushed) and reopened with backoff
  1 s doubling to 10 s.
- **Storage**: the ownership check and `statvfs` on hub entry, run on the
  persistence worker below (never on the UI thread).

## Running and testing

```sh
make -C tools/chefui test host host-test device
tools/chefui/build/host/chef-settings --fake-state tools/chefui/apps/settings/design.state \
    --data /tmp/x --run /tmp/x       # host window; P = power, mouse = touch
```

Options: `--fake-state FILE` (fixed state, no real sources; format in
`sys/fake.c`, design values in `apps/settings/design.state`), `--page
hub|display`, `--theme dark|light` and `--brightness PCT` (this run only, not
saved), `--snapshot PATH [--snapshot-after MS]` (write the frame as PPM and
exit), `--data DIR --run DIR` (settings roots). On the phone a root-owned
`/run/chef-settings/args` (options, one per line) is read once at start and
deleted, and `kill -USR1 $(pidof chef-settings)` writes the current frame to
`/run/chef-settings/snapshot.ppm` (see `chefui_snapshot()` in the
[platform guide](ui-platform.md#snapshots)).

Tests (`make -C tools/chefui test`): `test-settings` (file format, ownership
and marker impostors, syscall order, the worker with blocked storage, late
`/data`), `test-sys` (sources against a fake gps-manager, fake gpsd, sysfs
fixtures and uevents), `test-app` (fonts, theme restyle, status bar, tiles,
subtitles, page stack, power policy, auto-sleep), `test-netbus` (a fake
NetworkManager and BlueZ on a private dbus-daemon; skipped without
libsystemd and dbus-daemon). `make -C tools/chefui font-check` regenerates
the fonts and compares them byte for byte.

**Reference comparison.** `host-test` writes the four captures to
`tools/chefui/build/host/captures/`. Compare against the design renders
(`~/chef-cyclo-evidence/system-ui-r1-20261011/ref-png/`, 1:1 with 3 px rules):

```sh
python3 -I tools/chefui/compare-ref.py OUT.png REF.png CAPTURE.ppm:host [DEVICE.ppm:device] \
    --mask 0-95:140-230 --mask 0-80:262-818
```

The masks hide the design's temperature and the drawn notch. Remaining
differences are intended: Auto brightness and theme Auto disabled, the active
theme cell follows the real theme, and subtitles come from real sources.

## Fonts and icons

`tools/chefui/kit/fonts/gen-fonts.sh` builds the LVGL fonts from the committed
Barlow, Barlow Condensed (OFL) and Lucide (ISC) TTFs: `freeze_tnum.py`
(fontTools) points the digits at their tabular glyphs, then the pinned
`npx lv_font_conv@1.5.3 --no-compress --bpp 4` converts only the sizes and
glyphs the screens use (listed in the script and `kit/fonts.h`). Snap a new
design size within 2 px to an existing one before adding a font. Text is
placed by the CSS baseline (Barlow: 1.2 em content area, baseline 1.0 em
down) because `lv_font_conv` derives line heights from glyph bounds.
