# Standard hardware interfaces (standard interfaces step 9)

User request 2026-10-24 ("Start off on research for step 9 of standard
interfaces", then "lets start. clear their context first"). The scope is
step 9 of the [standard interfaces](../next-steps/standard-interfaces.md#order) order:
section 1 of that plan (UPower, `net.hadess.SensorProxy`, key passthrough,
vibrator as force feedback) plus the `powerd.c` split. Read that plan,
[battery and charging](../features/battery-and-charging.md),
[side buttons](../features/buttons-and-power-off.md),
[sensors](../features/sensors.md) (socket contract, units and axes),
[users and permissions](../features/users-and-permissions.md),
[services](../features/services.md), [local sockets](../features/local-sockets.md)
and `tools/blesensd/` (the basu daemon pattern) first; this file adds the
research results, the decisions and the working rules. The research notes
are in `~/chef-cyclo-evidence/hwif-20261024/research/` (`FINDINGS.md`, the
live logs, the fetched Alpine packages under `pkgs/x/` and the upstream
sources under `src/`: UPower 1.91.0, iio-sensor-proxy 3.9, feedbackd 0.8.9).

Roles: `hwif_impl` (Claude Opus) implements, `hwif_review` (Claude Opus)
reviews, `hwif_coord` coordinates and decides, and runs the live check.
Report with `herdr agent prompt hwif_coord "[From: hwif_impl] ..."` (or
`hwif_review`). Send a message when a milestone is ready for review, when
a review is done, when blocked, and at the end. Do not poll or wait on each
other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/hwif-20261024/`. Review diffs go to
`review/<milestone>.diff` there; the reviewer snapshots what it reviewed
(sha256 in its note) and checks the commit against that snapshot.

## Working rules

- Work on `main` in this checkout; the kernel change is a commit on the
  kernel submodule's branch `chef-cyclo-qpts30.61-18-10` plus a pointer
  bump in the superproject. Milestones in the order below, each reviewed
  before the next. One commit per milestone after the reviewer's explicit
  approval (M2: the kernel commit and the superproject commit). Do not
  push, either repo. Do not touch the `experiment/*` or `ui` branches.
  Keep this file and any edit of `standard-interfaces.md` out of your
  commits (the coordinator commits them at the end).
- No phone access. No `fastboot`, `phone.py`, `phone-boot.sh`, telnet or ssh
  to the phone: the coordinator runs the live check. Anything that can only
  be learned on the phone goes in your milestone note as a question for
  the live check.
- The implementer does not research or decide. Where this file and the
  code disagree, or this file does not settle something, stop and ask
  `hwif_coord` with the exact question and the options you see; the
  coordinator answers with the fix. The reviewer checks the code against
  this file and reports deviations as findings, not as new designs.
- Match the surrounding code's style and comment density. Service scripts
  stay short: header comment, variables, `depend()`, hooks only where
  needed.
- `make test` at the repo root must pass at every milestone. Stress or flake
  checks: short, targeted at the step, runtime stated first. No host test
  may write to the host's syslog (the step 6 overrides stay in force), and
  no host test may open the host's `/dev/uinput` or real system bus: the
  uinput path and the bus address are injected.
- Host tests run as the developer's uid, not root.
- Third-party config lines (UPower.conf, D-Bus policy XML, udev rules,
  OpenRC variables) are checked against the tool's own parser or
  documentation, not only this file: `dbus-daemon --config-file` on a test
  config that includes the policy, `udevadm verify` where the host has it,
  UPower's `up-config.c` for key names.
- Historical text keeps its words: `docs/archive/`, `docs/research/`,
  `logs/`, `docs/build-log.md` and finished handoffs.

## What the research found

All of it live on the installed pair (7fb63760 + 6d7adb8d, kernel #23)
from tmpfs, nothing installed; details in `FINDINGS.md`.

1. **UPower runs, but reports the battery wrong.** Alpine 3.24 `upower`
   1.91.0-r2 (128 KB apk; libgudev and libpolkit-gobject are already in the
   image) lists the `battery` supply and five line-power supplies (`usb`,
   `pc_port`, `dc`, `usbeb`, `wireless`; `bms` and `main` have types UPower
   does not know and are skipped). While the kernel says `Charging` UPower
   says `discharging`: it forces DISCHARGING whenever `current_now < 0`
   (`up-device-supply-battery.c`, the "axp20x" comment), the upstream ABI
   sign, and this Qualcomm gauge reports charging as negative. Energy is
   0 Wh and there is no time estimate: UPower reads `charge_full`,
   `charge_full_design`, `charge_now` and `cycle_count` from the Battery
   supply, and ours has them only on `bms` (`bms/charge_now` reads 0; the
   remaining charge is `charge_counter`).
2. **The kernel fix is local to the `battery` supply.** In
   `drivers/power/supply/qcom/qpnp-smb2.c`, `smb2_batt_get_prop()` serves
   `CURRENT_NOW` through `smblib_get_prop_batt_current_now()`, which reads
   `bms`. The Motorola heartbeat (`smb-lib.c` ~7165) calls that smblib
   function directly, not the psy, and no built driver reads the battery
   psy's `CURRENT_NOW` for control (charger-manager and p922x are not
   built; qpnp-qnovo reads it only for its own sysfs debug show, and its
   `current_qnovo` reads -22: inactive). Negating in the psy's
   get_property case therefore changes only what userspace (sysfs, uevent)
   and that qnovo debug file see. `bms` keeps the Qualcomm sign.
3. **upowerd works as non-root.** Under `su nobody` with a bus policy that
   lets it own `org.freedesktop.UPower` and `/var/lib/upower` its own, it
   reads everything (all psy attributes it reads are world-readable).
   Without logind its critical action falls back to PowerOff and the
   logind call fails. It writes sysfs only on an explicit
   `EnableChargeThreshold` call (polkit; our supply has no
   `charge_control_*`/`charge_types`), and history files under
   `/var/lib/upower` (tmpfs here) every 600 s. It polls the battery every
   30 s on top of the kernel's uevents (about every 52 s on USB). Cost
   with USB attached: 1 tick (10 ms) of CPU in 120 s.
4. **uinput works on 4.4 the legacy way.** `CONFIG_INPUT_UINPUT=y`,
   protocol version 4: no `UI_DEV_SETUP`/`UI_ABS_SETUP`, so the device is
   described by writing `struct uinput_user_dev` before `UI_DEV_CREATE`.
   `/dev/uinput` is root 0600 (misc, not coldplugged). A key device
   ("chef-buttons", BUS_VIRTUAL) appeared as `/dev/input/eventN` root:input
   0660 with udev `ID_INPUT=1`, `ID_INPUT_KEY=1`; `evtest` saw the events.
   Prototype: `research/proto/uinput-proto.c`.
5. **uinput force feedback works.** An FF device ("chef-vibrator",
   FF_RUMBLE, FF_PERIODIC, FF_SINE, FF_GAIN, 16 effects): a client's
   `EVIOCSFF` round trip through the owner (`EV_UINPUT`/`UI_FF_UPLOAD`,
   `UI_BEGIN_FF_UPLOAD`, `UI_END_FF_UPLOAD`) took 0.1 ms; play arrives as
   `EV_FF` code = effect id, value = repeat count (0 = stop); erase as
   `UI_FF_ERASE`, preceded by a stop; closing the client's fd stops and
   erases its effects. Writing the length in ms to timed_output buzzed.
   The qpnp-haptic `enable` clamps to its `timeout_ms` (15000 ms max) and,
   while already buzzing, only ever extends the remaining time (a shorter
   value does not shorten it); 0 stops.
6. **feedbackd's requirements.** feedbackd 0.8.9 uses an input device whose
   udev environment has `FEEDBACKD_TYPE=vibra` (its own rules match device
   names; we add a rule for ours) and refuses one without both FF_RUMBLE
   and FF_PERIODIC; with FF_GAIN it sets 75 %. It uploads FF_RUMBLE
   (`strong_magnitude`, `replay.length`) and FF_PERIODIC FF_SINE
   (`magnitude`, `replay.length`, `replay.delay` 200).
7. **net.hadess.SensorProxy** (iio-sensor-proxy 3.9,
   `src/iio-sensor-proxy/src/net.hadess.SensorProxy.xml`). Object
   `/net/hadess/SensorProxy`, interface `net.hadess.SensorProxy`:
   properties `HasAccelerometer` b, `AccelerometerOrientation` s
   (`undefined`, `normal`, `bottom-up`, `left-up`, `right-up`),
   `AccelerometerTilt` s (`undefined`, `vertical`, `tilted-up`,
   `tilted-down`, `face-up`, `face-down`), `HasAmbientLight` b,
   `LightLevelUnit` s (`lux` or `vendor`), `LightLevel` d, `HasProximity`
   b, `ProximityNear` b; methods `ClaimAccelerometer`,
   `ReleaseAccelerometer`, `ClaimLight`, `ReleaseLight`,
   `ClaimProximity`, `ReleaseProximity` (no arguments, no replies).
   Object `/net/hadess/SensorProxy/Compass`, interface
   `net.hadess.SensorProxy.Compass`: `HasCompass` b, `CompassHeading` d,
   `ClaimCompass`, `ReleaseCompass`. A claim is per bus name and ends
   when the name leaves the bus; claiming works before the sensor exists.
   Changes go out as `org.freedesktop.DBus.Properties.PropertiesChanged`.
   Upstream gates claims with polkit `claim-sensor` (allow_active: needs a
   logind session); our bus policy decides instead.
8. **Orientation and tilt.** Upstream's `orientation.c` (thresholds 35
   degrees portrait and landscape, 5 degrees same-axis hysteresis, no
   change on the shared border; tilt 20 and 70 degrees) puts `normal` at
   y = -1 g in its tests. sensord's accel is Android convention (upright
   y = +9.8, screen up z = +9.8). The upstream z convention for tilt is not
   pinned by a test; it is defined here by the documented meaning (screen
   up = `face-up`) and checked live with poses.
9. **monitor-sensor** (the acceptance client) runs on the image (gio only)
   and waits for the name. The `iio-sensor-proxy` package itself depends
   on libssc, which pulls python3, libqmi and protobuf-c: only the one
   binary is taken. Upstream monitor-sensor reads the initial compass
   heading from the wrong proxy and prints "heading: unset"; it prints
   "Compass heading changed: X" from the right one.
10. **buttond's discovery** scans `/dev/input` once at start by key
    capabilities (`evdev_key_mask`, no ABS): without a filter a restarted
    buttond could grab its own virtual key device. The kernel's
    `uinput-egis` (input5) is bus 0, the real keys bus 0 (`qpnp_pon`) and
    0x19 (`gpio-keys`).
11. **Nothing consumes KEY_POWER today**: no logind/elogind, no acpid.
    elogind's default would power off on a press.

## Decisions

### UPower: the kernel ABI fix, stock upowerd (user, 2026-10-24)

Kernel (`qpnp-smb2.c`, the `battery` supply only; build #24):

- `CURRENT_NOW`: negate the value `smblib_get_prop_batt_current_now()`
  returned, so `battery/current_now` follows the upstream ABI (positive
  while charging, negative while discharging). Comment why (UPower and the
  ABI; bms and every in-kernel user keep the Qualcomm sign because they go
  through smblib or `bms`).
- New properties in `smb2_batt_props[]`, each read from `chg->bms_psy`
  through `power_supply_get_property()` (`-EINVAL` when there is no
  `bms_psy`, like the existing helpers): `CHARGE_FULL`,
  `CHARGE_FULL_DESIGN`, `CYCLE_COUNT`, and `CHARGE_NOW` = bms
  `CHARGE_COUNTER` (comment: bms's own `charge_now` reads 0).
- Nothing else in the kernel changes. `kmake` must build with no new
  warning.

powerd reads `bms/current_now` instead of `battery/current_now` (today's
line 297), so `current_ma` in the log and state file keeps its meaning
(+ = discharging, the Qualcomm sign) across the kernel change; the tests'
fake sysfs moves with it. The battery guide says which sign each file
has.

Userspace:

- `upower` in `scripts/mkrootfs.sh`'s PKGS (whatever apk adds with it,
  `upower-udev` included, is fine). `mkinitramfs.sh` removes the D-Bus
  activation file `usr/share/dbus-1/system-services/org.freedesktop.UPower.service`
  (activation would start a root upowerd whenever the service is down) and
  nothing else of the package.
- User and group `upower`, uid/gid 207, no supplementary groups.
- `rootfs-overlay/etc/UPower/UPower.conf`, the whole file (UPower reads
  one file plus `UPower.conf.d`): `[UPower]`, `EnableWattsUpPro=false`,
  `IgnoreLid=true`, `UsePercentageForPolicy=true`,
  `PercentageLow=15.0`, `PercentageCritical=5.0`, `PercentageAction=2.0`,
  `AllowRiskyCriticalPowerAction=true`, `CriticalPowerAction=Ignore`,
  with a header saying powerd owns the low-battery and shutdown policy
  (warnings at 15 and 5 % match it), UPower must never act, and upowerd
  logs one "considered risky" warning at every start because of that
  (expected). No `NoPollBatteries`: 1.91.0 never reads it (review note,
  2026-10-24).
- D-Bus policy `rootfs-overlay/usr/share/dbus-1/system.d/chef-upower.conf`
  (our overlay keeps D-Bus files under `/usr/share/dbus-1`; a name of its
  own so it never replaces the package's file): user `upower` may own
  `org.freedesktop.UPower`. The package's policy stays as it is (root may
  own; everyone may call).
- OpenRC service `upower` (our own script, `supervise-daemon`):
  `command=/usr/libexec/upowerd`, `command_user="upower:upower"`,
  `no_new_privs=yes`, no capabilities; `depend() { need dbus; use logger;
  after powerd; }`; `start_pre` makes `/var/lib/upower` with `checkpath -d
  -o upower:upower -m 0755`; stderr (GLib warnings) to syslog with
  `error_logger="logger -t upowerd -p daemon.warning"` (check the OpenRC
  0.63 variable name). In the default runlevel.

### Key passthrough: every unclaimed key, raw (user, 2026-10-24)

buttond creates a uinput key device and passes the real key events on:

- Device: name `chef-buttons`, `BUS_VIRTUAL`, vendor/product 0, version 1,
  `EV_KEY` with `KEY_POWER`, `KEY_VOLUMEUP`, `KEY_VOLUMEDOWN`, no `EV_REP`.
- A key's press is passed when, at the press, no connection claims any
  gesture that uses that button: its `.short`, `.double`, `.long` and
  every chord containing it (power: `power+volup`, `power+voldown`; volup:
  `power+volup`; voldown: `power+voldown`). `watch` is not a claim. The
  decision holds until that key's release: a passed press always gets its
  release passed (a claim arriving in between does not leave a key stuck
  down), a press that was not passed never gets its release passed. Any
  repeat value (2) from the source follows the press's decision.
- Raw means the same code and value at the time buttond reads it, each
  followed by `SYN_REPORT`. buttond's own recognizer, defaults (screen
  toggle, power-off) and claims run exactly as now, whatever is passed; a
  long power hold reaches other programs as a long press and buttond still
  powers off on its release.
- The device lives as long as buttond. With `-G` (no grab, experiments)
  no key device is created: the real devices are already shared.

### Vibrator as force feedback: in buttond (user, 2026-10-24)

buttond also creates a uinput FF device and drives timed_output from it:

- Device: name `chef-vibrator`, `BUS_VIRTUAL`, `EV_FF` with `FF_RUMBLE`,
  `FF_PERIODIC`, `FF_SINE`, `FF_GAIN`; `ff_effects_max` 16. The owner side
  answers `UI_FF_UPLOAD` (store the effect; reply 0; an id outside 0..15
  or an unsupported type or waveform: reply `-EINVAL`) and `UI_FF_ERASE`
  (stop it if it plays, forget it, reply 0).
- Playback, one effect at a time (timed_output has one channel; the newest
  play wins): `EV_FF` code = id, value N > 0 plays the stored effect N
  times: wait `replay.delay`, buzz `replay.length` ms, repeat; value 0
  stops it if it is the one playing (write 0). A stop of another effect is
  ignored. Silent (no write, but the play still counts as playing for its
  duration) when the magnitude is 0 (`rumble`: both magnitudes 0;
  `periodic`: magnitude 0) or the gain is 0. Otherwise the motor is simply
  on: no amplitude control (`vmax_mv` stays untouched). `replay.length` 0
  means until stopped, written as 15000 ms and re-armed every 15 s until
  the stop. Because the driver never shortens a running buzz, a new play
  that replaces a running one writes 0 first.
- `FF_GAIN` events set the gain (0..0xFFFF); only 0 versus non-zero
  matters.
- Settled 2026-10-24 (M4 questions): a stop or a replacing play writes 0
  only while this FF play has the motor on (its buzz written and not yet
  over), so it never cuts another writer's buzz; an upload to a playing id
  stores the new parameters, the running phase keeps going and later
  phases and repeats use them; silence (magnitude 0, gain 0) is decided at
  each buzz start, a running buzz is not cut; a play of an id never
  uploaded or already erased is ignored (logged with `-v`).
- The timers go through buttond's existing poll deadline; no threads. The
  pure part (effect table, playback state machine, gain) takes the clock
  and a vibrator-write hook, so it is unit-tested without uinput.
- The direct writers stay: buttond's own buzzes, powerd's critical buzz
  and chef-settings' feedback keep writing `enable` as now (last writer
  wins, as today). A buttond buzz while an FF effect plays ends that
  effect (state back to idle).
- udev rule in `70-chef.rules`: `ACTION=="add", SUBSYSTEM=="input",
  KERNEL=="event*", ATTRS{name}=="chef-vibrator",
  ENV{FEEDBACKD_TYPE}="vibra"`. feedbackd is not installed.

Common to both devices:

- `-u PATH`: the uinput node (default `/dev/uinput`; `''` = no uinput
  devices). Failing to open it or to create a device is logged once and
  buttond carries on without that device: the buttons and the power-off
  must keep working.
- Discovery skips input devices whose `EVIOCGID` bustype is `BUS_VIRTUAL`
  (buttond reads physical buttons only), in `buttond.c`'s scan callback,
  not in the shared `evdev.h` helpers. An explicit `-i` is taken as given.
- A group `uinput`, gid 213, member `buttond`. udev rule `ACTION=="add",
  SUBSYSTEM=="misc", KERNEL=="uinput", GROUP="uinput", MODE="0660"`, and
  `/init` coldplugs that one node with its own `udevadm trigger
  --action=add --subsystem-match=misc --sysname-match=uinput` before the
  existing settle (a combined command would filter every subsystem by that
  name). The live check measures the boot-time cost.
- buttond's `status` line gains `"uinput":{"keys":true|false,"ff":true|false}`
  and `"ff":"idle|playing"`, appended after `watchers` (new members keep
  proto 1).

### SensorProxy: a bridge daemon (user, 2026-10-24)

A new daemon, `sensor-proxy`, in `tools/sensor-proxy/` (Makefile as
`tools/blesensd/`: host build against libsystemd's sd-bus, device build
against basu, `-DCHEF_BASU`), a client of `/run/chef/sensord/sensord.sock`
through `tools/common/lineclient` and the shared JSON reader; sensord does
not change.

- Owns `net.hadess.SensorProxy` on the system bus with exactly the
  interfaces, objects, properties and methods of research finding 7
  (introspectable; properties with `EMITS_CHANGE`; `PropertiesChanged`
  only when a value changes). `org.freedesktop.DBus.Peer` and
  `Introspectable` as sd-bus provides.
- Claims: one `sd_bus_track` per kind (accelerometer, light, proximity,
  compass), non-recursive, so a second claim from the same name is a
  no-op and one release ends it; `Release*` from a name that holds no
  claim is a no-op; a name leaving the bus (including `kill -9`) drops its
  claims. The first claimant of a kind sends the sensord claim, the last
  one gone sends the release:
  - accelerometer: `claim accel 10`; light: `claim illuminance`;
    proximity: `claim proximity`; compass: `claim heading`.
- Values:
  - `AccelerometerOrientation`, `AccelerometerTilt` from each accel
    sample: orientation with upstream's thresholds and hysteresis on
    (-x, -y, z) (sensord's axes turned into upstream's convention), tilt
    on the same vector, so upright portrait is `normal`, top down
    `bottom-up`, left edge up `left-up`, right edge up `right-up`, screen
    up `face-up`, screen down `face-down`. Our own implementation of those
    rules (no code copied from the GPLv3 sources), unit-tested with
    upstream's test vectors transformed accordingly. Both go back to
    `undefined` when the last accelerometer claim ends (upstream does the
    same).
  - `LightLevel` = the illuminance sample in lux, `LightLevelUnit`
    `lux`. The first sample after a claim is sensord's cached reading
    (an old stamp, see the sensors guide): publish it; it is the last
    known level.
  - `ProximityNear` from `near`.
  - `CompassHeading` from `heading` (magnetic, degrees, 0..360). Before
    the first sample it is -1 (documented).
- Presence: after each (re)connect, `list`; `Has*` true for a sensor
  `present` in it (compass: the `heading` entry, true when its inputs
  are). While sensord is unreachable every `Has*` is false (one
  `PropertiesChanged` when that changes) and the claims stay registered on
  the bus side; on reconnect `list`, then the sensord claims for every
  kind that has claimants. A sensord `error` line for a channel (for
  example `smgr refused report`) is logged; the property keeps its value.
- Settled 2026-10-24 (M5 questions): while the last `list` reply had
  `smgr` other than `ready`, `list` again every 5 s until one says ready
  (`Has*` from each reply); backoff 1 s doubling to 30 s,
  `lineclient_healthy()` on a `list` reply with smgr ready; a hello with
  another proto is logged once and the proxy carries on; `LightLevel`,
  `ProximityNear` and `CompassHeading` keep their last value when the last
  claim ends; options `sensor-proxy [-S SOCK] [-v]`, the bus through
  `sd_bus_open_system` (`DBUS_SYSTEM_BUS_ADDRESS` in the tests); chef_log
  lines for connect, lost, sensord errors, claims and releases sent, never
  a sample; files `orient.c` (pure orientation and tilt), `proxy.c` (claim
  bookkeeping and sensord lines, injected hooks), `bus.c`, `main.c`,
  `sensor-proxy.h`, tests `tests/test_orient.c` and
  `tests/test_sensor_proxy.py`.
- Reconnect with lineclient's backoff (its documented rules). If the bus
  connection drops, the daemon exits non-zero and its supervisor restarts
  it (claims of the old bus are gone with it anyway).
- User and group `sensorproxy`, uid/gid 208, supplementary group `sensord`
  (the socket). D-Bus policy `rootfs-overlay/usr/share/dbus-1/system.d/net.hadess.SensorProxy.conf`:
  user `sensorproxy` may own the name; the default context may send to it
  on `net.hadess.SensorProxy`, `net.hadess.SensorProxy.Compass`,
  `org.freedesktop.DBus.Properties`, `Introspectable` and `Peer` (as
  upstream's file). No polkit.
- OpenRC service `sensor-proxy` (`supervise-daemon`,
  `command_user="sensorproxy:sensorproxy"`, `no_new_privs=yes`, no
  capabilities, `depend() { need dbus; use logger; after sensord; }`), in
  the default runlevel. Logs through `chef_log`.
- `monitor-sensor`: `scripts/mkrootfs.sh` fetches the `iio-sensor-proxy`
  apk with the same static apk, cache and keys it uses for PKGS (`apk
  fetch`, signature checked) and extracts only `usr/bin/monitor-sensor`
  into the root. Also add `evtest` to PKGS (13 KB).
- Host tests in `tools/sensor-proxy/tests/`, as `tools/blesensd/tests/`
  do it: a private `dbus-daemon`, a fake sensord socket server in Python,
  dbus-python clients. Cover: every property and method, claim/release
  turning into sensord claim/release lines exactly once, two clients
  sharing a kind, `kill -9` of a client releasing, sensord going away
  (`Has*` false) and coming back (re-list, re-claim), orientation/tilt
  vectors and hysteresis, PropertiesChanged only on change.

### powerd split

`tools/powerd.c` moves to `tools/powerd/` like `tools/gps-manager/`, with
no behaviour change (the `bms/current_now` change is M2, not M1):
`powerd.h` (the shared structs, constants, hooks), `sample.c` (sysfs
reads, `powered()`, the uevent filter), `policy.c` (confirmations,
alerts, throttle, `policy_step`, poll cadence), `log.c` (state file, CSV
format and rotation, the batched `-l` log and flush), `main.c` (actions:
level write, buzz, shutdown; the loop, options, `status` and `flush`
subcommands). Its own Makefile with `make`, `make test`, `make device`,
`make install-device`; the top-level Makefile and `mkinitramfs.sh` build
and install it from there; `tools/tests/test_powerd.c` and
`test_powerd.sh` move to `tools/powerd/tests/` and link the pieces
instead of including `powerd.c` with `POWERD_NO_MAIN`. The phone binary's
behaviour is identical: compare `powerd --help`, a `status` run and the
test suite before and after.

## Milestones

Each: implement, `make test`, review diff to `review/Mx.diff`, reviewer
approval, commit.

- **M1** powerd split (pure refactor).
- **M2** kernel `qpnp-smb2.c` patch (kernel commit, `kmake` clean) and
  powerd on `bms/current_now` with its test change (superproject commit:
  pointer bump plus powerd). The kernel build number is #24.
- **M3** UPower: package, user, config, policy, service, activation file
  removed; `scripts/tests/test_services.py` and `test_users.py` extended
  as for the step 8 services.
- **M4** buttond: key passthrough, FF vibrator, BUS_VIRTUAL skip, `-u`,
  status members, uinput group, udev rules, `/init` coldplug; unit tests
  for the passthrough decision (every claim combination, claim between
  press and release) and the FF state machine (upload, play counts,
  delay, stop, erase while playing, gain 0, length 0 re-arm, replacement
  writes 0 first, buttond buzz ends the effect).
- **M5** sensor-proxy: daemon, tests, user, policy, service,
  monitor-sensor and evtest in the image.
- **M6** docs: battery guide (UPower, the sign of each `current_now`, the
  new battery attributes), buttons guide (passthrough rules, the FF device,
  status members), sensors guide (the SensorProxy facade replaces the
  "no D-Bus facade until a consumer needs it" deferral), users and
  permissions (upower, sensorproxy, uinput), services, repository layout.
  The plan's step 9 line is the coordinator's.

## Live check (coordinator, after M6)

On a RAM image built at the final commit, then on the installed pair:

- `upower -d`: battery percentage, `charging` on USB and on the wall
  charger, `discharging` unplugged, energy and time estimates present;
  `cat battery/current_now bms/current_now` opposite signs; powerd's
  `current_ma` unchanged in meaning; upowerd as user upower, nothing it
  writes outside `/var/lib/upower`.
- `monitor-sensor --all` (or `-a`, `-l`, `-p`, `-c`): light changes with a
  hand over the sensor, proximity near/far, the six poses give the six
  documented orientation/tilt values, compass heading follows a turn;
  `kill -9` of monitor-sensor releases (sensord `list` claims back to
  0); `rc-service sensord restart` and the proxy recovers.
- `evtest` on `chef-buttons`: volume and power events while nothing
  claims them; none for a claimed button (chef-settings claims
  `power.short`, so with the system UI up power is not passed); buttond
  restart does not grab its own device.
- FF: a client (`research/proto/uinput-proto play/periodic` or `fftest`
  if available) buzzes through `chef-vibrator`; udev shows
  `FEEDBACKD_TYPE=vibra`.
- Boot time to a lit status bar vs the step 8 baseline (5.66 s ordinary
  reboot), uinput coldplug cost, idle current A/B with upowerd and
  sensor-proxy running (the [A/B method](../next-steps/power-and-reliability.md#suspend-and-idle-power)).
  Old A/B scripts in the evidence dirs read `battery/current_now`, which
  has the opposite sign since kernel #24: sample `bms/current_now`.

## Result (2026-10-24)

Done. Commits on main, each approved by `hwif_review` and checked against
its reviewed snapshot: `a4e6ff7` (M1, `tools/powerd/` split),
`39fce6d` (M2, kernel #24 `1f0d322b6`: `battery/current_now` in the ABI
sign and the charge attributes from `bms`; powerd reads
`bms/current_now`), `9b0b818` (M3, UPower as user `upower`),
`e73d893` (M4, buttond passes unclaimed keys on through `chef-buttons`
and plays force feedback on `chef-vibrator`), `af22920` (M5,
`sensor-proxy` bridging sensord to `net.hadess.SensorProxy`, with
`monitor-sensor` and `evtest` in the image), `ac2f933` (M6, the guides),
`c973fe0` (M7, from the live check: the sensor-proxy methods carry
`SD_BUS_VTABLE_UNPRIVILEGED`, without which basu refused every caller
that was neither root nor `sensorproxy`; the upowerd start warnings, the
host-port AICL note and the live results in the guides).

The first pair stopped at the M5 review on usage limits; a fresh pair
finished the M5 review, M6 and M7. RAM images `f1ea7631` (through M6)
and `9015aead` (with M7) live verified on 2026-10-24 without the user at
the phone; the guided parts ran the same day on the installed pair and
passed (only a buttond buzz ending an effect stays host-tested). Details in
[the log](../../logs/hardware-interfaces-2026-10-24.txt).
