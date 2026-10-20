# sensord and blesensd on the shared library (standard interfaces step 4)

User request 2026-10-19 ("Next is step 4: moving sensord and blesensd
onto the shared library"). The scope is step 4 of the
[standard interfaces](../next-steps/standard-interfaces.md#order) order: section 2 of
that plan for `sensord` and `blesensd`, their written contracts, and the
`sensord.c` and `blesensd.c` splits. Read the plan's section 2, the
[local socket protocol](../features/local-sockets.md), the
[buttond handoff](buttond-handoff.md) and the
[gps-clients handoff](gps-clients-handoff.md) (steps 2 and 3,
the pattern this step repeats) first; this file adds the decisions and
the working rules.

Roles: `sensors_impl` (Claude Opus) implements, `sensors_review`
(Claude Opus) reviews, `sensors_coord` coordinates and decides, and runs
the live check. Report with
`herdr agent prompt sensors_coord "[From: sensors_impl] ..."` (or
`sensors_review`). Send a message when a milestone is ready for review,
when a review is done, when blocked, and at the end. Do not poll or wait
on each other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/sensors-blesensd-20261019/`
(create it). Review diffs go to `review/<milestone>.diff` there; the
reviewer snapshots what it reviewed (sha256 in its note) and checks the
commit against that snapshot.

## Working rules

- Work on `main` in this checkout. Milestones in the order below, each
  reviewed before the next. One commit per milestone after the
  reviewer's explicit approval. Do not push. Do not touch the
  `experiment/*` or `ui` branches. Keep this file and any edit of
  `standard-interfaces.md` out of your commits (the coordinator commits
  them at the end).
- No phone access. No `fastboot`, `phone.py`, `phone-boot.sh`, telnet or
  ssh to the phone: the coordinator runs the live check.
- Match the surrounding code's style and comment density (kernel style,
  tabs, the file-header comment that documents the protocol).
- `make test` at the repo root must pass at every milestone. Stress or
  flake checks: short, targeted at the step, runtime stated first.
- Ask `sensors_coord` before deciding anything this file and the plan do
  not settle.

## Today (what changes)

**sensord** (`tools/sensord/sensord.c`, 3445 lines) serves
`/run/sensord.sock` with its own client table (16), line reassembly
(128-byte lines), a 16384-byte output queue per client with
must-deliver replies (`client_reply`, overflow drops the client) and
lossy samples (`client_sample`, `{"dropped":N}`), `sock_listen`,
`sock_connect` and `kmsg_note`. Its lines are JSON without the envelope:
`{"hello":"sensord","proto":1}`, `{"ok":"claim",...}`,
`{"ok":"status","proto":1,...}`, `{"err":"...",...}`, typeless samples
`{"sensor":"accel","t":...}`. The 17th connection is closed after a log
line. Sections are already separated by comment rules: channels (252),
clients and protocol (407), timestamps (1219), SMGR client (1274), SAM
clients (1954), REG2/TIME2 servers (2523), then the process shell and
`client_main`. Tests: `tests/test_sensord.c` includes the file with
`SENSORD_NO_MAIN`; `tests/test_e2e.sh` and `tests/test_compass_e2e.sh`
run the real daemon on the fake transport.

Users of sensord's lines: `tools/chefui/sys/alsclient.c` (on lineclient
and the JSON reader since step 3, still the pre-envelope shapes, see its
header) and `tools/chefui/tests/test_alsclient.c`; the CLI
(`sensord list|status|get|watch|declination`); the scripts
`rootfs-overlay/usr/bin/sensors-compass-run` and `sensors-magcal-run`
(grep `"smgr":"ready"`, `"calibrated":true`, `"err"`, a `sed` over the
qmag status members); `tools/compass-check.py` and
`tools/mag-cal-check.py` (filter sample lines on `sensor` and `t`, other
lines skipped) and their tests; `tools/sensord/compass.c`'s
`compass_format` (the heading line) and `compass-replay.c`.

**blesensd** (`tools/blesensd/blesensd.c`, 2963 lines) already sends
the `type` envelope but no hello, writes each line with unbuffered
`send(MSG_DONTWAIT)` (any short write or `EAGAIN` drops the client),
has its own `sock_listen` (chmod 0600), 16 clients (the 17th closed
silently), 256-byte request lines (an overlong one discarded silently),
`notef` (stderr and kmsg), `json_str`, and a CLI that parses lines with
`strstr` (`has_kv`). Its sections are separated by comment rules
(helpers, state, clients, bus calls, object cache, values, connections,
scanning, pairing, forget, records, signals, the agent, bus life,
requests, main loop, client CLI). `blesens.c` is its pure measurement
code. Tests: `tests/test_blesens.c` and `tests/test_blesensd.py` (the
real binary against `tests/fake_bluez.py`). Users of its lines:
`tools/chefui/sys/blesclient.c` (on the JSON reader since step 3,
ignores unknown types), the CLI, `test_blesensd.py`.

## Decisions

### sensord's protocol, proto 2

The shared envelope. `proto` goes to 2 because every line changes shape.
Requests stay plain words; limits as today: 16 clients, 128-byte request
lines (127 before the newline), 16384-byte output queue. Samples (and
heading and rotvec lines) are lossy (`linesrv_send_lossy`), everything
else must-deliver.

| Request | Reply |
|---|---|
| (connect) | `{"type":"hello","daemon":"sensord","proto":2}` |
| `list` | `{"type":"list","smgr":"ready","sensors":[...]}` (array members as today) |
| `status` | `{"type":"status","smgr":"ready",...}` (today's members, minus `proto`: the hello carries it) |
| `claim SENSOR [HZ]` | `{"type":"ok","cmd":"claim","sensor":"accel","rate":50}`, then samples |
| `release SENSOR` | `{"type":"ok","cmd":"release","sensor":"accel"}` |
| `get SENSOR [HZ]` | one sample line, or `{"type":"error","cmd":"get","reason":"timeout","sensor":"accel"}` |
| `declination [DEG\|off]` | `{"type":"ok","cmd":"declination","declination":1.50}` (`null` when off) |
| a sample | `{"type":"sample","sensor":"accel","t":NS,"x":..,"y":..,"z":..}`; illuminance, proximity, heading (`compass_format`) and rotvec lines the same way: `"type":"sample"` first, then today's members in today's order |
| unknown sensor | `{"type":"error","cmd":"claim","reason":"unknown sensor","sensor":"<as sent, escaped>"}` (same for `get`, `release`; `magn_factory` stays unknown) |
| other refusals | `reason` `sensor not present`, `bad rate`, `not enabled (sensord -V)` (rotvec), `bad declination`, each with `cmd` and, where today, `sensor` |
| missing or extra arguments | `{"type":"error","cmd":"<word>","reason":"bad arguments"}` (new: `claim` with no sensor answered "unknown sensor" and extra words were ignored): `list` and `status` take none, `claim` and `get` 1 or 2, `release` 1, `declination` 0 or 1 |
| unknown command | `{"type":"error","cmd":"<as sent, escaped, at most 32 bytes>","reason":"unknown command"}` (the old `command` member goes) |
| the DSP refuses a report later | `{"type":"error","reason":"smgr refused report","sensor":"accel"}` to the claimants, no `cmd` (it answers no request); a heading claimant gets `"sensor":"heading","input":"magn"` as today |
| dropped samples | the library's `{"type":"dropped","count":N}` |
| line over 127 bytes, 17th connection | the library's `line too long` / `too many clients` errors |

Everything else does not change: what a claim powers, rates and
decimation, get-only claims, the get timeout, the heading and rotvec
channels, the QMAG fields riding on magn lines, the registry write-back.

`local-sockets.md` says `cmd` is absent "when there is no request word";
extend that sentence with "or when the error answers no request
(sensord's `smgr refused report`)".

The CLI keeps its commands, options (`-S`, `-n`) and exit codes: it never
prints the hello; `watch` does not print the `ok claim` reply; other
lines are printed raw; exit 1 on an `error` line or no connection, 64 on
usage errors; `-n` counts `sample` lines. Parse with the JSON reader
(`type`), no `strncmp` on line prefixes.

### blesensd's protocol, proto 1

No shape change beyond the envelope's rules; requests, replies, events
and their members stay as in the file header.

- `{"type":"hello","daemon":"blesensd","proto":1}` first on every
  connection (new).
- Unknown command: `{"type":"error","cmd":"<as sent, escaped, at most 32
  bytes>","reason":"unknown command"}` (was `unknown request`).
- A known command with a missing, extra or invalid argument (`scan`
  without `on|off`, `pair` with a bad address, `status foo`): `{"type":
  "error","cmd":"<word>","reason":"bad arguments"}` (was `unknown
  request`).
- 16 clients, 256-byte request lines (255 before the newline), output
  queue 16384 bytes per client, every line must-deliver (no lossy lines:
  a lost `value` null or `sensor` state would leave a client showing a
  stale state). New behavior from the library: the overlong line and the
  17th connection get their error lines; a client is dropped only when
  its queue overflows, not on the first `EAGAIN`.
- The socket stays 0600: keep the `chmod` after `linesrv_listen()`
  (step 8, daemon users, revisits it).
- The CLI (`status|list|watch|scan|pair|cancel|forget`) keeps its
  commands, end conditions and exit codes, never prints the hello, and
  parses with the JSON reader instead of `has_kv`/`strstr`.

`blesclient.c` and `alsclient.c` log once when the hello's `proto` is not
the one they know and carry on (the compatibility rule); `alsclient.c`
moves to the proto 2 shapes (hello by `type`, `ok`/`error` by `type` and
`cmd`, `status` by `type`, samples by `type` `sample`), with every rule in
its header kept (stale cached sample, warm-up, backoff to 30 s, retry on
an `error` line, logged once per distinct `reason`).

### Logging

`chef_log` (tag `sensord` / `blesensd`) replaces `kmsg_note` and
`notef`, with stderr echo always on (both write stderr on every kmsg
line today; `sensors-magcal-run` keeps its helpers' stderr and
`test_blesensd.py` keeps the daemon's). sensord's `stderr_note`, `vlog`
and the injected `s->log`/`s->elog` stay (the default `s->log` becomes
`chef_log`). Raise `chef_log`'s cut from 255 to 320 bytes (sensord's
old buffer; some of its lines are long) in `tools/common/chef_log.c`,
its header and its test. Same message texts.

### The splits

Both daemons keep their directories. Split along the existing comment
sections, no behavior change; an internal header holds the shared types
(not installed, not for other programs); functions shared between files
lose `static` and get a short prefix of the implementer's choice, the
same within a daemon and different from the existing `bs_` (blesens.c),
`sd_` (sd-bus) and `compass_` ones. The big file-header comment stays
in one place (the internal header or `main.c`); the protocol block goes
to the server file.

sensord, for example:

| File | Holds |
|---|---|
| `sensord.h` | `struct server`, `struct client`, `struct channel`, `struct sam`, `struct heading`, the constants |
| `channel.c` | channel table, names, sample conversion and formatting, timestamps |
| `server.c` | clients and the protocol (list/status lines, requests, deliver, publish, the get ticks) |
| `heading.c` | the compass glue (feed, emit, calibrated flag, heading sync) |
| `smgr.c` | the SMGR client |
| `sam.c` | the SAM clients (QMAG_CAL, rotation vector) |
| `regserv.c` | the REG2/TIME2 servers and the write-back |
| `main.c` | options, the poll loop, exit, `main` |
| `cli.c` | `client_main` |

blesensd, for example: `blesensd.h` (the state `D`, its types),
`server.c` (clients, requests, the event lines), `bus.c` (bus calls,
object cache, signals, bus life, the agent), `link.c` (values,
connections, scanning), `pair.c` (pairing and forget), `records.c` (the
chef-state children), `main.c`, `cli.c`. The implementer may group
differently if a cut falls badly; say so in the review note.

Tests link the module sources instead of including one file
(`test_sensord.c` loses `SENSORD_NO_MAIN`); keep and extend the
ASan/UBSan runs (sensord has them; add one for blesensd's C test if it
gains one, and run `test_blesensd.py` against an ASan build only if it
stays quick). Device binaries' sizes within a few percent of before the
split (state both, before and after).

## M1. Split `sensord.c`

The table above, no behavior change: the same tests pass with only their
include/link lines and paths changed (the diff of the test files in the
review note shows nothing else). Acceptance: `make test` passes; device
binary size stated before and after; the staged image's file list
unchanged.

## M2. sensord on the library, proto 2, contract

One commit, because the daemon and its clients must agree in every image:

- `server.c` on linesrv (hello in `on_connect`; claims released and
  `smgr_sync` in `on_close`; the deliver and notify loops safe against a
  drop mid-loop; samples through `linesrv_send_lossy`), `main.c` polls
  through `linesrv_pollfds()`/`linesrv_handle()` (keep the "another
  sensord is serving" check before `linesrv_listen()` unlinks the path),
  `linesrv_connect` in the CLI, `json_escape` from `tools/common` (the
  local copy goes), `chef_log` and its 320-byte cut.
- The protocol above; the file header's protocol block rewritten;
  `compass_format` emits `"type":"sample"` (and `compass-replay` with
  it).
- `alsclient.c` on proto 2, `test_alsclient.c` with the new lines.
- Scripts: `sensors-compass-run`, `sensors-magcal-run` (`"err"` greps
  become `"type":"error"`; check the qmag `sed`); the Python tools and
  their fixtures (`test_mag_cal_check.py` writes old hello and dropped
  lines: write the new ones).
- Tests: `test_sensord.c` (every reply in the envelope, hello first, bad
  arguments, unknown command, too many clients, a client whose replies
  overflow is dropped and its claims released, samples to a non-reader
  counted and `{"type":"dropped","count":N}` before the next one),
  `test_e2e.sh`, `test_compass_e2e.sh` (grep fixed members, as the suite
  does).
- Docs: the written contract in `docs/features/sensors.md` (rename its
  "Socket protocol" section to "Socket contract", like the buttons
  guide's: requests, replies, events, errors, claim lifetime, limits,
  `proto`, CLI output); `local-sockets.md` (table row, the `cmd`
  sentence); `inittab` comment if it quotes lines; grep the other docs
  outside `docs/archive`, `docs/research` and `logs` for old lines
  (`{"ok":`, `{"err":`, `{"hello":`, `{"dropped":`).

Acceptance: `make test` passes; no pre-envelope sensord line left in
code, scripts or current docs (grep in the review note).

## M3. Split `blesensd.c`

As M1, for blesensd. Acceptance: `make test` passes; device binary size
before and after; staged file list unchanged.

## M4. blesensd on the library, proto 1, contract

- `server.c` on linesrv (hello in `on_connect`; scan and hold dropped
  and synced in `on_close`, as `client_drop` does today; `broadcast`
  safe against a drop mid-loop), `main.c` polls through the library,
  `linesrv_connect` and the JSON reader in the CLI, `json_escape` from
  `tools/common` (the local `json_str` goes; check its output is the
  same for the names BlueZ gives, or say what differs), `chef_log`.
- The protocol changes above; the file header updated.
- `blesclient.c`: the proto check; nothing else should need to change
  (it ignores unknown types).
- Tests: `test_blesensd.py` (hello first, unknown command, bad
  arguments, line too long, too many clients, a watcher that does not
  read is dropped only when its queue overflows), the chefui BLE tests
  unchanged in what they check.
- Docs: a "Socket contract" part under "BLE sensors" in
  `docs/features/bluetooth.md` (the requests, replies, events, errors,
  per-connection scan and hold, limits, `proto`, CLI); `local-sockets.md`
  table row (both daemons on the protocol), `scripts/ble-sensor-emu.py`
  docstring if it quotes lines.

Acceptance: `make test` passes; no `strstr` JSON parsing left in
`tools/blesensd/` or `tools/sensord/` (grep in the review note).

## End

`sensors_impl` builds the final RAM image from a clean tree at the last
commit (`docs/building.md`) and reports its path, its sha256 and the
staged file list diff against the installed tree (expected to differ:
sensord, blesensd, chef-settings, chefui-demo, image-info, the two
sensors-*-run scripts) to `sensors_coord`. The coordinator runs the live
check: boot to the status bar; `sensord status`, `list`, `get
illuminance`, `watch accel -n 20` and `watch heading` over ssh in the
new shapes; Auto brightness following the light (the ALS client on
proto 2, counted in `sensord status` clients); a `kill -9` of a watcher
releasing its claim (`claims` back to 0 in `list`); `blesensd status`,
`list` and `watch`; the Bluetooth page's sensors and scan through the
blesensd client. Then the coordinator archives this file, records the
result, marks step 4 done in the plan, installs the pair and pushes.

## Progress and decisions since the start (coordinator)

- M1 committed `99b5948`, M2 `c534f0e`, M3 `6f21dd0`, each approved by
  the reviewer and checked against its snapshot. Notes, scripts and the
  reviewed diffs are in the evidence dir's `review/` (`m1-note.md`,
  `m2-note.md`, `m3-note.md`, `reviewer-notes.md`): read them before M4.
- Prefixes: functions shared between files are `sn_` in sensord and
  `bd_` in blesensd (static ones keep their names). Test diffs may show a
  mechanical rename; its sed script goes in the review note.
- blesensd's split has a `state.c` (the state `D`, helpers, lookups)
  beyond the example table above.
- An echoed argument (sensord's unknown sensor name) is cut to its first
  32 bytes, escaped, like the echoed request word; `local-sockets.md`
  says so for any echoed word or argument. Apply the same rule in M4.
- `sensors-up` now points at `tools/sensord/` and the sensors guide.
- `standard-interfaces.md` is the coordinator's (updated at the end).
- 2026-10-20: the first agent pair hit a session limit after M3; a new
  pair continues with M4. Nothing of M4 was on disk.

## Result (coordinator, 2026-10-20)

Done. Commits `99b5948` (M1), `c534f0e` (M2), `6f21dd0` (M3) and
`ce182df` (M4), each approved by sensors_review and checked against the
reviewed diff (a fresh agent pair took over for M4 after a session
limit). Accepted exception to the no-`strstr` grep:
`tools/sensord/compass-replay.c`, a host-only replayer of capture files.
buttond and gps-manager changed in the image only through `chef_log`'s
320-byte cut. The RAM image `b0813ef0` passed the coordinator's live
check: sensord `status`, `list`, `get`, `watch` (accel, heading),
`declination` and its errors in the new shapes; a `kill -9`ed watcher's
claim released at once; chef-settings' illuminance claim (Auto
brightness, the cached-sample rule) and Bluetooth page (hold) through the
proto 2 and proto 1 clients; blesensd `status`, `list`, `scan`; buttond
and gps-manager answering. Found, not caused by this step (the
installed blesensd does the same): while the saved sensor's reconnect
(`Device1.Connect`) is pending, BlueZ answers `StartDiscovery` with
`InProgress` or not at all, so the Bluetooth page does not scan for new
sensors. Record: [sensors and blesensd log](../../logs/sensors-blesensd-2026-10-19.txt).
