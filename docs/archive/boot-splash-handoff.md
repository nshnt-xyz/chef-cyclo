# Boot splash (B01)

User request 2026-10-25 ("lets start with the boot screen first!"), after the
2026-10-25 design round put a boot splash and a Home screen in the Claude
Design project. The splash comes first because it needs no new backend beyond
one marker file; Home and the ride screens wait for their backends (see
[UI and ride app](../next-steps/ui-and-ride-app.md#ride-application-design)). Read
[system UI](../features/system-ui.md), [UI platform](../features/ui-platform.md),
[services](../features/services.md), [runtime files](../features/runtime-files.md)
and `tools/chefui/apps/settings/main.c` first; this file adds the design, the
decisions and the working rules.

Roles: `splash_impl` (Claude Sonnet) implements, `splash_review` (Claude Opus)
reviews, `splash_coord` coordinates, decides and runs the live check. Report
with `herdr agent prompt splash_coord "[From: splash_impl] ..."` (or
`splash_review`). Send a message when a milestone is ready for review, when a
review is done, when blocked, and at the end. Do not poll or wait on each
other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/boot-splash-20261025/`. Design copies and
reference renders are in `design/`; review diffs go to `review/<milestone>.diff`;
the reviewer snapshots what it reviewed (sha256 in its note) and checks the
commit against that snapshot.

## Working rules

- Work on `main` in this checkout. Milestones in the order below, each
  reviewed before the next. One commit per milestone after the reviewer's
  explicit approval. Do not push. Do not touch the `experiment/*` or `ui`
  branches. Keep this file and any edit of `ui-and-ride-app.md` out of your
  commits (the coordinator commits them at the end).
- No phone access. No `fastboot`, `phone.py`, `phone-boot.sh`, telnet or ssh to
  the phone: the coordinator runs the live check. Anything that can only be
  learned on the phone goes in your milestone note as a question for the live
  check.
- The implementer does not research or decide. Where this file and the code
  disagree, or this file does not settle something, stop and ask
  `splash_coord` with the exact question and the options you see; the
  coordinator answers with the fix. The reviewer checks the code against this
  file and reports deviations as findings, not as new designs.
- Match the surrounding code's style and comment density.
- `make test` at the repo root must pass at every milestone. Stress or flake
  checks: short, targeted, runtime stated first. No host test may write to the
  host's syslog, read the host's `/run/openrc`, or depend on the host's
  services: every path is injected.

## The design (B01)

From `Home.dc.html` (`screen="splash"`), 1080x2246 device pixels; references
in `design/ref-png/B01-{dark,light}-k{0,1,6}.png` (k = steps done: 0, 1 and
all 6), rendered with `design/render-splash.sh K THEME OUT` (local Barlow
fonts, so renders are deterministic). The design draws the notch as a black
rectangle; that is the hardware, not ours to draw.

- **No status bar.** The splash covers the whole panel, safe top included,
  in the theme's `bg`.
- **Title block**, centred horizontally, vertically centred in the panel
  minus a 200 px bottom padding (so its centre is at y = 1023): `chef-cyclo`
  in Barlow Condensed 600, 150 px, line height 1, letter spacing 0.02 em
  (3 px), colour `fg`; 20 px below it the version line in Barlow 400, 40 px,
  `mut`, tabular figures.
- **Progress block**: left 120, right 120 (840 px wide), its bottom 360 px
  above the panel's bottom. Top row: the step text left (Barlow 500, 48 px,
  `fg`) and the count right (Barlow Condensed 600, 48 px, `mut`, tabular,
  `2 / 6` with spaces), baselines aligned. 32 px below: the bar, 24 px high,
  radius 12, track `line`, fill `acc` (radius 12), fill width = steps done / 6
  of 840 px.
- Text: the current step's label with an ellipsis (U+2026; the design font
  renders it as three dots), `Ready` when all six are done.

Fonts: check the generated kit fonts' glyph sets (`kit/fonts.h` comments,
`kit/fonts/gen-fonts.sh`). `chefkit_bc600_150` must hold `chef-cyclo`;
`chefkit_bc600_48` holds only digits and `/` today and needs the space; the
48 px Barlow 500 needs the step labels and U+2026; a 40 px Barlow 400 needs
the version line (`v`, `.`, digits, `a-f`, U+00B7, `+`, letters of `dev` and
`dirty`). Extend the generated subsets rather than adding new sizes.

## Decisions (user, 2026-10-25)

### Steps and when each is done

| # | Label (in progress) | Done when | Failed when (label shown) |
|---|---|---|---|
| 1 | `Mounting storage` | always done when the UI starts (`/init` runs `chef-storage boot` before OpenRC) | never |
| 2 | `Starting GPS modem` | `/run/chef/modem-ready` exists (new, below) | `/run/openrc/failed/modem-up` exists (`GPS modem failed`) |
| 3 | `Starting sensors` | `/run/openrc/started/sensord` and `/run/openrc/started/sensors-up` both exist | either is in `/run/openrc/failed/` (`Sensors failed`) |
| 4 | `Starting Bluetooth` | `started/bt-up` and `started/bluetooth` | either in `failed/` (`Bluetooth failed`) |
| 5 | `Starting Wi-Fi` | `started/networkmanager` | `failed/networkmanager` (`Wi-Fi failed`) |
| 6 | `Loading home` | the splash's own last step: done as soon as steps 1 to 5 are done or failed | never |

- The text shows the first step that is neither done nor failed, and the
  count is that step's number (`3 / 6`). A step that completes out of order
  (sensors start before the modem is ready) is simply already done when the
  text reaches it.
- The bar's fill is the number of leading steps that are done or failed,
  out of 6.
- A failed step shows its failed label (for example `Wi-Fi failed`) in place
  of the step text for 1.5 s, then the splash moves on; it counts as finished
  for the bar.
- When all six are finished the text reads `Ready`, count `6 / 6`, bar full,
  for 500 ms, then the splash goes and the hub (P01) shows. Home replaces the
  hub later; nothing else changes after the splash.
- **Cap**: 30 s after the app's start. Then every unfinished step is logged
  (`boot splash: <label> not finished after 30 s`) and the splash goes as
  above (no `Ready` hold). The cap also covers a step whose service never
  reaches `started` or `failed` (a crashed `modem-up`, NetworkManager's modem
  wait of up to 600 s).
- **Logging**: one `chef_log` line when each step finishes, with the time
  since boot from `CLOCK_BOOTTIME` (`boot splash: GPS modem ready at 12.31 s`,
  `... failed at ...`), and one when the splash ends. These are the first
  boot-timeline lines in syslog; keep the wording stable.

### Sources

- **OpenRC state**: `/run/openrc/started/<service>` and
  `/run/openrc/failed/<service>` (entries are symlinks, the directories are
  root 0755, readable by `chefui`; checked live on the installed pair
  2026-10-25). Existence only, with `fstatat(..., AT_SYMLINK_NOFOLLOW)` (or
  `lstat`), never follow or read the link. Not `faccessat` with
  `AT_SYMLINK_NOFOLLOW`: musl 1.2.6 on our 4.4 kernel returns `EINVAL` for it
  (no `faccessat2`; found by splash_review, 2026-10-25).
- **The modem**: `modem-up` publishes readiness only inside
  `/run/chef/lock/modem-up.lock/` (root:qmux 0750), and `chefui` must not get
  the `qmux` group (it would open the QMI socket). Add a public marker:
  `modem-up` creates `/run/chef/modem-ready` (root 0644, empty) right after its
  `touch "$LOCK/ready"`, and `cleanup()` removes it next to `rm -f "$LOCK/ready"`.
  Readers must treat it as information only, never as ownership (the lock
  directory stays the ownership contract). Document it in
  [runtime files](../features/runtime-files.md) and the modem-up header comment.
- **Watching**: inotify on `/run/openrc/started`, `/run/openrc/failed` and
  `/run/chef` (create, delete, moved) on the chefui loop through
  `chefui_watch_fd()`; on any event, rescan the eight paths above (cheap). A
  directory missing at start (a host run, a broken boot) is rescanned once a
  second until the splash ends; no other polling. Every watch and fd is closed
  when the splash ends.
- **Paths are injected**: the OpenRC root (default `/run/openrc`) and the run
  root (default `/run/chef`), so host tests use temporary directories.

### When the splash shows

- **Only once per boot**: when the app starts and
  `/run/chef/chef-settings/splash-done` does not exist. The app creates that
  file (0644) when the splash ends, whatever the reason. `/run` is fresh at
  every boot, so a later restart of `chef-settings` (crash, `kill -9`,
  `rc-service chef-settings restart`) goes straight to the hub, and a crash
  during the splash shows it again.
- **Screen-off boot**: with `/run/chef/screen/off` present at start the panel
  stays dark as today; the splash logic still runs (steps, logging, the done
  marker) without being seen, and a wake during it shows the splash in its
  current state.
- **Input during the splash**: touch is swallowed by the splash (nothing
  reaches the hub under it). A short Power press does what it does on the
  hub: screen off (and on again). Auto-sleep does not run while the splash
  shows; its timer starts when the splash ends.
- `--fake-state` runs and `--snapshot` captures never show the splash unless
  asked (below), so existing captures and tests are unchanged.

### Version line

`<version> · <commit, first 8 hex>` from `/etc/chef/image-info` (the parser
the About page already uses), with `+dirty` appended for a dirty build:
`dev · 7a13e594`, `dev · 7a13e594+dirty`. Without readable image info, the
line is empty (the title stays where it is).

### Host captures

- A fake-state key for the splash: `boot K` (K = 0..6 steps done; with
  `boot` present a fake run shows the splash frozen in that state, no
  sources, no timers ending it), plus the existing `about` keys for the
  version line. Fixtures `apps/settings/boot-k0.state`, `boot-k1.state`,
  `boot-k6.state` with `about version v0.9` and a commit starting `96e8c87f`,
  so captures match the references exactly.
- Compare with `tools/chefui/compare-ref.py`, masking only the notch
  (`0-80:262-818`). Expected: band residuals at the level of the round 1
  pages (antialiasing only).

## Milestones

**M1: marker and the step tracker (no LVGL).**
- `modem-up`: the `/run/chef/modem-ready` marker as above; its header comment;
  `scripts/tests/` coverage in whatever test already exercises modem-up's
  scripts (if none does, a static check in `test_services.py` that the touch
  and the rm sit next to their lock counterparts).
- `tools/chefui/sys/bootsteps.{c,h}`: the six steps, their done/failed
  rules, the "current step", "finished count", "all finished" and the
  30 s cap, driven by an injected clock and paths, with an inotify fd for the
  caller's loop and a `rescan()` entry. Pure C, no LVGL, like `gpsclient.c`.
- Host tests (`tools/chefui/tests/test_bootsteps.c`): every row of the table,
  out-of-order completion, a failure in each step, a missing directory then
  appearing, the cap, the marker appearing and disappearing.

**M2: the splash screen.**
- `tools/chefui/apps/settings/splash.{c,h}`: the B01 view on the full panel
  (above the status bar), fed by `bootsteps`; the failed-label 1.5 s hold, the
  `Ready` 500 ms hold, the once-per-boot marker, input and auto-sleep rules
  above; the version line; the font subsets.
- `main.c`: start the splash after `app_init()` when it should show; the
  fake-state `boot K` key and the three fixtures.
- Captures of the three states in both themes, compared with the references;
  the comparison output in the milestone note.
- Host tests: once-per-boot (marker present, absent), screen-off start, the
  holds and the cap with the injected clock, touch swallowed.

**M3: docs.**
- [System UI](../features/system-ui.md): a "Boot splash" section (what it shows,
  the steps and sources, once per boot, the log lines, captures).
- [Runtime files](../features/runtime-files.md): `/run/chef/modem-ready` and
  `/run/chef/chef-settings/splash-done`.
- [Services](../features/services.md): one line on the splash following the
  boot.

## Live check (coordinator, after M3)

One reboot of the installed pair onto a RAM image of the tree (user approved
2026-10-25), then: the splash appears with the build line, steps advance in
order, `Ready`, hub; `logread | grep 'boot splash'` gives the step times;
`rc-service chef-settings restart` shows no splash; `kill -9` of chef-settings
shows no splash; a screen-off boot is not tested live (host only). Install
only on the user's word.

## Result (2026-10-25)

Done. Commits on main, each approved by `splash_review` and checked against
its reviewed snapshot: `e43c37a` (M1, the `/run/chef/modem-ready` marker and
`sys/bootsteps`), `bbb8388` (M2, the splash in `chef-settings`), `70c2d88`
(T1, `test_settings_proc.sh` waits for the `ui-store` thread: its old wait
matched `chef-settings: start` and it had failed on every run since
`chef_log`, unseen because chefui's `host-test` target is not in the root
`make test`), `1070033` and `bb20b16` (M3, the guides). Decided during the
work: steps are sticky (first finish wins); the cap logs steps 1 to 5;
existence checks use `fstatat`/`lstat`; failed labels show in step order and
the bar follows the shown step; "Loading home" is never shown.

Live check on a RAM image of the tree (2026-10-25, nothing flashed): the
splash showed from `chef-settings`' start at 10.9 s to 20.9 s since boot,
waiting on the modem (ready at 16.45 s) and NetworkManager (20.37 s);
captures matched the design states; a restart and a `kill -9` went straight
to the hub. Not installed. Record: [log](../../logs/boot-splash-2026-10-25.txt).
