# System UI slice 2: the remaining settings pages

User request 2026-10-11 ("lets finish the slices"), after slice 1 was
committed as 475b6ff (kit, status bar, hub, Display page, boot; spec in
[system-ui-handoff.md](system-ui-handoff.md), guide
[system-ui.md](../features/system-ui.md)). This slice replaces every
"Coming in a later update" placeholder with the round-1 portrait design:
Wi-Fi (P15 to P18), GPS (P11 to P14), Bluetooth sensors (P07 to P10),
Sound and haptics (P03), Time and date (P04), Storage (P05), About (P06),
and the ALS-driven Auto brightness and theme Auto on P02.

Roles as before: `ui_impl` (Codex from M3) implements, `ui_review` (Codex)
reviews, `ui_coord` coordinates and talks to the user. Report with
`herdr agent prompt ui_coord "[From: ui_impl] ..."`. No em dashes anywhere. Work in the
milestones below, each reviewed before the next; one commit per
milestone after explicit reviewer approval; do not push.

References: 1:1 portrait renders (dark and light, true 3 px rules) in
`~/chef-cyclo-evidence/system-ui-r1-20261011/ref-png/`, design sources and
`design/render.sh` beside them, BLE prototype in `proto/` (`blehr.c`,
`hr_peripheral.py`, `blehr-live-2026-10-11.txt`). Slice 1 evidence and
lessons: `~/chef-cyclo-evidence/system-ui-live-20261011/`,
`logs/system-ui-slice1-2026-10-11.txt`.

## User decisions (2026-10-11)

- **BLE sensors**: no real sensors yet. Build the sensor support and
  verify it with sensors emulated on the PC's adapter (BlueZ GATT
  peripheral, as in `proto/hr_peripheral.py`). Real devices later.
- **PC bond removed**: the phone had the PC bonded as a classic device
  from the earlier BT tests, so `Connect` chose BR/EDR and failed. The
  user approved removing that bond on both sides; the coordinator did so
  on 2026-10-11. Classic BT tests (HID, PAN, A2DP) need a re-pair later.
- **GPS switch gates leases, no power modes.** Off: gps-manager refuses
  leases (persisted), rides record without a track. While the GPS page
  is open with GPS on, the page holds a lease to show searching, locked
  and satellites. The Precise / Balanced / Saver power mode is left out
  until a mode is measured to save power.
- **No backend, leave it out**: Wi-Fi Sync sections (ride upload, maps,
  routes, "rides waiting"), Touch sounds, manual time ("Set
  automatically" toggle), and the time zone picker (the row shows the
  fixed `TZ` setting, default IST). Also the Storage note "Rides are kept
  until uploaded over Wi-Fi" (no upload exists).
- **Landscape skipped** for now; it returns with the ride pages and a
  design round that respects the side notch.
- **Every omitted design element goes into the right next-steps doc**
  (user request, see the last milestone).

## Research results (coordinator, 2026-10-11)

- **BLE central works on the phone with basu.** Static `blehr`
  (`proto/blehr.c`) with `SetDiscoveryFilter {Transport: le}`,
  `StartDiscovery`, `Device1.Connect`, then `StartNotify` on 0x2A37:
  device object 0.8 s, connected 8.5 s, services resolved 11.3 s,
  notifications at 1 Hz from 12.0 s (HR 84 to 89 bpm). Battery 0x2A19
  read 80.
- **BlueZ stops notifications when the D-Bus client that called
  `StartNotify` leaves the bus**: one long-lived owner must hold them
  (one-shot `gdbus call` never sees a value).
- **Dual-mode peers**: a device BlueZ knows over BR/EDR is connected
  over BR/EDR. Real BLE sensors are LE-only; for emulation always
  discover with the LE transport filter, and keep the PC unbonded as a
  classic device while testing. `PreferredBearer` is experimental (not
  available without `bluetoothd -E`).
- **Host advertisement can stop after a connection** (host
  `ActiveInstances: 0`); the emulator must re-register its advertisement
  when that happens.
- **pkill -f gotcha on the host**: a pattern that also appears in your
  own command line kills your shell (exit 144); split the kill into its
  own command.
- **NetworkManager has no connectivity check configured**
  (`90-chef.conf`), so "internet OK" cannot come from NM today.
- **Vibrator**: `/sys/class/timed_output/vibrator/enable` (ms), as
  powerd uses it.
- **Time state**: `/run/chef-time` (`source=rtc-offset|floor|none`,
  `reason`, `saved_age_s`); chrony state through its socket
  (`chronyc -h /run/chrony/chronyd.sock`).
- **sensord is manual** (`sensors-up`); the ALS reports `illuminance`
  in lux at 5 Hz; room light measured 250 to 320 lux. Boot integration
  needs an idle-current measurement first
  ([sensors plan](sensors.md#boot-integration-and-power)).

## Milestones

### M1 Wi-Fi (P15 to P18)

- Master switch: NM `WirelessEnabled`. Off page (P15): icon and a neutral
  line (no Sync text).
- List (P16): scan (`RequestScan`) only while the page is visible and the
  screen is on, at a modest interval; "Scanning" indicator while a scan
  runs. Group access points by SSID (strongest), show security (open,
  WPA2, WPA3/SAE, WPA2/WPA3; enterprise shown but not joinable) and band
  from the frequency, signal icon `wifi-high`/`wifi-low`/`wifi-zero` from
  `Strength`. Connected card for the active connection (state, band, no
  internet claim unless NM reports connectivity). "Add hidden network".
- Join: known profile -> activate. New open network -> add and activate.
  New secured network -> P17 password page: text field with show/hide
  (`eye`/`eye-off`), on-screen keyboard styled as the design (keys 24 px
  radius, shift, delete, 123 layer, space, Join in accent), keyboard and
  field sized for gloves where possible. Join uses NM
  `AddAndActivateConnection` with the PSK in memory only; the saved
  keyfile stores it (persisted to `/data/v1/networkmanager/...`, "Saved
  on device"). Never log, snapshot to disk, or keep the PSK beyond the
  call; wipe the buffer. A wrong password (NM state reason) returns to
  P17 with an error and deletes the profile that just failed. Hidden
  network: SSID + security + password.
- Details (P18): Joined / IP address steps (Internet step only if NM
  connectivity is known, otherwise omitted), signal (NM strength as
  percent and a word; dBm is a next step), band and channel, security,
  IP and gateway; "Forget network" deletes the profile. No Sync section.
- Tests with a fake NM layer; live: scan, join a WPA2 network by
  keyboard (the coordinator relays the passphrase step to the user, or
  use a test AP the user names), wrong password, forget, toggle.

### M2 GPS (P11 to P14) and the gps-manager gate

- gps-manager: `enable` / `disable` requests and an `enabled` field in
  `status` and `watch`, persisted in `/data/v1/gps/enabled` (chef-state
  file rules; RAM-only fallback; default enabled). Disabled: new leases
  get `err disabled`, running GNSS is torn down, held leases park in a
  `DISABLED` state and resume on enable. Extend its tests
  (`tests/test_gps-manager.c` and the shell suite) and the GPS guide.
- Status bar: `locate-off` only when disabled; enabled and idle or
  searching `locate` muted; `RUNNING` `locate-fixed`.
- GPS page: switch = enable/disable. While visible, screen on and
  enabled, hold a `map` lease (released on leave and on screen off; the
  manager's 30 s grace covers short trips). Searching card (P12): elapsed
  time since the lease, satellites visible and in use from gpsd `SKY`.
  Locked (P13): fix 2D/3D, accuracy from `eph`, satellites used/visible,
  time to first fix in this lease, altitude, HDOP, PDOP, speed,
  position. No power mode row. Satellites row -> P14: sky plot (az/el),
  constellation counts (GPS, GLONASS, Galileo, BeiDou from `gnssid`),
  SNR bars scaled to fit however many satellites are visible.
- Never log a position or write it to disk (gps-manager's rule applies
  to the UI too); snapshots used as evidence must not show a real
  position: blur or use fake state for any committed image.
- Live: enable/disable round trip with a lease held from SSH, page
  lease taken and released (manager `status`), searching indoors; a fix
  at a window if the user offers one.

### M3 Bluetooth sensors (P07 to P10)

- A small resident daemon owns the BLE sensors (suggested name
  `blesensd`, inittab respawn), so the settings pages and the future
  ride recorder share one owner of connections and notifications. Line
  protocol on a Unix socket like `sensord`/`gps-manager` (`list`,
  `watch`, `scan on|off`, `pair ADDR`, `forget ADDR`, `status`), JSON
  values. It keeps its notifications for its own life; clients come and
  go.
- Sensors: Heart Rate 0x180D / 0x2A37 (8/16-bit, flags), Cycling Power
  0x1818 / 0x2A63 (instantaneous power, optional crank data), Cycling
  Speed and Cadence 0x1816 / 0x2A5B (cumulative revolutions and event
  times -> cadence rpm and speed with a wheel circumference, default
  2105 mm, configurable in the daemon; the UI for it is a next step).
  Battery 0x2A19 read and notify where offered. Handle rollover of the
  16-bit event times, stale values (show the design's dash after a
  timeout), disconnect and reconnect.
- "My sensors" = devices the daemon has paired/trusted that expose one
  of these services; reconnect with LE transport and backoff 5 s doubling to 30 s only
  while a client holds, or for a default 300 s window after daemon start,
  adapter power-on, bluetoothd appearing, pairing or link drop. Otherwise
  stay waiting with no Connect attempts and cancel outstanding Connect
  procedures at expiry. Keep established connections.
  One device record can contain several kinds; P08 shows one row per kind. Scan only while the Bluetooth page is visible (LE filter,
  service UUIDs). Pair (P09 modal, 20 s, Cancel): connect, resolve
  services, `Pair()` when the device needs bonding (NoInputNoOutput
  agent), mark trusted, store a small record (type, name, paired date)
  under `/data/v1/blesens/` with chef-state file rules. Forget: remove
  from BlueZ and the record. Signal: advertisement RSSI when known
  (BlueZ exposes no RSSI while connected; say so or show the last
  value). Master switch: adapter `Powered`.
- Pages: P07 off, P08 list with live values, P09 pairing, P10 sensor
  detail (live value card, status, name, signal, battery, service, paired
  date, Forget). Hub tile and P08 summary "N of M sensors connected".
- Emulation on the PC (`tools/` or `scripts/`): GATT peripherals for HR,
  CP and CSC with changing values, re-advertising after a disconnect,
  documented with the LE-only discovery requirement. Live: pair, live
  values at 1 Hz, reconnect after the peripheral restarts, after a
  phone reboot (bond and record persist), forget, BT off/on. Record the
  idle current with one HR sensor connected if practical (powerd log,
  USB out), otherwise note it as a next step.

### M4 Device pages (P03 to P06)

Status 2026-10-14: implemented and live-verified on a RAM image, awaiting review
([log](../../logs/system-ui-slice2-2026-10-11.txt)). Follow-ups it found: the
physical haptic feel and the 12-hour status bar corner glance join the final guided
batch; a zone picker, real timezone data and manual time stay out of scope.

- P03 Sound and haptics: alert volume slider, Haptics, Alert sounds,
  Quiet when paused, stored in the UI settings for the ride app (no
  sound is played by this slice); Haptics also drives a short tap buzz
  on UI taps and button actions through the vibrator (non-blocking, a
  few ms, rate-limited). No Touch sounds row.
- P04 Time and date: large clock and date in the `TZ`, source line
  ("Set by GPS" / "Set by network" / "Restored at boot, not yet
  verified" from chrony and `/run/chef-time`, with the last sync time),
  time zone row showing the fixed setting (not tappable), 24-hour clock
  switch (applies to the status bar and every page). No "Set
  automatically" switch.
- P05 Storage: free of total for `/data`, segmented bar and rows for
  Maps (`/data/v1/maps`), Rides (`/data/v1/rides`), Routes (pick a path
  and add it to the storage table), Other (the rest of used), Free;
  computed on the settings worker with a bounded walk and cached. No
  upload note.
- P06 About: image name and version from the build stamp, build commit
  and date, LVGL version, kernel release, display resolution, serial
  (`androidboot.serialno`), Bluetooth and Wi-Fi addresses, uptime.

### M5 Ambient light: Auto brightness and theme Auto

- First measure, then decide (bring the numbers to the coordinator, who
  asks the user): idle current with `sensord` running and no claims
  against not running, and with `illuminance` claimed at 5 Hz, using the
  powerd log with USB out (previous measurement method). If the cost is
  acceptable, start `sensors-up` from inittab
  ([sensors plan](sensors.md#boot-integration-and-power)) and close
  that next step.
- The UI claims `illuminance` only while the screen is on and either
  Auto brightness or theme Auto is set; it releases on screen off and
  reconnects after a sensord restart.
- Auto brightness: lux -> level curve (logarithmic, sensible floor and
  ceiling, smoothed so it does not flicker, faster brightening than
  dimming); a manual drag while Auto is on sets a persistent offset to
  the curve. Theme Auto: Light above a high lux threshold held for some
  seconds, Dark below a lower threshold held longer (hysteresis); start
  from measured values (room 250 to 320 lux) and make the thresholds
  tunable; log transitions without flapping.
- Live: covered sensor -> dim, room light, a lamp or window -> bright
  and Light theme; no flapping at a steady level.

### M6 Next-steps docs for everything left out

Add each omitted element to the plan that owns it, with what it needs,
then remove the design-mapping notes that are now done from the
[UI plan](ui-and-ride-app.md):

- Wi-Fi Sync (ride upload, maps and routes update, rides waiting) and the
  Storage upload note -> [connectivity](connectivity-and-sensors.md) (Wi-Fi)
  and the ride app section of the [UI plan](ui-and-ride-app.md).
- NM connectivity check ("internet OK") and signal in dBm ->
  [connectivity](connectivity-and-sensors.md) (Wi-Fi).
- Touch sounds and alert sound playback (volume, alert sounds, quiet
  when paused consumers) -> [connectivity](connectivity-and-sensors.md)
  (audio) and the UI plan's ride app.
- Manual time setting and the time zone picker ->
  [GPS and time](gps-and-time.md).
- GPS power modes (Precise / Balanced / Saver, needs a measurement) ->
  [GPS and time](gps-and-time.md).
- Wheel circumference UI and real BLE sensor validation ->
  [connectivity](connectivity-and-sensors.md) (BLE sensors).
- Landscape design round -> [UI plan](ui-and-ride-app.md).
- Re-pairing the PC as a classic peer for HID/PAN/A2DP tests ->
  [connectivity](connectivity-and-sensors.md).
- Anything else found while implementing.

## Acceptance

Per milestone: host tests for the new logic with fakes (NM, BlueZ,
gpsd/gps-manager, sensord, sysfs), the reviewer's independent runs,
host captures of every new page in dark and light compared with the
references (within a few pixels; intended differences listed), device
captures, and a live check on a RAM image. Guided user steps go through
the coordinator in the numbered STEP format; batch them per milestone or
at the end, whichever needs the user fewer times. Idle CPU on each new
static page stays near the slice 1 baseline. The settings app still
starts at boot, survives `kill -9`, and keeps all slice 1 behaviour.

## Process

- RAM images and reboots are fine (user OK this session); no install
  unless the user asks.
- Never delete anything under `/data/v1` you did not create. Wi-Fi
  profiles and BlueZ bonds created during tests are fine to remove
  afterwards; leave the user's own Wi-Fi profile alone.
- Update the feature guides (system UI, GPS, Bluetooth, storage
  tables, a BLE sensors section), the build log and a
  `logs/system-ui-slice2-<date>.txt` evidence log.
