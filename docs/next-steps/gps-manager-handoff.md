# GPS manager (handoff)

[Next steps index](README.md) · [GPS and time plan](gps-and-time.md#client-integration) · [GPS guide](../features/gps.md) · [Live testing](../live-testing.md)

**Status:** done. Implemented, host-tested and live-verified 2026-10-04
(see the [GPS guide](../features/gps.md#use-gnss-gps-manager) and the
[build log](../build-log.md)); kept as the design record. The optional
idle-current comparison was run: a held lease costs about +31 mA over `OFF`
([GPS guide](../features/gps.md#use-gnss-gps-manager)).

## Goal

Today LOC, gpsd and the `qmicli --loc-follow-nmea | nmea-broker` pipeline are
started by hand ([GPS guide](../features/gps.md#start-and-inspect-manually)).
Add one small daemon, `gps-manager`, that runs them on demand: consumers take
reference-counted leases over a Unix socket, the first lease brings GNSS up,
the last release (after a grace period) takes it down. The UI and the ride
recorder will be its clients later; this task delivers the daemon, a CLI
client, tests and live verification, not the UI integration.

## What changed since the original plan

The plan in [gps-and-time.md](gps-and-time.md#gps-manager) predates the
shared modem owner. Reconcile it, and update that section when done:

- `gps-up` now runs once at boot from inittab (`::once:`) and is the shared
  modem owner for GPS **and Wi-Fi**. The manager must **never** start, signal,
  reap or restart `gps-up`, and never close the modem. It waits for the owner
  with `/usr/lib/chef/modem-owner.sh` (`modem_owner_pid`) and treats a missing
  owner as `FAILED`/waiting, not something to fix. So the states cover the LOC
  session, gpsd and the pipeline only; "modem off at boot" no longer applies.
- The manager is the **only** user of its LOC CID. The GNSS-only RF run found
  that another `qmicli` call on a running follower's CID stops its NMEA
  stream. Allocate one CID per bring-up, keep it with
  `--client-no-release-cid`, release it on teardown, and document that manual
  LOC commands must use their own CID.
- The manager sets no modem operating mode (see the
  [Cellular RF](../features/gps.md#cellular-rf) section).
- chrony owns the clock: always run the broker with `-n`.

## Design (from the plan, keep unless evidence says otherwise)

- C, in `tools/` next to `buttond`/`powerd`, static musl build, one
  `::respawn:` inittab line for the manager only. No separate respawn
  entries for gpsd or the pipeline.
- Socket `/run/gps-manager.sock`, newline text protocol in the `buttond`
  style (`lease <kind>`, `release <kind>`, `status`, `watch`; replies
  `ok …`/`err …`; `watch` streams `state <S>` lines). A lease lives as long
  as its connection, so process death releases it. Kinds at least `map` and
  `ride`; counts are per connection.
- States `OFF`, `STARTING`, `ACQUIRING`, `RUNNING`, `STOPPING`, `FAILED`.
  `ACQUIRING`/`RUNNING` may follow gpsd/NMEA fix state only as a hint; fix
  data itself stays in gpsd. Last release starts a 30 s grace before
  `STOPPING`; a new lease inside the grace cancels it.
- Bring-up order: wait for the modem owner and a bridge answer, allocate the
  LOC CID, set NMEA types `gga|rmc|gsv|gsa|vtg`, start LOC session 1, start
  gpsd (`-N -n -b`, no `-G`, `udp://127.0.0.1:20175`), then the follower and
  `nmea-broker -n`. Teardown in reverse: follower/broker, LOC stop, CID
  release, gpsd.
- Supervision: if any owned process dies, tear the owned set down as one unit
  and retry with bounded backoff only while a lease remains. Port the
  silence and five-deaths rules proven on the dropped ride image
  (2026-09-18, [build log](../build-log.md)) into the manager. A manager
  restart (respawn) must not leave orphaned gpsd/followers or a held CID it
  can no longer release; decide and test how it finds and cleans its
  previous children.
- `gps-manager status` and a holding client (`gps-manager hold map`, like
  `buttond claim`) for shells and tests. Logs to kmsg/`/run`, RAM only.

## Acceptance

Host: unit tests in `make -C tools test` (lease counting, grace cancel,
connection death releases, ordered bring-up/teardown with injected command
runners, retry/backoff only while leased, no retry when unleased, restart
cleanup). Then rebuild the initramfs and a test boot image under another name
(`OUT=out/boot-gpsmgr.img`); keep `out/boot.img` the baseline until the
reviewer signs off.

Live (phone at a window): first lease → `RUNNING` and gpsd `TPV mode ≥2`;
second lease and its release keep it up; last release → grace → `OFF` with
LOC stopped, CID released, no gpsd/qmicli/broker left, `gps-up` untouched
(same PID and start time) and Wi-Fi still associated; a killed client
releases its lease; a killed broker or gpsd triggers one full teardown and
recovery while leased; manager `kill -9` plus respawn leaves no partial stack;
modem `crash_count` 0 and no EFS/persist writes. Optional, needs the user to
unplug USB: idle current with a lease held versus `OFF` (LOC session cost),
using the A/B sequencer from the GNSS-only RF run.

Docs: GPS guide (operating contract replaces the manual recipe as the main
path; keep the manual one for debugging), gps-and-time.md (drop the manager
section, keep client integration), build log with image hashes and evidence.

## Starting point and gotchas

- Phone: on the baseline image `12fc50c3`, USB plugged, at a window. The user
  allows fastboot boots of test images without asking; ask before any step
  that needs them to unplug USB or move the phone.
- Prior evidence and drivers: `~/chef-cyclo-evidence/gnss-rf-20261004T123649Z/`
  and the [GNSS-only RF handoff](gnss-only-rf-handoff.md) gotchas (setsid for
  anything that outlives a telnet command, never end a remote command with a
  bare `&`, NMEA/positions only in gitignored or redacted logs).
- Messages to the user: no em dashes.
