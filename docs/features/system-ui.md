# System UI

[Feature guides](README.md) · [UI platform](ui-platform.md) · [Display and touch](display-and-touch.md) · [Buttons](buttons-and-power-off.md) · [UI plan](../next-steps/ui-and-ride-app.md) · [Slice 1 handoff](../next-steps/system-ui-handoff.md)

`chef-settings` is the phone's system UI: a status bar, the settings hub,
the Display page, the Wi-Fi pages and the GPS pages, built on the [UI platform](ui-platform.md) from the
round-1 system design ([design contract](../next-steps/ui-and-ride-app.md#system-ui-design)).
Slice 2 ([spec](../next-steps/system-ui-2-handoff.md)) adds the remaining
pages milestone by milestone: Wi-Fi, then GPS.
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
| `locate`, muted | the GPS switch on: `OFF`, `STARTING`, `ACQUIRING`, `STOPPING` or `FAILED` |
| `locate-off`, muted | the switch off (`DISABLED`) or no manager |
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
| GPS | `3D fix · ±3 m`, `Searching…`, `On`, `Off`, `Failed`, `Unavailable` | gps-manager (`Off` = the switch off), gpsd TPV `mode`/`eph` while `RUNNING` |
| Wi-Fi | the connection name, `Connecting…`, `Not connected`, `Off`, `No device`, `Unavailable` | NetworkManager |
| Display | theme and brightness, `Dark · 70%` | settings |
| Sound and haptics | `Haptics on` | static placeholder (buttond's buzz is the only haptic) |
| Time and date | `14:16 · IST` | clock and zone abbreviation |
| Storage | `18.2 GB free`, `RAM only` | `statvfs("/data")` on hub entry when `/data` is ours |
| About | `Linux 4.4.192-…` | `uname` release |

The Wi-Fi and GPS tiles open their pages and the Display row the Display page;
the other tiles and rows open a placeholder page (header, Back, "Coming in
a later update.") until their milestone.

**Display (P02)**: brightness slider (5 to 100 %, drawn on a 0 to 100 track,
coalesced into the next frame), theme Dark or Light, and auto-sleep 30 s,
1 min (default), 5 min or Never. Auto brightness and theme Auto are shown
muted and do not take input: they need the light sensor, and `sensord` is
not started at boot yet. A theme switch restyles every widget in place.

## Wi-Fi pages

**Wi-Fi (P15, P16)**: a master switch (NetworkManager `WirelessEnabled`;
it follows NM's answer, not the tap) with the state as its subtitle. Off,
unavailable or without wlan0, the page shows an empty state instead of
the list ("Wi-Fi is off", "Wi-Fi unavailable", "No Wi-Fi device"). On:

- **Connected** card for wlan0's active connection: the SSID, "Connected"
  or "Connecting...", the band, and "internet OK", "sign-in needed" or "no
  internet" only when NetworkManager's connectivity check runs (it is not
  configured on the phone, so no internet claim is made today). The card
  opens the details.
- **Available**: one row per SSID (the strongest AP; hidden SSIDs and the
  connected one left out), strongest first, at most 24. Icon
  `wifi-high`/`wifi-low`/`wifi-zero` from NM `Strength` (>= 50, >= 25,
  below), subtitle `[Saved ·] security · band`, a lock for anything not
  open. Security from the AP flags: Open, WPA, WPA2, WPA3 (SAE only),
  WPA2/WPA3; WEP, OWE and Enterprise are listed as "not supported". The
  header shows "Scanning" while a scan runs.
- **Add hidden network** opens a page with the name, security (Open, WPA2,
  WPA3) and password.

A tap on a saved network activates its profile (`ActivateConnection`), on
an open one adds and activates a profile, on a new secured one opens the
**password page (P17)**: header with a close (x), "PASSWORD · WPA2", the
field (shown by default as the design has it; the eye button hides it),
"Saved on device. Never shown in logs." and the design's keyboard (letters
with a one-shot shift, a 123 and a #+= layer, so every printable ASCII
character can be typed; delete repeats when held; Join in the accent
colour). Join checks 8 to 63 characters, then calls
`AddAndActivateConnection` with `key-mgmt` `wpa-psk` (WPA, WPA2 and the
transition mode) or `sae` (WPA3 only). The profile NetworkManager writes
keeps the passphrase (`psk-flags` 0) in `/data/v1/networkmanager/system-connections`
([storage](storage.md)); the app gives each new profile a random UUID as
its identity. A join is the ActiveConnection its call returns: before that
reply wlan0's `StateChanged` (matched before the call goes out) only
records a FAILED reason; after it, that active connection's own
`StateChanged` (matched first) and one read of its `State` decide, so an
activation that ends before NM answers is still seen and an earlier
connection's changes never count. ACTIVATED closes the page; a failure
with device reason 7 (no secrets) or 8 (supplicant disconnect), or the
active connection's reason "no secrets", shows "Wrong password. Try
again." in the warn colour, other failures "Could not join". A join is
also ended by a 90 s timeout, by NetworkManager leaving the bus and by a
lost bus. A second join while one runs is refused and the running one is
left alone. A join started from a page that was left since shows its
outcome in the master row's subtitle.

**Deleting profiles.** A new profile whose join failed, and Forget, delete
by UUID: `GetConnectionByUuid` on NetworkManager's name, then `Delete`
addressed to the unique bus name that answered, never to an object path
remembered from earlier and never to whichever NetworkManager owns the
name by then (a restarted one numbers its paths again). Only a successful
`Delete`, or the current NetworkManager answering that no profile has the
UUID, confirms; a `Delete` error looks the UUID up again (three times at
once), other errors retry every 5 s up to 6 times and once more whenever
the bus or NetworkManager comes back, and an owner change drops every
answer still in flight. The queue holds 4 deletes and never drops one: a
new profile's join reserves its entry before the profile exists (used only
if the join fails), and a join or Forget is refused while the queue is
full. The UUID comes from `getrandom` without waiting for the entropy pool,
else from `/dev/urandom`. A saved profile that fails to connect is kept.

**The passphrase.** It is typed into the page's own buffer; the field's
label points at that buffer (or at bullets) and never copies it. Join
wipes the buffer before the call returns; the only other copy in the app
is the body of that one D-Bus message, which is wiped when NetworkManager
answers (libsystemd builds also mark it sensitive). Leaving the page wipes
the buffer too. A capture (`SIGUSR1`) shows bullets in the field while the
frame is written. Nothing logs an SSID or a passphrase: the log has
"wifi: joining a new WPA2 network", "wifi: join connected/refused the
passphrase/failed" and NM error names.

**Details (P18)** belongs to the profile it was opened for (the active
connection's UUID): steps Joined (device state IP_CONFIG or later) and IP
address, plus Internet while the connectivity check runs; Signal as NM's
strength percent and a word (strong >= 67, good >= 45, fair >= 25, weak),
Band and channel, Security, IP address and Gateway (IPv4). "Forget
network" takes two taps (the second within 4 s) and deletes that profile
by its UUID. When the connection goes away or another one replaces it
(also while the screen is off), the page closes and a pending
confirmation with it, so Forget can never reach the replacement.

**What it costs.** NetworkManager is only followed while a Wi-Fi page is
the visible page and the screen is on (matches installed, the view read
on change); the list page also scans (`RequestScan`) at once and every
20 s. Leaving the pages or the screen going off drops the matches and
stops scanning; a running join is still followed to its end.

Left out by decision (see the [handoff](../next-steps/system-ui-2-handoff.md)):
the design's Sync texts and section (no upload backend), the signal in
dBm (NM gives a percent), and "internet OK" without a connectivity check.

## GPS pages

**GPS (P11, P12, P13)**: the master switch is gps-manager's
[switch](gps.md#use-gnss-gps-manager) (`enable`/`disable` on the app's
watch connection; the manager saves it). Off (P11): the empty state, "No
position" and "Rides record without a track or map." On, the page holds a
`map` lease while it is seen with the screen on, and shows:

- **Searching (P12)** until a fix: STATUS "No fix" ("Failed" in `FAILED`,
  "Starting" before the lease is in), SEARCHING the time since the page's
  lease (m:ss, ticking once a second only while seen), "N satellites
  visible, M in use", and the sky plot (720 px, rings at 30 and 60
  degrees of elevation, north up) with a pill per satellite gpsd places,
  `G05` style (G GPS, R GLONASS, E Galileo, C BeiDou, J QZSS, S SBAS,
  I NavIC).
- **Locked (P13)** while `RUNNING` with a TPV mode 2 or 3: FIX 2D/3D (accent),
  ACCURACY `±eph m`, then Satellites used/visible, First fix (seconds from
  the page's lease to its first fix, "–" if the fix was there before),
  Altitude (MSL), HDOP, PDOP, Speed in km/h, and Position (5 decimals,
  N/S E/W). The Satellites row ("12 in use · 4 systems") opens P14.
- **Satellites (P14)**: the sky plot (840 px, N E S W), the legend, used/
  visible per system (GPS, GLONASS, Galileo, BeiDou), and one signal bar
  per satellite (SNR / 50 dB-Hz x 300 px, accent when in use), sorted by
  system; with more than 24 satellites the numbers above and the labels
  under the bars are left out so the bars still fit.

The view (TPV and SKY, `sys/gpsview.c`) is read from gpsd only while a GPS
page is seen; leaving the pages or the screen going off releases the lease
(the manager's 30 s grace keeps a short trip cheap) and forgets the view,
position included. It is also forgotten when gps-manager or gpsd goes away
or GNSS stops, so nothing stale is shown before fresh reports arrive. The
elapsed time ticks only while the page is seen. The position is never logged or written: a capture
(`SIGUSR1`) shows "–" in its place. Indoors the modem sends no GSV, so
gpsd has no SKY and the page shows 0 satellites until it sees the sky.

Left out by decision: the power mode (Precise / Balanced / Saver) until a
mode is measured to save power.

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
- **GPS**: a `watch` connection to `/run/gps-manager.sock`, reconnected
  with backoff 1 s doubling to 8 s. It carries no lease, except one `map`
  lease while a GPS page is seen (sent after the `enabled` line, again
  after a reconnect or when the switch comes back on; `err disabled` means
  none is held; `release map` when the page goes; a crash releases it with
  the connection); the switch goes out on it too. gpsd
  `127.0.0.1:2947` `?WATCH` JSON while `RUNNING` for TPV `mode` and `eph`,
  and while a GPS page is seen also in `ACQUIRING`, for its TPV and SKY.
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
- **Wi-Fi pages** (`sys/wifi.c`, the pure parts in `sys/wifi_model.c`):
  the same bus connection (netbus clients), only while watched as above.
  One refresh reads `GetDeviceByIpIface("wlan0")`, then in parallel the
  APs (`GetAllAccessPoints` and each AP's properties), the device (State,
  ActiveConnection, Ip4Config), its active connection (Id, Connection),
  IPv4 (AddressData, Gateway), the active AP and LastScan, NM's
  Connectivity and ConnectivityCheckEnabled/Available, and the saved
  profiles (`ListConnections`, each `GetSettings`, which carries no
  secrets). NM `PropertiesChanged`, Settings and Device.Wireless signals
  and NM's name owner rerun it 300 ms after the first change and at most
  every 1.5 s.

## Running and testing

```sh
make -C tools/chefui test host host-test device
tools/chefui/build/host/chef-settings --fake-state tools/chefui/apps/settings/design.state \
    --data /tmp/x --run /tmp/x       # host window; P = power, mouse = touch
```

Options: `--fake-state FILE` (fixed state, no real sources; format in
`sys/fake.c`, design values in `apps/settings/design.state`; up to four
files, read in order, so `wifi-off.state` after it gives P15), `--page
hub|display|wifi|wifi-password|wifi-hidden|wifi-details` (the password page
for the first listed network), `--keys TEXT` (with fake state only: typed on
the open entry page's keyboard, for captures; a real passphrase never comes
from arguments), `--page gps|gps-sats`, `--theme dark|light`, `--brightness PCT` and `--sleep
30|60|300|0` (this run only, not saved), `--snapshot PATH [--snapshot-after MS]` (write the frame as PPM and
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
libsystemd and dbus-daemon; `test-sys` also has the GPS page's lease, switch
and view against a fake gps-manager and gpsd, and the gpsd TPV/SKY parsers), `test-wifi` (`sys/wifi.c` against
`tests/fake_nm.py`, a fake NetworkManager in python3 dbus-python and GLib on
a private dbus-daemon: the view, scans, joins new/saved/open/hidden with the
passphrase NM received, a refused passphrase deleting its profile, a saved
profile kept on failure, a second join refused without touching the first,
activations that end before the reply (success and refusal), a failing
Delete looked up again and retried, a full delete queue (refused, not
evicted, drained later), the entropy pool not ready (`getrandom` wrapped),
a pending delete across an NM restart that reuses the profile's object
path for another profile, an owner change between a lookup and its
answer, every Delete addressed to a unique name, Forget by UUID, the
switch, no calls while unwatched, `NO_AUTO_START`; skipped without
those). `test-app` drives the
Wi-Fi pages against a recording stub (`tests/wifi_stub.c`): visibility and
scanning, rows and taps, the password page (length check, the buffer handed
over and wiped, bullets during a capture, the eye), outcomes, the hidden
network page, details bound to its profile (closed when another connection
replaces it during the confirmation), the Internet step following the check
and the two-tap Forget. `make -C tools/chefui
font-check` regenerates the fonts and compares them byte for byte.

**Reference comparison.** `host-test` writes 22 captures (hub, display,
wifi-off, wifi, wifi-password typed "ridefast2", wifi-details, wifi-hidden,
gps-off, gps-searching, gps-locked, gps-sats; dark and light) to
`tools/chefui/build/host/captures/`. The GPS ones read
`apps/settings/gps-{off,searching,locked}.state` after `design.state`
(the locked one carries the design's fake position). Compare against the design renders
(`~/chef-cyclo-evidence/system-ui-r1-20261011/ref-png/`, 1:1 with 3 px rules):

```sh
python3 -I tools/chefui/compare-ref.py OUT.png REF.png CAPTURE.ppm:host [DEVICE.ppm:device] \
    --mask 0-95:140-230 --mask 0-80:262-818
```

The masks hide the design's temperature and the drawn notch. Remaining
differences are intended: Auto brightness and theme Auto disabled, the active
theme cell follows the real theme, and subtitles come from real sources.
Wi-Fi: the master subtitle is the state ("On", "Off") instead of the Sync
line, P15 has one neutral line instead of the Sync text (so the empty state
sits 31 px lower), OpenRide's subtitle is "Open · 2.4 GHz" instead of
"sign-in may be needed", P18's signal is "82% · strong" instead of dBm and
it has no Sync section. The hidden-network page has no reference. GPS:
P11 has one line instead of the wheel-sensor text (the group sits lower),
the power mode row (P12) and section (P13) are left out (P13's Satellites
row moves up), the P12 card border is solid (LVGL draws no dashes), the
satellite labels use Barlow Medium for the design's 700 and 600 weights,
P13's Speed key reads "Speed km/h", and its Satellites row counts 12 in use
where the design's text says 13 (its own list has 12).

## Fonts and icons

`tools/chefui/kit/fonts/gen-fonts.sh` builds the LVGL fonts from the committed
Barlow, Barlow Condensed (OFL) and Lucide (ISC) TTFs: `freeze_tnum.py`
(fontTools) points the digits at their tabular glyphs, then the pinned
`npx lv_font_conv@1.5.3 --no-compress --bpp 4` converts only the sizes and
glyphs the screens use (listed in the script and `kit/fonts.h`). Snap a new
design size within 2 px to an existing one before adding a font. Text is
placed by the CSS baseline (Barlow: 1.2 em content area, baseline 1.0 em
down) because `lv_font_conv` derives line heights from glyph bounds.
