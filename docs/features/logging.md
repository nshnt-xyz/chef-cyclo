# Logging: syslogd and the kernel log

[Feature index](README.md) · [Runtime files](runtime-files.md) · [Persistent data](storage.md)

Since standard interfaces step 6 (2026-10-20) the image runs busybox `syslogd`, and our daemons and scripts log to it instead of `/dev/kmsg`. The kernel's own lines stay where they were, in `dmesg`, and so does everything that runs before syslogd or on the way down.

## The daemon

The first `::respawn:` line of `inittab` is `syslogd -n -C1024`: it binds `/dev/log` and keeps every message in a 1 MiB shared-memory ring (`CONFIG_SYSVIPC`, the same kernel option chrony's SHM refclock needs). Nothing goes to disk, nothing to the network, and there is no `klogd`: the vendor kernel's noise would evict our lines from the ring, and the kernel lines already have their own buffer, tools and pstore. Its pid file is busybox's default `/run/syslogd.pid`, written with `-n` too and removed when it exits on TERM or INT (not after a KILL).

syslogd stamps each line with its own clock, in local time (IST) to the second, unless the sender put a timestamp in the message (busybox `logger` and the C library's `syslog(3)` do, in the same local time). Our C daemons send none. Lines in the ring read

```text
Oct 20 23:41:02 <hostname> daemon.info buttond[612]: ready: /run/chef/buttond.sock, long 1500 ms
Oct 20 23:41:05 <hostname> daemon.info gps-up: modem ready
```

C daemons carry their pid; shell scripts, through busybox `logger`, do not (it has no option for it), except `dropbear[PID]`, whose pid `chef-sshd` puts in the tag.

A restarted syslogd starts with an empty ring (it zeroes the segment), so lines from before a respawn are gone.

`/init` raises the socket queue (`/proc/sys/net/unix/max_dgram_qlen`) from the kernel default of 10 datagrams to 512, as systemd does at boot, so the burst of every daemon starting at once does not fill it. A failed write is one kmsg line and the boot goes on.

busybox's ring writer takes its semaphore without `SEM_UNDO`, so a `kill -9` that lands inside a write leaves it held: the respawned syslogd then blocks on its first line and stops reading `/dev/log`. `chef_log()` falls back to kmsg as soon as the queue is full; `logger` and the `syslog(3)` callers block. The fix is a reboot.

## Reading

| What | How |
|---|---|
| Everything in the ring | `logread` |
| Follow new lines | `logread -f` |
| One program | `logread \| grep buttond` (the tag is the program's name, but for the few below) |
| The kernel, early boot, fallback lines | `dmesg` |

## Where each program's lines go

| Program | Where |
|---|---|
| `buttond`, `gps-manager`, `sensord`, `blesensd`, `powerd`, `abslot`, `chef-settings`, `chefui-demo` | the ring, through `chef_log()` (`tools/common/chef_log.c`); `abslot` also to stdout and `/run/chef/log/abslot.log` |
| busybox `init` | the ring (`starting pid ...`, `process '...' exited`, `The system is going down NOW!`), when syslogd is up |
| NetworkManager | the ring (`[logging] backend=syslog` in `etc/NetworkManager/conf.d/90-chef.conf`, default level) |
| `wpa_supplicant` | the ring (`-s` in the D-Bus activation file the overlay ships, `usr/share/dbus-1/system-services/fi.w1.wpa_supplicant1.service`) |
| `bluetoothd -n` | the ring (BlueZ always logs to syslog) |
| `dbus-daemon --system` | the ring (`<syslog/>` in the package's `system.conf`: activations and errors); the session bus writes its rare messages to the console |
| `chronyd -n` | the ring (`-d` would have sent it to the console) |
| `udhcpd -f -S` | the ring, and the console as before |
| `telnetd -F` | its rare errors to the console; nothing in the ring |
| `gpsd`, the qmicli follower, `nmea-broker` | their output files in `/run/chef/gps-manager/`, as before |
| `fbtouch`, `fblog` | `/dev/kmsg` on purpose: their markers line up with the kernel's display lines |
| `gps-up`, `sensors-up`, `audio-up`, `bt-up`, `chef-sshd` (and dropbear's lines, tag `dropbear[PID]`, at most 20 per 60 s), `chef-state` and `chef-storage` while the system runs, the NetworkManager dispatcher (`chef-wifi-ip`), inittab's BNEP guard (`bnep-guard`) | the ring, through the log helper `usr/lib/chef/log.sh` (below); the `*-up` scripts also to stdout (their `/run/chef/log/` files) |
| `sensors-compass-run`, `sensors-magcal-run` (tags `compass`, `magcal`) | the ring through the helper; the lines the person follows (instructions, countdown, steps, results, DONE) also straight to kmsg, where `fblog` shows them on the panel. Their evidence directories hold `syslog.txt` (`logread`) beside `dmesg.txt` |
| `/init` (`cyclo-init`), stage 1 (`cyclo-stage1`), `chef-state boot` and `chef-storage boot` | kmsg: they run before syslogd starts (the helper finds no syslogd and falls back on its own) |
| `chef-state shutdown`, `chef-storage shutdown`, `chef-reboot`, and everything they run (`powerd flush`) | kmsg: they set `CHEF_LOG_KMSG_ONLY=1`, because a hang or panic on the way down is what pstore keeps, and only kmsg lines reach it. Daemons TERMed meanwhile (powerd, chronyd, sensors-up) still log to the ring, which is up until init's final kill |
| the kernel | `dmesg`, and pstore after a panic |

Shell scripts also keep their own files under `/run/chef/log/` ([runtime files](runtime-files.md)): those hold command output, not only log lines.

Tags renamed on 2026-10-20, to map old `dmesg` greps:

| Before (kmsg) | Now (tag) |
|---|---|
| `cyclo-bt:` | `bt-up` |
| `chef Wi-Fi: <profile> IPv4 ...` | `chef-wifi-ip: <profile> IPv4 ...` |
| `cyclo: BNEP ingress guard failed ...` | `bnep-guard: BNEP ingress guard failed ...` |
| `chefui[chef-settings]:` | `chef-settings` |
| `chefui[demo]:`, `chefui[demo-peer]:` | `chefui-demo` |

## The fallback

### C: chef_log()

`chef_log()` sends one datagram per line, `<30>TAG[PID]: MESSAGE` (facility daemon, level info), without blocking: a wedged or full syslogd must not stall `buttond`, which is the power-off path. It keeps the socket open, reconnects once when syslogd went away or restarted, and writes the line to `/dev/kmsg` as `TAG: MESSAGE` when that fails, when the queue is full (`EAGAIN`, even with the raised queue) or on any other error. So lines from before syslogd binds at boot, and from a gap while it respawns, are in `dmesg` and not lost. A message is cut at 320 bytes.

`CHEF_LOG_KMSG_ONLY=1` in its environment (read by `chef_log_init()`) sends every line to kmsg, the same switch as the shell helper's.

### Shell: log.sh

Scripts source `${CHEF_LIB_DIR:-/usr/lib/chef}/log.sh` and call `chef_log TAG MESSAGE`. While syslogd is up it runs `logger -t TAG -p daemon.info -- MESSAGE`; otherwise, or when `logger` fails, it writes `TAG: MESSAGE` to kmsg. "Up" means `/run/syslogd.pid` (`CHEF_LOG_SYSLOGD_PID`) names a live process whose `comm` is `syslogd`, so a stale pid file after a crash cannot swallow lines. `CHEF_LOG_KMSG_ONLY=1` forces kmsg. Each script points the kmsg path (`CHEF_LOG_KMSG`) at its own test override (`CHEF_STATE_KMSG` and the like); `chef_log_syslog` sends to syslogd only and writes nothing while it is down (the sensor tools' panel cues, which write kmsg themselves). `chef-reboot` keeps its own direct kmsg write: the way back to the bootloader does not depend on the helper.

### Blocking

Only our `chef_log()` sender has that never-block guarantee. Third-party programs, busybox init and `logger` (so our scripts too) use the C library's `syslog(3)`, which blocks on a full queue and drops a line it cannot deliver. A crashed syslogd is respawned at once; a stopped (or wedged, above) one is not a case the image handles.

## Tests

No host test writes to the host's syslog. `CHEF_LOG_SOCKET` in the environment replaces `/dev/log` for `chef_log()`; `CHEF_LOG_SYSLOGD_PID` and `CHEF_LOG_LOGGER` point the shell helper at a pid file and a `logger` of the test's choosing, and `CHEF_LIB_DIR` at the overlay's copy of the helper. The top-level `Makefile` and every suite's `Makefile` export the first three as missing paths, and each test that runs a daemon or a script sets them itself (the in-process C tests call `chef_log_set_socket()`), so the lines go to the kmsg path each test sets (`powerd -K`, `abslot -K`, `CHEF_STATE_KMSG`, ...). The tests that check what reaches syslog bind a temporary socket or run a script named `syslogd` with a recording `logger` stub.
