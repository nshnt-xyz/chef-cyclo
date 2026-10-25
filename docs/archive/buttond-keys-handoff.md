# buttond key lines (press, hold, release)

User request 2026-10-25 ("go ahead with buttond handoff"), the first backend
piece under the ride screens (order in
[UI and ride app](../next-steps/ui-and-ride-app.md#ride-application-design), decision 1 of
2026-10-25: Hold Power to finish). Read
[buttons and power-off](../features/buttons-and-power-off.md),
[local socket protocol](../features/local-sockets.md),
[UI platform](../features/ui-platform.md), `tools/buttond/buttond.c` (its
header is the contract), `tools/chefui/buttons.{c,h}` and the glue in
`tools/chefui/chefui.c` first; this file adds the design, the decisions and
the working rules.

Roles: `keys_impl` (Claude Sonnet) implements, `keys_review` (Claude Opus)
reviews, `keys_coord` coordinates, decides and runs the live check. Report
with `herdr agent prompt keys_coord "[From: keys_impl] ..."` (or
`keys_review`). Send a message when a milestone is ready for review, when a
review is done, when blocked, and at the end. Do not poll or wait on each
other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/buttond-keys-20261025/`. Review diffs go
to `review/<milestone>.diff`; the reviewer snapshots what it reviewed (sha256
in its note) and checks the commit against that snapshot.

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
  `keys_coord` with the exact question and the options you see; the
  coordinator answers with the fix. The reviewer checks the code against this
  file and reports deviations as findings, not as new designs.
- Match the surrounding code's style and comment density.
- `make test` at the repo root must pass at every milestone (it runs
  `tools/buttond`, `tools/chefui` and `tools/tests/test_fblog.c`, which also
  includes `buttond.c`). Stress or flake checks: short, targeted, runtime
  stated first. No host test may write to the host's syslog or depend on the
  host's services: every path is injected, as the existing tests do.

## Why

Today a claimant only gets completed gestures: `power.long` fires at the
1.5 s threshold and its release emits nothing, and the step 9 passthrough on
`chef-buttons` passes no Power press while any Power gesture is claimed. So
no client can draw a ring that starts on the press, fills while the key is
held and cancels on an early release. The ride's "Hold Power to finish"
needs exactly that: the ride recorder claims `power.long` (which suppresses
the 3 s software power-off) and finishes the ride on it; the UI, which
claims `power.short`, draws the ring.

## The protocol addition

A new event type `key`, three states, for all three buttons:

```json
{"type":"key","button":"power","state":"down"}
{"type":"key","button":"power","state":"held"}
{"type":"key","button":"power","state":"up"}
```

- `button` is `power`, `volup` or `voldown` (`button_names`).
- `down`: the recognizer accepted a press (`recog_press` set `down`; a
  duplicate press it ignores sends nothing).
- `held`: the hold reached `long_ms`, at the very moment the recognizer
  fires that button's `.long`, whether or not anyone claims that gesture.
  Not sent when the press was consumed by a chord (no `.long` fires then).
- `up`: the recognizer accepted a release (`recog_release` found the key
  down). A release of a key held since before buttond started sends nothing,
  as the recognizer ignores it.
- Autorepeats (value 2) send nothing.

Coordinator decision (2026-10-25, refining decision 1): `held` is added so
that a client that does not claim `.long` (the UI; the recorder claims
`power.long`) still completes its ring exactly when buttond's `power.long`
fires, without its own timer and without claiming `power.long`, which would
suppress the power-off default when no ride runs.

**Who gets them**: at the moment a line is sent, every connection that claims
any gesture using that button (`button_gestures(b)`: its short, double and
long and each chord it is part of; the same mask the passthrough uses) and
every watcher. A connection that claims only `volup.*` gets no `power` key
lines. Claims can change mid-hold, so a client may see an `up` or `held`
without the `down`; clients ignore that.

**Must-deliver**, like every buttond line: a client whose queue does not take
one is dropped, claims and all (`linesrv_send` failing, as in
`server_dispatch`).

**Ordering**, per connection: lines follow the events. A key line comes
after every gesture fired before its event and before every gesture its own
event fires; `held` comes right before its `.long` gesture line.

| Input | Lines to a watcher |
|---|---|
| Power tap, `.double` not armed | `down`, `up`, gesture `power.short` |
| Power tap, `.double` armed | `down`, `up`, then gesture `power.short` `double_ms` later |
| Power double tap (armed) | `down`, `up`, `down`, `up`, gesture `power.double` |
| Power held 2 s | `down`, `held`, gesture `power.long`, then `up` at the release |
| Power held past `long_ms`, release read before the loop's tick ran | `down`, `held`, gesture `power.long`, `up` |
| VolUp held, then Power pressed (chord) | `volup` `down`, `power` `down`, gesture `power+volup`, then each `up` |

Implement it with `server_key(s, b, value, now)`, called by the main loop for
each evdev key event in place of the bare `recog_key`:

1. `recog_tick(now)`, then `server_drain(now)`: every deadline that passed
   before this event fires first. This also covers the late `.long`: the
   tick fires it (with its `held`) before the release is fed, so
   `recog_release`'s late path is not reached from `server_key` (keep that
   path; the tests that call `recog_key` directly still use it).
2. Note `recog.b[b].down`, call `recog_key`; if a press was accepted (down
   went false to true) send `down`, if a release was accepted (true to
   false) send `up`, to the audience of `b`.
3. `server_drain(now)`: the gestures this event fired.

`held` is sent in `server_dispatch`, when the gesture is a `.long`, to the
audience of that button, immediately before the gesture line goes to
anyone (a connection both claiming `power.long` and in the audience gets
`held` and then the gesture). The main loop's own `recog_tick`,
`server_drain`, `server_tick`, `ff_tick` after the reads stay as they are.

Step 1 changes behaviour in two edges, both within tick latency and both
now matching what an on-time tick does: a press arriving after an armed
double window expired, but before the loop's own tick ran, used to count as
a second tap; now the pending `.short` fires first and the press starts a
new sequence. And a chord partner pressed after the first key passed
`long_ms` but before the tick: that key's `.long` (with `held`) fires first
and the partner is a plain press, no chord (with Power first and
`power.long` unclaimed, that arms the power-off as any on-time hold does).
Test both edges (L1 of the M1 review, decided 2026-10-25).

The order of claimant delivery and the default action stays as in
`server_dispatch` today.

**Proto** stays 1 (a new type; clients ignore unknown types, see
[local sockets](../features/local-sockets.md#replies-and-events-the-envelope)).
`status` is unchanged. `buttond watch` and `buttond claim` print key lines
like any other line (no change needed, check it).

## The chefui client

`tools/chefui/buttons.{c,h}`: parse `key` lines into a second callback,
`cu_btn_key_cb`:

```c
enum cu_btn_key_state { CU_BTN_KEY_DOWN, CU_BTN_KEY_HELD, CU_BTN_KEY_UP };
typedef void (*cu_btn_key_cb)(const char *button, enum cu_btn_key_state state, void *user);
```

with `key_cb`/`key_user` members beside `app_cb`. An unknown `state` or a
missing `button` is ignored (no log). No default action: with no callback
set the lines are dropped. The header comment lists the new type.

`tools/chefui/chefui.{c,h}`: `chefui_on_key(chefui_key_cb cb, void *user)`
mirroring `chefui_on_button`, with the enum exposed as
`enum chefui_key_state { CHEFUI_KEY_DOWN, CHEFUI_KEY_HELD, CHEFUI_KEY_UP }`
(same values; a static assert or an explicit mapping, your choice, no
casts between unrelated enums without one). The SDL backend sends no key
events (it synthesizes gestures only; say so in its header comment line for
buttons). No application uses `chefui_on_key` yet: `chef-settings` behaves
exactly as before.

## Milestones

**M1: buttond.**
- `server_key`, the `held` line in `server_dispatch`, the audience rule;
  the main loop calls `server_key`.
- `buttond.c` header: the new event in the protocol list and a paragraph on
  the order and the audience.
- `tools/buttond/tests/test_buttond.c`: every row of the ordering table; the
  audience (claimant of `power.short` gets power lines, claimant of only
  `volup.long` does not, a claimant of `power+volup` gets both buttons', a
  watcher gets all); `up` without `down` for a key held at startup sends
  nothing; repeats send nothing; a claim made mid-hold yields `held`/`up`
  only; a client whose queue overflows on a key line is dropped and its
  claims released; the double-window edge above; the power-off state
  machine unchanged with key lines on (arm, cancel, ready).
- [Buttons and power-off](../features/buttons-and-power-off.md#socket-contract):
  the event, the audience, the ordering table, a line under `watch`.

**M2: chefui client.**
- `buttons.{c,h}`, `chefui.{c,h}` as above; the SDL header line.
- `tools/chefui/tests/test_buttons.c`: each state reaches the callback with
  its button; unknown state, missing button and no callback are ignored;
  key lines split across reads; gestures still dispatch as before when key
  lines are interleaved.
- [UI platform](../features/ui-platform.md#display-touch-buttons-and-lifecycle):
  `chefui_on_key` next to the button paragraph.

## Live check (coordinator, after M2)

On a RAM image of the tree (one reboot of the installed pair, which needs the
user's word at the time), with the steps on the on-screen prompt card:
`buttond watch` over ssh while the user does a Power tap (screen toggles as
before), a Power hold of about 2 s released before the second buzz (`down`,
`held`, `power.long`, `up`, and "power off cancelled" in `logread`), a VolUp
tap, and VolUp then Power (the chord). `buttond status` unchanged. Install
only on the user's word.

## Result (2026-10-25)

Done. Commits on main, each approved by `keys_review` and checked against
its reviewed snapshot: `3f91edd` (M1, buttond's `key` lines, `server_key`,
the guide's socket contract) and `aff3570` (M2, `chefui_on_key` and the
client parsing, the UI platform guide). Decided during the work: both tick
edges accepted and tested (L1 of the M1 review); an empty `button` is
ignored like a missing one.

Live check on a RAM image of the tree (2026-10-25, nothing flashed), with
keys injected through a throwaway uinput device into a second buttond on a
test socket: every line, order and audience as in the table above (watcher,
`power.short` claimant, `volup.long` claimant); the installed buttond and
`chef-settings` unaffected. Then a guided check with the user on the
physical buttons (VolUp tap, Power hold to the first buzz, the VolUp and
Power chord, two Power taps toggling the screen): all lines as specified,
the power-off cancelled on the early release, `chef-settings` still
toggling the screen. Installed the same day as `d965e201` + `d0109716`. Record:
[log](../../logs/buttond-keys-2026-10-25.txt).
