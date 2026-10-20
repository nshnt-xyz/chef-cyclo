# Runtime files: /run/chef

[Feature index](README.md) · [Local socket protocol](local-sockets.md) · [Persistent data](storage.md)

Everything our own programs keep in RAM at run time lives under `/run/chef/`: sockets, boot markers, logs, locks and one directory per component. Other packages keep their standard names directly under `/run`. Nothing of ours sits beside them since standard interfaces step 5 (2026-10-20), and there are no compatibility symlinks at the old paths: every client of every socket ships in the same image.

## Who creates what

`/init` mounts the `/run` tmpfs and immediately creates `/run/chef`, `/run/chef/log` and `/run/chef/lock` with `mkdir -p -m 0755`: owned by root and not group or world writable, since root-only files sit below them (the [SSH](ssh.md) host key in `/run/chef/ssh`, the BLE sensor socket). Everything below them is created by its owner as needed, with the mode in the table. Files a program writes atomically get a temp name beside the file they replace (`state/time.tmp`, `power/.state.tmp`) and are renamed over it; a lock with no owning directory goes to `lock/`.

C code takes these paths from `tools/common/chef_paths.h` (sockets, the screen files, `power`, `sensors/sns.reg`, `gps-manager`, `lock/gps-up.lock`, `log/abslot.log`, `state/time`, the settings store's base), so a client and its daemon cannot disagree. Shell scripts spell the paths out. Programs that take a run base directory default to `/run/chef` and build their paths from it: `chef-state` (`CHEF_STATE_RUN`), `chef-storage` (`CHEF_STORAGE_RUN`), `chef-reboot` (`CHEF_REBOOT_RUN`), `speaker-test-tone` (`RUN_DIR`), the settings store (`--run` of `chef-settings`, the same base for its `chef-settings` directory). Every override keeps working, and the host tests use them.

## Layout

| Path under `/run/chef/` | Owner (creates it) | Mode | What |
|---|---|---|---|
| `buttond.sock` | `buttond` | 0755 (umask) | [buttons](buttons-and-power-off.md#socket-contract) |
| `gps-manager.sock` | `gps-manager` | 0660 | [GPS leases](gps.md#socket-contract) |
| `sensord.sock` | `sensord` | 0755 (umask) | [sensors](sensors.md#socket-contract) |
| `blesensd.sock` | `blesensd` | 0600 | [BLE sensors](bluetooth.md#socket-contract) |
| `qmux_socket` | `qmuxd-lite` (started by `gps-up`) | 0600 | the QMUX bridge for `qmicli -d`; libqmi recognises it by the name `qmux_socket` in the path |
| `fb0.lock` | the first screen client (`chef-settings`, `fbtouch`, a hand-run `fblog`) | 0600 | the [screen lock](display-and-touch.md#framebuffer-and-touch-contract): `LOCK_EX` before fb0 is opened |
| `fb0.lit` | chefui applications | 0644 | the panel is lit; a start that finds it power-cycles the panel once ([UI platform](ui-platform.md)) |
| `screen-off` | chefui applications, `buttond`'s default `power.short`, a shell | 0644 | the screen is deliberately off ([display and touch](display-and-touch.md#the-screen-off-flag)) |
| `udev-ready` | `/init` | 0644 | network coldplug done (NetworkManager waits for it) |
| `dbus-helper-ready` | `/init` | 0644 | the D-Bus launch helper is set up (the system bus waits for it) |
| `wifi-nm-mode` | `/init`, `wifi-nm` | 0644 | NetworkManager owns WLAN; `wifi-up` refuses to run |
| `wifi-usb-guard-ready` | `wifi-usb-guard` | 0644 | the USB-interface firewall guard is in place (`wifi-prepare` checks it) |
| `bt-powered` | `bt-up` | 0644 | the Bluetooth controller was attached this boot |
| `wlan-prepare/` | `wifi-prepare` | 0755 | one WLAN preparation attempt per boot (`mkdir` as the test) |
| `usb-interface`, `udhcpd.conf` | `/init` (`usr/lib/chef/usb-gadget.sh`) | 0644 | the USB network interface name, and `/etc/udhcpd.conf` with it filled in for `udhcpd` |
| `log/gps-up.log` | `inittab` (redirect) | 0644 | `gps-up` |
| `log/sensors-up.log` (`.1`) | `inittab` (redirect, rotated over 256 KiB) | 0644 | `sensors-up` |
| `log/abslot.log` | `abslot` | 0644 | A/B slot marking |
| `log/audio-up.log` | by hand ([audio](audio.md)) | 0644 | `audio-up` run by hand |
| `log/chef-reboot.log` | `chef-reboot` | 0644 | the reboot helper's failures |
| `log/storage.log`, `log/storage-watchdog.log` | `chef-storage` | 0644 | `/data` checks and mounts, the shutdown watchdog |
| `lock/gps-up.lock/` (with `ready`) | `gps-up` | 0700 | modem ownership: a directory while `gps-up` owns the modem, `ready` once its stack is up; read by `gps-manager`, `wifi-prepare` and `usr/lib/chef/modem-owner.sh` |
| `lock/audio-up.lock/` | `audio-up` | 0755 | one `audio-up` at a time (`sensors-up` waits on it) |
| `lock/sensors-up.lock` | `sensors-up` | 0644 | `flock`: one `sensors-up` |
| `lock/storage.lock` | `chef-storage` | 0644 | `flock`: one storage operation |
| `lock/chef-reboot.lock` | `chef-reboot` | 0644 | `flock`: one reboot at a time |
| `lock/ui-settings.lock` | `chef-settings` | 0600 | `flock`: one settings writer |
| `speaker-test-tone-<pid>.wav` | `speaker-test-tone` (`RUN_DIR`) | 0644 | its test tone while it plays, removed after |
| `power/` | `powerd` | 0755 | [battery](battery-and-charging.md) state, `shutdown-pending`, the RAM power log and `log.pending` |
| `sensors/` | `sensors-up` | 0755 | the registry copy `sns.reg` (with `.boot_id`, `.persist`), `persist-ro/` while persist is mounted, `sensord.pid` |
| `sensortest/` | `sensors-compass-run`, `sensors-magcal-run` | 0755 | [sensor](sensors.md) test evidence directories |
| `gps-manager/` | `gps-manager` | 0700 | the output of its qmicli, broker and gpsd children |
| `wifi/` | `wifi-up` | 0755 | a manual Wi-Fi session (never with NetworkManager) |
| `ssh/` | `chef-sshd` | 0700 | the host key dropbear serves, `host-key-source` |
| `ui/` | `chef-settings` | 0700 | `settings` while `/data` is not ours ([system UI](system-ui.md#settings)) |
| `chef-settings/` | `chef-settings` | 0755 | `stop`, the one-shot `args`, `prompt` and `snapshot.ppm` |
| `state/` | `chef-state` | 0755 | `time` (boot clock provenance, read by the Time and date page), `gps-enabled` and `blesens` (while `/data` is not ours), `mag-last`, `timekeeper.pid`, a `<kind>.lock` per saved kind and its transient `<kind>.err` ([persistent data](storage.md#persistent-state)) |

## What stays outside

- **Other packages' standard names:** `/run/dbus`, `/run/udev`, `/run/NetworkManager`, `/run/wpa_supplicant`, `/run/chrony`, `/run/user` and `/run/lock`, plus the default files of busybox `udhcpd` (`/run/udhcpd.pid`) and `syslogd` (`/run/syslogd.pid`, [logging](logging.md)) and iptables (`/run/xtables.lock`). `/init`'s `/run/.var` exists only while it moves the volatile `/var` into place.
- **Stage 1.** The boot image's rescue world (`stage1/init`) keeps `/run`: `/run/rescue-reason`, `/run/usb-interface` and `/run/udhcpd.conf`. `usr/lib/chef/usb-gadget.sh` is shared with it, so it writes into `$USB_GADGET_RUN`, default `/run` for stage 1; the stage-2 `/init` sets `USB_GADGET_RUN=/run/chef` before sourcing it.
- **Live-test scratch** that host tooling uses on any image, including the installed fallbacks: `/run/push` (`phone.py push`), `/run/x`, `/run/test`, `/run/evidence`, `/run/CMD.out` and `/run/bootloader-restart.log` (`phone-boot.sh`). Logs started by hand from a guide (`/run/gpsd.log`, `/run/compass.log` and the like) are scratch too.

`scripts/tests/test_run_layout.py` (part of `make test`) keeps it that way: it scans the files that make up the image for `/run/<name>`, and the defaults of the run-directory variables in the scripts, and fails on any top-level name that is not `chef` or in its allow list, each entry with its reason.
