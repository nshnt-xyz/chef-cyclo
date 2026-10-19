# gps-manager and the client side (standard interfaces step 3)

User request 2026-10-18 ("start research on step 3, hand over impl and
review to the 2 panes"). The scope is step 3 of the
[standard interfaces](../next-steps/standard-interfaces.md#order) order: section 2 of
that plan for `gps-manager` and the clients, and the settings item of
section 5. Read both, the [local socket protocol](../features/local-sockets.md)
and the [buttond handoff](buttond-handoff.md) (step 2, the
pattern this step repeats) first; this file adds the decisions and the
working rules.

Roles: `gps_impl` (Claude Opus) implements, `gps_review` (Claude Opus)
reviews, `gps_coord` coordinates and decides. The user is asleep and
not available; `gps_coord` runs the live check alone. Report with
`herdr agent prompt gps_coord "[From: gps_impl] ..."` (or `gps_review`).
Send a message when a milestone is ready for review, when a review is
done, when blocked, and at the end. Do not poll or wait on each other's
panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/gps-clients-20261018/` (create it).
Review diffs go to `review/<milestone>.diff` there; the reviewer
snapshots what it reviewed (sha256 in its note) and checks the commit
against that snapshot.

## Working rules

- Work on `main` in this checkout. Milestones in the order below, each
  reviewed before the next. One commit per milestone after the
  reviewer's explicit approval. Do not push. Do not touch the
  `experiment/*` or `ui` branches. Keep this file and any uncommitted
  edit of `standard-interfaces.md` out of your commits (the coordinator
  commits them at the end).
- No phone access. No `fastboot`, `phone.py`, `phone-boot.sh`, telnet or
  ssh to the phone: the coordinator runs the live check.
- Match the surrounding code's style and comment density (kernel style,
  tabs, the file-header comment that documents the protocol).
- `make test` at the repo root must pass at every milestone. Stress or
  flake checks: short, targeted at the step, runtime stated first.
- Ask `gps_coord` before deciding anything this file and the plan do not
  settle.

## Today (what changes)

`tools/gps-manager.c` (2310 lines, one file) serves `/run/gps-manager.sock`
with its own client table, line reassembly, unbuffered
`send(MSG_DONTWAIT)` (a full socket drops the client), `sock_listen`,
`sock_connect` and `kmsg_note`, and a plain-text protocol: `ok lease map 1`,
`err disabled`, `ok status k=v...`, watchers' `enabled N`, `state S
detail` and `acq UP_MS FIX_MS` (file header and
[GPS guide](../features/gps.md#use-gnss-gps-manager)). The 17th
connection is closed silently. Its sections are already separated:
core state machine (lines 177 to 880), lease server (881 to 1194), owner
check and state file (1195 to 1363), the process shell and `main`
(1364 to the end). Tests: `tools/tests/test_gps-manager.c` (includes the
file with `GPSMGR_NO_MAIN`) and `tools/tests/test_gps-manager.sh` (real
binary, stub qmicli/gpsd, the real nmea-broker, `kill -9`, respawn, the
switch), both built and run from `tools/Makefile`.

Clients of the socket: `tools/chefui/sys/gps.c` (`strncmp`/`strcmp`
parsing, also the gpsd client), its tests in
`tools/chefui/tests/test_sys.c` (a fake manager writes the text lines),
the built-in CLI (`gps-manager status|watch|retry|enable|disable|hold`),
`test_gps-manager.sh`. No repo live-test script parses it. `chef-state
gps-load` prints `enabled 0|1` to the manager's pipe: that is chef-state's
interface, not the socket's, and does not change.

Client side: `chefui/sys/gps.c` (gps-manager + gpsd, LVGL timers and fd
watches mixed with the logic), `chefui/sys/gpsview.c` (pure gpsd TPV/SKY
parsing with `strstr`), `chefui/sys/blesens.c` (its own small JSON
parser `jparse`, pure `ble_apply_line`, LVGL connection code in the same
file), `chefui/sys/alsclient.c` (sensord, `strncmp`/`strstr` parsing),
and the header-only framing `chefui/sys/linebuf.h` used by all three.
Settings: `chefui/sys/settings.c` mixes the pure file code (defaults,
checks, parse, format) with the LVGL-driven store and its worker.

## Decisions

### The JSON reader's member limit: 128

`JSON_MAX_MEMBERS` goes from 32 to 128 in `tools/common/json.h` (first
change of M3). Measured: a live `sensord status` line has 50 top-level
members (alsclient reads it), gpsd's TPV from our NMEA feed 23 and up to
about 36 from other drivers. The limit stays an error, never a cut;
update the json tests (128 parses, 129 is `JSON_ELIMIT`) and the header
comment. `struct json_obj` becomes about 5 KB; parse on the caller's
stack as today, never in a signal handler.

### gps-manager's protocol, proto 1

The shared envelope of [local socket protocol](../features/local-sockets.md).
Requests stay as today (plain words). Limits as today: 16 clients,
128-byte request lines; output queue 4096 bytes per client. Every line
it sends is must-deliver (none lossy).

| Request | Reply |
|---|---|
| (connect) | `{"type":"hello","daemon":"gps-manager","proto":1}` |
| `lease K` | `{"type":"ok","cmd":"lease","kind":"map","held":1}` (`held`: this connection's count of that kind after the request) |
| `release K` | `{"type":"ok","cmd":"release","kind":"map","held":0}` |
| `watch` | `{"type":"ok","cmd":"watch"}`, then at once a `switch` line and a `state` line (below) |
| `enable` / `disable` | `{"type":"ok","cmd":"enable","enabled":true}` (`disable`: `"cmd":"disable","enabled":false`) |
| `retry` | `{"type":"ok","cmd":"retry"}` |
| `status` | `{"type":"status","state":"RUNNING","enabled":true,"leases":{"map":1,"ride":0},"clients":2,"watchers":1,"cid":1,"phase":"up","nmea":1234,"bringups":1,"failures":0,"streak":0,"leaked":0,"grace":false,"up_ms":84000,"fix_ms":80000,"detail":"CID 1, fix"}` (`cid` null when none) |
| unknown kind | `{"type":"error","cmd":"lease","reason":"unknown lease kind","kind":"<as sent, escaped, at most 32 bytes>"}` (same for `release`) |
| lease while the switch is off | `{"type":"error","cmd":"lease","reason":"disabled"}` |
| release without a lease | `{"type":"error","cmd":"release","reason":"no lease","kind":"map"}` |
| `retry` outside FAILED | `{"type":"error","cmd":"retry","reason":"not failed"}` |
| missing or extra arguments | `{"type":"error","cmd":"<word>","reason":"bad arguments"}` (new: `lease` with no kind answered "unknown lease kind" and extra words were ignored before) |
| unknown command | `{"type":"error","cmd":"<as sent, escaped, at most 32 bytes>","reason":"unknown command"}` |
| line over 127 bytes, 17th connection | the library's `line too long` / `too many clients` errors (new: the 17th was closed silently) |

Events, to watchers only, exactly where today's lines go out:

- `{"type":"switch","enabled":false}`: today's `enabled N`. Still sent
  before the state change it causes.
- `{"type":"state","state":"RUNNING","detail":"CID 1, fix","up_ms":84000,"fix_ms":80000}`:
  today's `state S detail` and the `acq` line after it, merged into one
  line. `up_ms` is the age of the current bring-up, `fix_ms` the time from
  its start to its first fix, each null when there is none (today's `-1`).
  `detail` JSON-escaped (it is the manager's own text, at most 111 bytes).

States, the switch semantics, lease lifetime (on the connection, counted
per connection and kind), the grace, supervision and the state file do
not change. Empty lines are ignored, CR before the newline stripped.

The CLI keeps its commands and exit codes and never prints the hello:
`status`, `retry`, `enable`, `disable` print the one reply line and exit
0 on a non-error reply, 1 on an error reply or no connection; `watch`
and `hold KIND...` print every later line raw (one JSON object per line)
until EOF or ^C; 64 on usage errors. `hold` sends its leases then
`watch` as today.

Logging: `chef_log` (tag `gps-manager`, echo with `-v`) replaces
`kmsg_note`/`notef`; the core keeps its injected `ops.log`. Same message
texts.

### gps-manager's directory and split

`tools/gps-manager/` in the `tools/buttond` pattern (own `Makefile`: host
`build/host/gps-manager`, `make test`, `device`, `install-device`;
listed in the root Makefile's components and `tools/device.mk`'s
comment; `tools/Makefile` loses the gps-manager rules and its entry in
`test-shell`). Split along the existing sections:

| File | Holds |
|---|---|
| `gps-manager.h` | internal header: the core's types and ops, the server and owner/record interfaces (not installed, not for other programs) |
| `core.c` | the state machine (today's "core" section) |
| `server.c` | the lease server: on linesrv from M3, the protocol and its file-header block |
| `owner.c` | modem owner check, `/proc` start times, the state-file record |
| `main.c` | the process shell: children, switch load/save, the poll loop, `main` |
| `cli.c` | the command-line client (`client_main`) |

The big file-header comment stays in one place (`main.c`, or a
`README`-style block at the top of `gps-manager.h`; the implementer
chooses, the protocol block goes to `server.c`). Functions shared
between files get a `gm_` prefix (most already have it) and lose
`static`. Tests move to `tools/gps-manager/tests/` and link the module
sources instead of including one file; add an ASan/UBSan build of the C
test as buttond has. The shell test needs a host `nmea-broker`: the new
Makefile compiles its own copy from `../nmea-broker.c` into
`build/host/` (no cross-directory build: the root `make test` runs
`tools` and `tools/gps-manager` in parallel). `nmea-broker` itself stays
in `tools/`.

### The client side

- **`tools/common/lineclient.{h,c}`** (new): the client connection core
  for every local-socket and gpsd client, no LVGL, no timers or poll loop
  of its own, the caller's monotonic clock passed in. Target: an
  AF_UNIX path or a TCP loopback port. Non-blocking connect with its
  completion on `POLLOUT` (`SO_ERROR`), then `POLLIN`; framing as
  `linebuf.h` does today (8192-byte line buffer, an overlong line dropped
  whole and counted, one CR stripped), bounded reads per wakeup (4 x
  4096 as today); callbacks `on_connected`, `on_line(char *line, size_t
  len)`, `on_lost` (exactly once per connection, whatever the reason);
  `lineclient_send()` for short requests (a partial send counts as a
  broken connection, as today); backoff configured per client (min,
  max, doubling), reset by the caller when it sees a healthy reply (each
  client keeps its rule: gps-manager on `ok watch`, gpsd on a TPV,
  sensord on a ready status, blesensd on `ok watch`). Interface for the
  caller's loop: `lineclient_pollfd()` (fd and events, fd -1 while
  disconnected), `lineclient_handle(revents, now)`,
  `lineclient_deadline()` (the retry time, -1 none) and
  `lineclient_tick(now)`. It replaces `chefui/sys/linebuf.h` (deleted
  in M5 once no user is left). Host tests in `tools/common/tests/`:
  connect refused then retry with backoff, connect completing later,
  framing across reads, overlong, CR, EOF and error both calling
  `on_lost` once, a callback closing the connection, the tick deadline.
- **chefui glue**: one small LVGL adapter in `chefui/sys/` (name it, for
  example, `lcwatch.c`) that keeps a lineclient's fd registered with
  `chefui_watch_fd()` (re-registering when the fd or events change) and
  one one-shot `lv_timer` on `lineclient_deadline()`. The three clients
  use it; nothing else in them touches sockets.
- **`chefui/sys/gpsclient.{h,c}`** (new, no LVGL): the gps-manager and
  gpsd clients. Two lineclients; the manager lines through the JSON
  reader (hello and `proto`, `ok`/`error` by `cmd`, `switch`, `state`;
  unknown types ignored); state, switch, detail and the bring-up's start
  and first-fix times in the caller's clock; wanted leases per kind
  (`map` for the GPS page today, `ride` for the recorder in ride slice
  1), sent after `ok watch` and the `switch` line, again after a
  reconnect or when the switch comes back on, released when no longer
  wanted, an `error` with `reason` `disabled` meaning none held; whether
  gpsd is wanted (today's rule: RUNNING, or SEARCHING while a view is
  wanted); gpsd's TPV and SKY into `struct gps_view` (the gpsview.c
  parsers move onto the JSON reader, satellites walked with
  `json_array_next`; same fields, ranges and DOP-only SKY rule) and the
  satellites' 10 s staleness as a deadline. A change callback with what
  changed (state, view). The recorder can run it with its own `poll()`.
- **`chefui/sys/gps.c`** becomes the thin wrapper: `sys_gps_*` API and
  `sys_state.gps`/`gps_view` mapping unchanged for the pages
  (`apps/settings/page_gps.c` and the status bar do not change), glue
  through `lcwatch`.
- **blesensd**: `ble_apply_line()` and the view move into an LVGL-free
  `chefui/sys/blesclient.{h,c}` with the connection core (lineclient,
  `watch`, hold/scan wants synced after `ok watch`, pair/forget
  requests); its `jparse` goes, the JSON reader replaces it with the same
  semantics (flat objects; note that `jparse` rejected duplicate keys:
  the reader takes the first, which is fine). blesensd's lines are
  already enveloped, so no protocol change. `blesens.c` is the wrapper.
- **alsclient.c**: onto lineclient and the reader, no split (the
  recorder does not use the ALS). sensord's lines today are pre-envelope
  (`{"hello":...}`, `{"ok":"claim",...}`, `{"ok":"status",...}`,
  `{"err":"..."}`, `{"dropped":N}`, typeless samples
  `{"sensor":"illuminance","t":...,"illuminance":...}`): parse those
  shapes with the reader now; step 4 changes sensord and this client
  together. Keep its timers (probe, reply deadline, the logic's tick) and
  every rule in its header (stale cached sample, warm-up, backoff to
  30 s).
- **LVGL-free proof**: every module named above as having no LVGL
  (`lineclient`, `gpsclient`, `gpsview`, `blesclient`, `settings_file`
  below, `als`) is compiled in a host test target without LVGL's include
  path, so an LVGL dependency fails the build.

### Settings: the reader contract

Split the pure file code out of `chefui/sys/settings.c` into an
LVGL-free `chefui/sys/settings_file.{h,c}`: defaults, the checks
(`ui_settings_valid`, `ui_tz_valid`), `ui_settings_parse`/`format`, the
sha256 use, and `ui_data_ours`/`ui_mountpoint` with the path rule. No
behavior change; `settings.c` keeps the store and its worker. Then write
the contract in the [system UI guide](../features/system-ui.md#settings)
(a "Reading the settings from another program" part):

- `chef-settings` is the only writer. Readers never write, lock or
  create the file or its directory.
- Which file: `/data/v1/ui/settings` when `/data` is ours (the
  `chef-layout` rule), else `/run/ui/settings`; the writer moves from
  `/run` to `/data` when `/data` appears later, so a reader re-applies
  the rule on every reload.
- Changes: the writer renames a temp file over the settings file, so a
  reader watches the directory with inotify (`IN_MOVED_TO`,
  `IN_CLOSE_WRITE`, `IN_CREATE` for the name `settings`), watches the
  parent while the `ui` directory does not exist yet, and re-reads the
  whole file; it never keeps it open.
- Checks: the same as chef-settings (`settings_file.c`: magic and
  version, every range, the sha256 line); a file that fails gives the
  defaults, as chef-settings does; at most `UI_SETTINGS_MAX_BYTES`.
- Access: the file is 0600 and its directory 0700, root's. A reader runs
  as root until step 8 (daemon users), which must give readers a group.
- The fields a second reader such as the ride recorder uses: alert
  volume, alert sounds, haptics, quiet when paused; and that the
  inotify reader itself lands with ride slice 1 (its first consumer),
  built on `settings_file.c`.

Add a host test that builds a stand-alone reader from `settings_file.c`
alone (no LVGL, no store) and checks it against files written by the
store's own `ui_settings_format` (valid, v1/v2 upgrade, bad sha256,
over-long), so the split cannot quietly depend on the store.

## M1. Move gps-manager to `tools/gps-manager/`

Pure move and build wiring, no code change: `git mv` of
`gps-manager.c` and both tests, the new Makefile (as above, still one
source file and the include-style test), root Makefile, `tools/Makefile`,
`tools/device.mk` comment, `.gitignore` if needed, the docs that name
the path (`docs/repository-layout.md`, `docs/building.md`,
`docs/features/gps.md`; `docs/archive/`, `docs/research/`, `logs/` and
dated build-log entries keep the old path). Acceptance: `make test`
passes; `build/device/gps-manager` byte-identical to the one `tools/`
built before (sha256 of both in the review note); the staged image's
file list unchanged.

## M2. Split `gps-manager.c`

The table above, no behavior change: the same tests pass with only their
include/link lines and paths changed (diff of the test files in the
review note shows nothing else), plus the new ASan/UBSan run. The
device binary's size within a few percent of M1's (state both).

## M3. gps-manager on the library, JSON protocol, contract

One commit, because the daemon and its clients must agree in every
image:

- `JSON_MAX_MEMBERS` 128 (above).
- `server.c` on linesrv (hello in `on_connect`, leases dropped and
  `gm_server_sync` in `on_close`, the broadcast loops safe against a
  drop mid-loop), `main.c` polls through `linesrv_pollfds()` /
  `linesrv_handle()`, `chef_log`, `linesrv_connect` in the CLI; the
  protocol above; the protocol block of the file header rewritten.
- `chefui/sys/gps.c`: the manager lines through the JSON reader (still
  in gps.c; M4 moves them). `sys_gps_parse_state`/`parse_acq` give way
  to whatever the new parsing needs; update `test_sys.c`'s fake manager
  to the JSON lines.
- Tests: `test_gps-manager.c` (every reply and event in JSON, plus hello
  first, bad arguments, too many clients, a watcher whose queue overflows
  is dropped and its leases released), `test_gps-manager.sh` (parses the
  JSON lines; `python3 -c` or `grep` on fixed members, as the suite
  prefers), `test_sys.c`.
- Docs: the written contract in `docs/features/gps.md` (a "Socket
  contract" section like the buttons guide's: requests, replies, events,
  errors, lease lifetime, limits, `proto`, CLI output), the protocol
  paragraph and the CLI examples there updated; `local-sockets.md` table
  row says gps-manager is on proto 1; `inittab` comment if it quotes the
  protocol; grep the other docs for the old lines (`ok lease`, `acq `,
  `enabled 0|1` as a socket line, `state RUNNING`) outside
  `docs/archive`, `docs/research` and `logs`.

Acceptance: `make test` passes; no old protocol line left outside the
historical directories (chef-state's `enabled 0|1` stays); the staged
image's file list unchanged.

## M4. lineclient and the GPS client split

`tools/common/lineclient` with its tests, `chefui/sys/lcwatch.c`,
`chefui/sys/gpsclient`, `gps.c` as the wrapper, `gpsview.c` on the
reader. `test_sys.c` keeps its socket-level GPS tests (late start,
partial and malformed lines, restart, the lease after a reconnect, the
switch) passing against the wrapper, and gains pure `gpsclient` tests
driven with an injected clock and no LVGL (a test binary that links no
LVGL). Acceptance: `make test` passes; the LVGL-free target builds;
pages and status bar sources unchanged.

## M5. blesensd and ALS clients

`blesclient` split and on the reader, `blesens.c` the wrapper,
`alsclient.c` on lineclient and the reader, `linebuf.h` deleted, the
LVGL-free target extended. Existing BLE and ALS tests pass unchanged in
what they check (only their plumbing changes); add reader-based cases
for sensord's 50-member status line and an `{"err":...}` line.
Acceptance: `make test` passes; no `strstr`/`strncmp` JSON parsing left
in `chefui/sys/` (grep in the review note).

## M6. Settings file split and the reader contract

`settings_file.{h,c}`, the stand-alone reader test, the contract text in
`system-ui.md`. Acceptance: `make test` passes; `settings.c` contains
no parse/format/check code; the docs describe exactly what the code
checks.

## End

`gps_impl` builds the final RAM image from a clean tree at the last
commit (`docs/building.md`) and reports its path, its sha256 and the
staged file list diff against the installed tree (only gps-manager,
chef-settings, chefui-demo and image-info expected to differ) to
`gps_coord`. The coordinator runs the live check alone: boot to the
status bar; `gps-manager status` and `watch` over ssh in JSON;
`gps-manager hold map` to RUNNING with a fix at the window, gpsd TPV;
the status bar's watch connection counted in `status` and following the
states; a `kill -9` of the holder releasing the lease and the grace
running out to OFF; disable and enable round trip with the switch saved;
the ALS and BLE clients connected (`sensord status` clients,
`blesensd` watchers). Then the coordinator archives this file, records
the result, marks step 3 done in the plan, installs the pair and pushes.

## Result (coordinator, 2026-10-19)

Done. Commits `fdb9a16` (M1), `1b29545` (M2), `aedcc78` (M3), `cd6ea65`
(M4), `dd4c2a3` (M5) and `4343f8c` (M6), each approved by gps_review and
checked against the reviewed diff (M3 committed by the coordinator from
the approved index after both agents hit a usage limit). The RAM image
`7a7971a5` passed the coordinator's live check without the user:
`gps-manager status`, `watch` and `hold map` in JSON, RUNNING with a 3D
fix at the window, a `kill -9`ed holder's lease released at once and the
grace ending in OFF, the switch off and on (saved, `lease` refused with
`disabled`), and chef-settings' own GPS page (map lease, fix, satellites
and first fix from the new clients), Auto brightness (illuminance claim)
and Bluetooth page (hold and scan) through the split clients. Record:
[GPS clients log](../../logs/gps-clients-2026-10-18.txt).
