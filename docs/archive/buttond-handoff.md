# buttond on the shared library (standard interfaces step 2)

User request 2026-10-18 ("research step 2, then task an implementation
and a review pane"). The scope is step 2 of the
[standard interfaces](../next-steps/standard-interfaces.md#order) order and section 2
of that plan; read both first, this file adds the decisions and the
working rules. The hold-progress events for Hold to finish are not part
of this step (they land with ride slice 1 as an addition to the JSON
protocol below).

Roles: `buttond_impl` (Claude Opus) implements, `buttond_review` (Claude
Opus) reviews, `buttond_coord` coordinates, decides and talks to the
user. Report with `herdr agent prompt buttond_coord "[From: buttond_impl] ..."`
(or `buttond_review`). Send a message when a milestone is ready for
review, when review is done, when blocked, and at the end. Do not poll or
wait on each other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/buttond-20261018/` (create it).
Review diffs go to `review/<milestone>.diff` there; the reviewer
snapshots what it reviewed (sha256 in its note) and checks the commit
against that snapshot.

## Working rules

- Work on `main` in this checkout. Milestones in the order below, each
  reviewed before the next. One commit per milestone after the
  reviewer's explicit approval. Do not push. Do not touch the
  `experiment/*` or `ui` branches.
- No phone access. No `fastboot`, `phone.py`, `phone-boot.sh`, telnet or
  ssh to the phone: the coordinator runs the live check with the user.
- Match the surrounding code's style and comment density (kernel style,
  tabs, the file-header comment that documents the protocol).
- `make test` at the repo root must pass at every milestone. Stress or
  flake checks: short, targeted at the step, runtime stated first.
- Ask `buttond_coord` before deciding anything this file and the plan do
  not settle.

## Today (what changes)

`tools/buttond.c` (1232 lines) speaks a plain-text protocol on
`/run/buttond.sock`: requests `claim G`, `release G`, `watch`, `status`;
replies `ok claim G`, `ok watch`, `ok status k=v...`, `err unknown
gesture x`, `err unknown command x`, `err line too long`; events
`event G`. It carries its own client table, line reassembly, unbuffered
`send(MSG_DONTWAIT)` (a full socket drops the client), `sock_listen`,
`sock_connect` and `kmsg_note`, copies of which also live in `sensord`,
`blesensd` and `gps-manager`.

Its clients: `tools/chefui/buttons.c` (`handle_line` with `strncmp`),
the built-in CLI (`buttond status|watch|claim`), and the tests
`tools/tests/test_buttond.c`, `tools/chefui/tests/test_buttons.c`,
`tools/chefui/tests/test_chefui.c` (a fake buttond writes `ok claim ...`
and `event power.short`), `tools/tests/test_fblog.c` (includes
`../buttond.c`), and `tools/tests/test_chef_state.py` (reads
`tools/buttond.c` by path). No live-test script parses the protocol.

## Decisions

### The shared envelope (all four sockets, buttond first)

Requests stay plain text: one line, words separated by spaces, as all
four daemons take them today (usable from a shell with `nc -U`). Every
line from a daemon is one JSON object with a `"type"` member (the
blesensd shape, like gpsd's `"class"`):

- `{"type":"hello","daemon":"buttond","proto":1}` first on every
  connection, before anything else.
- `{"type":"ok","cmd":"<request word>",...}` a request succeeded; extra
  members carry its result.
- `{"type":"error","cmd":"<request word>","reason":"<text>",...}` it
  failed; `cmd` is absent when there is no request word (line too long,
  too many clients).
- Any other `type` is a reply with data (`status`) or an event
  (`gesture` here).
- `{"type":"dropped","count":N}` before the next line that fits, when
  lossy lines were dropped for a slow client (library feature; buttond
  sends no lossy lines).

Compatibility rules (written into the contract): clients ignore unknown
`type`s and unknown members; adding either does not change `proto`; a
change that breaks an existing client bumps `proto`. A client that sees
a `proto` it does not know logs it once and carries on.

sensord (`{"ok":...}`/`{"err":...}`, typeless samples) and gps-manager
move onto this envelope in their own steps (3 and 4), not now.

### buttond's protocol, proto 1

| Request | Reply |
|---|---|
| (connect) | `{"type":"hello","daemon":"buttond","proto":1}` |
| `claim G` | `{"type":"ok","cmd":"claim","gesture":"G"}` |
| `release G` | `{"type":"ok","cmd":"release","gesture":"G"}` (also when not claimed, as today) |
| `watch` | `{"type":"ok","cmd":"watch"}` |
| `status` | `{"type":"status","buttons":{"power":"up","volup":"up","voldown":"down"},"long_ms":1500,"double_ms":300,"poweroff_hold_ms":1500,"poweroff":"idle","claims":{"power.short":1},"watchers":0}` (`claims` is `{}` when none) |
| unknown gesture | `{"type":"error","cmd":"claim","reason":"unknown gesture","gesture":"<as sent, escaped, at most 32 chars>"}` |
| missing or extra arguments | `{"type":"error","cmd":"claim","reason":"bad arguments"}` (new: extra words were ignored before) |
| unknown command | `{"type":"error","cmd":"<as sent, escaped, at most 32 chars>","reason":"unknown command"}` |
| line over 127 bytes | `{"type":"error","reason":"line too long"}`, the rest of the line is discarded |
| 17th connection | `{"type":"error","reason":"too many clients"}` best effort, then closed (new: closed silently before) |

Event: `{"type":"gesture","gesture":"power.short"}`, to every claimant
and watcher, exactly when `event G` goes out today. Gesture names,
claim semantics (claim on the connection, any claim suppresses the
default, double armed only while `.double` is claimed), the defaults and
the power-off state machine do not change. Empty lines are ignored. CR
before the newline is stripped.

The CLI keeps its commands. It never prints the hello. `status` prints
the status line and exits 0 (1 on an error reply or no connection);
`watch` and `claim G...` print every later line raw, one JSON object
per line, until EOF or ^C. Exit 64 on usage errors as today.

### `tools/common/`: the shared code

Compiled from source by each consumer (as `qrtr/qmi.c` is), no
archive. `tools/common/` builds no phone binary: add it to the root
Makefile's test suites (and `clean`), not to the device components. Its
own `Makefile` builds and runs `tools/common/tests/`. Three modules:

1. **`chef_log.{h,c}`**: `chef_log_init(tag, echo_stderr)` and
   `chef_log(fmt, ...)` (printf format attribute). Writes
   `"<tag>: <msg>\n"` to `/dev/kmsg` exactly as `kmsg_note` does today
   (open, write, close per message; same 256-byte message bound), and to
   stderr too when echo is on. Step 6 swaps the body for `syslog(3)`, so
   no daemon may write kmsg itself once it is on the library.
2. **`linesrv.{h,c}`**: the server side.
   - Listen: AF_UNIX stream, non-blocking, CLOEXEC, stale path unlinked,
     backlog 8 (today's `sock_listen`). Also `linesrv_connect(path)` for
     the CLI clients (today's `sock_connect`, blocking).
   - A fixed client table, its size, the request line maximum and the
     per-client output queue size set at init (buttond: 16 clients,
     128-byte lines, 4096-byte queue). The daemon keeps its own per-client
     state in a parallel array indexed by the library's slot number
     (`linesrv_slot(c)`); no user pointers.
   - Poll integration for daemons with their own loop:
     `linesrv_pollfds()` fills the listen fd and every client fd
     (`POLLIN`, plus `POLLOUT` while that client has queued output) and
     `linesrv_handle()` takes the same pollfd span after `poll()`:
     accept, read, frame lines (overlong discard as today), tokenize
     (space/tab, at most 8 words), call the line callback, flush on
     `POLLOUT`, drop on HUP/ERR/EOF.
   - Callbacks: `on_connect` (buttond sends the hello), `on_line(srv, c,
     argc, argv)`, `on_close` (exactly once per client, whatever the
     reason: EOF, error, overflow, shutdown; buttond drops the claims and
     re-arms doubles there).
   - Output: `linesrv_send(c, buf, len)` / `linesrv_sendf(c, fmt, ...)`
     must deliver: appended to the queue and flushed at once as far as the
     socket takes it; if the queue cannot hold the line the client is
     dropped (logged "client output full"). `linesrv_send_lossy()` counts
     a line that does not fit and sends the `dropped` line ahead of the
     next one that fits (sensord's behavior, for step 4; tested here).
   - Re-entrancy: a client may be dropped from inside any callback or
     send, including while the daemon loops over clients to broadcast;
     the library must not touch a dropped slot afterwards and must stop
     handling further lines of a dropped client (today's `c->fd < 0`
     check).
   - `linesrv_adopt(fd)` puts an already-connected fd in the table (for
     tests over socketpairs; `on_connect` runs for it too).
     `linesrv_close_all()` at exit closes every client and unlinks the
     socket path.
   - Logging through an injectable function (default `chef_log`), so
     tests capture it.
3. **`json.{h,c}`**: `json_escape()` for writing (printable ASCII
   passes; `"` and `\` escaped; control characters as `\u00XX`; bytes
   `>= 0x7f` become `?`), and the client-side **reader**, moved forward
   from step 3 because `buttons.c` needs it now. Parse one line into a
   top-level object: string, number, true/false/null, nested object or
   array returned as a raw span (which the same parser can parse again).
   Strict: anything that is not a single JSON object with optional
   whitespace around it is an error. Getters by key: string (unescaped
   into a caller buffer, `\uXXXX` and surrogate pairs decoded to UTF-8,
   false when missing, not a string or truncated), number (double and
   long), bool, null. A fixed member limit (32) reported as an error,
   not silently cut. No allocation. Step 3 moves `alsclient.c`,
   `gps.c` and `blesens.c` onto it.

Do not move `tools/evdev.h`, the qrtr code or `chefui/sys/linebuf.h` into
`tools/common/` in this step.

### buttond's directory

`tools/buttond/` with `buttond.c`, `tests/test_buttond.c` and its own
`Makefile` in the `tools/sensord` pattern (host `build/host/buttond`,
`make test`, `device`, `install-device`), listed in the root Makefile's
components. `tools/Makefile` loses the buttond rules and keeps
`test-fblog`, which now includes `../buttond/buttond.c` (and links the
common sources from M2 on). No split of `buttond.c`: it is not one of
the large files, and the server code leaving it shrinks it further.

## M1. Move buttond to `tools/buttond/`

Pure move and build wiring, no code change: `git mv`, the new Makefile,
the root Makefile, `tools/Makefile`, `test_fblog.c`'s include,
`test_chef_state.py`'s path, `.gitignore` if needed, and the docs that
name the path (`docs/repository-layout.md`, `docs/building.md`,
`docs/features/buttons-and-power-off.md`; `docs/archive/`,
`docs/research/`, `logs/` and dated build-log entries keep the old
path). Acceptance: `make test` passes; `build/device/buttond` is
byte-identical to the one `tools/` built before the move (sha256 of
both in the review note; if it differs, explain why before going on);
the staged image's file list is unchanged.

## M2. `tools/common/`

The three modules above with host tests in `tools/common/tests/`:
the reader on valid, invalid and edge input (escapes, surrogates,
nested spans, truncation, member limit, trailing garbage); `json_escape`
round-tripping through the reader; `linesrv` over a real AF_UNIX
listener in a temp dir and over socketpairs: hello on connect, framing
across partial writes and several lines in one read, CR stripping,
overlong discard, tokenizer limits, client limit reply, must-deliver
overflow dropping the client and calling `on_close` once, the lossy
`dropped` line, a drop during a broadcast loop, a drop from inside
`on_line` with more lines buffered, `POLLOUT` flushing a queued tail.
No daemon uses it yet. Acceptance: `make test` passes, the new suite
included; host build clean under `-Werror`.

## M3. buttond on the library, JSON protocol, contract

One commit, because buttond and its clients must agree in every image:

- `buttond.c`: its client table, line handling, send, listen/connect and
  `kmsg_note` replaced by the library; the protocol above; the file
  header's protocol block rewritten to match. Recognizer, defaults and
  power-off code unchanged.
- `tools/chefui/buttons.c`: parse with the reader (hello: check `proto`;
  `gesture`: dispatch; `error`: log; `ok`, `status`, unknown: ignore).
  Requests, reconnect and claim behavior unchanged. `buttons.h` header
  comment updated. chefui's Makefile compiles `../common/json.c` into
  both host and device builds.
- Tests: `test_buttond.c` (every reply and event in JSON, plus the new
  cases: hello first, bad arguments, too many clients, a slow claimant
  whose queue overflows is dropped and its claim's default resumes),
  `test_buttons.c`, `test_chefui.c` (its fake buttond sends the hello and
  JSON lines), `test_fblog.c`.
- Docs: the written contract in `docs/features/buttons-and-power-off.md`
  (replace "Inspect and integrate": requests, replies, events, errors,
  claim lifetime, limits, `proto` and the compatibility rules, CLI
  output). The shared envelope and the library in a new
  `docs/features/local-sockets.md` ("Local socket protocol"), linked
  from `docs/features/README.md`; the buttond guide links to it and
  sensord/blesensd/gps-manager guides are not changed yet.
  `docs/repository-layout.md` gains `tools/common/`; fix any other doc
  that quotes the old text protocol (`ui-platform.md`, `system-ui.md`;
  grep).

Acceptance: `make test` passes; a grep for the old lines (`event power`,
`ok claim`, `err unknown`) outside `docs/archive`, `docs/research` and
`logs` finds nothing of buttond's old protocol (gps-manager's own
`ok lease` text stays until step 3); buttond's line count drops; the
staged image's file list is unchanged (no new binaries).

## End

`buttond_impl` builds the final RAM image (`docs/building.md`) and
reports its path and sha256 to `buttond_coord`. The coordinator runs
the live check with the user: boot to the status bar; `buttond status`
and `buttond watch` over ssh show the JSON lines; a short power press
goes Back on a settings page and turns the screen off and on from the
hub (chef-settings' claim); with chef-settings stopped, a short press
uses buttond's default toggle (claims released on close); a power hold
arms and a release before the second buzz cancels. Then the coordinator
moves this file to `docs/archive/`, records the result and marks step 2
done in the plan.

## Result (coordinator, 2026-10-18)

Done. Commits `d3196e7` (M1), `c6aeba2` (M2) and `8d9920e` (M3), each
approved by buttond_review and checked against the reviewed diff. The
RAM image `3e5ba485` passed the live check with the user: `buttond
status` and `watch` in JSON, a `kill -9`ed claimer's claim released at
once, Back on a settings page, screen off and on from the hub through
chef-settings' claim, and a power hold armed and cancelled. The check
used a temporary claimer instead of stopping chef-settings. Record:
[buttond log](../../logs/buttond-2026-10-18.txt). Installed the same day on the
user's go-ahead: `system_a` `188b83e6` + `boot_a` `c5ed4d0e` (commit 3ca4a5f), see the log.
