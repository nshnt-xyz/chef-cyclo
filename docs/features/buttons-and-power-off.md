# Side buttons and power-off

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior

`tools/buttond.c` runs from the baseline `initramfs/etc/inittab`. It exclusively grabs the power/volume evdev devices (`qpnp_pon`, observed as event0; `gpio-keys`, observed as event6), discovers them by key capabilities, and skips ABS devices so touch remains available.

- Short power press toggles the log screen using `/run/fblog.off` and an `fblog` restart.
- Hold power: a short buzz at 1.5 seconds arms shutdown; release before 3 seconds to cancel. After the long buzz at 3 seconds, release to run `sync` and orderly `poweroff`.
- Unclaimed volume gestures and chords are logged without an action.

Toggle, gesture delivery/claims, shutdown cancellation, and shutdown were live-verified on 2026-09-19. The ride overlay does **not** start `buttond`; its logger owns screen state.

## Inspect and integrate

In the phone shell:

```sh
buttond status
buttond watch
buttond claim power.short
```

`watch` and `claim` are long-lived clients. A claim lasts while its connection is open. The daemon's Unix stream socket `/run/buttond.sock` accepts newline-delimited `claim G`, `release G`, `watch`, and `status`, replies `ok …` or `err …`, and emits `event G` lines.

Gestures are `<button>.short`, `.double`, `.long` for `power`, `volup`, and `voldown`, plus `power+volup` and `power+voldown`. Every claimant receives the gesture; any claim suppresses its built-in default. Process death releases claims. Double-tap recognition is enabled per button only while its `.double` is claimed, adding a 300 ms wait before a lone tap becomes `short`.

`buttond status` includes key state, claims, and `poweroff=idle|armed|ready`. A claimed `power.long` suppresses the shutdown default. `-p 0` disables that default. The shutdown countdown does not wake the screen; the buzzes supply feedback.

## Hardware reset and shutdown constraints

- **PM660 power-key hardware (read from the PON registers 2026-09-19, both under stock and our kernel, `0x840`–`0x84b` via `/sys/kernel/debug/regmap/spmi0-00/registers`):** power alone = S1 6720 ms + S2 2000 ms → reset type `0x8` (a hard-reset variant), *enabled*; RESIN (voldown) alone and the KPDPWR+RESIN combo have their S2 reset *disabled*. So "hold power ~8.7 s" is the only hardware reset, no `kpdpwr-bark` IRQ is wired to the kernel (no warning), and the Power+VolDown escape is that same reset with VolDown held into the bootloader. `PON_TRIGGER_EN` = `0xf4`: USB and the power key power the phone on, so after any shutdown a held key or an attached cable brings it straight back (Android off-mode charging on USB). Any software power-off must act on the key *release*, well before 8.7 s — `buttond` does at 3 s.

Unplug USB if the phone should stay off: the cable powers it back into Android off-mode charging after shutdown. Keep Power+VolDown available as the hardware escape into the bootloader. The proposed software power-menu chord is `power+volup`.

## Modify and verify

Run `make -C tools test`; `tools/tests/test_buttond.c` covers gestures, socket claims, dispatch, screen toggling, and shutdown arm/cancel/release using injected actions. These tests do not power off the host. Rebuild the baseline initramfs and boot image.

Live checks require a person at the phone: toggle both ways, watch volume/chord events, claim/release a gesture, cancel an armed shutdown, then verify shutdown on release after the second buzz. Stream evidence to the host before shutdown because phone RAM will be lost. See the [2026-09-19 build-log entries](../build-log.md) for results and [UI/recorder integration](../next-steps/ui-and-ride-app.md) for remaining work.
