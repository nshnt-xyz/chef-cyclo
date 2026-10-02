# Connectivity and sensors

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## BLE sensors

Pair/connect real HR, speed/cadence, and power sensors through BlueZ and verify GATT notifications. Scanning and IBS sleep are already verified; see [Bluetooth](../features/bluetooth.md). Persist pairing keys once [writable storage](storage-and-boot.md#persistent-storage) exists and compare BT asleep/off current with the power measurements.

## Standard Bluetooth tooling

Goal: any ordinary BlueZ client installed with `apk add` (or added to the rootfs) works as it would on a desktop distribution. The classic protocol layers, dual mode, root's session bus and the tool-first guide are done and live-verified; see [Bluetooth](../features/bluetooth.md). Remaining:

- **Persistence**: `/var/lib/bluetooth` and `apk add`ed packages are lost at reboot until [writable storage](storage-and-boot.md#persistent-storage) exists.
- **Classic HID**: not exercised live (no classic keyboard or mouse was available). Pair one and confirm `/dev/input` events through bluetoothd's default uhid path, then once with `UserspaceHID=false` for kernel HIDP.
- **NetworkManager PAN**: needs `networkmanager-bluetooth` and a deliberate change to NM's `unmanaged-devices` policy; the `bnep+` ingress rule already keeps PAN off the USB telnet.
- **Audio over Bluetooth**: A2DP through PipeWire/WirePlumber or bluez-alsa is untested; HFP voice needs the unwired board PCM path.

## Wi-Fi

[Standard NetworkManager Wi-Fi](../features/wifi-ui.md) is verified on clean
baseline0d17: automatic shared modem/NM/supplicant startup, native5GHz,
automatic chrony1970-to-current time, verified HTTPS and signed apk installation.
Exact/broad static reapply rejection and radio off/on passed with USB routing,
BT presence and zero modem crashes retained. Earlier55bb covers2.4GHz,
standard APIs, strict USB unmanaged behavior and disconnect cleanup. Final5GHz reconnect/ping5/5/HTTPS200, native wpa_cli, GPS LOC noop and
USB-only listeners passed, with no UDP123/323 listener. Ride is excluded from current
acceptance; its files are retained pending the user's planned removal.

Remaining work: build a UI on native NM/libnm APIs; add deliberate credential
persistence once writable storage exists; measure the always-resident stack and
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

The [audio guide](../features/audio.md) owns the verified playback contract, mmap requirement, installed diagnostic tools and permanent amplitude limits. Speaker protection was abandoned; the [investigation record](../research/speaker-protection.md) preserves the findings. Remaining work:

- **Native player dependency**: retain the packaged tinyalsa tools for diagnostics. For a resident player, vendor matching PCM/mixer source or use a build-only development package linked statically; do not add Android's audio HAL.
- **First alert integration**: prove the UI contract with a small `audio-alert` client that starts `audio-up` if necessary and invokes `tinyplay -M` on short, prebuilt 48 kHz/S16_LE/stereo WAV files. This is easy and uses the live-verified path, but each process/PCM open adds noticeable latency (the 1 s live tone took 1.69 s wall-clock), so it is a stepping stone rather than the final ride experience.
- **Resident `alertd` player**: build a small C daemon once alert latency matters. It should discover the `sdm660-snd-card` and MultiMedia1 PCM like the current scripts, open playback in mmap mode, own the `TERT_MI2S_RX Audio Mixer MultiMedia1` route, and expose a Unix socket such as `/run/alertd.sock`. The bike UI sends semantic events (`turn-left`, `turn-right`, `off-route`, `lap`, `gps-lost`, `low-battery`, `ride-start`, `ride-stop`) instead of mixer commands or file paths. Keep the protocol small and versioned; replies should distinguish queued, coalesced, dropped and unavailable.
- **Queue and priority rules**: serialize playback so two writers can never fight over PCM or mixer state. Keep a short bounded queue, coalesce repeated navigation prompts, apply per-event cooldowns, and let safety events such as low battery or off-route replace stale informational events. Decide explicitly whether a higher-priority alert interrupts the current one or waits; never mix two full-scale signals. UI calls must be non-blocking and audio failure must not stall navigation or ride recording.
- **Alert assets and sound design**: generate deterministic PCM assets at build time (or synthesize them once at daemon startup), rather than running `wavtone` for every event. Use short, distinguishable patterns and reserve spoken prompts/music for later. Keep 48 kHz, S16_LE, stereo to match the verified backend. Preserve the [playback limits](../features/audio.md#playback-limits-and-diagnostics). Add asset duration, peak and format checks to host tests.
- **Lifecycle and fallback**: while a ride is active, acquire audio once and keep `alertd` warm to avoid ADSP/PCM startup latency. When no ride is active, choose between leaving the ADSP resident and unloading it only after the [shared ADSP lifecycle experiment](power-and-reliability.md#adsp-lifecycle-and-power). Start one supervisor from inittab, not separate respawning `audio-up`/player processes. On startup failure or a dead daemon, fall back to the existing vibrator patterns and show a non-fatal UI indicator. On TERM, stop accepting events, drain or discard the queue by policy, close PCM, reset the mixer route to `Off`, remove the socket and preserve useful logs.
- **Alert validation**: host-test protocol parsing, priorities, coalescing, cooldowns, malformed clients, disconnects, mixer cleanup and a fake mmap backend. On-device, measure request-to-sound latency, run at least 100 mixed alerts without XRUNs or route leaks, confirm UI/ride logging remain responsive, test daemon death/restart and vibrator fallback, and measure idle/active current with the daemon cold and warm.
- **Headphones/earpiece path**: `INT0_MI2S_RX` through the PM660L analog codec is untouched — needed for a wired-headphone or earpiece output, separate from the TERT_MI2S_RX loudspeaker path.
- **Mic capture**: TERT_MI2S_TX exists in the DT (`tas2560.2-004c`'s capture side) but nothing exercises it; not needed for the bike-computer's core use case.

## Strava

After a ride, upload the recording ([bike-computer application](ui-and-ride-app.md#bike-computer-application)) from the phone itself over [Wi-Fi](#wi-fi): Strava's v3 API takes a FIT/GPX/TCX file at `POST /api/v3/uploads` with an OAuth2 token, so we need our own API application (client id/secret), a one-time browser authorisation done on the host, and the refresh token kept on writable storage ([persistent storage](storage-and-boot.md#persistent-storage)). Queue uploads while offline and flush when associated; show the result (activity id/URL) on the panel. Fallback plan: pull a finished GPX/FIT recording over USB/HTTP and upload from the host. USB/HTTP extraction is already verified for raw GPS logs; a Strava-ready recording/exporter remains to be built.

<a id="slpi-sensors"></a>

## On-board sensors

Current sensor and compass behavior is documented in the [sensor guide](../features/sensors.md). The [sensor roadmap](sensors.md) owns boot integration, retained calibration, wake/tap, optional compass refinements and the optional desktop interface. ALS backlight policy, GPS-course blending and declination belong to [UI integration](ui-and-ride-app.md#sensor-integration).
