# Side buttons and power-off

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior

`tools/buttond/buttond.c` runs as the supervised OpenRC service `buttond` ([services](services.md)), as user `buttond` in the `input`, `screen` and `vibrator` groups, with CAP_KILL for `poweroff` ([users and permissions](users-and-permissions.md)). It exclusively grabs the power/volume evdev devices (`qpnp_pon`, observed as event0; `gpio-keys`, observed as event6), discovers them by key capabilities through the shared `tools/evdev.h` (`/dev/input/event*` in numeric order, `evdev_key_mask`), and skips ABS devices so touch remains available.

- Short power press toggles the system-wide screen-off flag `/run/chef/screen/off`: off creates it, on removes it, and buttond logs that and does nothing else (since 2026-10-20 it no longer scans `/proc` to signal a process named fblog). With no chefui app running nobody holds fb0, so the panel is already off, and the next screen owner honours the flag (a hand-run `fblog` watches it, see [display and touch](display-and-touch.md#the-screen-off-flag)). A running [chefui](ui-platform.md) application claims `power.short` and toggles the screen itself (same flag). Since 2026-10-11 the [system UI](system-ui.md) runs from boot and holds that claim: Power goes Back on its pages and turns the screen off on the hub, so buttond's own toggle applies only while no chefui application runs.
- Hold power: a short buzz at 1.5 seconds arms shutdown; release before 3 seconds to cancel. After the long buzz at 3 seconds, release to run `sync` and orderly `poweroff`.
- Unclaimed volume gestures and chords are logged without an action.

Toggle, gesture delivery/claims, shutdown cancellation, and shutdown were live-verified on 2026-09-19.

## Inspect and integrate

In the phone shell:

```sh
buttond status
buttond watch
buttond claim power.short
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
| `status` | `{"type":"status","buttons":{"power":"up","volup":"up","voldown":"down"},"long_ms":1500,"double_ms":300,"poweroff_hold_ms":1500,"poweroff":"idle","claims":{"power.short":1},"watchers":0}` |

In `status`, `buttons` holds each key's state (`up` or `down`), `poweroff` the shutdown state machine (`idle`, `armed` after the first buzz, `ready` after the second), `claims` the number of connections claiming each claimed gesture (`{}` when none) and `watchers` the number of watching connections.

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

Run `make -C tools/buttond test`; `tools/buttond/tests/test_buttond.c` covers gestures, every reply, error and event of the socket contract, claims, a slow claimant being dropped, dispatch, screen toggling, and shutdown arm/cancel/release using injected actions, also under ASan/UBSan. The socket code itself is `tools/common/` ([local socket protocol](local-sockets.md#shared-code-toolscommon)). These tests do not power off the host. Rebuild the baseline initramfs and boot image.

Live checks require a person at the phone: toggle both ways, watch volume/chord events, claim/release a gesture, cancel an armed shutdown, then verify shutdown on release after the second buzz. Stream evidence to the host before shutdown because phone RAM will be lost. See the [2026-09-19 build-log entries](../build-log.md) for results and [UI/recorder integration](../next-steps/ui-and-ride-app.md) for remaining work.
