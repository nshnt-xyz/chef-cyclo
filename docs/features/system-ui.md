# System UI

[Feature guides](README.md) · [UI platform](ui-platform.md) · [Display and touch](display-and-touch.md) · [Buttons](buttons-and-power-off.md) · [UI plan](../next-steps/ui-and-ride-app.md) · [Slice 1 handoff](../next-steps/system-ui-handoff.md)

`chef-settings` is the phone's system UI: a status bar, the settings hub,
the Display, Sound and haptics, Time and date, Storage and About pages, the Wi-Fi, GPS and Bluetooth sensor pages, built on the [UI platform](ui-platform.md) from the
round-1 system design ([design contract](../next-steps/ui-and-ride-app.md#system-ui-design)).
Slice 2 ([spec](../next-steps/system-ui-2-handoff.md)) adds the remaining
pages milestone by milestone: Wi-Fi, GPS and Bluetooth sensors, and (M4) the four
device pages below.
It starts at boot and owns the panel (there is no log screen; read the log
with `dmesg` over USB telnet or SSH). Portrait only. Live-verified on a RAM image on 2026-10-11
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
| Bluetooth | `N of M connected` from sensor kinds when available, else `2 connected`, `On`, `Off`, `No adapter`, `Unavailable` | BlueZ |
| GPS | `3D fix · ±3 m`, `Searching…`, `On`, `Off`, `Failed`, `Unavailable` | gps-manager (`Off` = the switch off), gpsd TPV `mode`/`eph` while `RUNNING` |
| Wi-Fi | the connection name, `Connecting…`, `Not connected`, `Off`, `No device`, `Unavailable` | NetworkManager |
| Display | theme and brightness, `Dark · 70%`; with Auto brightness on `Auto · 45%` (the live level, refreshed at most once a second) | settings, light sensor |
| Sound and haptics | `Haptics on` / `Haptics off` | the haptics preference |
| Time and date | `14:16 · IST` or `2:16 pm · IST` | clock, the 12/24-hour preference and zone abbreviation |
| Storage | `18.2 GB free`, `RAM only` | `statvfs("/data")` on hub entry when `/data` is ours |
| About | `Linux 4.4.192-…` | `uname` release (a fake run shows the fixture's kernel, never the host's) |

The Bluetooth, Wi-Fi and GPS tiles and every row open their pages.

**Display (P02)**: brightness slider (5 to 100 %, drawn on a 0 to 100 track,
coalesced into the next frame), theme Dark or Light, and auto-sleep 30 s,
1 min (default), 5 min or Never. Auto brightness (switch, "Follows the light
sensor") and theme Auto work from the light sensor, see [Ambient light](#ambient-light-m5).
With Auto brightness on, the slider and the percent show the live level and
move with it (a drag in progress is never overridden); when the sensor has not
answered for 3 s the line reads "Light sensor not available" (same place and
ink). A theme switch restyles every widget in place.

## Ambient light (M5)

`sys/als.c` (pure logic, `test-als`) and `sys/alsclient.c` (the `sensord`
client, `test-alsclient`) drive Auto brightness and theme Auto.

**Claim rules.** The UI claims `illuminance` at 5 Hz on `/run/sensord.sock`
(`--sensord-sock PATH`) only while the screen is on and Auto brightness or
theme Auto is set; screen off or both settings off closes the connection (a
claim lives on its connection, no `release` is sent) and cancels any retry. A
missing or refusing socket, EOF, an `err` line (logged once per distinct text)
or a hung sensord closes and retries with a backoff of 1 s doubling to 30 s,
only while wanted; a ready status resets it. `{"ok":...}` and `{"dropped":N}`
lines are accepted silently, malformed or negative samples are ignored. Fake
runs never connect (`als LUX|none` in the fake state).

**The stream is on-change only.** The DSP reports illuminance when it changes:
at steady light the gaps between samples were up to 15.8 s at a 5 Hz claim
(measured, [evidence](../../logs/system-ui-slice2-2026-10-11.txt)). Silence is
therefore normal, never "sensor gone", and a sample is the light until the next
one. Liveness is a `status` request on the same connection after the claim is
acknowledged and then every 30 s (no reply within 5 s: close and reconnect).
The sensor is *available* while the claim is acknowledged and the last status
said `smgr` ready (sensord re-adds the report itself when the DSP restarts, so
the connection and claim stay); "Light sensor not available" shows after 3 s
without that. The pure logic is time-driven: `als_tick()` runs the theme holds
and the brightness smoothing with the held lux at `als_next_deadline_ms()`,
armed as one one-shot timer; at steady light there is no deadline and no
timer.

**The first line of a claim is a cached reading.** A fresh claim first delivers the DSP's last reading
with its old stamp (6 to 302 s old in the M5 visit); the first fresh sample is stamped about 0.23 to
0.42 s after the claim. The client takes CLOCK_MONOTONIC when it sends the claim and drops every
sample whose `t` is more than 200 ms (`ALS_STALE_SLACK_MS`) before that, before the logic sees it.
One kmsg line per claim names the dropped reading and its age
(`als: dropped a cached sample (N lux, X s older than the claim)`). Without it a bright cached value at
a dark wake would give Light at once and full backlight for about 25 s. With no fresh sample there is
no decision; the status probe still covers liveness.

**Brightness.** Percent from lux on a piecewise linear curve in
`log10(lux + 1)`: 0 lux 5 %, 1: 8, 10: 14, 50: 20, 300: 30, 1000: 45, 5000: 75,
20000 and above 100 % (a starting point; a 250 to 320 lux room is about 30 %).
The value is smoothed in the log domain with an exponential filter on real
elapsed time (tau 1 s brightening, 5 s dimming; the whole elapsed time counts, since the
last sample is the light until the next one); only the very first
sample ever sets the filter (a stream restart keeps it; the wake grace below lets it only
brighten for the first seconds), and it keeps stepping every 200 ms by ticks, with no
sample needed, until the level has settled. Target is the curve
plus the stored `auto_offset`, clamped to 5..100; the backlight changes only
when the target is 2 points or more from the applied level, or when the
floor or ceiling is reached exactly. The applied level is not saved. A slider
drag while Auto is on sets `auto_offset = slider - curve` (-95..95, so every slider level 5..100 is curve plus offset and the level never
moves under the finger; saved through the normal debounced path) and applies the
dragged level at once; with no lux known yet the drag sets the manual brightness instead.
Turning Auto off applies the manual brightness at once and keeps the offset.

**Theme Auto.** Light when the raw lux stays at or above 2500 for 5 s, Dark when
it stays at or below 800 for 20 s; between the two nothing changes and both
hold timers reset. The theme is Dark only before the first decision since
start (or since Auto was chosen from another mode; with a held lux known it
is decided at once, logged as `selected`). A stream restart (wake, reconnect) keeps the current
theme and the brightness filter (a long screen-off does not forget the light). The
holds complete by time with the last sample's lux: one 3000 lux sample then silence
gives Light 5 s after it.

**Wake grace.** When the screen wakes the user's hand is often on or near the top
edge (pressing power, picking the phone up), and a covered sensor reads LOW, never
high. So for `ALS_WAKE_GRACE_MS` (5 s, `sys/als.h`) after each claim acknowledgment
the logic uses the highest sample since the claim: Light at once when it reaches the
light threshold (logged `wake grace`), but no Dark decision, no Dark hold
and no dimming inside the window; at its end the latest sample takes over and the
normal hysteresis runs from there (a Dark change needs its 20 s counted from the end
of the window). A Dark flash outdoors on every wake is what this prevents. The window
length is a starting value to be set from live data (`live/coldstart.sh` checked a
cold sensor on 2026-10-16: no under-read in three rounds; the first line of a claim is a cached reading, see above). One kmsg line per change of the effective theme under Auto (no line when a
decision keeps the theme), as the app prints it:
`chefui[chef-settings]: theme auto -> light (lux 3120, held 5.0 s)`, with
`wake grace` or `selected` in place of the hold for a decision inside the wake window
or the change caused by choosing Auto. Brightness changes are not logged. Tunables (also through the one-shot args file):
`--als-light LUX`, `--als-dark LUX` (dark below light), `--als-light-hold S`,
`--als-dark-hold S` (1..600); an invalid value is a usage error.

## Guided prompts (testing)

A live test that needs the user shows its steps on the phone: a script writes
`/run/chef-settings/prompt` and `chef-settings` shows a card over the bottom of the
screen (3 px accent frame, theme-aware, never clickable, no activity, never wakes
the screen) until the file is removed. One `key value` per line, unknown keys
ignored:

```
title STEP 3/9
text COVER the sensor with your palm and hold still
until 123456789            CLOCK_MONOTONIC ms when the step ends (optional)
```

With `until` the card counts down whole seconds (`12 s`); a single 1 s timer exists
only while such a card is shown, so a screen without a prompt costs nothing. The
file is noticed by inotify on the directory (no polling); a writer makes a temp file
in `/run/chef-settings` and renames it over `prompt`. Same trust rules as the args
file: a regular file owned by root, not group or other writable, not a symlink, at
most 1 KiB, otherwise ignored with one kmsg line. Text is ASCII, other bytes show as
`?`. Example writer:

```sh
now=$(awk '{printf "%d", $1 * 1000}' /proc/uptime)
printf 'title STEP 3/9\ntext COVER the sensor\nuntil %d\n' $((now + 12000)) > /run/chef-settings/prompt.tmp
chmod 600 /run/chef-settings/prompt.tmp && mv /run/chef-settings/prompt.tmp /run/chef-settings/prompt
rm -f /run/chef-settings/prompt      # hide it
```

Host runs pass `--prompt-uid UID` to trust a prompt file of their own user (the
captures `display-prompt-{dark,light}`); tests in `test-prompt`.

## Device pages (M4)

**Status bar time.** The local time follows the 24-hour preference. In 12-hour
mode the digits are `h:mm` and a 20 px `am`/`pm` marker sits under them in a
column that stays 44 px high in both modes: the digits sit where 24-hour puts
them and the marker hangs below them, inside the 96 px ear (an intended
deviation from the reference). The bar's side padding is 36 px, the design's
24 plus 12 for the panel's rounded corners: the first window run (2026-10-16)
saw the minutes digit of the 12-hour clock cut by the corner when the marker had
raised the digits about 10 px; the corner radius is about 89 to 105 px (from that
observation), so the clock's ink corner stays inside an R=120 arc (115 px from its
centre) and the left GPS glyph is within 1 px of one (121 px), well inside the
measured 89 to 105.

**Sound and haptics (P03)**: alert volume 0 to 100 % (a preference for the
future ride app, default 60; it does not touch the backlight or ALSA), then
Haptics, Alert sounds and Quiet when paused switches. No sound is played and
there is no Touch sounds row. Only haptics is an active output: a 10 ms pulse
on `/sys/class/timed_output/vibrator/enable` at the start of a touch gesture
(zero to one or more contacts; not on drag, a second finger or release), on
Back by the power key and after the screen wakes. A close driven by state
(a join finishing, a forgotten sensor) and the header Back target (whose touch
already pulsed) add none. Volume keys, power long and
redraws never buzz. The haptics preference controls these UI pulses only:
buttond's and powerd's safety buzzes are independent and untouched. Alert
sounds and Quiet when paused are stored for the ride app, which does not
exist yet.

*Feedback worker.* A third thread, `ui-feedback` (UI + `ui-store` +
feedback is the maximum), is started only on the fbdev backend and never in a
fake or SDL run. The UI hands it one fixed record over a CLOEXEC pipe
(nonblocking write end): duration 10 ms, a 100 ms monotonic expiry and a
generation. A single atomic busy flag admits one request at a time; requests
while the worker is busy, within 100 ms of the last accepted one, or while
haptics is off or the screen is off are dropped, never queued or replayed.
Disabling haptics, screen off and teardown bump the generation, so a stale
record is discarded; a sysfs write that already started cannot be cancelled
(a small unavoidable race). A failing node is logged once per distinct error from the UI thread. The worker writes `10\n`, never `0\n`, blocks
the app signals like the persistence worker and touches no LVGL or app
state. The UI never waits on it.

**Time and date (P04)**: the local clock (condensed 280 px) and date, then a
read-only Time source row, the fixed Time zone row (`IST · UTC+05:30`, no
chevron, derived from the current local offset) and the 24-hour switch with
its example (`14:16 instead of 2:16 pm`). The last-sync line is
`Last synced 14:02 via GPS.` (a date is added when it is not today).

*Where the time came from.* `/run/chef-time` is boot provenance only. The
page therefore asks chrony while it is visible: on entry or wake, then every
30 s, `chronyc -c -n -h /run/chrony/chronyd.sock tracking` (fixed argv,
`LC_ALL=C`, posix_spawn, no shell), parsed strictly (14 CSV columns, leap
Normal, finite reference time in 2026 to 2100 not after now, no `LOCL`).
Reference `GPS` with refid `47505300` reads `Set by GPS`, a numeric NTP
address `Set by network`; anything else is unverified. Failure or leaving the
page clears the verified state at once (no stale claim). The last-sync time
is chrony's reference time (when it last processed a measurement), not when
the page opened. Unverified text comes from `/run/chef-time`: `Restored at
boot, not yet verified`, `Restored boot-time floor, not yet verified`, `Time
not yet verified`, else `Time source unavailable`. The query supervision
timer runs while the query is unsettled (2 s deadline, 4 KiB cap, own process
group, the child is signalled only before it is reaped, a 1 s reap timer
afterwards), never while the page is hidden or the screen is off.

**Storage (P05)**: the free space of the owned `/data` (`f_bavail`, the same
number as the hub) and the capacity (`f_blocks`), a bar and rows for Maps,
Rides, Routes, Other and Free. Maps, Rides and Routes are the allocated
blocks (`st_blocks`) under `/data/v1/maps`, `rides` and `routes`; a missing
directory is zero. Other is capacity minus free minus those three, so it
includes filesystem metadata, service state and reserved blocks (it is not
the OS image: that is on `system_a`). The scan runs on the settings worker
as a separate job: descriptor-relative (`openat`, `O_NOFOLLOW`), never
following symlinks, staying on `/data`'s mount (`mnt_id` from fdinfo), hard
links counted once (first category wins), only metadata. Bounds: depth 32,
10000 entries, 10000 inode keys and 250 ms of monotonic time; on a bound, an
error or a capacity change during the walk the numbers are shown as
estimates (`~6.4 GB`, note `Usage estimate; storage changed or could not be
fully scanned.`). A soft budget cannot interrupt one blocked eMMC call, which
is why it never runs on the UI thread. Each category is exact, partial
(a bound, depth, a foreign mount or an unreadable entry cut it short) or
unscanned (a global bound stopped the scan before it); a partial or incomplete
value shows `~`, an unscanned one `--` (and so does Other), and the bar
then shows the single neutral used segment. An entry deleted between listing
and open is skipped, not an error. The scan samples capacity again at its end
and that newest sample replaces the cheap one (a change during the walk marks
the categories an estimate); the production default checks that `/data` is a
mount point. It is requested once on entry or wake
when the cache (30 s) is stale, never periodically. The page shows only a
coherent snapshot: if the cheap capacity changes, ownership is lost or the
scan is missing, it shows the capacity, `Calculating usage...` and a single
neutral used segment. A foreign or unmounted `/data` shows `RAM only`.

**About (P06)**: firmware card (image name and version, `Build <commit>
 · <date>` with `+dirty` for a modified tree), then LVGL (compile-time),
Kernel (`uname`), Display (the real `1080 x 2246`, no pixel density), Serial
(the exact `androidboot.serialno=` token of `/proc/cmdline`), Bluetooth and
Wi-Fi addresses (`/sys/class/bluetooth/hci0/address`,
`/sys/class/net/wlan0/address`; sysfs reports `st_size` 4096, so they are read
by length, not size) and Uptime (`CLOCK_BOOTTIME`, advanced at minute
boundaries only while visible). Unknown values read `Unavailable`. The image
facts come from a second small file, `/etc/chef/image-info` (below); the
security `build-stamp` is unchanged.

*Image info.* `scripts/chef-image-info.py` writes, from `mkinitramfs.sh`
into the root (so `mksystem.sh` inherits it): `chef-image-info 1`, `name
chef-cyclo`, `version dev` (no release exists), the 40-hex HEAD `commit`,
`dirty 0|1`, and `epoch` (`SOURCE_DATE_EPOCH` or the HEAD commit time, never
the boot clock). It is read-only display metadata, not trust material; it
coexists with the strict root stamp (tested). An old image without it shows
`chef-cyclo` and `Build information unavailable`.

## Wi-Fi pages

**Wi-Fi (P15, P16)**: a master switch (NetworkManager `WirelessEnabled`;
it follows NM's answer, not the tap; since 2026-10-17 it survives a
reboot, see [Wi-Fi](wifi-ui.md)) with the state as its subtitle. Off,
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
  "Starting" before the lease is in), SEARCHING the age of gps-manager's
  current bring-up (m:ss from `up_ms` in its `state` lines, ticking once a second only while seen: a quick
  return to the page inside the manager's 30 s grace keeps counting, 0:00 only when the manager
  really starts on this lease), "N satellites
  visible, M in use", and the sky plot (720 px, rings at 30 and 60
  degrees of elevation, north up) with a pill per satellite gpsd places,
  `G05` style (G GPS, R GLONASS, E Galileo, C BeiDou, J QZSS, S SBAS,
  I NavIC).
- **Locked (P13)** while `RUNNING` with a TPV mode 2 or 3: FIX 2D/3D (accent),
  ACCURACY `±eph m`, then Satellites used/visible, First fix (seconds from
  the start of gps-manager's bring-up to its first fix, `fix_ms` in the same lines),
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
gpsd 3.27 sends one full SKY (with the satellites array) followed by two DOP-only
SKY lines (no `satellites` key) each second: a DOP-only line updates the DOPs and
keeps the satellites of the last full one, a full line (even with an empty array)
replaces them, and when no full SKY arrives for 10 s (`GPS_SATS_STALE_MS`) the
satellites are cleared (the first window run showed the count reading 0 two thirds of
the time before this).

Left out by decision: the power mode (Precise / Balanced / Saver) until a
mode is measured to save power.

## Bluetooth pages

**Bluetooth (P07/P08)**: the master switch asks BlueZ for adapter Powered
and stays at the confirmed property value until BlueZ signals a change.
An asynchronously refused Set without a signal leaves it at that value.
Off, unavailable or no adapter shows the empty
state. On, My sensors shows one row per kind, even if one device exposes
HR, power and CSC together. Each row carries name, connection state and
live value (bpm, W, km/h or cadence-only rpm); missing values show an en
dash. The subtitle and hub count kinds, not physical devices. Available
lists LE sensor advertisements, signal strength and Pair.

**Pairing (P09)**: a modal blocks the page below, names the sensor, shows
a 20-second progress bar and wake instructions. Cancel stops our attempt;
a failure shows an explanation and Close; success returns to the list.
The progress timer runs only while the page is visible with the screen on.

**Sensor detail (P10)**: bound to address and kind, with a live card,
status, name, advertisement RSSI (marked last seen when not live), battery,
service and paired date; power and CSC also show cadence. Forget takes two
taps within four seconds and asks the daemon to remove that device and all
its kinds. A missing device or kind closes the detail and disarms Forget,
so list reordering never changes the target.

The app always watches blesensd for the hub count. A visible Bluetooth page
with the screen on holds reconnects; only the list with Bluetooth on asks
for scan. Screen-off and leaving release both; established sensor links
stay connected. Daemon loss clears the view and fails our pairing, and a
reconnect re-sends the page's hold and scan wants. See the
[daemon and bounded reconnect policy](bluetooth.md#ble-sensors).

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
chef-ui-settings 3
brightness 40
theme dark
sleep 60
tz IST-5:30
alert_volume 60
haptics 1
alert_sounds 1
quiet_paused 0
clock_24h 1
auto_brightness 0
auto_offset 0
sha256 <hex of the lines above>
```

Defaults as shown. `tz` is a POSIX `TZ` string (no tzdata); POSIX reverses
the sign, so `IST-5:30` means UTC+05:30. Version 1 files are accepted with
the five new preferences filled from defaults. They are not rewritten just
for migration: the next actual setting change saves canonical version 2.
**The migration is one-way:** images from before M4 (M1 to M3) parse only
exact version 1, reject a version 2 file and fall back to defaults until a
save rewrites version 1, which drops the M4 preferences. M5 adds version 3
(`auto_brightness 0|1`, `auto_offset` -95..95 percent points): v1 and v2 files
load with the defaults for the new lines and the next save writes v3, one-way
again (an older image rejects v3 and uses its defaults). There is no zone
picker. The file follows
the chef-state rules: checked version, layout, ranges and checksum, a bad
file ignored with a kmsg line (defaults, replaced at the next save), never
fatal. Writes are debounced 2 s after the last change and flushed at exit:
the `ui` directory (0700; `/data/v1` itself is never created here) has its
parent fsynced on every save, then a 0600 temp file is written, fsynced,
renamed and the directory fsynced, under `/run/ui-settings.lock`. A failed
save is logged once per kind of error and retried at the next change or at
exit; the change stays in memory.

**Threads.** chefui applications are single-threaded; this app has two
exceptions (three threads in all, the maximum): the persistence worker and
the haptic feedback worker (above). Every storage access (the ownership
check, reads, writes, fsyncs, the hub's free-space `statvfs`, the Storage
scan, the boot provenance and About reads) runs on
that thread, so a slow or hung
eMMC cannot stall touch or rendering. It gets immutable jobs and returns
results over an eventfd on the chefui loop; LVGL and all app state stay on
the UI thread. Each change has a revision and a save only marks the
revision it wrote, so an older completion never hides a newer change. The
first read waits at most 1 s at start (defaults meanwhile, the file applied
when it arrives); the exit flush waits at most 2 s per step and otherwise
logs that the file may be stale. Waits use `CLOCK_MONOTONIC`, so a clock
step cannot stretch them. A *due* settings save (waiting for its debounce, or
newer than the revision that already failed) goes before the optional jobs
(free space, boot provenance, device info, usage; at most one of each is
pending, in that order); a save that failed for the newest revision waits for
the next change and blocks nothing, so a full `/data` cannot freeze the
Storage, Time and About reads. Leaving a page drops its queued, not yet
started optional job (usage, boot provenance, device info); one already
started may finish. The worker is a musl thread with a 128 KB stack:
nothing large may live on it (the usage scan's inode table is heap
allocated; a 400 KB stack table crashed the app on the phone while glibc's
8 MB hid it, now a test runs the scan on a 128 KB thread).

**Late `/data`.** On `/run`, the worker checks every 30 s whether `/data`
became ours. If settings were changed in this boot they win and are written
to `/data`; otherwise a valid `/data` file replaces them live (brightness,
theme, auto-sleep, zone). At shutdown init detaches `/data` before it
signals processes, so a change made in the last 2 s before a shutdown is
not saved.

## Boot and crash recovery

inittab respawns `/usr/bin/chef-settings` after buttond and powerd. It takes
the panel through the fb0 lock and tolerates NetworkManager,
BlueZ, gps-manager and `/data` appearing later (each source retries with
backoff or follows D-Bus name changes). A boot that starts with
`/run/fblog.off` stays dark. If `chefui_init()` fails the app sleeps 5 s
before exiting so respawn does not spin. When it dies (`kill -9` included),
the lock is released and respawn takes the panel back.

While the panel is lit the screen layer keeps a marker `/run/fb0.lit`
(created before every UNBLANK, removed after every successful POWERDOWN). A
clean exit or a screen-off removes it. If the app dies with the screen on,
the kernel's last close of fb0 powers the panel down behind the touch driver,
which would leave touch dead (see the TDDI gotcha in
[display-and-touch.md](display-and-touch.md)); the next start finds the
marker and power-cycles the panel once (about 0.5 s of black) so touch
resumes. A boot, a start with the screen off and a clean restart need no
cycle (`/run` is fresh at every boot).

To keep the UI off the panel, for example to run another screen client:

```sh
mkdir -p /run/chef-settings; touch /run/chef-settings/stop
kill $(pidof chef-settings)                                    # the panel goes dark and stays dark
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
- **GPS**: a `watch` connection to `/run/gps-manager.sock` (its JSON
  lines, the [socket contract](gps.md#socket-contract), read with the
  shared reader), reconnected with backoff 1 s doubling to 8 s. It carries
  no lease, except one `map` lease while a GPS page is seen (sent after
  the `switch` line, again after a reconnect or when the switch comes back
  on; an error with reason `disabled` means none is held; `release map`
  when the page goes; a crash releases it with the connection); the
  switch goes out on it too. gpsd
  `127.0.0.1:2947` `?WATCH` JSON while `RUNNING` for TPV `mode` and `eph`,
  and while a GPS page is seen also in `ACQUIRING`, for its TPV and SKY.
  Both clients are `sys/gpsclient.c`, which has no LVGL (the ride recorder
  runs it with its own `poll()` and clock); `sys/gps.c` is its chefui
  wrapper and puts the connections on the loop through `sys/lcwatch.c`
  (a `tools/common/lineclient` on `chefui_watch_fd()` and one `lv_timer`).
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
- **BLE sensors** (`sys/blesens.c`): a nonblocking watch connection to
  `/run/blesensd.sock`, retrying from 1 to 8 seconds when absent; structured
  JSON updates for records, values, advertisements and our pairing.
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
from arguments), `--page gps|gps-sats|bt|bt-sensor|sound|time|storage|about` (sensor detail uses the first listed kind), `--prompt-uid UID`, `--theme dark|light|auto`, `--auto-brightness on|off`, `--brightness PCT`, `--clock 12|24` and `--sleep
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
and view against a fake gps-manager and gpsd, and the gpsd TPV/SKY parsers),
`test-gpsclient` (`sys/gpsclient.c` built without LVGL's include path,
driven with an injected clock and its own `poll()`: the manager's lines,
reconnect deadlines, both lease kinds, the switch, gpsd, staleness), `test-wifi` (`sys/wifi.c` against
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

Bluetooth fake state uses `ble JSON_OBJECT` lines matching daemon output,
and `blepair ADDR SECONDS` or `blepair ADDR failed REASON`; fixtures are
`bt.state`, `bt-off.state`, `bt-pairing.state`. `test-app` drives those pages
against a fake daemon socket, including visibility, screen state, pairing
outcomes, combo rows, null values and address/kind identity through Forget.
`test-sys` tests malformed JSON, escapes, foreign pair events, daemon loss,
reconnect backoff and wants re-sent after reconnect.

M4 fake lines (`sys/fake.c`): `clockepoch EPOCH` freezes the clock under the saved
zone (the design's `1791449160` is Thursday 8 October 2026 14:16 IST), `timesync
gps|network|restored|floor|none|unknown REF_EPOCH` injects the Time page's source
model (never a subprocess), `storage FREE [TOTAL]`, `storageusage MAPS RIDES
ROUTES [estimate]`, and `about KEY VALUE` (name, version, commit, builddate, dirty,
lvgl, kernel, resolution, serial, bluetooth, wifi, uptime). A bad line changes
nothing. Fixtures `time.state`, `time-unverified.state`, `storage.state`,
`storage-estimate.state`, `storage-ram.state`, `storage-calc.state`, `about.state`
layer after `design.state` (the fake firmware 0.9.3 and serial exist only there).
M4 tests: `test-app` (fonts, text helpers, 12/24-hour status bar and hub, the four
pages, the haptic gestures against a FIFO standing in for sysfs), `test-sys` (fake
lines, usage scan on a 128 KB stack, sysfs address read), `test-timeview` (the
chronyc client incl. an adversarial held-open pipe), `test-settings` (v1 to v3), `test-als`, `test-alsclient`, `test-prompt`,
`scripts/tests/test_chef_image_info.py`.

**Reference comparison.** `host-test` writes 61 captures (M5 adds display-auto, display-auto-nosensor and hub-auto in both themes plus display-theme-auto, and the prompt card over P02 in both themes, 9 files): the 52 earlier ones are the 30 below plus sound, time,
time-12h, time-unverified, storage, storage-estimate, storage-ram, storage-calc, about,
about-unknown and hub-12h (the first 30 are hub, display,
wifi-off, wifi, wifi-password typed "ridefast2", wifi-details, wifi-hidden,
gps-off, gps-searching, gps-locked, gps-sats, bt-off, bt, bt-pairing,
bt-sensor; dark and light) to
`tools/chefui/build/host/captures/`. The GPS ones read
`apps/settings/gps-{off,searching,locked}.state` after `design.state`
(the locked one carries the design's fake position). Compare against the design renders
(`~/chef-cyclo-evidence/system-ui-r1-20261011/ref-png/`, 1:1 with 3 px rules):

```sh
python3 -I tools/chefui/compare-ref.py OUT.png REF.png CAPTURE.ppm:host [DEVICE.ppm:device] \
    --mask 0-95:140-230 --mask 0-80:262-818
```

The masks hide the design's temperature and the drawn notch. Remaining
differences are intended: the active
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

M4 pages (means 2.1 to 3.7 of 255 against P03 to P06): Touch sounds row omitted;
P04's Set automatically row replaced by the read-only source row and the zone row
has no chevron and shows the fixed zone; P05's System becomes Other, the order is
Maps, Rides, Routes, Other, the Wi-Fi upload help is omitted and its corner marks
are clipped as in the reference; P06's Display value is `1080 x 2246` without ppi
and the firmware shows the build metadata; the 12-hour status bar adds the marker.

Bluetooth: en dash for no value; found CSC subtitle "Speed / cadence"
instead of "Cadence"; Pair pill about 6 px narrower from kerning; power/CSC
detail adds cadence; non-live RSSI says "(last seen)"; empty lists have help
lines. P09 keeps the actual list under its scrim, whereas the reference
uses a simplified background.

## Live acceptance of the device pages

The 12-hour status bar sits about 10 px toward the rounded panel corner; a glance
at the top-right corner in 12-hour mode belongs in the final guided batch, with
the physical feel of the haptic pulses (only the user can judge it). Preference
changes were verified by injecting touches into the NVT evdev node from the host
(evidence only; the tool is not in the repository or the image).

## Fonts and icons

`tools/chefui/kit/fonts/gen-fonts.sh` builds the LVGL fonts from the committed
Barlow, Barlow Condensed (OFL) and Lucide (ISC) TTFs: `freeze_tnum.py`
(fontTools) points the digits at their tabular glyphs, then the pinned
`npx lv_font_conv@1.5.3 --no-compress --bpp 4` converts only the sizes and
glyphs the screens use (listed in the script and `kit/fonts.h`). Snap a new
design size within 2 px to an existing one before adding a font. Text is
placed by the CSS baseline (Barlow: 1.2 em content area, baseline 1.0 em
down) because `lv_font_conv` derives line heights from glyph bounds.
