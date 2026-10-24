# Users and permissions

[Feature index](README.md) · [Services](services.md) · [Runtime files](runtime-files.md) · [Persistent data](storage.md) · [Local socket protocol](local-sockets.md)

Since standard interfaces step 8 (2026-10-23) the daemons that do not need root run under their own users (`upower` and `sensor-proxy` since step 9, 2026-10-24), as on a desktop distribution: each gets exactly the device nodes, sockets, directories and capabilities its work needs, through groups, udev rules and the OpenRC service script. Nothing in the daemons' C code changed for the drop: OpenRC's `supervise-daemon` sets the user, the groups and the capabilities before it execs the program.

## Who runs as whom

| Service | User | Supplementary groups | Capabilities | Why |
|---|---|---|---|---|
| `buttond` | `buttond` | `input`, `screen`, `vibrator`, `uinput` | `cap_kill` | evdev grab, the screen-off flag, buzz, `poweroff`; `/dev/uinput` for its key passthrough and force-feedback devices |
| `powerd` | `powerd` | `vibrator` | `cap_kill` | buzz, `poweroff`; `system_temp_level` through its own group (below) |
| `sensord` | `sensord` | `net_raw` (3004) | `cap_net_bind_service` | the IPC router (below); `/dev/sensors` through its own group |
| `blesensd` | `blesensd` | none | none | BlueZ over D-Bus, which BlueZ's D-Bus policy opens to every user |
| `gps-manager` (and its gpsd, qmicli, nmea-broker) | `gps` | `qmux`, `chrony` | none | the QMUX bridge socket and modem-up's lock; NTP SHM unit 2 |
| `upower` | `upower` | none | none | the power_supply attributes it reads are world-readable; its bus name through its policy, `/var/lib/upower` its own (below) |
| `sensor-proxy` | `sensorproxy` | `sensord` | none | `sensord`'s socket; its bus name through its policy (below) |
| `chef-settings` | `chefui` | `video`, `input`, `screen`, `vibrator`, `netdev`, `buttond`, `sensord`, `gps`, `blesensd` | none | panel, touch, backlight, the screen handover, buzz, NetworkManager through polkit, the four daemons' sockets |

Each of these service scripts sets `command_user="<user>:<group>"`, `capabilities=` where the table has one, and `no_new_privs=yes`. supervise-daemon (`--user`) sets the uid, the gid and the supplementary groups from `/etc/group` (initgroups, so the user's own group is in the list too); `--capabilities "^cap_x"` puts the capability in the inheritable and ambient sets, so the non-root program keeps exactly that one in its effective set; `--no-new-privs` sets `PR_SET_NO_NEW_PRIVS`, so nothing it execs can gain privileges. gps-manager's children run as `gps` too. gpsd started as non-root changes no credentials, so it keeps the `PR_SET_PDEATHSIG` gps-manager gives it and dies with the manager.

Programs a daemon starts run as that daemon too: sensord's `chef-state mag-save`, blesensd's `chef-state blesens-*`, gps-manager's `chef-state gps-*`.

### Capabilities

- **`cap_kill` (buttond, powerd).** busybox `poweroff` is SIGUSR2 to PID 1, and a non-root process may not signal init (EPERM) without CAP_KILL. CAP_KILL lets a process signal any process, but a daemon that may power the phone off already holds the bigger power; it is the proportionate grant. Both daemons end their power-off chain with `reboot(RB_POWER_OFF)` if even the signal fails; that syscall needs CAP_SYS_BOOT, which neither has, so it fails with EPERM. The line logged just before it (`init would not take SIGUSR2 (...)`) says why init was not reached. With CAP_KILL the signal is the real path.
- **`cap_net_bind_service` (sensord).** Creating and publishing a server on AF_MSM_IPC (the REG2 server the ADSP reads the sensor registry from) needs CAP_NET_RAW or CAP_NET_BIND_SERVICE (the kernel's `check_permissions()`); sensord has the weaker of the two. Sending to a service that has no rule of its own (the SMGR) needs gid 3004, the kernel's default rule (AID_NET_RAW): hence the `net_raw` group. Without the capability REG2 publishing fails EPERM; without the group every SMGR send does.

## Users and groups

Fixed IDs in `rootfs-overlay/etc/passwd` and `etc/group`. Files on `/data` keep their numeric owners across images, so these numbers never change once installed. Home `/var/empty`, shell `/sbin/nologin`, gecos the name.

| User and its group | uid/gid |
|---|---|
| `buttond` | 200 |
| `powerd` | 201 |
| `sensord` | 202 |
| `blesensd` | 203 |
| `gps` | 204 |
| `chefui` | 205 |
| `polkitd` | 206 |
| `upower` | 207 |
| `sensorproxy` | 208 |

| Group | gid | Members | Gives |
|---|---|---|---|
| `qmux` | 210 | `gps` | the QMUX bridge (`/run/chef/qmux_socket`, the modem's QMI) and modem-up's lock directory |
| `screen` | 211 | `buttond`, `chefui` | the screen handover in `/run/chef/screen` |
| `vibrator` | 212 | `buttond`, `powerd`, `chefui` | the vibrator's `enable` |
| `uinput` | 213 | `buttond` | `/dev/uinput` |
| `net_raw` | 3004 | `sensord` | the IPC router's default rule (AID_NET_RAW) |
| `input` | 23 | `buttond`, `chefui` | `/dev/input/event*` |
| `video` | 27 | `root`, `chefui` | `/dev/fb0`, the backlight's `brightness` |
| `netdev` | 28 | `chefui` | NetworkManager through polkit |
| `chrony` | 102 | `gps` | NTP SHM unit 2 |
| `buttond`, `gps`, `blesensd` | their user's | `chefui` | the daemon's socket (0660 in its 0750 directory) |
| `sensord` | 202 | `chefui`, `sensorproxy` | the same |

`scripts/tests/test_users.py` checks the lines, the IDs and the members.

## Device nodes and sysfs attributes

`/init` coldplugs `net`, `input`, `graphics`, `leds`, `timed_output` and `ssc` in one `udevadm trigger` (about 10 ms each), and misc's `uinput` node alone with a trigger of its own (`--subsystem-match=misc --sysname-match=uinput`; in the combined command the name would filter every subsystem). The stock `50-udev-default.rules` gives `event*` root:input 0660 and `fb0` root:video 0660. Ours, `rootfs-overlay/usr/lib/udev/rules.d/70-chef.rules`:

| Node or attribute | Owner and mode | Rule |
|---|---|---|
| `/dev/sensors` (class `ssc`) | root:sensord 0640 | `GROUP="sensord", MODE="0640"` |
| `/sys/class/leds/lcd-backlight/brightness` | root:video 0664 | `RUN+="/bin/chgrp video ..."`, `RUN+="/bin/chmod 0664 ..."` |
| `/sys/class/timed_output/vibrator/enable` | root:vibrator 0664 | the same with `vibrator` |
| `/dev/uinput` (misc) | root:uinput 0660 | `GROUP="uinput", MODE="0660"` |

buttond's uinput devices `chef-buttons` and `chef-vibrator` get their `event*` nodes root:input 0660 from the stock rule, like the real keys ([buttons](buttons-and-power-off.md)); `70-chef.rules` also gives `chef-vibrator` `FEEDBACKD_TYPE=vibra`, which feedbackd looks for (not a permission).

`/sys/class/power_supply/battery/system_temp_level` gets root:powerd 0664 from powerd's `start_pre` (root), not from a rule: `power_supply` stays out of the coldplug, because reading its uevent costs 0.4 s for the battery alone (2.35 s for all eight supplies). If it fails, powerd's write fails too and it leaves the charge throttle disabled, as any failed write does. storage stays out of the coldplug too (that was the slow probe).

## Runtime directories and sockets

Each daemon's service makes its directories under `/run/chef` in `start_pre` with `checkpath`, which sets owner and mode on every start; the socket is in the daemon's own directory, 0660 (`tools/common/linesrv.c` sets it between `bind()` and `listen()`), so its group can connect. The full table is in [runtime files](runtime-files.md#layout); the owners:

| Directory | Owner and mode |
|---|---|
| `/run/chef/buttond/`, `sensord/`, `blesensd/` | the daemon's user and group, 0750 |
| `/run/chef/gps-manager/` | `gps:gps` 0750 |
| `/run/chef/power/` | `powerd:powerd` 0755 (`powerd status` reads it as anyone) |
| `/run/chef/chef-settings/` | `chefui:chefui` 0755; `/run/chef/ui/` `chefui:chefui` 0700; the file `/run/chef/lock/ui-settings.lock` `chefui:chefui` 0600 |
| `/run/chef/state/mag/`, `state/gps/`, `state/blesens/` | `sensord`, `gps`, `blesensd`, 0700: chef-state's lock and RAM files for the saves run on that daemon's behalf |
| `/run/chef/screen/` | `root:screen` 0775 (`/init`), `fb0.lock` in it root:screen 0660 |
| `/run/chef/sensors/` | root 0755 (`sensors-up registry`): the pristine registry copy `sns.reg.persist` is 0444, so sensord's bias saves can compare against it |
| `/run/chef/qmux_socket` | `root:qmux` 0660 (modem-up sets it after qmuxd-lite binds; failing is fatal) |
| `/run/chef/lock/modem-up.lock/` | `root:qmux` 0750 (modem-up; failing is fatal), so gps-manager reads `ready` and `owner` |

`/run/chef`, `log/`, `lock/` and `state/` themselves stay root 0755.

Outside `/run/chef`: `/var/lib/upower` (UPower's history files, on the volatile `/var`) is `upower:upower` 0755, from the `upower` service's `start_pre` (`checkpath`); the image's copy from the package is root's.

## /data

`chef-storage boot` ([persistent data](storage.md)) sets `/data/chef-layout` 0644 and `/data/v1` 0711 on every boot (and `format` makes them so): the non-root consumers must read the marker, and reach their own directories without listing `v1`. Every boot, before any service starts, it makes `power` (powerd), `sensors` (sensord), `gps` (gps), `blesens` (blesensd) and `ui` (chefui), each 0700, and `chown -R -h`s each to its user, so files a root process wrote there (images before 2026-10-23, a shutdown flush) belong to the daemon again; `crash`, `time` and `ssh` stay root's, `chrony` chrony's. Then it syncs `/data/v1` once, so those directory entries are durable: the daemons cannot open `/data/v1` for a sync of their own. The chef-settings Storage page opens `/data/v1` with `O_PATH` (search, never list) for the same reason.

## Root and the daemons' directories

**Root never creates, writes, truncates or mounts through a path inside a directory a daemon user owns** (`/run/chef/<daemon>/`, `/run/chef/state/<kind>/`, `/data/v1/<dir>` of the five). A daemon could have put a link there. Root may there only: `unlink` (`rm -f`, which never follows), `chown -R -h` (no traversal, busybox's default `-P`), `checkpath -d` of the directory itself (its parent is root's), and `mv -fT` of a file it prepared in a root-owned directory (the rename replaces a planted link; `-T` refuses a directory). So:

- `/init` sets `fs.protected_hardlinks=1` and `fs.protected_symlinks=1` (both 0 on this kernel by default) before `chef-storage boot`. With protected hardlinks a daemon cannot hard-link a root file into its directory, which keeps chef-storage's `chown -R -h` to the daemon's own files.
- `sensors-up registry` builds sensord's working copy in its own `/run/chef/sensors`, then `chown sensord:sensord`, `chmod 0600` and `mv -fT` into `/run/chef/sensord/sns.reg`.
- `chef-state shutdown` runs the daemons' work as the daemon, with busybox `su -s /bin/sh <user> -c ...` (root needs no password): the last bias save as `sensord`, the power log flush as `powerd`. Their own log lines go to syslog, since kmsg is root's.
- No lock file is pre-created by root in a daemon's directory: chef-state, running as the daemon, makes its lock on first use.
- The screen files are opened with `O_NOFOLLOW` everywhere (`fbdev.h`'s lock, chefui's `fb0.lit` and `off`, buttond's `off`, fbtouch, fblog): root tools use them too, and the directory is group-writable.
- The one exception: the hand-typed live-test commands that write chef-settings' `args`, `stop` and `prompt` as root into `/run/chef/chef-settings` ([live testing](../live-testing.md#gotchas)). Nothing automatic runs them; chef-settings reads `args` only when it is a root-owned regular file, and `prompt` only when it is also not group or other writable; it must be able to read both as `chefui`: 0644, never 0600.

## D-Bus names: UPower and SensorProxy

Both daemons own their system-bus name as their own user, through a policy file of ours in `rootfs-overlay/usr/share/dbus-1/system.d/` (where our overlay keeps D-Bus files):

- **`chef-upower.conf`**: user `upower` may own `org.freedesktop.UPower`. It only adds the owner; the package's `org.freedesktop.UPower.conf` stays as shipped (root may own, everyone may call). The image has no activation file for UPower (`scripts/mkinitramfs.sh` removes it): dbus-daemon-launch-helper would start a root upowerd whenever the service is down. UPower's own polkit actions are not used here: its one write, `EnableChargeThreshold`, finds no threshold attributes on our battery ([battery](battery-and-charging.md#upower)).
- **`net.hadess.SensorProxy.conf`**: user `sensorproxy` may own `net.hadess.SensorProxy`, and everyone may call it on `net.hadess.SensorProxy`, `net.hadess.SensorProxy.Compass`, `org.freedesktop.DBus.Properties`, `Introspectable` and `Peer` (iio-sensor-proxy's own list). No polkit: upstream gates claims with `claim-sensor` for an active logind session, which this image does not have; whoever may call may claim ([sensors](sensors.md#d-bus-nethadesssensorproxy)). Its methods carry `SD_BUS_VTABLE_UNPRIVILEGED`, without which sd-bus itself refuses every caller but root and its own uid (found in the live check of 2026-10-24).

## The clock: chrony and gpsd

- **NTP SHM unit 2.** gpsd runs as `gps`; gpsd 3.27.3 started as non-root creates and uses only units 2 and up (`shmget(IPC_CREAT | 0666)`), so chrony reads `refclock SHM 2:perm=0660` (a driver option, after the colon). chronyd creates the segment at start, before it drops root, so the segment takes chronyd's starting uid and gid: `rootfs-overlay/etc/conf.d/chronyd` starts it as `command_user="root:chrony"` (supervise-daemon sets gid chrony, stays uid 0 with every capability, and chronyd's own `-u chrony` drops the rest later), and the segment comes out root:chrony 0660. `gps` is in group `chrony`, so gpsd's `shmget` passes the group check. Details in [GPS time](gps.md#gps-time).
- **Commands.** chronyd's Unix socket `/run/chrony/chronyd.sock` takes every command, but only from root or chrony (chronyc binds its reply socket in `/run/chrony`, 0700): chef-state, the timekeeper and the NetworkManager dispatcher use it. Its UDP command port 323 listens on localhost only, 127.0.0.1 (`bindcmdaddress 127.0.0.1`) and ::1 (chrony's default), and answers monitoring commands only (`tracking`, `sources`) from localhost (the default `cmdallow`): chef-settings' Time page runs `chronyc -c -n -h 127.0.0.1 tracking` as `chefui`. `port 0`: no NTP server.

## NetworkManager and polkit

NetworkManager's D-Bus policy lets any user call it; NM itself authorizes. With `auth-polkit=true` (`etc/NetworkManager/conf.d/90-chef.conf`) root is still always allowed, and everyone else is asked of polkit. `rootfs-overlay/usr/share/polkit-1/rules.d/60-chef-networkmanager.rules` gives the `netdev` group (chef-settings, as `chefui`) every `org.freedesktop.NetworkManager.*` action without authentication; anyone else gets the defaults of NetworkManager's policy, which here, with no authentication agent, means no. The stock `50-default.rules` keeps its wheel admin rule; NetworkManager's own `01-org.freedesktop.NetworkManager.settings.modify.system.rules` (the `plugdev` group, and only for an active session) never matches here: there is no `plugdev` group and no logind.

- **polkitd** (polkit 127, with polkit-common, polkit-noelogind-libs, duktape-libs and linux-pam, about 4.3 MB installed; `scripts/mkrootfs.sh`) is not an OpenRC service: D-Bus activates it with the package's service file on the first check (dbus-daemon-launch-helper starts it as root), and it drops to user `polkitd` before it takes its bus name `org.freedesktop.PolicyKit1`; it then stays. Since 2026-10-24 upowerd's start asks polkit about its actions, so polkitd is activated at boot and runs from then on (as `polkitd`; the polkit setup is unchanged).
- **`/etc/polkit-1/rules.d`** is root:polkitd 0750, as polkit-common ships it (apk `--usermode` drops the group): `scripts/mksystem.sh` sets it in `system_a` (and reads it back), `/init` in the RAM image. Our rule lives in `/usr/share/polkit-1/rules.d` (0755). There is no `/etc/polkit-1` of ours.
- **No new setuid or setgid files.** `scripts/mkinitramfs.sh` removes polkit's `pkexec` and `polkit-agent-helper-1` (setuid root in the package; no authentication agent runs) and linux-pam's `unix_chkpwd` (setgid shadow; pam_unix password checks for non-root users, which nothing here does). The packages' setuid and setgid files left are the D-Bus launch helper and `wall`; apk `--usermode` drops every bit anyway, and the image's only privileged file is the launch helper (root:messagebus 4750, from `mksystem.sh` and `/init`). `scripts/tests/test_setuid.py` checks both.

## What stays root

| Service | Why |
|---|---|
| `syslog`, `udhcpd`, `bluetooth`, `networkmanager` | packaged daemons, root as on Alpine |
| `dbus` | dbus-daemon drops to `messagebus` itself |
| `chronyd` | starts as `root:chrony` and drops to `chrony` itself |
| `chef-sshd` | logins |
| `bt-up` | the UART, rfkill, the HCI attach |
| `modem-up` and its children | raw block devices, the subsystem nodes, the firmware mount, the AF_MSM_IPC servers; not supervised by OpenRC |
| `sensors-up`, `audio-up` | the ADSP boot, the persist mount |
| `root-session-bus` | root's own session bus |
| `chef-timekeeper` | the RTC and chrony's control socket for the clock state; all its input is local and trusted |

polkitd is new and drops to `polkitd` itself.

## The lost kmsg fallback

`/dev/kmsg` is root 0644, so a non-root daemon's `chef_log` lines are lost while syslogd is down (its respawn takes about a second); syslog itself (`/dev/log`, 0666) works for every user ([logging](logging.md)). Accepted: the window is short.

## How to check

| What | How |
|---|---|
| Users and groups of the running daemons | `ps -o user,group,args` |
| One process's groups, capabilities, no_new_privs | `grep -E '^(Groups\|Cap(Inh\|Prm\|Eff\|Amb)\|NoNewPrivs)' /proc/PID/status` (CapEff 0x20 is cap_kill, 0x400 cap_net_bind_service) |
| The nodes and attributes | `ls -l /dev/uinput /dev/input/event* /dev/fb0 /dev/sensors /sys/class/leds/lcd-backlight/brightness /sys/class/timed_output/vibrator/enable /sys/class/power_supply/battery/system_temp_level` |
| Runtime directories and sockets | `ls -la /run/chef /run/chef/*/ /run/chef/state/*/` |
| /data | `ls -la /data /data/v1` |
| The SHM segment | `ipcs -m`, then `ipcs -m -i <id>` for key `0x4e545032`: owner root, group chrony, mode 660 |
| NetworkManager as chef-settings would see it | `su -s /bin/sh chefui -c 'nmcli general permissions'` (all `yes`) |
| chrony as a non-root user | `su -s /bin/sh chefui -c 'chronyc -h 127.0.0.1 tracking'` |
| Who owns the bus names | `dbus-send --system --print-reply --dest=org.freedesktop.DBus / org.freedesktop.DBus.GetConnectionUnixUser string:org.freedesktop.UPower` (207; `string:net.hadess.SensorProxy`: 208) |
| Setuid and setgid files | `find / -xdev -perm /6000 -type f` (only `/usr/libexec/dbus-daemon-launch-helper`) |

## Tests

`scripts/tests/test_users.py` (accounts, groups, the udev rules, NetworkManager's polkit setting and rule), `scripts/tests/test_services.py` (`test_daemon_users`: every service's supervise-daemon command line, `--user`, `--capabilities`, `--no-new-privs`; `test_runtime_directories`: each `checkpath`; chrony's command addresses; the UPower and SensorProxy bus policies on a private `dbus-daemon` that, like the system bus, lets nobody own a name by default: each file's user owns its name and no other), `scripts/tests/test_setuid.py`, `tools/tests/test_storage.py` (the `/data` modes, owners and the one sync), `tools/tests/test_chef_state.py` (the kind directories, the shutdown's `su` lines, no `/data/v1` sync from a save), `tools/tests/test_wifi_nm.py` (`/init`'s rules directory fixup), `tools/tests/test_modem-up.sh` (the qmux socket and lock modes), `scripts/tests/test_run_layout.py` (`/init`'s screen directory and link protection). Host tests run as the developer's uid and cannot chown: ownership is checked through stubs and command lines, and what only the phone can show is in the live check.
