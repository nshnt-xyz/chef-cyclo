# Daemon users and the shell helper library (standard interfaces step 8)

User request 2026-10-23 ("lets start researching step 8 of standard
interfaces. Once you're ready, hand over impl and review to fresh herdr
agents", then "install and push once everything looks good"). The scope is
step 8 of the [standard interfaces](../next-steps/standard-interfaces.md#order) order:
the "Daemon users" item of its section 5 ("Run the daemons that do not need
root under their own users, with the device and socket permissions they
need (udev rules for device nodes)"), on top of the OpenRC service files,
plus the rest of the shell helper library in `/usr/lib/chef` (moved from
step 7). Read that plan, [services](../features/services.md),
[runtime files](../features/runtime-files.md),
[persistent data](../features/storage.md), [local sockets](../features/local-sockets.md),
`rootfs-overlay/etc/init.d/`, `rootfs-overlay/init`,
`rootfs-overlay/usr/bin/chef-state` and `chef-storage` first; this file
adds the research results, the decisions and the working rules. The
research notes are in `~/chef-cyclo-evidence/daemon-users-20261023/research/`
(`FINDINGS.md` and the experiment outputs).

Roles: `users_impl` (Claude Opus) implements, `users_review` (Claude Opus)
reviews, `users_coord` coordinates and decides, and runs the live check.
Report with `herdr agent prompt users_coord "[From: users_impl] ..."` (or
`users_review`). Send a message when a milestone is ready for review, when
a review is done, when blocked, and at the end. Do not poll or wait on each
other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/daemon-users-20261023/`. Review diffs
go to `review/<milestone>.diff` there; the reviewer snapshots what it
reviewed (sha256 in its note) and checks the commit against that snapshot.

## Working rules

- Work on `main` in this checkout. Milestones in the order below, each
  reviewed before the next. One commit per milestone after the reviewer's
  explicit approval. Do not push. Do not touch the `experiment/*` or `ui`
  branches. Keep this file and any edit of `standard-interfaces.md` out of
  your commits (the coordinator commits them at the end).
- No phone access. No `fastboot`, `phone.py`, `phone-boot.sh`, telnet or ssh
  to the phone: the coordinator runs the live check. Anything that can only
  be learned on the phone goes in your milestone note as a question for
  the live check.
- The implementer does not research or decide. Where this file and the
  code disagree, or this file does not settle something, stop and ask
  `users_coord` with the exact question and the options you see; the
  coordinator answers with the fix. The reviewer checks the code against
  this file and reports deviations as findings, not as new designs.
- Match the surrounding code's style and comment density. Service scripts
  stay short: header comment, variables, `depend()`, hooks only where
  needed.
- `make test` at the repo root must pass at every milestone. Stress or flake
  checks: short, targeted at the step, runtime stated first. No host test
  may write to the host's syslog (the step 6 overrides stay in force).
- Host tests run as the developer's uid, not root, and cannot chown: test
  ownership through stubs or by checking the commands and arguments
  (as `test_services.py` checks start command lines), never by needing
  root.
- Historical text keeps its words: `docs/archive/`, `docs/research/`,
  `logs/`, `docs/build-log.md` and finished handoffs. Everything that
  describes the current system says how it works now.

## What the research found

All verified on the phone on the installed OpenRC pair, temporarily, on
2026-10-23 (FINDINGS.md has the outputs).

1. **supervise-daemon drops privileges for us.** `command_user="u:g"`
   (`--user`) sets uid, gid and the supplementary groups from
   `/etc/group`; `capabilities="^cap_x,^cap_y"` (`--capabilities`) puts
   them in the inheritable and ambient sets, so a non-root child keeps
   exactly those (CapEff 0x420 for `^cap_kill,^cap_net_bind_service`).
   `no_new_privs` (`--no-new-privs`) is supported. No code in our daemons
   is needed for any of it.
2. **`poweroff` needs CAP_KILL.** busybox `poweroff` is SIGUSR2 to PID 1;
   a non-root `kill -0 1` is EPERM, with ambient CAP_KILL it works.
   CAP_KILL lets a process signal anything, but a daemon that may power
   the phone off already holds the bigger power; it is the proportionate
   grant for buttond and powerd.
3. **The IPC router needs a capability and a group.** Creating and
   publishing on AF_MSM_IPC needs CAP_NET_RAW or CAP_NET_BIND_SERVICE
   (kernel `check_permissions()`); sending to a service with no rule of its
   own needs gid 3004 (the kernel's default rule, AID_NET_RAW; our `irsc`
   adds only a gid-0 rule for LOC). sensord as uid 202 with ambient
   CAP_NET_BIND_SERVICE (the weaker of the two) and supplementary gid 3004
   worked fully: SMGR ready, samples, REG2 published, dsps timestamps.
   Without the capability REG2 publishing fails EPERM; without the group
   every SMGR send fails EPERM.
4. **Device nodes have no groups today.** /init coldplugs only `net`, so
   devtmpfs nodes stay root 0600. A coldplug of `input`, `graphics`,
   `leds`, `timed_output` and `ssc` costs about 10 ms each (the stock
   eudev rules then give `event*` root:input 0660 and `fb0` root:video
   0660); `power_supply` costs 0.4 s for the battery alone (2.35 s for all
   eight supplies) because reading its uevent is slow, so it stays out of
   the coldplug. /dev/sensors is class `ssc`. A group-writable sysfs
   attribute works for a group member (tested on `system_temp_level`).
5. **Non-root `chef-state` would silently lose /data.** `/data/chef-layout`
   is root 0600 and `/data/v1` root 0700, so `ours` reads false for any
   other user, and the consumer falls back to /run without an error.
6. **Our daemons need writable directories.** They bind sockets in
   root-owned `/run/chef`; chef-state writes locks, `.err` scratch files,
   `mag-last` and the RAM fallbacks in root-owned `/run/chef/state`;
   chef-settings creates `/run/chef/lock/ui-settings.lock`; the screen
   handover creates and removes flag files in `/run/chef`.
7. **gpsd as non-root uses NTP SHM unit 2.** gpsd 3.27.3 started as
   non-root creates and uses only units 2 and up (`gpsd/timehint.c`),
   shmget(IPC_CREAT | 0666). chronyd creates its configured unit at start
   with its `perm` (default 0600) while still root: chrony 4.8 adds its
   refclocks in RCL_Initialise, before SYS_DropRoot, so the segment takes
   chronyd's starting euid and egid (corrected 2026-10-23 after M3 found
   it; the earlier "owned by chrony" was wrong). chronyd therefore starts
   as `root:chrony` (conf.d `command_user`), the segment comes out
   root:chrony, and with `refclock SHM 2:perm=0660` and the gps user in
   group `chrony`, gpsd's shmget passes the group check.
8. **chronyc for a non-root user.** Over the Unix socket chronyc needs root
   or chrony (it binds its reply socket in /run/chrony, 0700). Monitoring
   commands (`tracking`) over UDP 127.0.0.1 port 323 are what a desktop
   user's chronyc uses; chrony accepts only monitoring commands there.
   Ours has `cmdport 0`.
9. **NetworkManager for a non-root UI: polkit.** NM's D-Bus policy lets any
   user call it; NM itself authorizes (`auth-polkit`, ours `root-only`).
   With polkit 127 and NM's `auth-polkit=true`, the rule below gave a
   netdev member `yes` for all 17 NM permissions and `nmcli radio wifi on`
   worked; a user outside netdev was refused ("Not authorized to perform
   this operation"). polkitd drops to user `polkitd` by itself. This is how
   Ubuntu and Arch let a UI drive NM. Not yet tested: D-Bus activation of
   polkitd through `dbus-daemon-launch-helper` (live check).
10. **The kmsg fallback is root-only.** `/dev/kmsg` is root 0644, so a
    non-root daemon's `chef_log` lines are lost while syslogd is down (its
    respawn); syslog itself (`/dev/log`, 0666) works. Accepted, documented.

## Decisions

### Who runs as whom

| Service | User | Supplementary groups | Capabilities | Why |
|---|---|---|---|---|
| `buttond` | `buttond` | `input`, `screen`, `vibrator` | `cap_kill` | evdev grab, the screen-off flag, buzz, `poweroff` |
| `powerd` | `powerd` | `vibrator` | `cap_kill` | buzz, `poweroff`; `system_temp_level` through its group (below) |
| `sensord` | `sensord` | `net_raw` (3004) | `cap_net_bind_service` | the IPC router (finding 3); `/dev/sensors` through its group |
| `blesensd` | `blesensd` | none | none | BlueZ over D-Bus (open to every user by BlueZ's policy) |
| `gps-manager` (and its gpsd, qmicli, nmea-broker) | `gps` | `qmux`, `chrony` | none | the QMUX bridge socket; NTP SHM 2 |
| `chef-settings` | `chefui` | `video`, `input`, `screen`, `vibrator`, `netdev`, `buttond`, `sensord`, `gps`, `blesensd` | none | panel, touch, backlight, screen handover, buzz, NM through polkit, the four sockets |

Every one of the six also gets `no_new_privs=yes`. The service scripts set
`command_user`, `capabilities` (only where the table has one) and
`no_new_privs`; nothing in the daemons' C code changes for the drop.

Stay root, each for its reason (the services guide lists them):
`syslog`, `udhcpd`, `dbus` (drops to messagebus itself), `bluetooth`,
`networkmanager` (packaged daemons, root as on Alpine), `chronyd` (drops to
chrony itself), `chef-sshd` (logins), `bt-up` (UART, rfkill, HCI attach),
`modem-up` and its children (raw block devices, the subsystem nodes, the
firmware mount, AF_MSM_IPC servers; not supervised by OpenRC),
`sensors-up` and `audio-up` (ADSP boot, persist mount), `root-session-bus`
(root's own bus), `chef-timekeeper` (RTC and chrony's control socket for
the clock state; all its input is local and trusted). polkitd is new and
drops to `polkitd` itself.

### Users and groups

Fixed IDs, added to `rootfs-overlay/etc/passwd` and `etc/group` (static
files; files on /data keep numeric owners across images, so the numbers
never change once installed). Home `/var/empty`, shell `/sbin/nologin`,
gecos the name, as Alpine's `ntp` line.

| Name | uid/gid |
|---|---|
| `buttond` | 200 |
| `powerd` | 201 |
| `sensord` | 202 |
| `blesensd` | 203 |
| `gps` | 204 |
| `chefui` | 205 |
| `polkitd` | 206 |

Groups only: `qmux` 210 (may use the QMUX bridge, i.e. the modem's QMI),
`screen` 211 (takes part in the screen handover), `vibrator` 212 (may buzz),
`net_raw` 3004 (the kernel's AID_NET_RAW, the IPC router's default rule).
Members are listed on the group lines (the existing `input` 23, `video` 27,
`netdev` 28 and `chrony` 102 lines get theirs added) exactly as the table
above. Each daemon's own group has `chefui` as a member where chef-settings
uses its socket (`buttond`, `sensord`, `gps`, `blesensd`).

### Device nodes and sysfs attributes: udev

- New `rootfs-overlay/usr/lib/udev/rules.d/70-chef.rules`, header comment
  saying what it is for and pointing at the new permissions guide:
  - `ACTION=="add", SUBSYSTEM=="ssc", KERNEL=="sensors", GROUP="sensord", MODE="0640"`
  - `ACTION=="add", SUBSYSTEM=="leds", KERNEL=="lcd-backlight", RUN+="/bin/chgrp video /sys%p/brightness", RUN+="/bin/chmod 0664 /sys%p/brightness"`
  - `ACTION=="add", SUBSYSTEM=="timed_output", KERNEL=="vibrator", RUN+="/bin/chgrp vibrator /sys%p/enable", RUN+="/bin/chmod 0664 /sys%p/enable"`
  - `input` and `graphics` need no rule of ours: the stock
    `50-udev-default.rules` gives them `input` and `video`.
- `/init`: the existing coldplug line becomes one trigger for `net`,
  `input`, `graphics`, `leds`, `timed_output` and `ssc` (one
  `--subsystem-match` each), with the same single `udevadm settle
  --timeout=20` and the same `udev-ready` marker logic. Update its comment:
  storage stays out (that was the slow probe), `power_supply` stays out
  (0.4 s for the battery alone; finding 4), and say which rules give which
  group.
- `system_temp_level`: powerd's `start_pre` (root) runs
  `chgrp powerd /sys/class/power_supply/battery/system_temp_level` and
  `chmod 0664` on it (a failure logs and leaves the throttle disabled, as a
  failed write does today). Comment why it is not a udev rule.

### Runtime directories and the socket paths

Each daemon's runtime directory is made by its service's `start_pre` with
OpenRC's `checkpath` (owner and mode set every start), and its socket moves
into it. The listening socket is mode 0660: `tools/common/linesrv.c`
`chmod()`s the path to 0660 right after `bind()`, before `listen()`,
whatever the umask (the directory's 0750 already keeps others out of the
window). The paths in `tools/common/chef_paths.h` change, and every client,
CLI default, test, live-test script and current doc with them:

| Old | New | Directory owner and mode |
|---|---|---|
| `/run/chef/buttond.sock` | `/run/chef/buttond/buttond.sock` | `buttond:buttond` 0750 |
| `/run/chef/sensord.sock` | `/run/chef/sensord/sensord.sock` | `sensord:sensord` 0750 |
| `/run/chef/sensors/` (registry copy `sns.reg`, `sns.reg.persist`) | `/run/chef/sensord/` (same names) | same directory |
| `/run/chef/blesensd.sock` | `/run/chef/blesensd/blesensd.sock` | `blesensd:blesensd` 0750 |
| `/run/chef/gps-manager.sock` | `/run/chef/gps-manager/gps-manager.sock` (its logs and record stay there) | `gps:gps` 0750 |
| `/run/chef/power/` | unchanged | `powerd:powerd` 0755 (state stays world-readable) |
| `/run/chef/fb0.lock`, `/run/chef/fb0.lit`, `/run/chef/screen-off` | `/run/chef/screen/fb0.lock`, `/run/chef/screen/fb0.lit`, `/run/chef/screen/off` | `root:screen` 0775, made by `/init` |
| `/run/chef/chef-settings/` | unchanged | `chefui:chefui` 0755 |
| `/run/chef/ui/` | unchanged | `chefui:chefui` 0700 |
| `/run/chef/qmux_socket` | unchanged (libqmi needs the name) | stays in `/run/chef`; `root:qmux` 0660 |

- The screen directory: `/init` makes `/run/chef/screen` (root:screen
  0775) and creates `fb0.lock` in it as root:screen 0660, right after it
  makes `/run/chef`. `fb0.lit` and `off` are created and removed by their
  users as today (group write on the directory lets chef-settings remove a
  flag buttond made and the other way round; no sticky bit). The lock's
  openers keep `O_RDWR | O_CREAT`: the pre-created 0660 file is what lets
  a non-root opener open a lock a root tool might otherwise have created
  0600 first.
- sensord's `start_pre`: `checkpath` the directory first, then the bounded
  `sensors-up registry` as today (root, writing into it). The registry
  write-back needs only the directory (temp file and rename). chef-state's
  `SNS_RUN` default and sensors-up's paths change with the move.
- modem-up: right after its `[ -S /run/chef/qmux_socket ]` check and before
  `touch "$LOCK/ready"`, `chgrp qmux` and `chmod 0660` the socket; failing
  either is fatal the same way the missing socket is (log, exit 1).
- chef-settings' `start_pre`: `checkpath` `/run/chef/chef-settings`,
  `/run/chef/ui` and the file `/run/chef/lock/ui-settings.lock`
  (`chefui:chefui` 0600), so the settings store needs no write access to
  `/run/chef/lock`.
- `test_run_layout.py` and the runtime files guide follow the new layout.

### chef-state's per-daemon state

The three kinds chef-state runs on behalf of a non-root daemon get a
directory each under `/run/chef/state`, owned by that daemon, mode 0700,
made by the daemon's service `start_pre` with `checkpath` (and its lock
file in it with `checkpath -f`, same owner, 0600): `mag/` (sensord),
`gps/` (gps-manager, user `gps`), `blesens/` (blesensd). Their files move
in: `lock` (was `$ST/<kind>.lock`), `last` (was `$ST/mag-last`), `enabled`
(the RAM GPS switch, was `$ST/gps-enabled`), `sensors` (the RAM records,
was `$ST/blesens`). The `time` and `log` kinds stay where they are (root
only; `$ST/time` is the boot clock status file, an interface).

- chef-state never creates these three directories; when one is missing,
  that kind's command logs one line and fails, as a save does today when
  its directory cannot be made.
- No scratch files: every `unseal ... 2>"$ST/<kind>.err"; why=$(cat ...);
  rm -f ...` becomes `body=$(unseal "$f" 2>/dev/null); rc=$?` and, only
  when `rc` is 1, `why=$(unseal "$f" 2>&1 >/dev/null)` (the files are at
  most 4096 bytes). That applies to all kinds, `time` included. Root runs
  of chef-state (sensors-up's `mag-restore`, the shutdown save) then never
  leave a root-owned file in a daemon's directory, except `mag/last` when
  the shutdown bias save writes it, which no later run of the boot reads.
- `locked()` opens `$ST/<kind>/lock` for the three kinds (an `8>` open of
  an existing file the daemon owns; root opens it too).

### /data ownership

- `chef-storage`: `/data/chef-layout` 0644 (at boot, every boot, and in
  `format`), `/data/v1` 0711 (traverse without listing; at boot and in
  `format`). Comment why: the non-root consumers must read the marker and
  reach their directories.
- `optional_consumers()` makes and owns, every boot, before any service
  starts: `crash`, `time`, `ssh` (root, as now), `power` (powerd),
  `sensors` (sensord), `gps` (gps), `blesens` (blesensd), `ui` (chefui),
  each 0700, with `chown -R -h <user>:<group>` on the directory so files a
  root process wrote there (the shutdown flushes, the images before this
  one) belong to the daemon again before it starts. `chrony` stays as it
  is. A failure is the same logged, non-fatal "optional state dir" line.
- The consumers' own `mkdir` of their `/data/v1/<dir>` stay (harmless when
  it exists) but no consumer creates `/data/v1` itself (unchanged).

### Clock

- `rootfs-overlay/etc/chrony/chrony.conf`: `refclock SHM 0 ...` becomes
  `refclock SHM 2:perm=0660 ...` (same refid, offset, delay, precision),
  and `rootfs-overlay/etc/conf.d/chronyd` sets `command_user="root:chrony"`
  (finding 7);
  `cmdport 0` goes, `bindcmdaddress 127.0.0.1` is added beside the
  existing Unix socket line (monitoring commands from localhost only, the
  chrony default `cmdallow`). Comment both. The gps-time docs and
  `test_sysvipc_gate.py` (if it names the unit) follow.
- chef-settings' Time page runs `chronyc -c -n -h 127.0.0.1 tracking`
  instead of the Unix socket. chef-state (root) keeps the socket.

### polkit and NetworkManager

- `scripts/mkrootfs.sh` adds `polkit` (pulls `polkit-common`,
  `polkit-noelogind-libs`, `duktape-libs` and `linux-pam`), with a comment
  in the style of its other package notes (version, sizes, why).
- `scripts/mkinitramfs.sh` removes `/usr/bin/pkexec` and
  `/usr/lib/polkit-1/polkit-agent-helper-1` (setuid root in the package,
  and useless without an authentication agent), the way it removes
  unused packaged init scripts. linux-pam stays installed (a library
  set; report its staged size in the milestone note). A test checks the
  staged image's setuid and setgid files: exactly the list before this
  step (report it in the note), nothing from polkit. (Decided M4 Q3:
  apk --usermode strips the bits, so two tests: the staged trees have
  none beyond mksystem's launch helper, and the packaged headers of what
  survives mkinitramfs's removals are exactly the launch helper and
  `wall`; `usr/sbin/unix_chkpwd` from linux-pam is removed too.)
- New `rootfs-overlay/usr/share/polkit-1/rules.d/60-chef-networkmanager.rules`:
  ```
  polkit.addRule(function(action, subject) {
  	if (action.id.indexOf("org.freedesktop.NetworkManager.") == 0 &&
  	    subject.isInGroup("netdev"))
  		return polkit.Result.YES;
  });
  ```
  with a comment saying what it grants and to whom (the stock
  `50-default.rules` keeps its wheel admin rule). No rule of ours in
  `/etc/polkit-1`; the packaged `/etc/polkit-1/rules.d` stays and is made
  root:polkitd 0750 by mksystem.sh and /init, like the launch helper
  (decided 2026-10-23, M4 Q4: our images are otherwise root:root and
  polkitd would log a permission error on every rule load).
- polkitd is D-Bus activated with the package's own service file and D-Bus
  policy (it is not an OpenRC service): it starts on the first NM request
  that needs a check and stays.
- `etc/NetworkManager/conf.d/90-chef.conf`: `auth-polkit=true`, comment
  (root is still always allowed; others through polkit's rules).

### The shell helper library

New `rootfs-overlay/usr/lib/chef/sys.sh`, sourced like `log.sh`
(`. "${CHEF_LIB_DIR:-/usr/lib/chef}/sys.sh"`), header comment listing each
function, what it prints or returns, and the variable it reads (each
caller keeps setting that variable from its own test override, as today):

| Function | From | Reads |
|---|---|---|
| `part NAME` | modem-up, sensors-up, audio-up, speaker-test-tone (identical) | `SYSFS_BLOCK` |
| `server_present HEX` | modem-up, sensors-up, audio-up; sensord's `start_post` grep | `DUMP_SERVERS` |
| `find_subsys NAME` | sensors-up, audio-up | `SUBSYS_DIR` |
| `card_ready` | audio-up, speaker-test-tone | `ASOUND_DIR` |
| `child_running PID` | modem-up, audio-up | (none) |

Keep the most complete comment of each copy (the dump_servers column
format, the `[ -s ]` warning on `/proc/asound/cards`, the zombie reason)
in the library and delete the copies. sensord's `start_post` sets
`DUMP_SERVERS` and calls `server_present 10f`. Not moved: stage 1's and
chef-storage's partition lookups (stricter: unique match; stage 1 must
stay self-contained) and the per-script `log()` wrappers (one line over
`chef_log` with each script's own tag and output stream). A host test
exercises each function against a fake tree, under busybox ash as the
other shell tests do.

## Milestones

- **M1: shell helper library.** `usr/lib/chef/sys.sh`, the callers, its
  test. The image build installs it like `log.sh`.
- **M2: users, permissions and paths, daemons still root.** passwd and
  group; the udev rules and the /init coldplug; the screen directory; the
  runtime directories and socket moves with `checkpath` (owners already
  the new users) and the 0660 socket mode; the qmux socket group; the
  chef-state kind directories and the scratch-file removal; chef-storage's
  modes and ownership; powerd's `system_temp_level` group;
  chef-settings' lock pre-creation. Behaviour is unchanged apart from the
  paths: every daemon still runs as root, so this milestone isolates the
  path and permission changes from the privilege drop.
- **M3: the five daemons drop root.** `command_user`, `capabilities`,
  `no_new_privs` for buttond, powerd, sensord, blesensd and gps-manager;
  `refclock SHM 2:perm=0660`. Check every file each one writes or creates
  against the directories above (a list per daemon in the note).
- **M4: chef-settings drops root.** polkit (packages, removed setuid
  helpers, the rule, the setuid test), `auth-polkit=true`, chrony's
  localhost command port and the Time page's `-h 127.0.0.1`, and
  chef-settings' `command_user` and `no_new_privs`.
- **M5: docs.** New `docs/features/users-and-permissions.md` (who runs as
  whom and why, the groups, the capabilities, the udev rules and
  `system_temp_level`, the runtime directory owners, /data owners,
  polkit, what stays root and why, the lost kmsg fallback, how to check:
  `ps -o user,group,args`, `grep Cap /proc/PID/status`, `ls -l` of the
  nodes), linked from the feature index and from services (whose table
  gains a User column). Every current guide that names a moved path or
  describes a changed permission says how it works now (the M2 to M4
  edits may already have done most; this pass finds the rest). Update
  `docs/repository-layout.md` for the new files.

Each milestone note (in `review/`) lists the files changed, the tests
added, and the questions for the live check.

## Live check (coordinator, after M5)

On a RAM image built from a clean worktree at the final commit, then on
the installed pair:

- `ps -o user,group,args`: the six as their users, everything else as
  before; `/proc/PID/status` caps and groups for each.
- Nodes and attributes after boot: `event*`, `fb0`, `sensors`,
  backlight, vibrator, `system_temp_level`, the qmux socket, the four
  sockets and their directories, `/run/chef/screen`, `/run/chef/state/*`,
  `/data/chef-layout`, `/data/v1` and its directories (owners after the
  first boot from root-owned files: the migration).
- Each daemon working: buttons (short press screen toggle with and
  without chef-settings, the 3 s power-off hold: `poweroff` as buttond),
  powerd (`powerd status`, a `system_temp_level` write), sensors (stream,
  REG2 served at an ADSP boot, `mag-save` landing on /data), blesensd
  (`blesensd status`), GPS (a lease, gpsd, `chronyc sources` showing the
  SHM 2 refclock; samples if a fix comes), chef-settings (touch,
  brightness, Wi-Fi scan, connect and switch through polkit, polkitd
  activated by D-Bus, Time page tracking, a settings save on /data).
- `kill -9` respawns, `rc-service` restarts, boot time to chef-settings
  against 6.15 s, idle CPU with polkitd present, power-off and
  `chef-reboot bootloader` with the saved ring and drift.

## Result (2026-10-23)

Done. Commits on main, each approved by `users_review` and checked
against its reviewed snapshot: `a10d733` (M1, `/usr/lib/chef/sys.sh`),
`0697efa` (M2, users, groups, udev rules, runtime directories and socket
moves, chef-state kind directories, `/data` ownership, the rule that root
never writes into a daemon's directory), `139298a` (M3, buttond, powerd,
sensord, blesensd and gps-manager as their users; chronyd started as
`root:chrony` because it creates the SHM segment before it drops root;
chef-storage syncs `/data/v1` for the daemons), `69116b4` (M4,
chef-settings as `chefui`, polkit 127 with NetworkManager `auth-polkit`,
chrony's localhost command port, the setuid tests), `ebf004b` (M5,
[users and permissions](../features/users-and-permissions.md) and the
guides), `84fda46` (M6, from the live check: the refclock option is
`SHM 2:perm=0660`, after a colon, and the guided-test prompt must be
readable by `chefui`, 0644).

The pair was restarted fresh before M3 (the implementer had lost its
connection mid-milestone). RAM images `2ed33ede` (through M5, chronyd
fixed by a bind-mounted config) and `40915b4a` (with M6) live verified on
2026-10-23. Details in [the log](../../logs/daemon-users-2026-10-23.txt).
