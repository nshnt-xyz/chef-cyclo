# Services: busybox init and OpenRC

[Feature index](README.md) · [Boot and recovery](boot.md) · [Logging](logging.md) · [Runtime files](runtime-files.md) · [Persistent data](storage.md)

Since standard interfaces step 7 (2026-10-21) OpenRC starts, supervises and stops our services, the way Alpine does it: busybox `init` stays PID 1 and runs OpenRC from `inittab`. Host-tested; the live check is pending.

## Boot

1. Stage 1 (`stage1/init`, in `boot_a`) checks and mounts the read-only `system_a` root and switches to it ([boot](boot.md)).
2. `/init` (`rootfs-overlay/init`) mounts `/proc`, `/sys`, `/dev`, the `/run`, `/tmp`, `/var` and `/root` tmpfs, makes `/run/chef`, sets the host name, the D-Bus helper and the chrony directories, brings up loopback and the USB network, runs udevd with the network coldplug, `chef-storage boot` and `chef-state boot`, and execs busybox init.
3. busybox init runs `inittab`: the USB telnet shell, OpenRC, and `abslot`'s 30 s mark.
4. OpenRC runs its own sysinit step, then the `default` runlevel: every service below, one at a time in dependency order.

### What stays in inittab

| Line | Why not a service |
|---|---|
| `::respawn:` telnetd on 172.16.42.1 | The passwordless USB recovery shell: it must come up whatever happens to OpenRC. |
| `::once:` `openrc sysinit; exec openrc default` | OpenRC itself. `::once`, not `::wait`: busybox init runs `::sysinit` and `::wait` lines to completion before it starts any `::respawn` line, so a hanging service would hold back telnetd for ever. |
| `::once:` `sleep 30; abslot mark-successful` | Marks `boot_a` successful once the boot has lasted 30 s ([boot](boot.md#ab-slot-flags)); nothing to supervise. |
| `::ctrlaltdel:`, `::shutdown:`, `::restart:` | The way down (below). |

`scripts/tests/test_services.py` checks that `inittab` holds exactly these lines.

### OpenRC's sysinit step

`openrc sysinit` runs `/usr/libexec/rc/sh/init.sh` and the `sysinit` runlevel, which is empty (on Alpine it holds `devfs`, `dmesg`, `mdev`, `hwdrivers`, `cgroups` and the like; `/init` does that work here). From reading `init.sh` (0.63.2): `/proc` and `/run` are mounted already, so it mounts nothing; it creates `/run/openrc` (OpenRC's state) and `/run/lock` (root:uucp 0775; the `uucp` group is in `etc/group`, so no warning); there is no `/var/cache/rc` to copy; it writes `softlevel`. `openrc-run`'s cgroup code acts only when `/sys/fs/cgroup` is mounted, and nothing mounts it. So OpenRC mounts nothing; the live check compares `/proc/mounts` with the baseline. The `boot` runlevel is empty too and is not run.

### Settings

`rootfs-overlay/etc/rc.conf.d/chef.conf`, read by every `openrc-run` after the package's `/etc/rc.conf` (kept as shipped) and before the service's `conf.d` file, so the packaged services get them too:

| Setting | Why |
|---|---|
| `rc_parallel="NO"` | Serial and deterministic (`rc.conf` warns that a parallel start can lock the boot). |
| `rc_interactive="NO"` | No "press I" prompt; there is no console to answer it. |
| `rc_logger="YES"` | OpenRC's own output for each runlevel change (`openrc default`, `openrc shutdown`: every start and stop and its errors) in `/var/log/rc.log`, on the volatile `/var`. `rc-service` calls are not in it. |
| `respawn_delay="1s"`, `respawn_max=0` | Every supervised daemon is respawned for as long as its service runs (below). |
| `retry="TERM/5/KILL/2"` | A stop sends TERM, waits up to 5 s, then KILL and up to 2 s more. Without it OpenRC gives up after the 5 s and leaves the process running. |

## The services

All in the `default` runlevel (`rootfs-overlay/etc/runlevels/default/`). Third-party daemons use Alpine's packaged script, adjusted only by `etc/conf.d/<service>`; our programs have our own scripts in `etc/init.d/`, named after the program.

| Service | Script | Runs | Depends | Readiness and preconditions |
|---|---|---|---|---|
| `syslog` | packaged (`busybox-openrc`) | `syslogd -t -C1024 -n` ([logging](logging.md)) | provides `logger`; every other service uses it, so it starts first | |
| `chef-settings` | own | the system UI ([system UI](system-ui.md)) | before `modem-up`, `dbus`, `udhcpd`, `chronyd`, `bt-up` | picks up everything else whenever it appears |
| `modem-up` | own | the modem and its support daemons ([GPS](gps.md)) | after `chef-settings`, before the slower services | not supervised (below); its start returns at once, consumers wait for the modem themselves |
| `udhcpd` | packaged (`busybox-extras-openrc`) | `udhcpd -f -S /run/chef/udhcpd.conf -f` (DHCP for the USB host) | before `dbus` and `sensord`, so the lease does not wait for them | needs `/run/chef/udhcpd.conf` (from `/init`) |
| `dbus` | packaged | the system bus | | needs `/run/chef/dbus-helper-ready` (from `/init`); waits up to 10 s for `/run/dbus/system_bus_socket`, else the start fails |
| `bluetooth` | packaged (`bluez-openrc`) | `bluetoothd -n` | needs `dbus` | the BNEP firewall rule in `start_pre`; without it the start fails (`bnep-guard` log line) |
| `blesensd` | own | BLE sensors ([Bluetooth](bluetooth.md)) | after `bluetooth` | retries BlueZ on its own |
| `bt-up` | own | the WCN3990 attach | | |
| `buttond` | own | side buttons ([buttons](buttons-and-power-off.md)) | | |
| `chef-sshd` | own | dropbear ([SSH](ssh.md)) | | |
| `chronyd` | packaged (`chrony-openrc`) | `chronyd -f /etc/chrony/chrony.conf -F 1 -u chrony -n` | | needs its config |
| `chef-timekeeper` | own | `chef-state timekeeper` ([persistent data](storage.md#persistent-state)) | after `chronyd` | |
| `gps-manager` | own | GNSS leases ([GPS](gps.md#use-gnss-gps-manager)) | after `modem-up` | waits for the modem owner on its own |
| `powerd` | own | battery policy ([battery](battery-and-charging.md)) | | `-l /data/v1/power -L 4194304` when `/data` is ours (`start_pre`) |
| `root-session-bus` | own | root's session bus at `/run/user/0/bus` | | |
| `sensord` | own | the SMGR client and REG2 server ([sensors](sensors.md)) | after `modem-up` | `sensors-up registry` in `start_pre` (this boot's registry copy, persist read-only; bounded at 30 s, since it runs inside the serial boot); waits up to 10 s for REG2 (`0x10f`) on the IPC router, else the start fails |
| `sensors-up` | own, one-shot | `sensors-up adsp`: starts `audio-up` (detached) to boot the ADSP unless it is up | needs `sensord`, after `modem-up` | nothing stays running under it; sensord polls for the ADSP's sensor manager |
| `networkmanager` | packaged | `NetworkManager --state-file=... -n` ([Wi-Fi](wifi.md)) | needs `dbus`, uses `modem-up` (so a refused `modem-up` stop leaves it running); after every other service | `wifi-nm` in `start_pre`: USB guard, udev coldplug, no manual Wi-Fi session, the modem owner (up to 600 s, or until a shutdown begins), the WLAN driver; any failure fails the start |

That is the start order, as `rc-depend` computes it on the staged tree. `openrc shutdown` stops them in the reverse order of their dependencies: NetworkManager first, `gps-manager`, `sensors-up` and `sensord` before `modem-up`, `syslog` last.

While `wifi-nm` waits for the modem (about 6 s on every boot), NetworkManager is "starting" and its start holds the service's lock; a shutdown in that window would make the stop of `dbus` wait up to 60 s for it. So the wait gives up within a second once `openrc shutdown` has set the runlevel (`/run/openrc/softlevel` reads `shutdown`), and NetworkManager's start fails.

A failed precondition that cannot change later in the boot (a marker `/init` did not write, the BNEP rule, `wifi-nm`'s checks) fails the start once; nothing retries it every 10 s as the old inittab wrappers did. `rc-status` shows such a service stopped or failed, and `rc-service <name> start` tries again. The same for the sensors: when sensord's registry step fails or REG2 does not appear, sensord's start fails, and `sensors-up` (which needs it) does not run, so nothing boots the ADSP for the sensors; `rc-service sensors-up start` tries both again.

**The packaged scripts' dependencies.** Their `depend()` lines name Alpine boot services (`localmount`, `hostname`, `bootmisc`, `clock`, `net`, ...) that would start even outside the runlevel, and `clock` can be `hwclock`, which would set the clock from the RTC over `chef-state`'s restore. Each `conf.d` file removes them (`rc_need="!localmount"` and the like) and adds `rc_use="logger"`; `test_services.py` checks that nothing outside the runlevel is needed, wanted or ordered against.

**Packages.** `scripts/mkrootfs.sh` adds `openrc` (0.63.2; it pulls `ifupdown-ng`, `bridge`, `openrc-user` and `libcap2`, none of them run) and the six `-openrc` subpackages whose scripts run. apk would add every installed package's `-openrc` and ifupdown-ng's executors on its own (install_if); `!` entries keep them out. About 4.7 MB more in the image, about 0.2 MB of it LZMA-compressed. `ifupdown-ng` takes `/sbin/ifup` and `/sbin/ifdown` from busybox; nothing uses either.

**Only our scripts in the image.** `scripts/mkinitramfs.sh` removes every packaged `/etc/init.d` script but the six we configure (and `functions.sh`, the compatibility shim), and the packaged `/etc/conf.d` defaults no remaining script reads. So Alpine's boot and shutdown services (`hwclock`, `localmount`, `bootmisc`, `killprocs`, `mount-ro`, ...) cannot be started by hand on this system, and OpenRC's dependency scan reads 18 scripts instead of 71. `test_services.py` runs that step on every packaged script and checks what is left.

## Readiness

OpenRC 0.63.2's own readiness signalling (`notify=`) is not used: both modes are broken here, verified on a host build. `notify=fd:N` breaks every respawn (the supervisor closes the pipe after the first start but keeps the mode, so each respawned child dies in `dup2()` before `exec`, and the supervisor gives up), and `notify=socket:ready` leaves `/run/openrc/supervise-<service>.sock` behind (it unlinks the wrong name), so the next start fails with "Address already in use", and its wait has no timeout. Research notes: `~/chef-cyclo-evidence/openrc-20261021/research/FINDINGS.md`.

So readiness is a bounded wait in `start_post` where a dependent cannot cope without it (`dbus`, for `bluetooth` and NetworkManager; `sensord`'s REG2, which the ADSP must find before `sensors-up` boots it), and everything else stays with the consumer, which already retries: chef-settings' sources, blesensd, gps-manager, `wifi-nm`'s wait for the modem.

## Supervision

Every service but `modem-up` and the one-shot `sensors-up` runs its program in the foreground under `supervise-daemon` (one supervisor process per service). The program leads its own process group (the supervisor `setsid()`s it).

- **Respawn.** A daemon that exits or is killed (`kill -9` included) is started again after `respawn_delay` (1 s), with no limit. supervise-daemon adds 128 ms for every earlier respawn within `respawn_period` (1800 s, from `rc.conf`), up to 30 s, so a daemon that keeps dying backs off instead of spinning. The supervisor logs each one: `logread | grep supervise-daemon`.
- **Anything that kills a supervised daemon gets it respawned.** To stop one, stop its service: `rc-service <name> stop`. `chef-state shutdown` does that for chronyd, powerd, sensord and the timekeeper.
- **A stop signals the main process only** (TERM, then KILL after 5 s). Where that leaves children behind, the service's `stop_post` TERMs the rest of its process group: `chef-timekeeper` (a save in flight with its chronyc and rtc-edge, which would keep the save lock), `bt-up` (btattach) and `chef-sshd` (dropbear, which would keep port 22). sensord's own children (`chef-state mag-save`) are its business: it kills them when it exits.
- **A respawn reaches only the new process.** When the `chef-sshd` or `bt-up` script itself is killed (`kill -9`), its child lives on in the old process group, which neither the respawn nor a later stop reaches (`stop_post` TERMs the current group only). So each program clears its predecessor's leftover first and logs one line: chef-sshd stops the dropbear named in `/run/chef/ssh/dropbear.pid` (the port), bt-up every running btattach (the UART; only bt-up starts it), TERM, up to 2 s, then KILL. The old dropbear's open sessions are left running; the old log loop ends when the last of them closes. Live 2026-10-22, before this: the new dropbear found port 22 taken and retried every 10 s, the new bt-up hung on the UART.
- **`modem-up` is not supervised.** One modem lifetime per boot, as before: start-stop-daemon backgrounds it once (output to `/run/chef/log/modem-up.log`, pid in `/run/chef/modem-up.pid`), and `rc-status` shows it crashed if it dies. Its `stop` refuses outside `openrc shutdown`: a modem restart within a boot was never tested, and Wi-Fi rides on it. A refused stop leaves everything running: NetworkManager only `use`s modem-up (a `need` dependent would be stopped before modem-up's `stop` refuses; live 2026-10-22 it was). At shutdown it gets 15 s for its cleanup before the KILL: closing `/dev/subsys_modem` takes 10 s, the kernel's wait for a shutdown ack the modem never sends (`wait_for_shutdown_ack(): [modem]: Timed out waiting for shutdown ack`, at every clean stop; closing it before the support daemons changed nothing), so the cleanup takes about 11 s.

## Using it

| What | How |
|---|---|
| What runs, what failed | `rc-status` (`rc-status -a` for every script, `rc-status -c` for crashed ones) |
| Stop, start, restart a service | `rc-service <name> stop\|start\|restart`; a stop also stops the services that `need` it (NetworkManager and bluetooth with dbus, sensors-up with sensord); `rc-service bluetooth stop` takes about 5 s while bt-up runs (bluetoothd waits for the adapter to power off before it exits; at shutdown bt-up stops first and bluetoothd exits at once) |
| The supervisors' lines (starts, respawns, exits) | `logread \| grep supervise-daemon` |
| OpenRC's own boot output | `cat /var/log/rc.log` |
| Run a daemon by hand with other options | `rc-service <name> stop`, then start it from the shell |

`rc-update` does not work: the runlevel is a set of symlinks in `/etc/runlevels/default` on the read-only `system_a` root (on a RAM image it can change them, until the next boot). Change `rootfs-overlay/etc/runlevels/default/` and rebuild instead.

**A runtime stop takes up to 1 s.** A stopped supervisor is an orphan of PID 1, and `supervise-daemon --stop` waits until it is reaped; busybox init's main loop reaps once a second. During the `::shutdown` lines init waits for each line and reaps at once, so the way down is not slowed.

## Shutdown, reboot and the return to fastboot

`poweroff`, `reboot`, buttond's and powerd's power-off all go through busybox init, which runs the `::shutdown` lines and then TERMs and KILLs every process:

1. `timeout -k 1 5 chef-state shutdown`: the last RTC offset save; chronyd, powerd, sensord (it writes the registry copy back on exit) and the timekeeper stopped through OpenRC (all at once; chronyd after the offset query) so chronyd writes its drift file and powerd its pending power log rows; the last magnetometer bias ([persistent data](storage.md#persistent-state)).
2. `timeout -k 1 3 chef-state log-save`: the syslog ring onto `/data/v1/log` ([logging](logging.md#the-shutdown-save)).
3. `timeout -k 1 30 openrc shutdown`: every other service in reverse dependency order, modem-up's modem stop included, while `/data` is still mounted. The shutdown runlevel is empty. Live 2026-10-22 the step took 14.2 s, about 11 s of it modem-up's stop (the modem's 10 s, above). 30 s covers modem-up's whole TERM 15 s and KILL 2 s plus one other service stuck for its TERM 5 s and KILL 2 s and the quick stops of the rest.
4. `timeout -k 1 10 chef-stop-supervisors`: normally finds nothing. When `openrc shutdown` did not finish (its bound expired, or a stop failed), the services it had not reached yet are still running, each with its supervisor, which would respawn whatever the next step TERMs. This step TERMs every `supervise-daemon` still alive (a supervisor then stops its daemon, TERM 5 s and KILL 2 s, and never respawns), waits up to 8 s, KILLs any supervisor left and logs (kmsg) which services it had to stop. What it cannot catch: a start-stop-daemon service (`modem-up`) that `openrc shutdown` did not reach keeps running without its clean stop until init's final TERM and KILL.
5. `chef-storage shutdown`: `/data` read-only and detached; its `fuser` sweep finds only what OpenRC does not run.
6. `umount -a -r`.

`chef-reboot bootloader` ([persistent data](storage.md)) runs the same steps itself, each in the background with a bound, so that a step stuck in D state cannot keep the phone from reaching the bootloader; its `openrc shutdown` step is bounded at 45 s (`timeout -k 1 40`), `chef-stop-supervisors` at 12 s. It pauses init first (SIGTSTP), and a paused busybox init reaps only once a second, so each serial stop there ends on the next tick: live 2026-10-22 the step took 23.5 s, about 11 one-second ticks plus the modem's 11 s, against 14.2 s in a power-off.

`::restart` (SIGQUIT to init) runs only `chef-state shutdown`, `chef-storage shutdown` and `exec /sbin/init`: before it, init has run every `::shutdown` line (OpenRC's stop included) and killed every process. The new init's `::once` line starts the services again. Not live-tested with OpenRC: `umount -a -r` may leave `/run` read-only for the new init.

## Boot time

The first `openrc` run of a boot scans every script in `/etc/init.d` into its dependency tree (about 0.35 s for 82 scripts on the phone; the image keeps only its 18), then each service costs about 25 ms to start serially. The order above puts chef-settings, modem-up and udhcpd right after syslog. The baseline before OpenRC (installed pair 9a28e590 + 981d4f03): every inittab child started 6.70 to 6.73 s after boot, chef-settings at 6.73 s, "modem up" about 6 s after init, NetworkManager about 10 s after init. The live check measures the same points.

## Tests

`scripts/tests/test_services.py`: every script and `conf.d` file parses (`sh -n`, also under busybox ash), the runlevel links resolve, the image build keeps only the runlevel's scripts and their `conf.d` files, `inittab` has only the lines above, no service needs, wants or is ordered against anything outside the runlevel, and the hooks run in a harness that sources the files in `openrc-run`'s order with stub supervisors, so the exact command line of every start is checked (respawn and retry settings, pid files, arguments), as are the moved inittab logic (powerd's `/data` choice, the BNEP rule, the dbus wait, `wifi-nm`'s result), sensord's bounded registry step and REG2 wait, the one-shot `sensors-up`, modem-up's stop refusal and the process-group stops. `test_chef_state.py` checks that `chef-state shutdown` stops through `rc-service`, never signals a daemon itself and waits for the daemon its supervisor recorded (not a client of the same binary), `test_chef_reboot.py` the new steps and their bounds, `test_wifi_nm.py` `wifi-nm`'s output and its wait ending at a shutdown. `test_services.py` also runs `chef-stop-supervisors` against stand-in supervisors (one that ignores TERM). `test_ssh.py` and `test_bt_up.py` kill a running chef-sshd with `kill -9` (or start a stray btattach) and check that the next start stops the leftover, TERM or KILL, and leaves anything else alone.
