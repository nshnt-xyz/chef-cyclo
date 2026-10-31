# Live testing on the phone

[Project overview](../README.md) · [Build and boot](building.md) · [Device reference](device.md)

Two host scripts drive every live test: `scripts/phone-boot.sh` boots an image
and `scripts/phone.py` talks to the booted image's USB shell. `fastboot boot`
flashes nothing; a normal reboot boots the image installed in `boot_a` (ours
since 2026-10-04, see [standalone boot](features/boot.md#standalone-boot)),
not Android. Host needs: `fastboot`, `telnet` and python3-pexpect (`adb` only
for the old Android path).

## Boot an image

```sh
scripts/phone-boot.sh                          # out/boot.img
scripts/phone-boot.sh out/boot-test.img        # any image
EXPECT=<sha256> scripts/phone-boot.sh IMG      # refuse a different build
```

It gets the phone into fastboot from wherever it is (already in fastboot, one of
our images via `chef-reboot bootloader` over telnet, or stock Android via
`adb reboot bootloader` if it is ever reinstalled), runs `fastboot boot`, and waits until 172.16.42.1
answers (about 20 s after `fastboot boot`). If the first `fastboot boot` fails,
as it can right after `adb reboot bootloader` (garbled `getvar` replies,
`FAILED (remote: 'unknown command')`), it runs `fastboot reboot bootloader` and
retries once. If that fails too, move the cable to another USB port.

Every adb and fastboot call is pinned to the phone's serial (`CHEF_SERIAL`,
default `ZF6223WZGL`, which our images' USB gadget also reports), and the
172.16.42.1 checks also require the host interface on 172.16.42.0/24 to belong
to a USB device with that serial. The script refuses to run while another
fastboot device is attached, and before falling back to adb, while another adb
device is attached: on 2026-10-31 a second phone was plugged in, and the old
unscoped `adb reboot bootloader` would have hit it first.

To return to fastboot by hand from one of our images, run `chef-reboot
bootloader` (in a telnet session or detached with `setsid`) rather than a bare
`btprobe restart bootloader`: it pauses init, runs `chef-state shutdown` (RTC
offset, chronyd's drift file, pending power log rows), `chef-state log-save`
(the syslog ring onto `/data/v1/log`), `openrc shutdown` and
`chef-stop-supervisors` (every other service, the modem's clean stop
included; about 24 s live against 14 s in a power-off, see
[services](features/services.md#shutdown-reboot-and-the-return-to-fastboot))
and `chef-storage shutdown` (`/data` read-only and detached), syncs and then
calls `btprobe restart bootloader`. A failing or hanging step is logged to kmsg and
`/run/chef/log/chef-reboot.log` and the reboot still happens; if btprobe itself returns,
init is resumed. Images built before the helper (2026-10-10) lack it;
`scripts/phone-boot.sh` falls back to `chef-storage shutdown --pause-init; sync;
sleep 2; btprobe restart bootloader` there, which loses that state. Stage 1's
rescue shell has neither chef tool, so `btprobe restart bootloader` is the way
there.

Build test images under another name (`OUT=out/boot-foo.img scripts/mkboot.sh`)
so `out/boot.img` stays the known-good baseline. A test image marks `boot_a`
successful 30 s after boot like the installed one (`abslot`); while `boot_a` is
not yet marked (right after a flash), every boot, `fastboot boot` included,
takes one of its seven retries.

## Talk to the booted image

```sh
scripts/phone.py run 'uname -a; cat /proc/uptime'   # exit status = remote status
echo 'logread | tail' | scripts/phone.py run         # command on stdin
scripts/phone.py -t 300 run 'long-command'           # timeout in s (default 60)
scripts/phone.py push build/tool -d /run/test        # verified by sha256
scripts/phone.py pull /run/evidence -o ev.tgz        # byte-exact tar.gz
scripts/phone.py stream 'logread -f' syslog.txt     # until the phone goes away
scripts/phone.py stream 'cat /dev/kmsg' kmsg.txt     # the kernel's side
scripts/phone.py --log DIR run '...'                 # keep a session transcript
```

Where the lines are ([logging](features/logging.md)): our daemons and
scripts, NetworkManager, BlueZ, wpa_supplicant, D-Bus and chrony in
syslogd's RAM ring (`logread`, `logread -f`, `logread | grep TAG`); the
kernel, `/init`, stage 1, the boot-time `chef-state` and `chef-storage`
lines, everything on the way down and lines from before syslogd started in
`dmesg`. Save both as evidence (`logread > syslog.txt; dmesg > dmesg.txt`).

`telnet 172.16.42.1` gives the same passwordless root shell interactively.

Without the USB cable (for example during physical tests), images built since
2026-10-10 take [SSH](features/ssh.md) logins with the dedicated key on every
interface: `scripts/phone.py --ssh ADDR run|push|pull|stream ...` (or
`PHONE_SSH=ADDR`) with the Wi-Fi address from the Wi-Fi details page or
`logread | grep chef-wifi-ip` (`chef-wifi-ip: ... ssh root@ADDR`). `scripts/phone-boot.sh` still needs USB (fastboot).
Push a rebuilt static binary instead of rebooting when iterating on a tool;
`/run` and `/tmp` are RAM.

### Services

Since 2026-10-21 OpenRC starts and supervises the services
([services](features/services.md)); `inittab` keeps only telnetd, OpenRC
itself, `abslot` and the way down.

```sh
rc-status                       # what runs; rc-status -c: crashed, rc-status -a: every script
rc-service powerd restart       # stop|start|restart one service (up to 1 s per stop)
rc-service sensord stop         # also stops what needs it (sensors-up)
logread | grep supervise-daemon # starts, exits and respawns of the supervised daemons
cat /var/log/rc.log             # OpenRC's own output at boot and shutdown
```

A supervised daemon that dies or is killed (`kill -9` included) is started
again by its supervisor after 1 s, so to keep one down, or to run it by hand
with other options, stop its service first. A new `chef-sshd` or `bt-up`
first stops the dropbear or btattach a killed predecessor left (one log
line). `modem-up` refuses `rc-service modem-up stop|restart` (one modem
lifetime per boot) and leaves everything running; a stopped
service with a precondition that failed at boot (`rc-status` shows it
stopped or failed) is retried with `rc-service <name> start`. `rc-update`
does not work on the read-only root: change
`rootfs-overlay/etc/runlevels/default/` and rebuild.

## Gotchas

- **Root writes into chef-settings' directory by hand only.** The commands
  that drive the system UI in a live test write `args`, `stop` and the
  guided-step `prompt` as root into `/run/chef/chef-settings`, which
  belongs to chef-settings' user (`chefui`). That is the one exception to
  the rule that root never creates or writes through a path in a daemon's
  directory ([users and permissions](features/users-and-permissions.md)):
  they are typed by hand, and nothing automatic runs them. `args` and
  `prompt` must stay root-owned and readable by `chefui` (0644, which
  root's umask 022 gives): a 0600 `prompt` is ignored with `prompt
  ignored: ...: Permission denied`, a 0600 `args` is skipped without a
  log line and left in place.
- **Background jobs die with the session.** Closing telnet SIGHUPs foreground
  and `&` jobs. Start anything that must outlive the command with
  `setsid CMD </dev/null >/run/CMD.out 2>&1 &`.
- **`pull` reads regular files.** `/proc` and `/sys` entries come through tar
  as zero bytes; read them with `run 'cat ...'`.
- **busybox, not GNU.** `base64` has no `-w` (the scripts wrap and rejoin);
  the applet links are made at build time (`scripts/link-applets.sh`).
  busybox `timeout` leaves a daemonized watchdog (stdio on `/dev/null`) for up
  to a second after the command ends.
- **The root is read-only** with the installed stage-1 image (`system_a`,
  see [installed layout](building.md#installed-layout-phase-3)). Push test
  binaries to `/run` or `/tmp`; `/var` and `/root` are tmpfs, `/data` is
  persistent. Package installs (`apk add`) need the RAM image.
- **Rescue shell.** If stage 1 refuses `system_a` (wrong slot, missing or
  foreign partition, wrong label or stamp, mount failure) it stays in its
  ramdisk with the same USB network: `telnet 172.16.42.1`, the reason in
  `/run/rescue-reason` and `dmesg | grep cyclo-stage1`. Only busybox and
  `btprobe` are there; `scripts/phone-boot.sh` gets from it to fastboot as
  from any of our images.
- **Our runtime files are under `/run/chef`** (sockets, markers, logs, locks;
  [runtime files](features/runtime-files.md)) in images built since
  2026-10-20. Older images keep the same names directly under `/run`, and
  the screen-off flag there is `/run/fblog.off`. Images built before
  2026-10-23 have the sockets directly in `/run/chef` (`buttond.sock`,
  `sensord.sock`, `blesensd.sock`, `gps-manager.sock`), the registry copy in
  `/run/chef/sensors` and the screen files there too (`fb0.lock`, `fb0.lit`,
  `screen-off`); since then each socket is in its daemon's directory
  (`/run/chef/buttond/buttond.sock` and so on) and the screen files in
  `/run/chef/screen` (`fb0.lock`, `fb0.lit`, `off`). The scratch names above
  (`/run/push`, `/run/x`, `/run/CMD.out`) work on all of them.
- **Evidence under `/run` and `/tmp` is in RAM.** Pull it before rebooting. A phase 2 image persists the reviewed BlueZ and NetworkManager directories under `/data`; see [storage](features/storage.md).
- **`poweroff` with USB attached comes back.** The cable re-powers the phone in
  about 24 s (`androidboot.mode=charger`), and the installed image boots (the stage-1 layout ran the full OS on such a charger-mode boot on 2026-10-05). Unplug
  first if it should stay off.
- **A kernel panic is in pstore for one boot only.** A panic comes back by
  itself after 30 to 50 s (the panic restart wedges until the watchdog bites)
  with a warm PON and `androidboot.bootreason=kernel_panic`. On that very boot,
  `mount -t pstore -o ro pstore /run/x` and copy `dmesg-ramoops-0` and
  `console-ramoops-0` off the phone; the next ordinary reboot is a cold PON
  and the records are gone. The 2026-10-05 shutdown hang (a Bluetooth UART
  use-after-free, fixed in kernel #22) was found this way, see the
  [shutdown hang](archive/shutdown-hang-handoff.md#result-rootfs_impl-2026-10-05).
- **The panel is not visible from here.** Display, touch and button checks need
  someone watching the phone. Announce a window ("tap now for 25 s"), then
  run the check (`fbtouch show -t 25 -r`, `chefui-demo`), and ask what they saw.
  For guided multi-step runs use numbered `STEP k/N` lines and give at least
  8 s to move and 10 s to hold.
- **Do not `cat /proc/nvt_fw_version`** (or touch IC sysfs) while the panel is
  off: it does I2C reads on the unpowered controller and breaks touch until the
  next resume.

## Evidence

Keep raw transcripts and pulls of a live run in one directory per run, for example
`~/chef-cyclo-evidence/<topic>-<UTC stamp>/` (outside the repository), and copy
the parts worth keeping into `logs/` with a [build log](build-log.md) entry.
Record the image hash (`phone-boot.sh` prints it).

Back to the installed image: `scripts/phone.py run reboot`. Into fastboot
without a working image: hold Power with VolDown for about 9 s. A fallback
for `boot_a` that never reads `system_a` is the full RAM image
(`out/boot.img`, the baseline `42f362b0` with kernel #22, or a fresh `out/boot-ram.img`). Android is
retired; its restore route is under
[images to restore](features/boot.md#images-to-restore).
