# syslog (standard interfaces step 6)

User request 2026-10-20 ("step 6, syslog"). The scope is step 6 of the
[standard interfaces](../next-steps/standard-interfaces.md#order) order: the syslog item
of its section 5. Read that item, [runtime files](../features/runtime-files.md),
[persistent data](../features/storage.md) (crash records, `boots.log`, the
shutdown order) and `tools/common/chef_log.h` first; this file adds the
decisions and the working rules.

Roles: `syslog_impl` (Claude Opus) implements, `syslog_review` (Claude
Opus) reviews, `syslog_coord` coordinates and decides, and runs the live
check. Report with `herdr agent prompt syslog_coord "[From: syslog_impl] ..."`
(or `syslog_review`). Send a message when a milestone is ready for review,
when a review is done, when blocked, and at the end. Do not poll or wait on
each other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/syslog-20261020/` (create it).
Review diffs go to `review/<milestone>.diff` there; the reviewer snapshots
what it reviewed (sha256 in its note) and checks the commit against that
snapshot.

## Working rules

- Work on `main` in this checkout. Milestones in the order below, each
  reviewed before the next. One commit per milestone after the reviewer's
  explicit approval. Do not push. Do not touch the `experiment/*` or `ui`
  branches. Keep this file and any edit of `standard-interfaces.md` out of
  your commits (the coordinator commits them at the end).
- No phone access. No `fastboot`, `phone.py`, `phone-boot.sh`, telnet or ssh
  to the phone: the coordinator runs the live check.
- Match the surrounding code's style and comment density.
- `make test` at the repo root must pass at every milestone. Stress or flake
  checks: short, targeted at the step, runtime stated first.
- **No host test may write to the host's syslog.** This host has a real
  `/dev/log` (journald). Every test that runs a binary or script on the
  logging path points it at a temporary socket or a missing path (the
  overrides below); the top-level `make test` also exports both overrides
  to a missing path as a backstop. Check with `journalctl --since` over a
  full `make test` that nothing of ours arrived, and say so in the M1 and
  M2 notes.
- Historical text keeps its words: `docs/archive/`, `docs/research/`,
  `logs/`, `docs/build-log.md` and finished handoffs. Everything that
  describes the current system (code, comments, `docs/features/`,
  `docs/live-testing.md`, `docs/repository-layout.md`, open plans) says
  where lines go now (`logread` instead of `dmesg` for our programs).
- Ask `syslog_coord` before deciding anything this file and the plan do not
  settle.

## Today

There is no syslog daemon and no `/dev/log`. Our programs write to
`/dev/kmsg` and read back with `dmesg`:

- C daemons on `tools/common/`: `buttond`, `gps-manager`, `sensord`,
  `blesensd` through `chef_log()` (open, write, close of `/dev/kmsg` per
  line, optional stderr echo, cut at 320 bytes; `chef_log_set_kmsg()` for
  tests). `linesrv` logs through the same call.
- C with their own copy: `powerd` (`kmsg_note`, `-K` path for tests),
  `abslot` (stdout, kmsg, `/run/chef/log/abslot.log`, `-K`), chefui
  (`chefui_log()` in `tools/chefui/chefui.c`, `chefui[<app>]:` prefix, test
  path `cu_paths.kmsg`; host SDL builds log to stderr only), and
  `chef-settings`' pre-init note (`tools/chefui/apps/settings/main.c`
  around 261). Debug tools `fbtouch` and `fblog` write kmsg markers on
  purpose, to line up with the kernel's display lines.
- Shell: a `log()` in each of `gps-up`, `sensors-up`, `audio-up`, `bt-up`,
  `chef-state`, `chef-storage`, `chef-reboot`, `chef-sshd` (it also
  forwards dropbear's `-E` stderr, rate limited), the NM dispatcher
  `30-chef-wifi-ip`, `/init`, `stage1/init`, the inittab BNEP guard line,
  and the hand-run `sensors-compass-run` and `sensors-magcal-run` (which
  also save `dmesg > $O/dmesg.txt` as evidence). Each has its own
  `*_KMSG` test override.
- Third-party programs that log to syslog today lose every line:
  NetworkManager (`--no-daemon`, syslog backend), `bluetoothd -n`,
  `dbus-daemon`, `wpa_supplicant` (D-Bus activated by NM with `-u` only, so
  its stdout goes nowhere). `chronyd -d` and `udhcpd -f` log to stderr,
  which inittab leaves on the console (nowhere useful).

The Alpine busybox (1.37.0-r31) has `syslogd`, `logread`, `logger` and
`klogd`, and the image already links them (`sbin/syslogd`, `sbin/logread`,
`usr/bin/logger`). The kernel has `CONFIG_SYSVIPC` (added 2026-10-04 for
chrony's SHM refclock; `scripts/tests/test_sysvipc_gate.py`), which
`syslogd -C` and `logread` need.

## Decisions

### The daemon (M1)

- busybox `syslogd -n -C1024` (a 1 MiB shared-memory ring, read with
  `logread` and `logread -f`), as the **first** `::respawn:` line of
  `inittab` with a short comment. RAM only: no `/var/log/messages`, no
  `-O`, no remote. Verify from the busybox 1.37.0 `sysklogd/syslogd.c`
  source (fetch it; if downloads crawl, say so and the user turns on WARP)
  that `-C` logs to the ring only, that `-n` still writes
  `/var/run/syslogd.pid` (here `/run/syslogd.pid`, `/var/run` links to
  `../run`) and removes it on exit, and how it timestamps a message that
  carries none. Report what you found. If `-n` writes no pid file, stop
  and ask.
- **No `klogd`.** The kernel log stays in `dmesg` (and in pstore after a
  panic). The vendor kernel's noise would evict our lines from the ring,
  and the kernel lines already have their own buffer and tools.
- `/run/syslogd.pid` is busybox's own default name: add it to
  `scripts/tests/test_run_layout.py`'s allow list with its reason, and to
  the "What stays outside" list in `runtime-files.md`, as `udhcpd.pid` is.

### The C log call (M1)

`chef_log()` keeps its signature, its 320-byte cut and its stderr echo, and
changes only where the line goes:

- It sends one datagram per line to the syslog socket (default `/dev/log`;
  `CHEF_LOG_SOCKET` in the environment, read in `chef_log_init()`,
  overrides it; plus `chef_log_set_socket()` for tests) in the standard
  form `<30>TAG[PID]: MESSAGE` (facility daemon, level info; no timestamp,
  so syslogd stamps it with its own clock). Not `syslog(3)`, for two
  reasons that must be in the header comment: a blocking `send()` to a
  wedged or full syslogd would stall the caller (`buttond` is the power-off
  path), so it sends with `MSG_DONTWAIT | MSG_NOSIGNAL`; and a line that
  cannot be delivered must not vanish.
- The socket is opened and connected lazily (`SOCK_DGRAM | SOCK_CLOEXEC`)
  and kept. On a lost connection (`ECONNREFUSED`, `ENOTCONN`, `ENOENT`,
  `ECONNRESET`, `EPIPE`: syslogd restarted or not up yet) it reconnects
  once and resends. If that fails, or on `EAGAIN`, or on any other error,
  the line goes to `/dev/kmsg` exactly as today (`TAG: MESSAGE`). So boot
  lines written before syslogd binds, and lines during a syslogd respawn,
  end up in `dmesg` instead of being lost.
- `buttond`, `gps-manager`, `sensord`, `blesensd` and `linesrv` need no
  change beyond what the library gives them.
- `powerd`, `abslot` and chefui move onto `chef_log.c` (compiled from
  source as the others do; add it to their Makefile rules): `powerd`'s
  `kmsg_note` and `abslot`'s kmsg write become `chef_log` with tags
  `powerd` and `abslot` (their `-K` options map onto `chef_log_set_kmsg`,
  so existing tests keep working; abslot keeps stdout and its
  `abslot.log`). `chefui_log()` logs through `chef_log` with the tag set to
  the app name (`chef-settings` today; `cfg.app_name` is documented as the
  syslog tag; keep the host SDL stderr-only behavior and the test path);
  check that every in-image app name is the program's name and list them.
  `chef-settings`' pre-init note uses the same call with tag
  `chef-settings`.
- `fbtouch` and `fblog` keep writing kmsg (say why in one comment each, if
  none is there).
- Tests in `tools/common/tests/test_chef_log.c`: a line arrives on a temp
  datagram socket in the exact wire form; no socket means kmsg; a socket
  whose receive queue is full means kmsg and no block (bounded wall time);
  a socket that disappears and comes back is reconnected; the 320-byte cut
  and the stderr echo hold. Daemon tests that read the kmsg file keep
  passing by pointing the socket at a missing path.

### Third-party programs (M1)

- `chronyd -n` instead of `-d` (syslog instead of stderr; check the flag
  in chrony's man page for the packaged version), `udhcpd -f -S`.
- NetworkManager: `[logging] backend=syslog` in
  `rootfs-overlay/etc/NetworkManager/conf.d/90-chef.conf` (explicit; level
  stays the default INFO).
- `wpa_supplicant`: ship `rootfs-overlay/usr/share/dbus-1/system-services/fi.w1.wpa_supplicant1.service`
  as the package's file with `-s` added (`Exec=/sbin/wpa_supplicant -u -s`),
  and a comment where the overlay keeps its overrides if there is such a
  place. Check that the overlay copy replaces the package's in both images.
- `bluetoothd`, `dbus-daemon`, `telnetd`: nothing to change; list what
  each logs in the guide.
- `gpsd` and the rest of `gps-manager`'s pipeline keep their output files
  in `/run/chef/gps-manager/`.

### Shell (M2)

- One helper, `rootfs-overlay/usr/lib/chef/log.sh` (beside `usb-gadget.sh`
  and `modem-owner.sh`; step 7 grows this directory into the shell helper
  library), sourced by the scripts. `chef_log TAG MESSAGE...`: when
  syslogd is up it runs `logger -t TAG -p daemon.info -- MESSAGE`,
  otherwise it writes `TAG: MESSAGE` to kmsg. "Up" means the pid file
  (`CHEF_LOG_SYSLOGD_PID`, default `/run/syslogd.pid`) names a live
  process whose `/proc/PID/comm` is `syslogd` (a stale pid file after a
  crash must not swallow lines). `CHEF_LOG_KMSG_ONLY=1` forces kmsg.
  Test overrides for the kmsg path keep each script's existing variable
  (`CHEF_STATE_KMSG` and the like); keep the per-script `log()` functions
  as thin wrappers where they also write stderr or a file, so their other
  outputs do not change.
- **To syslog:** `gps-up`, `sensors-up`, `audio-up`, `bt-up`, `chef-sshd`
  (dropbear's forwarded lines keep the rate limit, which now protects the
  ring; tag `dropbear` with the pid in the message or the tag, your call,
  documented), `chef-state` and `chef-storage` in their running modes, the
  NM dispatcher, the inittab BNEP guard line (through `logger` or the
  helper). `chef-state boot` and `chef-storage`'s boot work run from
  `/init` before init, so the helper sends them to kmsg on its own; check
  that and say so.
- **Stay on kmsg (`CHEF_LOG_KMSG_ONLY` or a direct write):** `/init`,
  `stage1/init`, `chef-state shutdown`, `chef-storage shutdown`, the
  shutdown log save below, and all of `chef-reboot`. A hang or panic on the
  way down is what pstore keeps, and only kmsg lines reach it.
- `sensors-compass-run` and `sensors-magcal-run`: their step lines move to
  the helper, and the evidence they save gains `logread > $O/syslog.txt`
  beside `dmesg.txt` (sensord's lines are no longer in `dmesg`). Update
  their tests.
- `sensors-up`'s dmesg excerpt on failure stays (kernel lines).
- The `/run/chef/log/*.log` files stay as they are: they hold command
  output, not only log lines.

### Persisting on /data (M3)

The ring is RAM; decided 2026-10-20: it is saved once per orderly
shutdown, so the last runs (a ride ended by a power-off, a low-battery
shutdown) can be read afterwards. Nothing is written while the system
runs.

- `chef-state log-save`: if `/data` is ours and syslogd is up, `logread`
  into `/data/v1/log/syslog-<seq>` where `<seq>` is this boot's sequence
  number from `boots.log` (the line with this boot's `boot_id`; without
  one, `syslog-noseq-<boot_id>`), written as a temp file, fsynced, renamed
  and the directory synced, the way `chef-state`'s other saves are. The
  directory `log` is 0700, files 0600 (Wi-Fi names and Bluetooth addresses
  are in there). Keep the newest 8 files, and remove the oldest while the
  directory holds more than 8 MiB. Its own lines go to kmsg. Skips with
  one kmsg line when `/data` is not ours or syslogd is down.
- Called as its own `::shutdown:` line, `timeout -k 1 3`, after
  `chef-state shutdown` and before `chef-storage shutdown`, the same in the
  `::restart` line, and in `chef-reboot` as a bounded step between its
  steps 2 and 3 (update its header list and `WATCHDOG_SECS` arithmetic and
  the test that checks it). syslogd is still running at that point (busybox
  init runs the `::shutdown` lines before it signals every process); check
  that in init's source and say so.
- A row in the storage guide's table, with how to read the files.

### Docs (M1 to M3)

- New `docs/features/logging.md` (linked from the feature index): where
  each program's lines go (ring, kmsg, files), how to read them (`logread`,
  `logread -f`, `logread | grep TAG`, `dmesg` for the kernel and early
  boot), the fallback rule, the ring size, the shutdown save, and what
  third-party programs log. Short, like `runtime-files.md`.
- Every current guide that says `dmesg | grep <our program>` says
  `logread | grep ...` (`battery-and-charging.md`, `sensors.md`,
  `storage.md`, `ssh.md`, `system-ui.md`, `bluetooth.md`, ...), and
  `docs/live-testing.md` says where to look. Update `chef_log.h`'s and
  `tools/common/Makefile`'s "kmsg today, syslog from step 6" remarks and
  `docs/repository-layout.md` (`log.sh`).

## Milestones

1. **M1 daemon and C**: inittab `syslogd`, `chef_log` on the socket with
   the kmsg fallback, `powerd`, `abslot`, chefui onto it, the third-party
   changes, the run-layout allow entry, `logging.md` first version.
2. **M2 shell**: `log.sh`, every script in the lists above, the sensor
   evidence tools, guide and live-testing updates.
3. **M3 shutdown save**: `chef-state log-save`, the inittab lines,
   `chef-reboot`, storage guide, `logging.md` final.

## Acceptance (host side)

`make test` passes; nothing of ours reached the host journal; the staged
image holds the same files as before apart from `log.sh`, the
`wpa_supplicant` service override and changed files (compare the file
lists); a grep of the staged scripts and `strings` over the staged binaries
finds no `/dev/kmsg` writer outside the kmsg list above and `chef_log.c`.

## End (coordinator)

RAM image live check: `logread` after boot holds our daemons' ready lines
and NetworkManager, bluetoothd, wpa_supplicant, dbus and chronyd lines;
`dmesg` holds only kernel lines, `/init`'s, and the boot-time `chef-state`
and `chef-storage` lines; `kill -9` of syslogd: lines during the gap land
in `dmesg`, the respawned syslogd gets the next ones; ring use per hour
idle and with Wi-Fi and a GPS lease (is 1 MiB enough for a long ride);
a power-off, then `/data/v1/log/syslog-<seq>` on the next boot ends with
the shutdown lines; `chef-reboot bootloader` saves too; idle current
unchanged. Then archive this file, write `logs/syslog-2026-10-20.txt`, mark
step 6 done in the plan, install, push.

## Result (2026-10-21)

Done. Commits on main, each approved by `syslog_review` and checked
against its reviewed snapshot: `3a595ca` (M1 `syslogd -n -C1024` first in
inittab, `chef_log` on `/dev/log` with the kmsg fallback, powerd, abslot
and chefui onto it, chronyd `-n`, udhcpd `-S`, NetworkManager's syslog
backend, wpa_supplicant `-s`, [logging](../features/logging.md)),
`ea8bef1` (M2 `usr/lib/chef/log.sh` and every script on it,
`CHEF_LOG_KMSG_ONLY` for the shutdown paths, the sensor tools' cues on
both), `6903738` (M3 `chef-state log-save` to `/data/v1/log/syslog-<seq>`
from the `::shutdown` lines and `chef-reboot`), `b31d547` (M4, from the
live check: `syslogd -t`, since musl's `syslog(3)` stamps in UTC, and
`/init` sets the host name from `/etc/hostname`).

Decided during the work: `/init` raises `net.unix.max_dgram_qlen` to 512
(the kernel's 10 sent boot bursts to the kmsg fallback); no log save in
`::restart` (busybox init kills everything before it runs that line);
`chef_log` honours `CHEF_LOG_KMSG_ONLY` too; the overlay already carried
the wpa_supplicant service file, so `-s` went there; chefui-demo's tag is
`chefui-demo`; tags renamed `cyclo-bt` to `bt-up`, `chef Wi-Fi` to
`chef-wifi-ip`, the BNEP guard to `bnep-guard`. Caveats in the logging
guide: a respawned syslogd starts an empty ring, and a `kill -9` inside a
ring write can wedge the respawned one (busybox takes the semaphore
without `SEM_UNDO`). RAM images `ef52201f` (M1 to M3) and `b387396a`
(with M4) live verified on 2026-10-21. Details in
[the log](../../logs/syslog-2026-10-20.txt).
