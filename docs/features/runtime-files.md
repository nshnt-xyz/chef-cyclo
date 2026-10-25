# Runtime files: /run/chef

[Feature index](README.md) · [Local socket protocol](local-sockets.md) · [Persistent data](storage.md)

Everything our own programs keep in RAM at run time lives under `/run/chef/`: sockets, boot markers, logs, locks and one directory per component. Other packages keep their standard names directly under `/run`. Nothing of ours sits beside them since standard interfaces step 5 (2026-10-20), and there are no compatibility symlinks at the old paths: every client of every socket ships in the same image.

## Who creates what

`/init` mounts the `/run` tmpfs and immediately creates `/run/chef`, `/run/chef/log`, `/run/chef/lock` and `/run/chef/state` with `mkdir -p -m 0755`: owned by root and not group or world writable, since root-only files sit below them (the [SSH](ssh.md) host key in `/run/chef/ssh`). Right after, it creates the screen handover directory `/run/chef/screen` (root:screen 0775) and the screen lock `screen/fb0.lock` in it (root:screen 0660), so every member of the `screen` group can open the lock and remove a flag another member made.

Each daemon's service makes the daemon's directory in its `start_pre` with OpenRC's `checkpath`, which sets the owner and mode on every start: `buttond/`, `sensord/`, `blesensd/`, `gps-manager/`, `power/`, `chef-settings/`, `ui/`, and, for the `chef-state` saves run on a daemon's behalf, `state/mag/`, `state/gps/` and `state/blesens/` (the owners are in the table and in [users and permissions](users-and-permissions.md)). A daemon's socket is in its own directory, mode 0660 whatever the umask (`tools/common/linesrv.c` sets it between `bind()` and `listen()`), so its group can connect and the directory's 0750 keeps everyone else out. Everything else is created by its owner as needed, with the mode in the table. Files a program writes atomically get a temp name beside the file they replace (`state/time.tmp`, `power/.state.tmp`) and are renamed over it; a lock with no owning directory goes to `lock/`.

C code takes these paths from `tools/common/chef_paths.h` (sockets, the screen files, `power`, `sensord/sns.reg`, `gps-manager`, `lock/modem-up.lock`, `log/abslot.log`, `state/time`, the settings store's base), so a client and its daemon cannot disagree. Shell scripts spell the paths out. Programs that take a run base directory default to `/run/chef` and build their paths from it: `chef-state` (`CHEF_STATE_RUN`), `chef-storage` (`CHEF_STORAGE_RUN`), `chef-reboot` (`CHEF_REBOOT_RUN`), `speaker-test-tone` (`RUN_DIR`), the settings store (`--run` of `chef-settings`, the same base for its `chef-settings` directory). Every override keeps working, and the host tests use them.

## Layout

The mode column gives the owner and group where they are not root.

| Path under `/run/chef/` | Owner (creates it) | Mode | What |
|---|---|---|---|
| `buttond/` | the `buttond` service (`checkpath`) | `buttond:buttond` 0750 | `buttond.sock` (0660), the [buttons](buttons-and-power-off.md#socket-contract) socket |
| `buttond/poweroff` | `buttond` (just before its power-off action; failing to make it is logged and never stops the power-off) | `buttond:buttond` 0644, empty | the power-off hint: it tells `chef-state shutdown` the reason is a power-off ([buttons](buttons-and-power-off.md)) |
| `sensord/` | the `sensord` service (`checkpath`) | `sensord:sensord` 0750 | `sensord.sock` (0660), the [sensors](sensors.md#socket-contract) socket; the working registry copy `sns.reg` (`sensord:sensord` 0600), which `sensors-up registry` builds in `sensors/` and moves in (`mv -fT`), and into which `sensord` writes the DSP's writes back |
| `sensors/` | `sensors-up registry` | 0755 | the registry copy as read from persist `sns.reg.persist` (0444, so the bias saves run as `sensord` can compare against it), the boot it was made in `sns.reg.boot_id`, the staging copy `sns.reg.tmp`, and `persist-ro/` while persist is mounted |
| `blesensd/` | the `blesensd` service (`checkpath`) | `blesensd:blesensd` 0750 | `blesensd.sock` (0660), the [BLE sensors](bluetooth.md#socket-contract) socket |
| `gps-manager/` | the `gps-manager` service (`checkpath`) | `gps:gps` 0750 | `gps-manager.sock` (0660), the [GPS leases](gps.md#socket-contract) socket; the output of its qmicli, broker and gpsd children |
| `qmux_socket` | `qmuxd-lite` (started by `modem-up`, which sets its group) | `root:qmux` 0660 | the QMUX bridge for `qmicli -d`; libqmi recognises it by the name `qmux_socket` in the path, so it stays here |
| `screen/` | `/init` | `root:screen` 0775 | the screen handover; group write lets one member remove a flag another made, so every program opens or creates the three files with `O_NOFOLLOW` (root tools use them too) |
| `screen/fb0.lock` | `/init` | `root:screen` 0660 | the [screen lock](display-and-touch.md#framebuffer-and-touch-contract): `LOCK_EX` before fb0 is opened (`chef-settings`, `fbtouch`, a hand-run `fblog`) |
| `screen/fb0.lit` | chefui applications | 0644 | the panel is lit; a start that finds it power-cycles the panel once ([UI platform](ui-platform.md)) |
| `screen/off` | chefui applications, `buttond`'s default `power.short`, a shell | 0644 | the screen is deliberately off ([display and touch](display-and-touch.md#the-screen-off-flag)) |
| `udev-ready` | `/init` | 0644 | the boot coldplug done (NetworkManager waits for it) |
| `dbus-helper-ready` | `/init` | 0644 | the D-Bus launch helper is set up (the system bus waits for it) |
| `wifi-nm-mode` | `/init`, `wifi-nm` | 0644 | NetworkManager owns WLAN; `wifi-up` refuses to run |
| `wifi-usb-guard-ready` | `wifi-usb-guard` | 0644 | the USB-interface firewall guard is in place (`wifi-prepare` checks it) |
| `bt-powered` | `bt-up` | 0644 | the Bluetooth controller was attached this boot |
| `wlan-prepare/` | `wifi-prepare` | 0755 | one WLAN preparation attempt per boot (`mkdir` as the test) |
| `usb-interface`, `udhcpd.conf` | `/init` (`usr/lib/chef/usb-gadget.sh`) | 0644 | the USB network interface name, and `/etc/udhcpd.conf` with it filled in for `udhcpd` |
| `modem-up.pid` | start-stop-daemon (the `modem-up` service) | 0644 | the pid of `modem-up` ([services](services.md)) |
| `log/modem-up.log` | the `modem-up` service (start-stop-daemon) | 0644 | `modem-up` |
| `log/sensors-up.log` (`.1` from images before 2026-10-21) | the `sensord` and `sensors-up` services (appended: a few lines per start) | 0644 | `sensors-up registry` and `sensors-up adsp`, and the `audio-up` it starts |
| `log/abslot.log` | `abslot` | 0644 | A/B slot marking |
| `log/audio-up.log` | by hand ([audio](audio.md)) | 0644 | `audio-up` run by hand |
| `log/chef-reboot.log` | `chef-reboot` | 0644 | the reboot helper's failures |
| `log/storage.log`, `log/storage-watchdog.log` | `chef-storage` | 0644 | `/data` checks and mounts, the shutdown watchdog |
| `lock/modem-up.lock/` (with `ready`) | `modem-up` | `root:qmux` 0750 (so `gps-manager`, as `gps`, can read `ready` and `owner`) | modem ownership: a directory while `modem-up` owns the modem, `ready` once its stack is up; read by `gps-manager`, `wifi-prepare` and `usr/lib/chef/modem-owner.sh`. `modem-up` was called `gps-up` until 2026-10-21, and so were this lock and `log/modem-up.log` (older logs say `gps-up.lock`, `gps-up.log`) |
| `lock/audio-up.lock/` | `audio-up` | 0755 | one `audio-up` at a time (`sensors-up adsp` starts none while it exists) |
| `lock/sensors-up.lock` | `sensors-up` | 0644 | `flock`: one `sensors-up` |
| `lock/storage.lock` | `chef-storage` | 0644 | `flock`: one storage operation |
| `lock/chef-reboot.lock` | `chef-reboot` | 0644 | `flock`: one reboot at a time |
| `lock/ui-settings.lock` | the `chef-settings` service (`checkpath`) | `chefui:chefui` 0600 | `flock`: one settings writer (made by the service, so the store needs no write access to `lock/`) |
| `speaker-test-tone-<pid>.wav` | `speaker-test-tone` (`RUN_DIR`) | 0644 | its test tone while it plays, removed after |
| `power/` | the `powerd` service (`checkpath`) | `powerd:powerd` 0755 | [battery](battery-and-charging.md) state (world-readable: `powerd status`), `shutdown-pending`, the RAM power log and `log.pending` |
| `sensortest/` | `sensors-compass-run`, `sensors-magcal-run` | 0755 | [sensor](sensors.md) test evidence directories |
| `wifi/` | `wifi-up` | 0755 | a manual Wi-Fi session (never with NetworkManager) |
| `ssh/` | `chef-sshd` | 0700 | the host key dropbear serves, `host-key-source` |
| `ui/` | the `chef-settings` service (`checkpath`) | `chefui:chefui` 0700 | `settings` while `/data` is not ours ([system UI](system-ui.md#settings)) |
| `chef-settings/` | the `chef-settings` service (`checkpath`) | `chefui:chefui` 0755 | `stop`, the one-shot `args`, `prompt`, `snapshot.ppm` and `splash-done` |
| `chef-settings/splash-done` | `chef-settings` | 0644, empty | the [boot splash](system-ui.md#boot-splash) has ended this boot; with it present a restarted `chef-settings` goes straight to the hub |
| `modem-ready` | `modem-up` (right after its lock's `ready`; removed in its cleanup) | `root` 0644, empty | the modem is up, for readers outside the `qmux` group (the boot splash). Information only, never ownership: that stays with `lock/modem-up.lock/` |
| `shutting-down` | `chef-state shutdown [poweroff\|reboot]` as its very first act (`chef-reboot` passes `reboot`; temp file in `/run/chef`, then `mv -f`; the first writer wins, an existing one is never rewritten); `chef-reboot` removes it when it resumes init without rebooting | `root` 0644, one line | the system is going down, and why: `poweroff`, `reboot` or `unknown`. Without an argument the reason is `poweroff` when `power/shutdown-pending` or `buttond/poweroff` exists, else `unknown`. It puts up the shutdown screen of `chef-settings` |
| `state/` | `/init` | 0755 | `chef-state`'s files ([persistent data](storage.md#persistent-state)): `time` (boot clock provenance, read by the Time and date page), `time.lock` and `log.lock`, and the directories below |
| `state/mag/` | the `sensord` service (`checkpath`) | `sensord:sensord` 0700 | `lock` (the bias saves), `last` (when the last one was written) |
| `state/gps/` | the `gps-manager` service (`checkpath`) | `gps:gps` 0700 | `lock`, and `enabled` (the GPS switch) while `/data` is not ours |
| `state/blesens/` | the `blesensd` service (`checkpath`) | `blesensd:blesensd` 0700 | `lock`, and `sensors` (the BLE sensor records) while `/data` is not ours |

## What stays outside

- **Other packages' standard names:** `/run/dbus`, `/run/udev`, `/run/NetworkManager`, `/run/wpa_supplicant`, `/run/chrony`, `/run/user` and `/run/lock`, OpenRC's state in `/run/openrc` and its supervisors' pid files `/run/supervise-<service>.pid` ([services](services.md)), plus the default files of busybox `udhcpd` (`/run/udhcpd.pid`) and `syslogd` (`/run/syslogd.pid`, [logging](logging.md)) and iptables (`/run/xtables.lock`). `/init`'s `/run/.var` exists only while it moves the volatile `/var` into place.
- **Stage 1.** The boot image's rescue world (`stage1/init`) keeps `/run`: `/run/rescue-reason`, `/run/usb-interface` and `/run/udhcpd.conf`. `usr/lib/chef/usb-gadget.sh` is shared with it, so it writes into `$USB_GADGET_RUN`, default `/run` for stage 1; the stage-2 `/init` sets `USB_GADGET_RUN=/run/chef` before sourcing it.
- **Live-test scratch** that host tooling uses on any image, including the installed fallbacks: `/run/push` (`phone.py push`), `/run/x`, `/run/test`, `/run/evidence`, `/run/CMD.out` and `/run/bootloader-restart.log` (`phone-boot.sh`). Logs started by hand from a guide (`/run/gpsd.log`, `/run/compass.log` and the like) are scratch too.

`scripts/tests/test_run_layout.py` (part of `make test`) keeps it that way: it scans the files that make up the image for `/run/<name>`, and the defaults of the run-directory variables in the scripts, and fails on any top-level name that is not `chef` or in its allow list, each entry with its reason.
