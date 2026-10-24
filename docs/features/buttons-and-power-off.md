# Side buttons and power-off

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior

`tools/buttond/buttond.c` runs as the supervised OpenRC service `buttond` ([services](services.md)), as user `buttond` in the `input`, `screen`, `vibrator` and `uinput` groups, with CAP_KILL for `poweroff` ([users and permissions](users-and-permissions.md)). It exclusively grabs the power/volume evdev devices (`qpnp_pon`, observed as event0; `gpio-keys`, observed as event6), discovers them by key capabilities through the shared `tools/evdev.h` (`/dev/input/event*` in numeric order, `evdev_key_mask`), and skips ABS devices so touch remains available, and `BUS_VIRTUAL` devices (its `EVIOCGID` bus type), so a restarted buttond never grabs its own [`chef-buttons`](#key-passthrough-chef-buttons). An explicit `-i DEV` is taken as given.

- Short power press toggles the system-wide screen-off flag `/run/chef/screen/off`: off creates it, on removes it, and buttond logs that and does nothing else (since 2026-10-20 it no longer scans `/proc` to signal a process named fblog). With no chefui app running nobody holds fb0, so the panel is already off, and the next screen owner honours the flag (a hand-run `fblog` watches it, see [display and touch](display-and-touch.md#the-screen-off-flag)). A running [chefui](ui-platform.md) application claims `power.short` and toggles the screen itself (same flag). Since 2026-10-11 the [system UI](system-ui.md) runs from boot and holds that claim: Power goes Back on its pages and turns the screen off on the hub, so buttond's own toggle applies only while no chefui application runs.
- Hold power: a short buzz at 1.5 seconds arms shutdown; release before 3 seconds to cancel. After the long buzz at 3 seconds, release to run `sync` and orderly `poweroff`.
- Unclaimed volume gestures and chords are logged without an action.

Toggle, gesture delivery/claims, shutdown cancellation, and shutdown were live-verified on 2026-09-19.

Since standard interfaces step 9 (2026-10-24) buttond also makes two uinput devices through `/dev/uinput` (root:uinput 0660, below), so standard programs see the buttons and the vibrator the usual Linux way. Both live as long as buttond. `-u PATH` names the uinput node (default `/dev/uinput`; `-u ''` makes neither); a device that cannot be made is logged once and buttond carries on without it, the buttons and the power-off unaffected.

Live 2026-10-24: `chef-buttons` and `chef-vibrator` existed (event nodes root:input 0660, `/dev/uinput` root:uinput 0660 after the coldplug, `ID_INPUT_KEY=1` and `FEEDBACKD_TYPE=vibra`), `status` said `"uinput":{"keys":true,"ff":true}`; an FF rumble upload, play and erase from a client buzzed the motor, with `"ff":"playing"` during it and `"idle"` after; a user outside group `input` cannot open `chef-vibrator` (as designed); `rc-service buttond restart` grabbed only the real key devices, never `chef-buttons`. Not yet seen live (they need button presses): key events on `chef-buttons` with `evtest`, and a buttond buzz ending a running effect.

### Key passthrough: `chef-buttons`

The grab hides the buttons from every other program, so buttond passes them on, raw, on the uinput key device `chef-buttons` (`BUS_VIRTUAL`, vendor and product 0, version 1, `EV_KEY` with `KEY_POWER`, `KEY_VOLUMEUP` and `KEY_VOLUMEDOWN`, no `EV_REP`; `/dev/input/eventN` root:input 0660, udev `ID_INPUT_KEY=1`). Raw means each event with the code and value buttond read, followed by `SYN_REPORT`.

- A press is passed when, at the press, no connection claims any gesture that uses that button: its `.short`, `.double` and `.long` and every chord with it (power: `power+volup` and `power+voldown`; volup: `power+volup`; voldown: `power+voldown`). `watch` is not a claim.
- The decision holds until that key's release: a passed press always gets its release passed (a claim that arrives meanwhile never leaves a key stuck down), and a press that was not passed never gets its release. Repeats (value 2) from the source follow the press. A release or repeat with no press seen (a key held while buttond started) is not passed.
- buttond's own gestures, defaults and claims work exactly as above whatever is passed: a long power hold reaches other programs as a long press, and buttond still powers off on its release.

Nothing on the image reads `chef-buttons` yet (no logind or acpid). With the system UI up, chef-settings claims `power.short`, so Power is not passed; the volume keys are. With `-G` (no grab, for experiments) there is no key device: the real devices are shared then.

### Force feedback: `chef-vibrator`

The uinput force-feedback device `chef-vibrator` (`BUS_VIRTUAL`, `EV_FF` with `FF_RUMBLE`, `FF_PERIODIC` with `FF_SINE`, and `FF_GAIN`; 16 effects) plays its clients' effects on the vibrator's timed_output `enable`. A client opens its event node (group `input`) and uses the standard `EVIOCSFF` (upload), `EV_FF` writes (play, stop, gain) and `EVIOCRMFF` (erase). udev gives it `FEEDBACKD_TYPE=vibra` (`70-chef.rules`), the property feedbackd looks for; feedbackd itself is not installed.

- **Upload:** stored and answered 0; an id outside 0..15, another effect type or another waveform is refused with `EINVAL`. An upload to an effect that plays keeps its running phase; later phases and repeats use the new values.
- **Play:** `EV_FF` with code = the effect's id and value N > 0 plays it N times: wait `replay.delay`, buzz `replay.length` ms, repeat. One effect at a time (timed_output has one channel): the newest play replaces the one before. `replay.length` 0 means until stopped (written as 15000 ms, the driver's maximum, and re-armed every 15 s). The motor is simply on: there is no amplitude control.
- **Silent plays:** with magnitude 0 (rumble: both magnitudes; periodic: its magnitude) or the gain at 0, nothing is written, but the play still lasts its time. Silence is decided at each buzz start; a running buzz is not cut. `FF_GAIN` (0..0xFFFF) matters only as 0 or not.
- **Stop and erase:** value 0 stops the effect if it is the one playing (a stop of another is ignored); an erase stops it if it plays and forgets it; closing the client's fd does both for its effects (the kernel's ff-core). A play of an id never uploaded or already erased is ignored (logged with `-v`).
- **Other writers:** buttond's own buzzes, powerd's critical buzz and chef-settings' feedback write `enable` directly, as before; the last writer wins. Because the driver never shortens a running buzz, a stop or a replacing play writes 0 first, but only while this device's play has the motor on, so it never cuts another writer's buzz. A buzz of buttond's own ends an effect that plays.

The timers run on buttond's poll deadline, with no threads.

## Inspect and integrate

In the phone shell:

```sh
buttond status
buttond watch
buttond claim power.short
evtest                    # pick chef-buttons: the keys nobody claims; chef-vibrator is beside it
udevadm info /dev/input/eventN | grep FEEDBACKD   # on chef-vibrator: FEEDBACKD_TYPE=vibra
```

`status` prints the status line and exits 0 (1 on an error reply or when buttond is not running). `watch` and `claim G...` are long-lived clients: they print every line that follows, one JSON object per line, until EOF or ^C; a claim lasts while that command runs. None of them prints the hello. Usage errors exit 64.

Gestures are `<button>.short`, `.double`, `.long` for `power`, `volup`, and `voldown`, plus `power+volup` and `power+voldown`. Every claimant receives the gesture; any claim suppresses its built-in default. Double-tap recognition is enabled per button only while its `.double` is claimed, adding a 300 ms wait before a lone tap becomes `short`. A claimed `power.long` suppresses the shutdown default; `-p 0` disables that default. The shutdown countdown does not wake the screen; the buzzes supply feedback.

## Socket contract

`/run/chef/buttond/buttond.sock` ([runtime files](runtime-files.md)) speaks the shared [local socket protocol](local-sockets.md): requests are words on one line, every line from buttond is a JSON object with a `type`. This is proto 1 (2026-10-18; before it buttond spoke plain `ok`/`err`/`event` lines).

| Request | Reply |
|---|---|
| (connect) | `{"type":"hello","daemon":"buttond","proto":1}` |
| `claim G` | `{"type":"ok","cmd":"claim","gesture":"G"}` |
| `release G` | `{"type":"ok","cmd":"release","gesture":"G"}`, also when G was not claimed |
| `watch` | `{"type":"ok","cmd":"watch"}`; from then on every gesture, without suppressing any default |
| `status` | `{"type":"status","buttons":{"power":"up","volup":"up","voldown":"down"},"long_ms":1500,"double_ms":300,"poweroff_hold_ms":1500,"poweroff":"idle","claims":{"power.short":1},"watchers":0,"uinput":{"keys":true,"ff":true},"ff":"idle"}` |

In `status`, `buttons` holds each key's state (`up` or `down`), `poweroff` the shutdown state machine (`idle`, `armed` after the first buzz, `ready` after the second), `claims` the number of connections claiming each claimed gesture (`{}` when none), `watchers` the number of watching connections, `uinput` which of the two uinput devices exist (`keys`: `chef-buttons`, `ff`: `chef-vibrator`) and `ff` whether an effect plays (`idle` or `playing`). `uinput` and `ff` are new since 2026-10-24 and kept proto 1.

Event, to every claimant of the gesture and every watcher, when it fires:

```json
{"type":"gesture","gesture":"power.short"}
```

`long` fires at the hold threshold while the key is still down; its release emits nothing. A chord fires on its second press and swallows both releases.

Errors:

| Cause | Line |
|---|---|
| unknown gesture | `{"type":"error","cmd":"claim","reason":"unknown gesture","gesture":"<as sent>"}` (`cmd` is `release` for a release) |
| missing or extra words | `{"type":"error","cmd":"claim","reason":"bad arguments"}` (for `release`, `watch` and `status` too) |
| unknown command | `{"type":"error","cmd":"<as sent>","reason":"unknown command"}` |
| line over 127 bytes | `{"type":"error","reason":"line too long"}`; the rest of the line is discarded |
| 17th connection | `{"type":"error","reason":"too many clients"}`, then closed |

A word sent back (`<as sent>`) is its first 32 bytes, JSON-escaped. Empty lines are ignored; a CR before the newline is stripped.

Claim lifetime and limits: claims and `watch` live on the connection and go when it closes, so a client that dies releases its claims and buttond's defaults resume (this is why the ride recorder, not the UI, is to claim `power.long`). Up to 16 connections. Each has a 4096-byte output queue; a client that stops reading until a gesture or reply no longer fits is dropped like one that closed, and its claims with it. Clients ignore unknown `type`s and members; a new member or type keeps proto 1, a change that breaks a client bumps it (the [compatibility rules](local-sockets.md#replies-and-events-the-envelope)).

## Hardware reset and shutdown constraints

- **PM660 power-key hardware (read from the PON registers 2026-09-19, both under stock and our kernel, `0x840`–`0x84b` via `/sys/kernel/debug/regmap/spmi0-00/registers`):** power alone = S1 6720 ms + S2 2000 ms → reset type `0x8` (a hard-reset variant), *enabled*; RESIN (voldown) alone and the KPDPWR+RESIN combo have their S2 reset *disabled*. So "hold power ~8.7 s" is the only hardware reset, no `kpdpwr-bark` IRQ is wired to the kernel (no warning), and the Power+VolDown escape is that same reset with VolDown held into the bootloader. `PON_TRIGGER_EN` = `0xf4`: USB and the power key power the phone on, so after any shutdown a held key or an attached cable brings it straight back (Android off-mode charging on USB). Any software power-off must act on the key *release*, well before 8.7 s — `buttond` does at 3 s.

Unplug USB if the phone should stay off: the cable powers it back on after shutdown. Since our image is in `boot_a` (2026-10-04) that boot is ours with `androidboot.mode=charger`, about 24 s after `poweroff`, and the full OS starts ([charger mode follow-up](../next-steps/storage-and-boot.md#remaining)). Keep Power+VolDown available as the hardware escape into the bootloader. The proposed software power-menu chord is `power+volup`.

## Modify and verify

Run `make -C tools/buttond test`; `tools/buttond/tests/test_buttond.c` covers gestures, every reply, error and event of the socket contract, claims, a slow claimant being dropped, dispatch, screen toggling, shutdown arm/cancel/release using injected actions, the passthrough decision (each gesture claimed alone against each button, claims between press and release, repeats, a claimant dying mid-press) and the force-feedback playback (uploads, play counts, delay, stops, erase while playing, gain and magnitude 0, the length 0 re-arm, the replacing play's 0, a buttond buzz ending an effect) with an injected clock and motor, also under ASan/UBSan. No host test opens `/dev/uinput`: making the devices, writing the passed keys and the uinput upload/erase round trip, and the `BUS_VIRTUAL` skip are for the live check. The socket code itself is `tools/common/` ([local socket protocol](local-sockets.md#shared-code-toolscommon)). These tests do not power off the host. Rebuild the baseline initramfs and boot image.

Live checks require a person at the phone: toggle both ways, watch volume/chord events, claim/release a gesture, cancel an armed shutdown, then verify shutdown on release after the second buzz. Stream evidence to the host before shutdown because phone RAM will be lost. See the [2026-09-19 build-log entries](../build-log.md) for results and [UI/recorder integration](../next-steps/ui-and-ride-app.md) for remaining work.
