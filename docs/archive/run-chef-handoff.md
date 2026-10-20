# /run/chef, the screen handover and tzdata (standard interfaces step 5)

User request 2026-10-20 ("Step 5 is next: moving runtime files under
/run/chef/, tidying the screen handover, and adding tzdata"). The scope is
step 5 of the [standard interfaces](../next-steps/standard-interfaces.md#order) order:
section 4 of that plan, the first item (`/run/chef/`) and the `tzdata` item
of its section 5. Read those sections, the
[display guide](../features/display-and-touch.md), the
[UI platform](../features/ui-platform.md) screen sections and the
[local socket protocol](../features/local-sockets.md) first; this file adds
the decisions and the working rules.

Roles: `runchef_impl` (Claude Opus) implements, `runchef_review` (Claude
Opus) reviews, `runchef_coord` coordinates and decides, and runs the live
check. Report with `herdr agent prompt runchef_coord "[From: runchef_impl] ..."`
(or `runchef_review`). Send a message when a milestone is ready for review,
when a review is done, when blocked, and at the end. Do not poll or wait on
each other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/run-chef-20261020/` (create it).
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
- Historical text keeps the old paths: `docs/archive/`, `docs/research/`,
  `logs/`, `docs/build-log.md` and the finished handoffs still in
  `docs/next-steps/` (`*-handoff.md` other than this one). Everything that
  describes the current system (code, comments, `docs/features/`,
  `docs/repository-layout.md`, `docs/live-testing.md`, `docs/device.md`,
  the open plans in `docs/next-steps/`) uses the new ones.
- Ask `runchef_coord` before deciding anything this file and the plan do not
  settle. In particular: any `/run` name not in the tables below.

## Today

About 40 of our names sit directly under `/run` (grep of the live tree on
2026-10-20): four daemon sockets plus `qmux_socket`, the screen files
`fb0.lock`, `fb0.lit` and `fblog.off`, boot markers polled by `inittab` and
scripts, logs, `flock` files, and per-component directories (`power`,
`sensors`, `wifi`, `ssh`, `ui`, `chef-settings`, `gps-manager`, ...).
Socket paths are defined several times over in C (`sensord.h` and
`chefui/sys/alsclient.c`; `blesensd.h`, `chefui/sys/blesclient.h` and
`blesens.h`; `gps-manager.h` and `chefui/sys/gpsclient.h`; `buttond.c` and
`chefui/buttons.h`). `/init` mounts the `/run` tmpfs (line 28) and each
owner creates its own files.

`buttond`'s default short power press creates `/run/fblog.off` and then
SIGTERMs every process whose `/proc/PID/comm` is `fblog` (`kill_comm`,
`buttond.c` around 587 to 790). `fblog` has not been in the image since
2026-10-17 (`7ccc163`); it is a host-built debug tool run by hand
(`docs/features/display-and-touch.md`, "fblog (debug tool, not in the
image)"). `tools/tests/test_fblog.c` includes `buttond.c` and tests the
pair.

The UI applies the settings file's `tz` line (`IST-5:30`, a POSIX string,
no tzdata) with `setenv("TZ")` in `tools/chefui/sys/clock.c`. The image has
no `/etc/localtime` and no `/usr/share/zoneinfo`, so every other program
(`date`, logs over ssh) shows UTC.

## Decisions

### Layout (M1, M2)

`/init` creates `/run/chef`, `/run/chef/log` and `/run/chef/lock` with
`mkdir -p -m 0755` (owner root, not group or world writable: root-only
files sit below it, such as dropbear's RAM host key in `/run/chef/ssh`;
its `-D` authorized-keys walk is under `/etc/chef/ssh`) immediately after
`mount -t tmpfs tmpfs /run`, before anything else writes under `/run`.
Component directories are created by their owners as today, with their
current modes.

| Today | Under `/run/chef/` | Notes |
|---|---|---|
| `/run/buttond.sock`, `/run/gps-manager.sock`, `/run/sensord.sock`, `/run/blesensd.sock` | `buttond.sock`, `gps-manager.sock`, `sensord.sock`, `blesensd.sock` | modes unchanged (blesensd 0600) |
| `/run/qmux_socket` | `qmux_socket` | libqmi needs `qmux_socket` in the path; qmuxd-lite's check stays |
| `/run/fb0.lock`, `/run/fb0.lit` | `fb0.lock`, `fb0.lit` | M2 |
| `/run/fblog.off` | `screen-off` | M2, renamed |
| `/run/udev-ready`, `/run/dbus-helper-ready`, `/run/wifi-nm-mode`, `/run/wifi-usb-guard-ready`, `/run/bt-powered`, `/run/wlan-prepare` (directory), `/run/usb-interface`, `/run/udhcpd.conf` | same names | markers and generated config; OpenRC (step 7) replaces most markers later |
| `/run/gps-up.log`, `/run/sensors-up.log` (+`.1`), `/run/abslot.log`, `/run/audio-up.log`, `/run/chef-reboot.log`, `/run/storage.log`, `/run/storage-watchdog.log` | `log/` + same names | the `sensors-up` rotation in `inittab` follows |
| `/run/gps-up.lock` (directory with `ready`), `/run/audio-up.lock`, `/run/sensors-up.lock`, `/run/storage.lock` | `lock/` + same names | the cross-program exclusive locks |
| `/run/power`, `/run/sensors`, `/run/sensortest`, `/run/gps-manager`, `/run/wifi`, `/run/ssh`, `/run/ui`, `/run/chef-settings` | same names | per-component directories |
| `/run/chef-reboot.lock` | `lock/chef-reboot.lock` | added 2026-10-20 after the reviewer's baseline grep |
| `speaker-test-tone`'s `RUN_DIR` default `/run` (temp `.wav`) | `RUN_DIR` default `/run/chef` | added 2026-10-20 |
| `/run/sensors-up.sensord.pid` | `sensors/sensord.pid` | private to sensors-up |
| `/run/chef-time`, `/run/chef-state-gps-enabled`, `/run/chef-state-blesens`, `/run/chef-state-<kind>.lock`, `.err`, `/run/chef-state-mag-last`, `/run/chef-state-timekeeper.pid` | `state/time`, `state/gps-enabled`, `state/blesens`, `state/<kind>.lock`, `state/<kind>.err`, `state/mag-last`, `state/timekeeper.pid` | chef-state's own directory, prefix dropped; `chefui/sys/settings.c` and `timeview.c` read `state/time`; its temp `chef-time.tmp` becomes `state/time.tmp` |

Rule for names like the last three: a program's temp files live beside
the file they replace; a lock with no owning directory goes to `lock/`.
The guard test also checks the defaults of `RUN`-style variables in the
scripts where it can; names built from `$RUN` at run time are checked by
hand at review.

Programs that take a run base directory today (`chef-state`
`CHEF_STATE_RUN`, `chef-storage` `CHEF_STORAGE_RUN`, `chef-reboot`
`CHEF_REBOOT_RUN`, the settings store's `run` (`/run` -> `ui/settings`),
`chef-settings`'s `O.run`) default to `/run/chef` and build the paths above
from it. Every existing override (environment variables, `-S`, `-d`, `-o`,
`-q` and friends) keeps working; host tests keep using them.

Left where they are:

- **Other packages' standard names:** `/run/dbus`, `/run/udev`,
  `/run/NetworkManager`, `/run/wpa_supplicant`, `/run/chrony`, `/run/user`,
  `/run/lock`, and `/init`'s transient `/run/.var`.
- **Stage 1** (`stage1/init`) is untouched: its rescue world keeps
  `RUN=/run`, `/run/rescue-reason` and `/run/udhcpd.conf`. Because
  `usr/lib/chef/usb-gadget.sh` is shared with stage 1 (`mkstage1.sh` copies
  it), it takes its directory from a variable, `USB_GADGET_RUN`, default
  `/run` (what stage 1 relies on); `/init` sets `USB_GADGET_RUN=/run/chef`
  before sourcing it, and `inittab`'s `udhcpd` reads
  `/run/chef/udhcpd.conf`. `tools/tests/test_stage1.py` must pass
  unchanged in what it checks about stage 1.
- **Live-test scratch** used by host tooling against any image, the
  installed fallbacks included: `/run/push` (`phone.py push`), `/run/x`,
  `/run/test`, `/run/evidence`, `/run/CMD.out`,
  `/run/bootloader-restart.log` (`phone-boot.sh`), and the logs of
  hand-run debug commands in the guides (`/run/gpsd.log`, `/run/gps/`,
  `/run/magcal.log`, `/run/compass.log`, `/run/pipewire.log`,
  `/run/wireplumber.log`; accepted after M1). Only `phone.py`'s
  `/run/ssh/host-key-source` hint changes (to `/run/chef/ssh/...`).

**No compatibility symlinks.** Every client of every socket is in this
repository and ships in the same image; nothing outside connects.

**One header for the C paths.** `tools/common/chef_paths.h` (header only,
next to `chef_log.h`) defines every `/run/chef` path C code uses: the five
sockets, the screen files, `power`, `sensors/sns.reg`, `gps-manager`,
`lock/gps-up.lock`, `log/abslot.log`, `state/time`, the settings run base.
Daemons, chefui and their clients use it instead of their own literals, so
a client and its daemon cannot disagree again. Comments that name a path
keep naming it (`/run/chef/sensord.sock`); shell scripts keep literal
paths (a shell library is step 7).

**A guard test.** A host test (in `scripts/tests/`, run by `make test`)
scans the files that make up the image (`rootfs-overlay/`, non-test
sources under `tools/` excluding `build/` and vendored code such as LVGL,
the `scripts/mk*.sh` builders) for `/run/<name>` and fails on any top-level
name that is not `chef` or in an allow list with a reason per entry (the
standard names above, `.var`, the `USB_GADGET_RUN` default). Comments
count. `stage1/` is excluded with its reason.

**A guide.** `docs/features/runtime-files.md`: the `/run/chef` layout
(the table above in its final form, owner per entry), who creates what and
with which mode, what stays outside and why (standard names, stage 1,
scratch), linked from `docs/features/README.md`, `local-sockets.md` and
`storage.md` (its RAM fallbacks). Feature guides that list paths point to
it.

### Screen handover (M2)

- The three files move as in the table; `screen-off` replaces
  `fblog.off` everywhere current (the "historical name" remarks go).
  Users: `tools/fbdev.h`, `tools/chefui/fbscreen.h` and `fbscreen.c`,
  `tools/buttond/buttond.c` (`-o` default and help), `tools/fblog/fblog.c`,
  `tools/fbtouch.c`, `tools/chefui/lv_conf.h`'s comment, `inittab`
  comments, the tests and the guides.
- `buttond` no longer scans `/proc` or signals anything: remove
  `kill_comm` and its hook. The default `power.short` creates or removes
  the flag and logs that, nothing else. With chef-settings stopped nobody
  holds fb0, so the panel is already off; the next screen owner honours the
  flag as today.
- `fblog` stays a hand-run debug tool and watches the flag itself while
  lit: the same inotify watch of the flag's directory (now the quiet
  `/run/chef`, not `/run`) it already uses while idle, with the 250 ms
  recheck fallback. The flag appearing while lit: `FBIOBLANK POWERDOWN`,
  close fb0, idle (what the SIGTERM plus respawn did). Removal wakes it as
  today. `tools/tests/test_fblog.c` tests the pair through the flag only.
- The handover itself (`LOCK_EX` before fb0, the lock fd across `exec` in
  `CHEFUI_LOCK_FD`, the `fb0.lit` power-cycle after a crash) does not
  change beyond the paths. Write in your final note how the live check can
  trigger an exec handover on the phone (which command or page does it).

### tzdata (M3)

- `scripts/mkrootfs.sh` adds `tzdata` (Alpine v3.24 `2026e-r0`, 191 KB
  apk, 452 KB installed) with a comment like its neighbours, and checks
  `usr/share/zoneinfo/Asia/Kolkata` in its final `ls`. Fetching it into
  `toolchain/apk/cache` needs the network once; if downloads crawl, say
  so (the user turns on WARP).
- The system zone is `Asia/Kolkata` the Alpine way: `/etc/localtime` a
  symlink to `/usr/share/zoneinfo/Asia/Kolkata` and `/etc/timezone`
  holding `Asia/Kolkata`, from `rootfs-overlay/etc/`. Check that the
  symlink survives into both the RAM initramfs and `system_a`
  (`mkinitramfs.sh`, `mksystem.sh`) and say how you checked.
- chefui (`sys/clock.c`): when the system zone file (default
  `/etc/localtime`; settable for tests) opens and starts with `TZif`, set
  `TZ=":/etc/localtime"` (the path form, which musl and glibc both read);
  otherwise use the settings file's `tz` POSIX string as today (`UTC0` if
  empty). Log once at start which one is in use. The settings file format
  does not change (no version bump; rollback to an older image stays
  safe): `tz IST-5:30` is now documented as the POSIX fallback used when
  the zone file is missing. Fix the "no tzdata" remarks in `clock.c`,
  `settings_file.h`, `time.state` and `docs/features/system-ui.md`.
- The Time and date page keeps its text: `IST · UTC+05:30` and the status
  bar `14:16 · IST`. Host tests cover both paths: a real zone file (the
  built `out/rootfs` one or the host's `/usr/share/zoneinfo/Asia/Kolkata`,
  skipped with a message when neither exists) gives `%Z` `IST` and offset
  +05:30; a missing or non-TZif file gives the POSIX result. Existing tests
  that expect POSIX behavior point the zone file at a missing path (this
  host's own `/etc/localtime` is Asia/Kolkata, so do not rely on it).
- Audit everything that formats local time and list it in your final
  note. Known: persisted records already use UTC or epoch (`chef-state`
  `date -u`, `powerd` and `blesensd` `gmtime_r`, `deviceinfo` UTC);
  `sensors-compass-run` and `sensors-magcal-run` name their evidence
  directories with local `date`, which becomes IST (fine, say so in the
  sensors guide).
- Report the image size change (system_a file list and `du`, the RAM image
  size) and that `check-chef-loader-budget.py` still passes for the RAM
  image.

## Milestones

1. **M1 `/run/chef` layout**: `/init`'s directories, every move in the
   table except the screen files, `USB_GADGET_RUN`, `chef_paths.h`, the
   guard test (with the screen files temporarily allowed, removed in M2),
   `runtime-files.md` and the guide updates.
2. **M2 screen handover**: the three files, the flag rename, `buttond`
   without the `/proc` scan, `fblog` watching the flag, tests, guides.
3. **M3 tzdata**: package, `/etc/localtime` and `/etc/timezone`, chefui's
   zone choice, tests, the audit, size report.

## Acceptance (host side)

`make test` passes; the guard test passes with no screen-file exception;
the staged image holds the same files as before apart from tzdata and the
new `/etc` files (compare the file lists); `strings` over the staged
binaries and a grep of the staged scripts find no old `/run` path outside
the allow list.

## End (coordinator)

RAM image live check with the user: `ls /run` shows only `chef` and the
standard names, `ls -lR /run/chef`; boot to a lit status bar; ssh login
(dropbear permission walk); buttons and Back through chef-settings' claim;
screen off and on with a short power press with chef-settings running and
stopped, and a boot with `screen-off` present stays dark; the exec
handover; `kill -9` of chef-settings with the panel lit (respawn
power-cycles, touch works); GPS lease, ALS, Wi-Fi and Bluetooth pages;
`date` shows IST, the Time and date page unchanged; with
`/etc/localtime` hidden by a bind mount, a restarted chef-settings still
shows IST through the POSIX fallback. Then archive this file, write
`logs/run-chef-2026-10-20.txt`, mark step 5 done in the plan, install,
push.

## Result (2026-10-20)

Done the same day. Commits on main, each approved by `runchef_review` and
checked against its reviewed snapshot: `f745ab6` (M1 `/run/chef` layout,
`tools/common/chef_paths.h`, `scripts/tests/test_run_layout.py`,
[runtime files](../features/runtime-files.md)), `5180d67` (M2 screen
handover: `screen-off`, buttond without the `/proc` scan, fblog watching
the flag), `151c524` (M3 tzdata, `/etc/localtime`, chefui's zone choice).
Decided during the work: `chef-reboot.lock`, `ui-settings.lock` in `lock/`;
speaker-test-tone's `RUN_DIR` default `/run/chef`; `state/time.tmp`; the
hand-run debug logs in the guides stay scratch. RAM image `0893646a` live
verified with the user (every item of the End list above, plus fblog run by
hand); `/run` also holds `udhcpd.pid` and `xtables.lock`, busybox udhcpd's
and iptables' own default names. Details in
[the log](../../logs/run-chef-2026-10-20.txt).
