# Connectivity and sensors

[Next-steps index](README.md) · [Current features](../features/README.md)

Remaining integration and validation work, updated 2026-10-13 through system UI slice 2 M3. Linked feature guides describe implemented behavior; candidate approaches must be checked when implementing.

## BLE sensors

The BLE daemon and P07 to P10 settings pages are implemented and verified
with PC emulation; see the [protocol, reconnect policy and test loop](../features/bluetooth.md#ble-sensors).
Remaining:

- Validate HR16, power and CSC against real sensors, including encrypted
  sensors, multi-service devices and their reboot/reconnect behavior. Needs:
  a real HR strap, a speed/cadence sensor and a power meter (the PC emulator
  covers only the protocol); blocked on getting the hardware, nothing else.
- Add wheel-circumference selection to the UI (the daemon accepts `-w MM`,
  default 2105 mm), before making speed and distance part of a ride. Needs: a
  setting for the CSC sensor's wheel (the sensor detail page P10 is the natural
  place), stored per sensor next to its sealed record or in the UI settings, and
  a way to hand it to `blesensd` (a restart with `-w`, or a socket command).
  Blocked on the ride application defining which speed source it trusts (CSC
  against GPS), since GPS speed needs no wheel size.
- BLE idle current (measured 2026-10-16, USB out, screen off, Wi-Fi radio off, ADSP and
  `sensord` up, 4 min blocks, noise about 11 to 18 mA per block, two cycles; see the
  [slice 2 log](../../logs/system-ui-slice2-2026-10-11.txt)): adapter off 179.7 mA, a
  paired sensor absent and `blesensd` retrying (the first 300 s after a drop) 186.8 mA
  (+7.1 mA), the same sensor absent after the retry window 180.2 mA (+0.5 mA, not
  measurable). Open: (a) the **connected** cost was not measured cleanly: against the PC
  emulator the link flapped while connected (the state fell to `connecting` for 10 to 40 s
  several times per 4 min block, cause unknown: the PC adapter, the phone's BLE or the
  emulator), and the two blocks read about +3 mA on their clean rows, inside the noise; measure
  it with a real HR strap (and look at the flapping with `btmon` on the phone, then with
  the real sensor). (b) The **+7 mA while retrying** is the cost of a held connection to
  an absent sensor (the ride app holds it): a design question for the retry backoff (the
  5 to 30 s Connect cycle) once the ride app exists, for example a longer backoff or
  an advertising-driven reconnect after the first minutes.
- No scan for new sensors while a saved one is being reconnected (found
  live 2026-10-20 in the step 4 check, [log](../../logs/sensors-blesensd-2026-10-19.txt);
  the blesensd installed before step 4 does the same). With a saved sensor
  switched off, the Bluetooth page's hold makes `blesensd` retry
  `Device1.Connect` (an LE create-connection that waits for the sensor's
  advertisement), and while that is pending BlueZ answers `StartDiscovery`
  with `org.bluez.Error.InProgress` or not at all (`NoReply` after 25 s):
  `scanning` stays false, the page never shows "Scanning", and a new sensor
  cannot be found or paired until the saved one is back or forgotten.
  Candidate fixes, to check against BlueZ first: no `Connect` in flight
  while any client wants a scan (cancel the pending one, scan, and connect
  a saved sensor when the scan sees its advertisement); or leave the
  reconnection to BlueZ's own LE auto-connect for these devices if it can
  be enabled without a profile. Acceptance: with a saved sensor off and
  the page open, `blesensd status` shows `scanning` true within a few
  seconds, a second (emulated) sensor is found and paired, and the saved
  one still reconnects when it comes back, with and without the page open.
- Re-pair the PC as a classic peer for HID/PAN/A2DP after the LE emulator
  tests, which remove only the test peer's previous classic bond. Needs: the
  PC adapter free (the emulator and the classic tests share it) and the pairing
  run from the [Bluetooth tooling notes](../features/bluetooth.md); blocked only
  on scheduling a bench session, no code is missing.

## Standard Bluetooth tooling

Goal: any ordinary BlueZ client installed with `apk add` (or added to the rootfs) works as it would on a desktop distribution. The classic protocol layers, dual mode, root's session bus, classic HID (uhid and kernel HIDP, emulated keyboard), PAN (D-Bus and NetworkManager), A2DP through PipeWire/WirePlumber, `bluetooth-meshd` with a local `mesh-cfgclient` network and bonded idle current are done and live-verified; see [Bluetooth](../features/bluetooth.md). Remaining:

- **Mesh devices**: only a local network was created; provisioning and configuring a real mesh node is untried, and meshd takes the adapter from bluetoothd while it runs.
- **Real classic devices**: HID was verified with a keyboard emulated on the build PC; a real keyboard or mouse (and its reconnect after power cycling) is still untried. bluez-alsa is untried as an alternative to PipeWire. HFP voice needs the unwired board PCM path.
- **PAN lease overlap**: NM's dispatcher rejects `wlan0` leases overlapping the USB subnet but not `bnep*` ones. The ingress rule still blocks PAN traffic to USB, but a lease containing the USB host's address would break USB telnet (the local table is consulted before table 142). Extend the check to PAN (NM passes `bnep0` on up and the device's address on down) and test DHCP over `bnep0`, which is untested; both live runs used a manual address.
- **Phone as NAP**: untested. BlueZ `NetworkServer1` and NM `bluetooth.type nap` both bridge `bnep*`, so traffic arrives on the bridge and the `-i bnep+` ingress rule does not match; a NAP needs an interface-agnostic guard first (for example drop the USB subnet on anything but the USB interface and `lo`), which is a design decision.

## Wi-Fi

[Standard NetworkManager Wi-Fi](../features/wifi-ui.md) is verified on clean
baseline0d17: automatic shared modem/NM/supplicant startup, native5GHz,
automatic chrony1970-to-current time, verified HTTPS and signed apk installation.
Exact/broad static reapply rejection and radio off/on passed with USB routing,
BT presence and zero modem crashes retained. Earlier55bb covers2.4GHz,
standard APIs, strict USB unmanaged behavior and disconnect cleanup. Final5GHz reconnect/ping5/5/HTTPS200, native wpa_cli, GPS LOC noop and
USB-only listeners passed, with no UDP123/323 listener.

The [Wi-Fi settings pages](../features/system-ui.md#wi-fi-pages) (P15 to P18)
are implemented on NetworkManager's D-Bus API and accepted in slice 2 M1.
Live scan, existing-profile reconnect, details, radio off/on and page lifecycle
checks passed on a RAM image; see the [slice 2 log](../../logs/system-ui-slice2-2026-10-11.txt).
Guided keyboard join with a wrong then correct passphrase and two-tap Forget
remain in the final guided batch.

- **The Wi-Fi switch state is not persisted across reboots: fixed 2026-10-17.** `wifi-nm` now starts NetworkManager with `--state-file` on `/data/v1/networkmanager`, so `WirelessEnabled` survives a reboot; see the [log](../../logs/wifi-switch-and-bl-dcs-2026-10-17.txt). Installed 2026-10-17 ([record](../../logs/install-wifi-bldcs-2026-10-17.txt)).

Left out of the slice 2 pages and still to build:

- **Wi-Fi Sync** (ride upload, maps and routes update, rides waiting) and the
  Storage page's upload note. The design shows a Sync section on the Wi-Fi master
  page and a line on P05. Needs: the ride recorder's file layout and a "waiting"
  count, a transfer protocol and server for rides and maps (for example HTTPS to
  the owner's PC or a Strava bridge, see [Strava](#strava)), a retry and quiet
  policy while riding, and the [ride application](ui-and-ride-app.md#bike-computer-application)
  to produce rides. Blocked on the ride application and on choosing the sync
  target.
- **Internet check and signal in dBm.** The pages show "Connected" and a
  percentage with a word (strong, fair); the design shows "internet OK" and dBm.
  Needs: NetworkManager's `Connectivity` and its check URI enabled (a periodic
  probe costs radio time, measure it first) and the access point's signal in
  dBm from `wpa_supplicant` or the AccessPoint properties. Blocked on deciding
  whether a probe is acceptable on a ride's power budget.

Remaining work: complete those guided checks; measure the always-resident stack and
associated/disabled current and assess demand-driven power management and long
rides. Controlled overlapping DHCP leases remain untested live (host checks
cover the hook). GPS/time architecture modernization is deferred: retain the
implemented client-only chrony, with no added time unit or new GPS fallback.
Future assistance/log-upload work is in [GPS/time](gps-and-time.md).

USB remains unmanaged with its DHCP server. Ingress protection and dedicated
USB routing are independent of bounded dispatcher sampling. Telnet binds to
172.16.42.1; gpsd stays loopback-only when manually started. Chrony has no NTP
server or UDP command listener and uses private Unix control. Preserve BT's
verified pre-shutdown restart behavior.

## Audio

The Sound and haptics page (P03) stores alert volume, haptics, alert sounds and
"quiet when paused" for the ride application; no sound is played by the system UI
yet, and touch sounds are not implemented. Left out of slice 2 and still to build:
**touch sounds** (a short click on taps, off by default, needs the resident
player below to be cheap enough) and **alert sound playback** honoring the volume,
the alert-sounds switch and "quiet when paused" (the consumer, a paused ride,
must be known to `alertd` or the UI before it plays). Both are blocked on the
resident `alertd` player below and on the ride application, which owns the
events. The [ride application](ui-and-ride-app.md#bike-computer-application)
section lists the consumer side.

The [audio guide](../features/audio.md) owns the verified playback contract, mmap requirement, installed diagnostic tools and permanent amplitude limits. Speaker protection was abandoned; the [investigation record](../research/speaker-protection.md) preserves the findings. Remaining work:

- **Native player dependency**: retain the packaged tinyalsa tools for diagnostics. For a resident player, vendor matching PCM/mixer source or use a build-only development package linked statically; do not add Android's audio HAL.
- **First alert integration**: prove the UI contract with a small `audio-alert` client that starts `audio-up` if necessary and invokes `tinyplay -M` on short, prebuilt 48 kHz/S16_LE/stereo WAV files. This is easy and uses the live-verified path, but each process/PCM open adds noticeable latency (the 1 s live tone took 1.69 s wall-clock), so it is a stepping stone rather than the final ride experience.
- **Resident `alertd` player**: build a small C daemon once alert latency matters. It should discover the `sdm660-snd-card` and MultiMedia1 PCM like the current scripts, open playback in mmap mode, own the `TERT_MI2S_RX Audio Mixer MultiMedia1` route, and expose a Unix socket such as `/run/chef/alertd.sock`. The bike UI sends semantic events (`turn-left`, `turn-right`, `off-route`, `lap`, `gps-lost`, `low-battery`, `ride-start`, `ride-stop`) instead of mixer commands or file paths. Keep the protocol small and versioned; replies should distinguish queued, coalesced, dropped and unavailable.
- **Queue and priority rules**: serialize playback so two writers can never fight over PCM or mixer state. Keep a short bounded queue, coalesce repeated navigation prompts, apply per-event cooldowns, and let safety events such as low battery or off-route replace stale informational events. Decide explicitly whether a higher-priority alert interrupts the current one or waits; never mix two full-scale signals. UI calls must be non-blocking and audio failure must not stall navigation or ride recording.
- **Alert assets and sound design**: generate deterministic PCM assets at build time (or synthesize them once at daemon startup), rather than running `wavtone` for every event. Use short, distinguishable patterns and reserve spoken prompts/music for later. Keep 48 kHz, S16_LE, stereo to match the verified backend. Preserve the [playback limits](../features/audio.md#playback-limits-and-diagnostics). Add asset duration, peak and format checks to host tests.
- **Lifecycle and fallback**: while a ride is active, acquire audio once and keep `alertd` warm to avoid ADSP/PCM startup latency. When no ride is active, choose between leaving the ADSP resident and unloading it only after the [shared ADSP lifecycle experiment](power-and-reliability.md#adsp-lifecycle-and-power). Start it as one supervised OpenRC service ([services](../features/services.md)), not separate respawning `audio-up`/player processes. On startup failure or a dead daemon, fall back to the existing vibrator patterns and show a non-fatal UI indicator. On TERM, stop accepting events, drain or discard the queue by policy, close PCM, reset the mixer route to `Off`, remove the socket and preserve useful logs.
- **Alert validation**: host-test protocol parsing, priorities, coalescing, cooldowns, malformed clients, disconnects, mixer cleanup and a fake mmap backend. On-device, measure request-to-sound latency, run at least 100 mixed alerts without XRUNs or route leaks, confirm UI/ride logging remain responsive, test daemon death/restart and vibrator fallback, and measure idle/active current with the daemon cold and warm.
- **Headphones/earpiece path**: `INT0_MI2S_RX` through the PM660L analog codec is untouched — needed for a wired-headphone or earpiece output, separate from the TERT_MI2S_RX loudspeaker path.
- **Mic capture**: TERT_MI2S_TX exists in the DT (`tas2560.2-004c`'s capture side) but nothing exercises it; not needed for the bike-computer's core use case.

## Strava

After a ride, upload the recording ([bike-computer application](ui-and-ride-app.md#bike-computer-application)) from the phone itself over [Wi-Fi](#wi-fi): Strava's v3 API takes a FIT/GPX/TCX file at `POST /api/v3/uploads` with an OAuth2 token, so we need our own API application (client id/secret), a one-time browser authorisation done on the host, and the refresh token kept on writable storage ([persistent storage](../features/storage.md)). Queue uploads while offline and flush when associated; show the result (activity id/URL) on the panel. Fallback plan: pull a finished GPX/FIT recording over USB/HTTP and upload from the host. USB/HTTP extraction is already verified for raw GPS logs; a Strava-ready recording/exporter remains to be built.

<a id="slpi-sensors"></a>

## On-board sensors

Current sensor and compass behavior is documented in the [sensor guide](../features/sensors.md). The [sensor roadmap](sensors.md) owns boot integration, wake/tap, optional compass refinements and the optional desktop interface. ALS backlight policy, GPS-course blending and declination belong to [UI integration](ui-and-ride-app.md#sensor-integration).
