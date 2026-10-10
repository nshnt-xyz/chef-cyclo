# GNSS-only RF (handoff)

**Archived 2026-10-11.** Completed and live verified on the date recorded below. Current behavior belongs in the linked feature guide; application follow-ups belong in [next steps](../next-steps/README.md). The original design and process instructions below are historical, not active assignments.

[Archive index](README.md) · [GPS and time plan](../next-steps/gps-and-time.md) · [GPS guide](../features/gps.md) · [Live testing](../live-testing.md)

**Status:** done, documentation only. Live-measured 2026-10-04: the boot
default draws the same idle current as DMS `low-power`, so no mode is applied
at boot; a prototyped helper was dropped (see the [GPS guide](../features/gps.md#cellular-rf)
and the [build log](../build-log.md)). Kept as the design record.

## Goal

The baseline image starts one resident modem owner (`gps-up`) at boot, shared
by GPS and Wi-Fi. No RIL ever talks to the modem, so we do not know whether
it is scanning for or camping on cellular networks while it provides GNSS and
the WLAN PD. Find out, and if it is, put the modem in a mode that turns
cellular RF off while GNSS **and Wi-Fi** keep working, then measure what it
saves.

Answer, with evidence:

1. What `qmicli -d /run/qmux_socket --dms-get-operating-mode` reports on a
   fresh baseline boot (before anything else touches the modem), and whether
   that changes after LOC starts.
2. Whether the modem is scanning/camping: SIM state (`--uim-get-card-status`),
   `--nas-get-serving-system`, `--nas-get-system-info`, `--nas-get-signal-info`,
   `--nas-get-system-selection-preference`, `--dms-get-capabilities`. A
   "searching" registration state with no SIM still means the RF front end
   is cycling.
3. Which mode disables cellular RF and keeps GNSS and Wi-Fi alive. Prior
   expectation (unverified on this device): DMS `low-power` is what the
   Qualcomm RIL uses for Android airplane mode, where GPS and Wi-Fi keep
   working. The alternative is a NAS system-selection change. Do **not** use
   `persistent-low-power` (persisted in NV) or `offline` (needs a modem
   reset to leave). Try the volatile mode first.
4. In the chosen mode: LOC session start, `--loc-get-gnss-sv-info` tracking
   satellites, and a real fix (NMEA status A / gpsd TPV mode 3). Indoors only
   proves tracking; a fix needs the phone at a window or outdoors, which
   needs the user (announce it, see guided prompts below). Also re-check
   Wi-Fi association + ping in that mode, and that `--dms-get-operating-mode`
   reads back as set.
5. Idle current, cellular on vs off: panel off, USB unplugged, Wi-Fi and BT
   in a fixed state, modem up in both. Use an unattended on-device A/B
   sequencer (alternate online/low-power blocks of a few minutes, ABAB, log
   `battery/current_now` every second plus the mode readback per block),
   started only after the unplug is detected; pull after replug. Sign: + =
   discharging. Report per-block means and spread, not a single number.

## Implementation (only after the live answer)

If a mode is chosen, make the baseline apply it once the modem owner is ready
(in `gps-up` or a small helper it calls; reuse the existing QMUX bridge and
`qmicli`, no new daemon), with a host test in `make -C tools test` style.
It must be best-effort (a failure logs and leaves GPS/Wi-Fi up, never tears
the owner down), idempotent across LOC client restarts, and documented as a
config knob if there is a reason to keep cellular on. If the modem turns
out to boot already in a non-RF mode, the outcome is documentation only.

Update `docs/features/gps.md`, the GNSS-only section of
`docs/next-steps/gps-and-time.md` (drop it once done), the suspend/idle line
in `docs/next-steps/power-and-reliability.md` with the measured delta, and a
`docs/build-log.md` entry with image hashes and evidence links.

## Starting point

- Phone: on stock Android (`adb devices` shows `ZF6223WZGL`). Ask the user
  once before the first reboot into fastboot; nothing is ever flashed.
- Baseline image: `out/boot.img` `12fc50c366053bbdf257a35d7e0ae557e291af5e4ae2e9ce5e3cbdc2ffa692ef`
  (== `out/boot-evdev.img`, HEAD `0ed01bc`, live-verified 2026-10-04). Boot it
  with `EXPECT=12fc50c3… scripts/phone-boot.sh`. Build test images under
  another name so `out/boot.img` stays the baseline.
- Drivers: `scripts/phone-boot.sh`, `scripts/phone.py run|push|pull|stream`
  ([live testing](../live-testing.md)). Wait for the modem owner with
  `/usr/lib/chef/modem-owner.sh` as in the GPS guide; never start a second
  `gps-up`, never signal it.
- `qmuxd-lite` relays any QMI service, so DMS/NAS/UIM work through
  `/run/qmux_socket`. Host `qmicli` (same libqmi 1.38.0) against a host-built
  `qmuxd-lite` reproduces bridge problems in seconds.

## Gotchas (from earlier live sessions)

- `rmtfs -r` RAM-shadow is mandatory; nothing writes EFS/persist. Mode
  changes may write NV into the shadow; that is fine, it is lost at reboot.
  Keep the diskstats no-write proof for EFS partitions in the evidence.
- Default NMEA type set is `none`; set `gga|rmc|gsv|gsa|vtg` before a fix
  attempt. Gate LOC on modem ready + ~20 s.
- Anything that must outlive a telnet command: `setsid CMD </dev/null >/run/x.out 2>&1 &`.
  The `phone.py`/remote wrappers append a status echo, so never end a command
  with a bare `&`; use `& sleep 1`.
- Background A/B runs must wait for the unplug before the first block.
  With USB attached the host CDP port AICL-collapses to ~150 mA and corrupts
  current readings.
- User-facing guided steps: numbered `STEP k/N`, at least 8 s to move and
  10 s to hold, and a countdown before anything the user must watch.
- Evidence: raw runs in `~/chef-cyclo-evidence/gnss-rf-<UTC>/`; anything
  copied into `logs/` that carries NMEA/positions must match a gitignored
  pattern (`logs/gpsd-live-test-*.tgz`) or be redacted. Record image hashes.
- Messages to the user: no em dashes.
