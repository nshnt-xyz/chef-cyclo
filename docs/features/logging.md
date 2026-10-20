# Logging: syslogd and the kernel log

[Feature index](README.md) · [Runtime files](runtime-files.md) · [Persistent data](storage.md)

Since standard interfaces step 6 (2026-10-20) the image runs busybox `syslogd`, and our daemons log to it instead of `/dev/kmsg`. The kernel's own lines stay where they were, in `dmesg`.

## The daemon

The first `::respawn:` line of `inittab` is `syslogd -n -C1024`: it binds `/dev/log` and keeps every message in a 1 MiB shared-memory ring (`CONFIG_SYSVIPC`, the same kernel option chrony's SHM refclock needs). Nothing goes to disk, nothing to the network, and there is no `klogd`: the vendor kernel's noise would evict our lines from the ring, and the kernel lines already have their own buffer, tools and pstore. Its pid file is busybox's default `/run/syslogd.pid`, written with `-n` too and removed when it exits on TERM or INT (not after a KILL).

syslogd stamps each line with its own clock, in local time (IST) to the second, unless the sender put a timestamp in the message (busybox `logger` and the C library's `syslog(3)` do). Our C daemons send none. A line in the ring reads

```text
Oct 20 23:41:02 <hostname> daemon.info buttond[612]: ready: /run/chef/buttond.sock, long 1500 ms
```

A restarted syslogd starts with an empty ring (it zeroes the segment), so lines from before a respawn are gone.

`/init` raises the socket queue (`/proc/sys/net/unix/max_dgram_qlen`) from the kernel default of 10 datagrams to 512, as systemd does at boot, so the burst of every daemon starting at once does not fill it. A failed write is one kmsg line and the boot goes on.

busybox's ring writer takes its semaphore without `SEM_UNDO`, so a `kill -9` that lands inside a write leaves it held: the respawned syslogd then blocks on its first line and stops reading `/dev/log`. `chef_log()` falls back to kmsg as soon as the queue is full; `logger` and the `syslog(3)` callers block. The fix is a reboot.

## Reading

| What | How |
|---|---|
| Everything in the ring | `logread` |
| Follow new lines | `logread -f` |
| One program | `logread \| grep buttond` (the tag is the program's name) |
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
| shell scripts (`/init`, `gps-up`, `sensors-up`, `chef-state`, `chef-storage`, `chef-reboot`, `chef-sshd`, ...) | `/dev/kmsg` for now; the shell log helper moves the running ones to the ring |
| the kernel | `dmesg`, and pstore after a panic |

Shell scripts also keep their own files under `/run/chef/log/` ([runtime files](runtime-files.md)): those hold command output, not only log lines.

## The fallback

`chef_log()` sends one datagram per line, `<30>TAG[PID]: MESSAGE` (facility daemon, level info), without blocking: a wedged or full syslogd must not stall `buttond`, which is the power-off path. It keeps the socket open, reconnects once when syslogd went away or restarted, and writes the line to `/dev/kmsg` as `TAG: MESSAGE` when that fails, when the queue is full (`EAGAIN`, even with the raised queue) or on any other error. So lines from before syslogd binds at boot, and from a gap while it respawns, are in `dmesg` and not lost. A message is cut at 320 bytes.

Only our `chef_log()` sender has that never-block guarantee. Third-party programs, busybox init and `logger` use the C library's `syslog(3)`, which blocks on a full queue and drops a line it cannot deliver. A crashed syslogd is respawned at once; a stopped (or wedged, above) one is not a case the image handles.

## Tests

No host test writes to the host's syslog. `CHEF_LOG_SOCKET` in the environment replaces `/dev/log` for `chef_log()`; the top-level `Makefile` and every suite's `Makefile` export it as a missing path, so the lines go to the kmsg path each test sets (`powerd -K`, `abslot -K`, `chef_log_set_kmsg()`), and the tests that check the wire form bind a temporary socket.
