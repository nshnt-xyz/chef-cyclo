# Local socket protocol

[Feature index](README.md) · [Build instructions](../building.md)

Our daemons serve contracts that have no standard Linux equivalent (gesture claims, GPS leases, fast sensor streams) on private Unix stream sockets under `/run`, as gpsd and PipeWire do. This page is the shape all of them share: one way to frame requests and replies, one envelope, one set of compatibility rules, and the code in `tools/common/` that implements it. Each daemon's own requests, replies and events are written down in its feature guide.

| Socket | Daemon | On this protocol |
|---|---|---|
| `/run/buttond.sock` | [buttond](buttons-and-power-off.md#socket-contract) | yes, proto 1 (since 2026-10-18) |
| `/run/gps-manager.sock` | [gps-manager](gps.md#socket-contract) | yes, proto 1 (since 2026-10-18) |
| `/run/sensord.sock` | [sensord](sensors.md#socket-contract) | yes, proto 2 (since 2026-10-19) |
| `/run/blesensd.sock` | [blesensd](bluetooth.md#socket-contract) | yes, proto 1 (since 2026-10-20) |

## Requests

A request is one line of plain words separated by spaces or tabs, ending in a newline, so a shell can speak it with `nc -U`. One carriage return right before the newline is stripped, and an empty or blank line is ignored. The first word is the command. A line holds at most a fixed number of bytes before its newline (127 for buttond); a longer one is answered with a `line too long` error and the rest of it, up to the next newline, is discarded. A line of more than 8 words reaches the daemon with the 9th and later words unsplit, so no command accepts it by accident (buttond answers `bad arguments`, or `unknown command` for a first word it does not know).

## Replies and events: the envelope

Every line a daemon sends is exactly one JSON object with a `"type"` member:

| Line | Meaning |
|---|---|
| `{"type":"hello","daemon":"buttond","proto":1}` | First on every connection, before anything else. `proto` is the version of that daemon's protocol. |
| `{"type":"ok","cmd":"<request word>",...}` | The request succeeded; further members carry its result. |
| `{"type":"error","cmd":"<request word>","reason":"<text>",...}` | It failed. `reason` is a fixed text a program can compare; further members say more. `cmd` is absent when there is no request word (`line too long`, `too many clients`) or when the error answers no request (sensord's `smgr refused report`). A word of the request echoed back, the command or an argument such as sensord's sensor name, is its first 32 bytes as sent, JSON-escaped (bytes from 0x7f up become `?`). |
| `{"type":"dropped","count":N}` | N lines a slow client may lose were not sent (see below). |
| any other `type` | A reply with data (buttond's `status`) or an event (buttond's `gesture`), defined by the daemon. |

Compatibility rules:

- A client ignores a `type` it does not know and members it does not know.
- Adding a `type` or a member does not change `proto`. A change that breaks an existing client (a member removed, renamed or with a new meaning, a request that answers differently) bumps it.
- A client that sees a `proto` it does not know logs that once and carries on.

## Connections and limits

- **State lives on the connection.** What a client claims or leases is released when its connection closes, whatever the reason: the client exits, is killed, or is dropped by the daemon. A client that wants to keep it keeps the connection open and reconnects (and claims again) after the daemon restarts.
- **Client limit.** Each daemon has a fixed table (16 clients for buttond). The next connection gets `{"type":"error","reason":"too many clients"}`, best effort, and is closed.
- **Output queues.** Each client has a bounded output queue (4096 bytes for buttond); the daemon never blocks on a client. Lines are of two kinds. Replies and events a client must see are *must-deliver*: when one does not fit in the queue, the client is not reading and is dropped (closed, its state released). Lines a slow client may lose, such as sensor samples, are *lossy*: one that does not fit is counted, and `{"type":"dropped","count":N}` goes out right before the next lossy line that fits, never ahead of a must-deliver reply. buttond sends no lossy lines.

## Shared code: `tools/common/`

Compiled from source into each daemon and client that uses it (no archive, no phone binary of its own); `make -C tools/common test` runs its host tests, also under ASan/UBSan, and the root `make test` includes them. Each header documents its interface.

- **`linesrv.{h,c}`**, the server side: `linesrv_new()` with the client limit, line maximum and queue size; `linesrv_listen()` (non-blocking, CLOEXEC, stale path unlinked, backlog 8); `linesrv_pollfds()` and `linesrv_handle()` around the daemon's own `poll()`; callbacks `on_connect` (where a daemon sends its hello), `on_line(argc, argv)` and `on_close` (exactly once per client); `linesrv_send()`/`linesrv_sendf()` (must-deliver) and `linesrv_send_lossy()`; `linesrv_close_all()` at exit. A daemon keeps its per-client state in its own array indexed by `linesrv_slot()`. A client may be dropped from inside any callback or send, also while the daemon loops over clients to broadcast. `linesrv_connect()` is the blocking connect for a daemon's command-line client.
- **`lineclient.{h,c}`**, the client side, for every local-socket and gpsd client: one connection to an AF_UNIX path or a TCP loopback port, non-blocking connect finished on `POLLOUT`, newline framing (8192-byte lines, an overlong one dropped whole and counted, one CR stripped, at most 4 reads of 4096 bytes per wakeup), callbacks `on_connected`, `on_line` and `on_lost` (exactly once per connection that came up), `lineclient_send()` for short requests (a partial send breaks the connection), and reconnection with a doubling backoff the client resets with `lineclient_healthy()` when a reply proves the peer works. No timers or loop of its own: the caller polls `lineclient_pollfd()`, calls `lineclient_handle()` and `lineclient_tick()` at `lineclient_deadline()`, always with its own monotonic clock; chefui runs it through `sys/lcwatch.c`.
- **`json.{h,c}`**: `json_escape()` for writing, and the reader clients parse daemon lines with: one line into its top-level members, strict (anything but a single valid JSON object is an error), at most 128 top-level members, nested objects and arrays as spans to parse again, no allocation.
- **`chef_log.{h,c}`**: `chef_log()` writes `<tag>: <message>` to `/dev/kmsg` (and stderr with `-v`); daemons on the library do not write kmsg themselves, so moving to syslog (step 6) changes one file.
