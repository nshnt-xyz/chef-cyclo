# Shutdown screen (B02)

User request 2026-10-25 ("just like we added a boot screen, lets add a
shutdown loader too"), right after the [boot splash](../archive/boot-splash-handoff.md)
(B01) was installed. Read that handoff first, then
[system UI](../features/system-ui.md) ("Boot splash"),
[services](../features/services.md) ("Shutdown, reboot and the return to
fastboot"), [runtime files](../features/runtime-files.md),
`tools/chefui/sys/bootsteps.{c,h}` and `tools/chefui/apps/settings/splash.{c,h}`.
This file adds the design, the decisions and the working rules.

Roles: `shutdown_impl` (Claude Sonnet) implements, `shutdown_review` (Claude
Opus) reviews, `shutdown_coord` coordinates, decides and runs the live check.
Report with `herdr agent prompt shutdown_coord "[From: shutdown_impl] ..."`
(or `shutdown_review`). Send a message when a milestone is ready for review,
when a review is done, when blocked, and at the end. Do not poll or wait on
each other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/shutdown-screen-20261025/`. Review diffs
go to `review/<milestone>.diff`; the reviewer snapshots what it reviewed
(sha256 in its note) and checks the commit against that snapshot, not
against the diff file (the implementer may refresh it in place).

## Working rules

- Work on `main` in this checkout. Milestones in the order below, each
  reviewed before the next. One commit per milestone after the reviewer's
  explicit approval. Do not push. Do not touch the `experiment/*` or `ui`
  branches. Keep this file out of your commits (the coordinator commits it
  at the end).
- No phone access. No `fastboot`, `phone.py`, `phone-boot.sh`, telnet or ssh
  to the phone: the coordinator runs the live check. Anything that can only
  be learned on the phone goes in your milestone note as a question for the
  live check.
- The implementer does not research or decide. Where this file and the code
  disagree, or this file does not settle something, stop and ask
  `shutdown_coord` with the exact question and the options you see; the
  coordinator answers with the fix. The reviewer checks the code against
  this file and reports deviations as findings, not as new designs.
- Match the surrounding code's style and comment density.
- `make test` at the repo root must pass at every milestone, and so must
  `make -C tools/chefui host-test` for the chefui milestones. Stress or flake
  checks: short, targeted, runtime stated first. No host test may power off
  or reboot the host, signal the host's init, write to the host's syslog,
  read the host's `/run/openrc` or `/run/chef`, or depend on the host's
  services: every path is injected.

## What happens today

`poweroff`, `reboot`, buttond's 3 s hold and powerd's low-battery power-off
all go through busybox init's `::shutdown` lines (`rootfs-overlay/etc/inittab`):
`chef-state shutdown` (up to 5 s), `chef-state log-save` (up to 3 s),
`openrc shutdown` (14.2 s live, about 11 s of it modem-up's stop: the kernel
waits 10 s for a modem ack that never comes), `chef-stop-supervisors`,
`chef-storage shutdown`, `umount`. `chef-reboot bootloader` runs the same
steps itself with init paused (`openrc shutdown` took 23.5 s there).

`chef-settings` starts right after syslog (`before dbus udhcpd modem-up
chronyd bt-up`, and modem-up is `after chef-settings`), so `openrc shutdown`
stops it after modem-up and before syslog: nearly last. Today the panel
shows the live hub (or whatever page is open) for the whole way down, its
status bar losing Wi-Fi and Bluetooth, and goes dark when chef-settings
exits, about 1 s before the power is cut. With the screen off it stays off.

## The design (B02)

The B01 layout exactly (same geometry, fonts, colours and theme rules: see
the B01 handoff, "The design"), with shutdown text:

- No status bar; the cover fills the panel in the theme's `bg`, above
  everything chef-settings draws (pages, pull-downs, the guided prompt
  overlay, the boot splash).
- Title block: `chef-cyclo` and the build line, exactly as on B01.
- Progress block: the current step's label with an ellipsis (U+2026) on the
  left, `k / 6` on the right, the bar below. Bar fill: the number of leading
  steps finished, out of 6, except that once step 6 is shown the bar is full.

No design references exist for B02. The captures are checked against the
B01 captures instead (below).

## Decisions (user, 2026-10-25, and coordinator)

### Steps and when each is finished

A service counts as stopped when `<openrc>/started/<svc>`,
`<openrc>/starting/<svc>`, `<openrc>/stopping/<svc>` and
`<openrc>/inactive/<svc>` are all absent (existence only, `fstatat` or
`lstat` without following, as in `bootsteps.c`; never `faccessat` with
`AT_SYMLINK_NOFOLLOW`). A service that failed at boot, or was never
started, is already stopped.

| # | Label (in progress) | Finished when |
|---|---|---|
| 1 | `Saving state` | `<openrc>/softlevel` reads `shutdown` (first line, trailing newline ignored): `openrc shutdown` has begun, so `chef-state shutdown` and `log-save` are over |
| 2 | `Stopping Wi-Fi` | `networkmanager` stopped |
| 3 | `Stopping Bluetooth` | `bt-up` and `bluetooth` stopped |
| 4 | `Stopping sensors` | `sensord` and `sensors-up` stopped |
| 5 | `Stopping GPS modem` | `modem-up` stopped. Not the `/run/chef/modem-ready` marker: modem-up removes it at the start of its cleanup, before the 10 s close |
| 6 | `Powering off` / `Restarting` / `Shutting down` (by the reason, below) | never: shown once 1 to 5 are finished, until chef-settings is stopped |

- Steps are sticky (first finish wins) and finish out of order freely
  (`chef-state shutdown` stops sensord before `openrc shutdown` begins); the
  text shows the first unfinished step and the count is its number, as on
  B01.
- There are no failed states and no cap: a stop that never finishes keeps
  its step on screen until chef-settings itself is stopped (by `openrc
  shutdown` or `chef-stop-supervisors`). No holds either: each finish moves
  the text at once.
- Logging: one `chef_log` line when the screen starts, with the time since
  boot (`CLOCK_BOOTTIME`) and the reason (`shutdown screen: shown at
  123.45 s (poweroff)`), one per finished step (`shutdown screen: Wi-Fi
  stopped at 125.02 s`, `... state saved at ...`, `... GPS modem stopped at
  ...`), and one when it ends (TERM, or the marker removed). syslogd outlives
  chef-settings, so these reach `logread` but not the ring saved on `/data`
  (`log-save` ran before them); that is accepted. Keep the wording stable.

### The marker and the reason

New public marker `/run/chef/shutting-down` (root 0644, one line: `poweroff`,
`reboot` or `unknown`), written atomically (temp file in `/run/chef`, then
`mv -f`), never rewritten once present (the first writer wins).

- `chef-state shutdown [poweroff|reboot]` writes it as its very first act,
  before `ours` and before anything that can block, so the screen appears
  at once and also on a RAM-only boot. Without an argument the reason is
  `poweroff` when `/run/chef/power/shutdown-pending` (powerd, written just
  before its power-off) or `/run/chef/buttond/poweroff` (new, below) exists,
  else `unknown`. Paths injectable like the script's others. The inittab
  line stays as it is (no argument); the `::restart` path gets `unknown`
  unless a marker exists.
- buttond: right before its power-off action (the injected `s->poweroff`,
  so the hint is host-testable), creates `<its run dir>/poweroff` (0644,
  empty; default `/run/chef/buttond/poweroff`, injectable). A failure to
  create it is logged and never stops the power-off.
- `chef-reboot` calls `chef-state shutdown reboot`. On every path where it
  resumes init without rebooting (its exit trap, INT/TERM, the watchdog's
  `kill -CONT 1` cannot, accepted), it removes the marker, so a system that
  stays up leaves the shutdown screen.
- Step 6's label: `poweroff` gives `Powering off`, `reboot` gives
  `Restarting`, anything else (or an unreadable file) `Shutting down`.
- Document the marker and `buttond/poweroff` in
  [runtime files](../features/runtime-files.md).

### chef-settings

- Watches `/run/chef` (injected run root) for the marker for its whole life:
  one inotify fd on the chefui loop (`IN_CREATE`, `IN_MOVED_TO`,
  `IN_DELETE`, `IN_MOVED_FROM`), plus the check at start (a chef-settings
  that starts with the marker present shows the screen at once). While the
  screen shows it also watches `<openrc>` (for `softlevel`: `IN_CLOSE_WRITE`,
  `IN_MOVED_TO`, `IN_CREATE`) and the four state directories; any event
  rescans (cheap). A missing directory is retried once a second while the
  screen shows, as in `bootsteps`.
- **Lights the panel**: if the screen is off when the screen starts (Power
  held in the dark, powerd's low-battery power-off), it turns the screen on
  (the app's normal screen-on path: flag removed, UNBLANK, brightness).
- Input while it shows: touch swallowed; Power short ignored (the screen
  stays lit; buttond's own hold-to-power-off is untouched); auto-sleep does
  not run.
- During the boot splash: the shutdown screen replaces it. The splash ends
  at once (no Ready hold, its `ended` hook and `splash-done` marker as on
  any end), logged with one extra line `boot splash: ended by shutdown at
  12.34 s`.
- Marker removed while it shows (a `chef-reboot` that gave up): the screen
  goes and the hub shows, logged. The system is then half stopped; that is
  `chef-reboot`'s failure, not the UI's.
- TERM while it shows: the app exits as today (clean exit, panel powered
  down). No delay of the exit.

### Code layout (coordinator)

- **No duplicate code anywhere (user, 2026-10-25: "do not have duplicate
  code").** This replaces the earlier "two small trackers" call. Nothing
  `bootsteps.c` already does is written a second time: the inotify setup,
  watch list and drain, the missing-directory retry and its deadline, the
  existence check (`fstatat`/`lstat` without following), the
  `CLOCK_BOOTTIME` seconds for the log lines, the sticky step states and
  the finished/current counting. That machinery moves into a shared,
  LVGL-free module (`tools/chefui/sys/steptrack.{c,h}`: a step table of
  rules plus the watch dirs, driven by the caller's loop), and
  `bootsteps.c` becomes its boot step table and rules. `struct bootsteps`
  embeds a named `struct steptrack t`, so its users read `b.t.state[]`
  and so on (no `-fms-extensions`). The proof that the move changed
  nothing: every `bootsteps_*` signature unchanged, the
  `test_bootsteps.c` diff only those field-path renames, and the
  boot-k captures byte-identical. The same rule applies to the shell side and the tests: a
  helper used twice is shared, not copied. The reviewer treats any
  duplicated logic as a finding, in every milestone from M2 on.
- `tools/chefui/sys/shutsteps.{c,h}`: the shutdown step table and rules on
  `steptrack` (the four state directories, `softlevel`, no cap), and the
  marker reader (reason parse). Thin.
- **Reuse the splash, do not rebuild it (user, 2026-10-25: "make sure the
  screen is mostly just reused from the start splash").** Everything visual
  and every piece of view plumbing the splash already has is shared, not
  copied: the full-panel cover object and its layering, the title and
  build line (including the `/etc/chef/image-info` read and its format),
  the step text, count and bar widgets and their update function (label,
  `k / N`, fill), the theme handling, and the touch-swallowing. The only
  differences are the inputs: which tracker feeds it, the labels, and the
  start and end rules. If a piece of `splash.c` would be duplicated in
  `shutdown.c`, it moves into `cover.c` instead. The reviewer treats any
  duplicated drawing, layout constant or image-info code as a finding.
- `tools/chefui/apps/settings/cover.{c,h}`: that shared code, moved out of
  `splash.c` with no visual change, so `splash.c` and the new `shutdown.c`
  both drive it. The boot-k0/k1/k6 captures must stay byte-identical before
  and after the move.
- `tools/chefui/apps/settings/shutdown.{c,h}`: thin: the marker watch, the
  shutsteps wiring, the reason label, and the input and screen rules above,
  all drawing through `cover.c`; `app.c` asks `shutdown_active()` where it
  asks `splash_active()`.
- Fonts: extend the generated subsets (`kit/fonts/gen-fonts.sh`) with the
  new labels' letters (48 px Barlow 500: `Saving state`, `Stopping Wi-Fi`,
  `Stopping Bluetooth`, `Stopping sensors`, `Stopping GPS modem`,
  `Powering off`, `Restarting`, `Shutting down`, U+2026). No new sizes.

### Host captures

- Fake-state key `shutdown K REASON` (K = 0..5 steps finished, or 6 for step
  6 shown; REASON `poweroff`, `reboot` or `unknown`): a fake run shows the
  screen frozen in that state, no sources, no watches. It wins over `boot`.
- Fixtures in `apps/settings/` with the same `about` keys as
  `boot-k*.state`: `shutdown-k0.state` (`Saving state… 1 / 6`, poweroff),
  `shutdown-k4.state` (`Stopping GPS modem… 5 / 6`, poweroff),
  `shutdown-k6-reboot.state` (`Restarting… 6 / 6`, bar full).
- Captures of the three in both themes. Check with
  `tools/chefui/compare-ref.py` against the boot captures of the same fill
  (shutdown-k0 vs boot-k0 and so on, masking the step text row): the title
  block, the count position and the bar must match to the pixel. Put the
  captures and the comparison output in the milestone note.

## Milestones

**M1: the marker and its writers (no chefui).**
- `chef-state shutdown [poweroff|reboot]`: the marker as above; usage text
  and header comment; `tools/tests/test_chef_state.py` cases (written
  first even without `/data`, each reason source, an existing marker kept,
  atomic write, the reason argument).
- buttond's `poweroff` hint with its test in `test_buttond.c` (created before
  the injected power-off runs; a failure logged, power-off still runs).
- `chef-reboot`: `chef-state shutdown reboot`, marker removed on the
  resume paths; `test_chef_reboot.py` cases.
- `docs/features/runtime-files.md` rows.

**M2: the tracker (`sys/shutsteps`).** Host tests
(`tests/test_shutsteps.c`): every row of the table, the four state
directories, a service in `stopping/` not yet stopped, out-of-order
finishes, a never-started service, `softlevel` written in place and by
rename, a missing directory then appearing, each reason including a missing
and a garbage marker.

**M3: the screen.** `cover.{c,h}` move (boot captures byte-identical, show
the hashes), `shutdown.{c,h}`, `main.c` wiring, fake-state key and
fixtures, fonts. Host tests (`tests/test_shutdown.c`): shown at start with
the marker, shown on the marker's arrival, screen lit from off, touch
swallowed, Power short ignored, auto-sleep paused, replaces a running
splash (its log line and `splash-done`), the marker's removal back to the
hub, step text and counts through all six, the bar full at step 6.
Captures and comparison as above.

**M4: docs.** [System UI](../features/system-ui.md): a "Shutdown screen"
section after "Boot splash" (what it shows, steps and sources, the marker
and reasons, the log lines, captures). [Services](../features/services.md):
one line in "Shutdown, reboot and the return to fastboot".
[Buttons](../features/buttons-and-power-off.md): the hint file.

## Live check (coordinator, after M4)

On a RAM image of the tree, with the user watching the panel: a buttond
hold (`Powering off`, the steps advancing, the modem step about 11 s, dark
about 1 s before power-off), a `chef-reboot bootloader` (`Restarting`), a
hand `poweroff` with the screen off (lit, `Shutting down`), the log lines
through `logread -f` over telnet. Install only on the user's word.

## Result (2026-10-30)

Done. Commits on main, each approved by `shutdown_review` and checked against
its reviewed snapshot (hashes after the rebase onto `dc9ef84`): `4f96c1c` (M1,
the marker and its writers), `07f378b` (M1b, the marker through `put()`),
`07b1847` (M2a, `sys/steptrack` with `bootsteps` and `shutsteps` on it),
`0065e19` (M2b, shared test helpers), `e6c0819` (M3, `cover.c`, `shutdown.c`,
`sys/dirwatch`), `91f233a` (M3b, `prompt.c` onto `dirwatch`), `95f0fa6` (M4,
the guides). Changed during the work, from the user's "do not have duplicate
code": the separate tracker became `sys/steptrack`, the watch `sys/dirwatch`
(also used by the prompt card), and the marker write goes through `put()`.

Live check on a RAM image of the tree (2026-10-30, the user away; panel read
from streamed snapshots): `chef-reboot bootloader` showed the screen over a
prompt card with the reason `reboot`; a `poweroff` with the screen off lit the
panel 50 ms after the marker (`unknown`); with buttond's hint the reason was
`poweroff`; every run logged `state saved` at the start of `openrc shutdown`.
Found: step 6 is never seen (chef-settings is stopped within 0.3 s of the
modem's stop, so the panel goes from "Stopping GPS modem" to black), and
`logread -f` dies on a power-off while syslogd stays up
([next steps](../next-steps/power-and-reliability.md#crash-recovery)).
Record: [log](../../logs/shutdown-screen-2026-10-30.txt).
