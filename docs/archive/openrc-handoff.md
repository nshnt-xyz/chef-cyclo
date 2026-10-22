# OpenRC (standard interfaces step 7)

User request 2026-10-21 ("lets go ahead with the research, like usual hand
over implementation and review to fresh herdr agents"). The scope is step 7
of the [standard interfaces](../next-steps/standard-interfaces.md#order) order: its
section 3, "OpenRC for start order, readiness and supervision". Read that
section, `rootfs-overlay/etc/inittab`, `rootfs-overlay/init`,
[logging](../features/logging.md), [runtime files](../features/runtime-files.md),
[persistent data](../features/storage.md) (the shutdown order) and
`rootfs-overlay/usr/bin/chef-reboot` first; this file adds the research
results, the decisions and the working rules. The research notes and
scripts are in `~/chef-cyclo-evidence/openrc-20261021/research/`
(`FINDINGS.md`).

Roles: `openrc_impl` (Claude Opus) implements, `openrc_review` (Claude
Opus) reviews, `openrc_coord` coordinates and decides, and runs the live
check. Report with `herdr agent prompt openrc_coord "[From: openrc_impl] ..."`
(or `openrc_review`). Send a message when a milestone is ready for review,
when a review is done, when blocked, and at the end. Do not poll or wait on
each other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/openrc-20261021/`. Review diffs go to
`review/<milestone>.diff` there; the reviewer snapshots what it reviewed
(sha256 in its note) and checks the commit against that snapshot.

## Working rules

- Work on `main` in this checkout. Milestones in the order below, each
  reviewed before the next. One commit per milestone after the reviewer's
  explicit approval (M1 has two: the rename, then the OpenRC work). Do not push. Do not touch the `experiment/*` or `ui`
  branches. Keep this file and any edit of `standard-interfaces.md` out of
  your commits (the coordinator commits them at the end).
- No phone access. No `fastboot`, `phone.py`, `phone-boot.sh`, telnet or ssh
  to the phone: the coordinator runs the live check. Anything that can only
  be learned on the phone goes in your milestone note as a question for
  the live check.
- Match the surrounding code's style and comment density. Service scripts
  are short: a header comment saying what the service is and anything
  unusual, then variables, `depend()`, and hooks only where needed.
- `make test` at the repo root must pass at every milestone. Stress or flake
  checks: short, targeted at the step, runtime stated first. No host test
  may write to the host's syslog (the step 6 overrides stay in force).
- Historical text keeps its words: `docs/archive/`, `docs/research/`,
  `logs/`, `docs/build-log.md` and finished handoffs. Everything that
  describes the current system says how it works now.
- Ask `openrc_coord` before deciding anything this file and the plan do not
  settle.

## What the research found

Alpine v3.24 has OpenRC **0.63.2** (`openrc` 4.4 MB installed; it pulls
`ifupdown-ng`, `bridge`, `openrc-user` and `libcap2`). Busybox init stays
PID 1 and runs OpenRC from `inittab`, as Alpine does.

1. **No `notify=` readiness.** supervise-daemon's two readiness modes are
   both unusable here, verified on a host build of 0.63.2:
   - `notify=fd:N` breaks respawn: after the first start the supervisor
     closes the pipe but keeps the mode, so every respawned child dies in
     `dup2()` before `exec` and the supervisor gives up after
     `respawn_max`.
   - `notify=socket:ready` leaves `/run/openrc/supervise-<svc>.sock`
     behind (it unlinks the wrong name), so the next start of the service
     fails with "Address already in use"; its wait has no timeout either.
   So readiness is a bounded wait in `start_post` (OpenRC's `ewaitfile`,
   or a small loop for things that are not files), and only where a
   dependent cannot cope without it. Everything else stays with the
   consumer, which already retries (chefui's sources, blesensd, gps-manager).
2. **Cost on the phone** (a chroot under `/run` on the installed pair, 18
   dummy supervised services): about 25 ms per service started serially,
   0.35 s for the first dependency-tree build of 82 scripts, 1.0 s for an
   `openrc default` from an empty `/run/openrc` including the tree. A
   `kill -9` of a supervised child is respawned at once. An idle
   supervisor never wakes.
3. **Stops take up to 1 s at runtime, not at shutdown.** A stopped
   supervisor is an orphan of PID 1; `supervise-daemon --stop` waits until
   the zombie is reaped, and busybox init's main loop reaps only once a
   second. While busybox init runs a `::sysinit`, `::wait` or `::shutdown`
   line it reaps any child immediately, so `openrc shutdown` as a
   `::shutdown:` line is fast. A runtime `rc-service X stop|restart` costs
   up to 1 s. Accepted; say so in the guide.
4. **Busybox init order:** `::sysinit` (waited), `::wait` (waited), `::once`
   (started, not waited), then the `::respawn` loop. A `::wait:` OpenRC
   would hold telnetd back until OpenRC finishes, or forever if it hangs.
5. **Alpine's packaged scripts** (`dbus`, `bluetooth`, `chronyd`, `syslog`,
   `udhcpd`, `networkmanager`) can be switched to supervise-daemon with
   `supervisor=supervise-daemon` in `/etc/conf.d/<svc>`, but their
   `depend()` lines pull in Alpine boot services (`localmount`,
   `hostname`, `bootmisc`, `clock`, `net`). A `need` starts the needed
   service even when it is in no runlevel, and the `clock` provider can be
   `hwclock`, which would set the clock from the RTC over chef-state's
   restore. `rc_need="!name"` (and `rc_after`, `rc_use`, ... the same way)
   in the conf.d file removes a dependency.
6. **Supervision defaults:** supervise-daemon gives up after 10 respawns
   over the service's whole lifetime unless `respawn_max=0`; `respawn_delay`
   takes units (`500ms`, `1s`) and does not slow a stop.
7. **Baseline** (installed pair, boot of 2026-10-21): every inittab child
   started 6.70 to 6.73 s after boot (`/proc/PID/stat` field 22),
   chef-settings at 6.73 s; sensord 6.94 s; "modem up" about 6 s after
   init; NetworkManager about 10 s after init. `FINDINGS.md` has the
   details.

## Decisions

### PID 1, inittab and OpenRC's configuration (M1)

- Busybox init stays PID 1. `inittab` keeps only what must not depend on
  OpenRC, and runs OpenRC:
  - the passwordless USB **telnetd** `::respawn:` line, unchanged (the
    recovery shell; starts whatever happens to OpenRC);
  - `::once:` running `openrc default` (no `::wait`: a hanging service must
    not hold back telnetd). The sysinit and boot runlevels stay empty:
    `/init` does that work. Check what OpenRC's own sysinit step
    (`/usr/libexec/rc/sh/init.sh`) does to our mounts and say so; it is
    known to create `/run/openrc` and `/run/lock` (owner `root:uucp`: add
    the `uucp` group if the warning is the only effect, or say why not);
  - `abslot`'s 30 s `::once:` line, unchanged;
  - the `::shutdown:`, `::ctrlaltdel:` and `::restart:` lines (below).
- OpenRC settings in a drop-in `rootfs-overlay/etc/rc.conf.d/chef.conf`
  (the package's `rc.conf` stays as shipped): `rc_parallel="NO"`
  (serial and deterministic; rc.conf itself warns parallel can lock the
  boot), `rc_interactive="NO"`, OpenRC's own boot output kept somewhere
  readable (`rc_logger` to its default `/var/log/rc.log` on the volatile
  `/var`, or `logger`: your call, documented), and the respawn defaults
  for every supervised service (`respawn_delay=1s`, `respawn_max=0`):
  check that variables from the drop-in reach `supervise_start` for every
  service, packaged ones included, or set them per service.
- Make sure OpenRC mounts nothing new (cgroups, devfs, ...): compare
  `/proc/mounts` in your notes from what the scripts would run, and list
  it as a live-check item.
- Runlevel symlinks for `default` live in `rootfs-overlay/etc/runlevels/`.
  `rc-update` cannot change them on the read-only root; say so in the
  guide.
- Packages: add `openrc` and only the `-openrc` subpackages whose scripts
  are used (`busybox-openrc`, `dbus-openrc`, `bluez-openrc`,
  `chrony-openrc`, `networkmanager-openrc`) to `scripts/mkrootfs.sh`, with
  a comment like the others. Report the RAM image size (the loader margin
  is about 24 MB).

### Services (M1)

Rules:

- Every former `::respawn:`/`::once:` line except telnetd and abslot
  becomes an OpenRC service in the `default` runlevel. Logic now inline
  in `inittab` moves into the service file (powerd's `/data` or `/run`
  choice, chronyd's directories, bluetoothd's BNEP rule).
- **Third-party daemons use Alpine's packaged script**, adjusted only by
  `/etc/conf.d/<svc>` (`supervisor=supervise-daemon`, arguments, removed
  dependencies, `required_files`, a `start_pre`/`start_post` the packaged
  script does not define). Where the packaged script already defines the
  hook we would need, put the work where its state is owned instead (for
  example chronyd's `/var/lib/chrony` ownership in `chef-storage boot`,
  which already binds it) or, if that is not possible, replace the script
  under the same name in the overlay with a header comment saying why.
  Every Alpine boot-service dependency they carry is removed in conf.d:
  **no Alpine boot service may start** (`localmount`, `hostname`,
  `bootmisc`, `hwclock`/`swclock`/`osclock`, `sysctl`, `modules`, `devfs`,
  `cgroups`, `networking`, `loopback`, `killprocs`, `mount-ro`,
  `savecache`, ...).
- **Our programs get our own scripts** named after the program.
- Daemons run in the foreground under supervise-daemon (`-n`, `-f`,
  `--nofork` as each needs; check each packaged script's arguments for a
  forking default).
- `need` only where a daemon cannot run without the other (it stops with
  it); `after`/`use` where the consumer copes on its own. Every service
  `use logger` or `after syslog`, so syslog is first.
- A failed precondition that cannot change later in the boot (a missing
  marker from `/init`, a failed firewall rule) fails the start; it is not
  retried every 10 s as the inittab wrappers did. List which ones.
- Long-running output files stay where they are (`/run/chef/log/modem-up.log`,
  `abslot.log`) through `output_log`/`error_log` or the program's own
  redirection.

The list (names are suggestions where marked; settle them in M1 and list
them in the guide):

| inittab today | Service | Notes |
|---|---|---|
| `syslogd -n -t -C1024` | `syslog` (packaged) | `SYSLOGD_OPTS="-t -C1024"`; drop its `clock`/`hostname`/`localmount` needs. First in the runlevel. |
| `udhcpd -f -S ...` | `udhcpd` (packaged) | config path and `-S` through `UDHCPD_OPTS`; drop `need net` (it serves the USB link `/init` set up). |
| system `dbus-daemon` | `dbus` (packaged) | `required_files=/run/chef/dbus-helper-ready` replaces the sleep loop; readiness: bounded wait for `/run/dbus/system_bus_socket` in `start_post`. |
| bluetoothd with the BNEP guard | `bluetooth` (packaged) | `need dbus`; the BNEP rule in `start_pre`, a failure fails the start with the existing `bnep-guard` log line; `-n`. |
| root's session bus | own, e.g. `root-session-bus` | as today. |
| `gps-up` (`::once`) | `modem-up` (own, renamed from `gps-up`: see below) | Not respawned (as today: one modem lifetime per boot): start-stop-daemon backgrounding, so `rc-status` shows it crashed if it dies. `stop` refuses outside `openrc shutdown` (`RC_RUNLEVEL=shutdown`) with a clear message: a modem restart within a boot was never tested and WLAN rides on it. Its start returns at once (no wait for the modem). |
| `wifi-nm` | `networkmanager` (packaged, conf.d) | `need dbus modem-up`, `after *` so its wait holds nothing else back. `start_pre` does what `wifi-nm` does before `exec` (USB guard, `udev-ready`, the manual-Wi-Fi check, a bounded wait for the modem owner, `wifi-prepare`, the state-file choice into `command_args`); keep that logic in a script the hook calls if that keeps it testable. The wait for the modem stays on this side (consumer readiness): a `start_post` wait in `modem-up` would hold the serial boot for about 6 s. Its bound is today's (600 s); NetworkManager simply starts last. |
| chronyd | `chronyd` (packaged) | `FAST_STARTUP=yes` (foreground, `-n`) and `-u chrony`; drop `need net` so it starts early (its GPS refclock needs no network); check `-F 1` (seccomp filter; the kernel has `CONFIG_SECCOMP_FILTER`) and keep it unless it breaks something. `/run/chrony` and `/var/lib/chrony` must be `chrony:chrony 0700` before it starts: decide where (see the rules). |
| `chef-state timekeeper` | own, e.g. `chef-timekeeper` | |
| `gps-manager`, `bt-up`, `blesensd`, `buttond`, `chef-sshd`, `chef-settings` | own | `blesensd` `after bluetooth`; `chef-settings` as early as possible after syslog (see boot time). |
| powerd with its `/data` choice | `powerd` (own) | the choice in `start_pre` (`chef-state ours`). |
| sensors-up wrapper | `sensors-up` (own) | M1: like for like (the resident `sensors-up` supervised, its log rotation in `start_pre`). M2 splits it. `after modem-up`: the modem stack starts first, as today (it also brings up `/firmware` and the servreg-locator that audio-up then reuses). |

Not services: telnetd and abslot (inittab), udevd and the coldplug (still
started by `/init` before init, with its `udev-ready` marker as a
precondition for NetworkManager), wpa_supplicant and the NM dispatcher
(D-Bus activated), gps-manager's gpsd/qmicli pipeline and blesensd's
`chef-state` calls (short-lived helpers, as the plan says).

### Rename gps-up to modem-up (M1, its own commit first)

Decided 2026-10-21 with the user. `gps-up` does no GPS work: it boots the
modem and keeps its support daemons (rmtfs, servreg-locator, tftp, the
qmux bridge) running for the whole boot; GPS is `gps-manager`, and Wi-Fi
rides on the same modem. The name now says so, before it becomes a
service that NetworkManager `need`s.

- Program `rootfs-overlay/usr/bin/gps-up` becomes `modem-up`, its lock
  `/run/chef/lock/modem-up.lock`, its log `/run/chef/log/modem-up.log`,
  its syslog tag `modem-up`, its test `tools/tests/test_modem-up.sh`. Every
  user follows: `modem-owner.sh`, `wifi-prepare`, `audio-up`, `sensors-up`,
  gps-manager's owner check (`tools/gps-manager/`, `chef_paths.h`), the
  build and its tests, and every comment and current doc that names it
  (about 38 files outside the historical ones; `git grep gps-up`).
  Historical text keeps the old name; `runtime-files.md` and the GPS guide
  say it was renamed on this date so old logs still make sense.
- Behaviour does not change: same script logic, one modem lifetime per
  boot, nothing restarts it.
- One commit with the rename only, reviewed on its own (`review/M1a.diff`),
  before the OpenRC work (`review/M1b.diff`, the second M1 commit).
- Not part of this step: splitting rmtfs, servreg-locator, tftp and the
  qmux bridge into services of their own. Whether the modem survives a
  helper restart mid-lifetime is untested, and servreg-locator is shared
  with the ADSP; that belongs with the ADSP lifecycle work.

### Boot time (M1)

The UI must not light later. With a serial runlevel the order matters:
`syslog`, then `chef-settings` and the other quick services, then the
slower ones; `networkmanager` last. Tell me the expected order (`rc-depend`
or your own reading of the dependencies) in the M1 note. The target for
the live check: chef-settings' start within 0.3 s of the baseline (6.73 s
after boot) and `modem-up` within 0.5 s of it. If you think the first
dependency-tree build (about 0.2 to 0.35 s per boot) needs avoiding, say
so with a plan; do not build it in without asking.

### Shutdown, reboot and anything that kills daemons (M1)

A supervised daemon killed by anyone but its supervisor is respawned, so
every path that today TERMs a daemon must stop it through OpenRC instead:

- `chef-state shutdown` TERMs chronyd, powerd, sensors-up and the
  timekeeper itself. It stops those services through OpenRC instead (in
  parallel, within its existing 1.8 s deadline; never by signalling the
  daemon), and keeps everything else it does.
- The `::shutdown:` order becomes: `chef-state shutdown` (bounded as now),
  `chef-state log-save` (as now; syslogd still runs), **`openrc shutdown`**
  (new, bounded with `timeout`; stops every remaining service in reverse
  dependency order, modem-up's clean modem stop included, while `/data` is
  still mounted), `chef-storage shutdown`, `umount -a -r`. The shutdown
  runlevel stays empty. Choose the bound from what the stops can take
  (modem-up's cleanup, NetworkManager) and say why.
- `chef-reboot` gets the same `openrc shutdown` step between its log save
  and `chef-storage shutdown`, bounded like its other steps; update its
  header list, `WATCHDOG_SECS` arithmetic and the test that checks it.
  Its init pause (SIGTSTP) stays for the inittab lines.
- `::restart:` runs the same sequence as the shutdown lines before
  `exec /sbin/init` (the new init's `::once` line then starts the services
  again).
- `chef-storage shutdown`'s fuser sweep runs after `openrc shutdown`, so
  only non-OpenRC users of `/data` are left for it; check nothing else in
  it relies on init's respawn being off.
- `grep` the tree (scripts, C, tests, `docs/live-testing.md`) for other
  places that kill or restart our daemons by pid or name (`pidof`,
  `kill`, live-test helpers that restart a daemon) and move them to
  `rc-service`.

### Sensors: OpenRC supervises sensord (M2)

The plan's "daemons stop supervising their own long-running helpers":
`sensors-up` stops being resident.

- **`sensord`** becomes its own supervised service. `start_pre` does what
  `sensors-up` does before starting it today (debugfs, the persist-safe
  registry copy or reuse with `blockdev --setro`, `chef-state mag-restore`,
  `irsc`); `start_post` waits, bounded, for REG2 (`0x10f`) to be served,
  because the ADSP must find it before it boots. The orphan-sensord code
  and `sensors-up`'s pid file go: the supervisor owns the pid.
- **`sensors-up`** becomes a one-shot service, `need sensord` and
  `after modem-up`: boots the ADSP through `audio-up` unless it is online or
  audio-up is running, as today; no SMGR wait (sensord polls for it). Its
  output keeps going to `/run/chef/log/sensors-up.log` if that is still
  useful; one run per boot needs no rotation.
- **The registry save on write-back moves into sensord**: after it writes
  the `/run` copy back it runs `chef-state mag-save` itself (a child in its
  own process group, as blesensd runs chef-state), with the same 60 s rate
  limit and the same keep-pending-until-accepted rule. That removes
  `sensors-up`'s once-a-second poll loop. The final save stays in
  `chef-state shutdown` (it already does `CHEF_STATE_MAG_FINAL=1`).
- A `kill -9` of sensord: the supervisor restarts it; the registry copy of
  this boot is reused (as it is today when sensors-up restarts); say in
  your note whether the ADSP needs anything else then, from the code and
  the sensors guide, so the live check can confirm it.
- Update `sensors-compass-run`, `sensors-magcal-run`,
  `tools/sensord/tests/test_sensors-up.sh` and the sensors guide.

### Docs (M1 to M3)

- New `docs/features/services.md`, linked from the feature index and from
  `boot.md`: busybox init plus OpenRC and what stays in inittab and why;
  the service list with what each needs and its readiness; `rc-status`,
  `rc-service`, `rc-update` not usable on the read-only root; respawn
  behaviour; the runtime stop latency; the shutdown order; the readiness
  findings (why no `notify=`).
- Every guide and doc that describes inittab lines, restarting a daemon
  (`kill` and let init respawn it), or the shutdown order says what is
  true now. `docs/live-testing.md` says how to stop, start and restart a
  service and how to see what failed (`rc-status`, `logread | grep
  supervise-daemon`, the rc log).
- `docs/repository-layout.md`: `etc/init.d`, `etc/conf.d`,
  `etc/runlevels`, `etc/rc.conf.d`.

## Milestones

1. **M1 OpenRC**: first the `gps-up` to `modem-up` rename (its own
   commit), then packages, inittab, `chef.conf`, every service converted
   like for like (sensors-up still resident), the shutdown/reboot/restart
   paths and every daemon-killing path, the host tests, `services.md`
   first version.
2. **M2 sensors**: `sensord` service, one-shot `sensors-up`, mag-save in
   sensord, tests, sensors guide.
3. **M3 docs and cleanup**: the remaining guide updates, live-testing,
   repository layout, `services.md` final, anything left in the tree that
   still describes inittab respawn.

## Acceptance (host side)

`make test` passes. New host tests: every script in `etc/init.d` that we
ship or configure passes `sh -n`; every runlevel symlink resolves to a
script in the image; the staged image's `inittab` has only the lines named
above; static checks over the conf.d files that every packaged script's
Alpine boot-service dependency is removed (a list in the test); and tests
for the moved logic (powerd's choice, the NM `start_pre` script, chef-state
shutdown stopping through OpenRC with a stub `rc-service`, chef-reboot's
new step, sensord's mag-save trigger). The staged image holds the same
files as before plus OpenRC and the new config (compare the file lists and
explain every other difference).

## End (coordinator)

RAM image live check: `rc-status` lists every service started and none of
Alpine's boot services; `/proc/mounts` unchanged against the baseline;
`rc-service <name> restart` for each one (only its `need` dependents
follow); `kill -9` of each supervised daemon is respawned, and modem-up
refuses a stop; a hanging service (a test script with a stuck
`start_pre`) does not stop telnetd from coming up; boot times against the
baseline above (chef-settings start, screen on, modem up, NetworkManager
start); idle current unchanged within the A/B method; power-off and
`chef-reboot bootloader` with the shutdown log lines and the saved ring
(`/data/v1/log/syslog-<seq>`) as before, chronyd's drift file written,
nothing respawned during shutdown; sensord `kill -9` with sensors still
working (ALS on the status bar). Then archive this file, write
`logs/openrc-2026-10-21.txt`, mark step 7 done in the plan, install, push.

## Result (2026-10-22)

Done. Commits on main, each approved by `openrc_review` and checked
against its reviewed snapshot: `0720bc2` (M1a, `gps-up` renamed
`modem-up`), `d2c7dbe` (M1b, busybox init plus OpenRC: packages and the
prune, `inittab`, `chef.conf`, every service converted, the shutdown,
`chef-reboot` and daemon-killing paths, `chef-stop-supervisors`,
[services](../features/services.md)), `8051738` (M2, `sensord` its own
service, a one-shot `sensors-up`, mag-save in sensord), `856510e` (M3, the
guides, live testing, repository layout and code comments), `911122c` (M4,
from the live check: `chef-sshd` and `bt-up` stop a killed predecessor's
dropbear or btattach first, NetworkManager `use`s modem-up so a refused
stop leaves it running, modem-up's stop bounds raised because the modem
never acks a shutdown and the kernel waits 10 s for it).

The pair was cleared and restarted fresh after the hold of 2026-10-21 (M3
resumed from the implementer's hold notes). The rest of the shell helper
library in `/usr/lib/chef` moved to step 8. RAM images `014feeab` (through
M3) and `fbf7be01` (with M4) live verified on 2026-10-22. Details in
[the log](../../logs/openrc-2026-10-21.txt).
