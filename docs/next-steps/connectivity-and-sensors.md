# Connectivity and sensors

[Next-steps index](README.md) · [Current features](../features/README.md)

These are plans, not implemented behavior. Package versions and candidate approaches reflect the 2026-09-19 notes and must be checked when implementing.

## BLE sensors

Pair/connect real HR, speed/cadence, and power sensors through BlueZ and verify GATT notifications. Scanning and IBS sleep are already verified; see [Bluetooth](../features/bluetooth.md). Persist pairing keys once [writable storage](storage-and-boot.md#persistent-storage) exists and compare BT asleep/off current with the power measurements.

## Wi-Fi

qcacld + firmware blobs from stock, wpa_supplicant, then NTP pool for chrony ([time synchronization](gps-and-time.md#time-synchronization)) and XTRA assistance ([warm starts](gps-and-time.md#warm-starts-and-assistance)) and log upload without the USB cable. WCN3990 shares rails with BT, so BT restarts must keep using the pre-shutdown path once Wi-Fi is up. Measure Wi-Fi off vs. associated current during a ride.

**Before Wi-Fi ships**: `telnetd` is a passwordless root shell on every interface today, fine on the USB link only — bind it to `usb0` (`telnetd -b 172.16.42.1`) or replace it with dropbear + keys, and keep 2947/chrony loopback-only.

## Audio

ADSP bring-up, the ALSA card and mmap tone playback through the TAS2560 loudspeaker were live-verified on 2026-09-19 (manual steps; the scripts are pending re-verification after three fixes) — see [Audio](../features/audio.md) and `docs/build-log.md`. Remaining work, in roughly the order it'll be needed:

- **Script re-verification**: `audio-up` (card wait fixed: `/proc/asound/cards` has st_size 0, never `[ -s ]` it) and `speaker-test-tone` (`tinyplay -M`, protection untouched by default, `-s` status, `-p` best-effort) end to end on the device.
- **Speaker protection (`TAS2560_ALGO_FF_MODULE`)**: unreachable from userspace on this stack today. Observed live: `tinymix set … ENABLE` (or `DISABLE`, or by index) returns `Error: invalid enum value` whenever the `TERT_MI2S_RX` AFE port is inactive (the put handler forwards an AFE set-param to port `0x1004`, which the DSP rejects with no active port); set 1 s into a running stream it returns rc=0 (`Sending Rx-Enable data 1`) but reads straight back as 0 (`Recieving Rx-Enable data 0`) — the ADSP keeps the module disabled. Needs calibration data (Rdc/F0/Q via the ACDB path there is no loader for) and/or the right port-active sequencing; until then the -6 dBFS amplitude ceiling in `speaker-test-tone`/`wavtone` is the only speaker protection, and any real player must keep a conservative level. `speaker-test-tone -p` logs the set rc and read-back so this can be re-checked cheaply.
- **Every ALSA client must use mmap** (or raise `stop_threshold` to the boundary): `msm-pcm-q6-v2`'s copy path acks periods as soon as the DSP has copied them, so a normal read/write client XRUN-loops (`state: XRUN`, WARN in `msm_pcm_trigger`, broken audio). tinyplay's `-M` is the verified workaround; a future alert player needs the same in its own PCM setup.
- **Headphones/earpiece path**: `INT0_MI2S_RX` through the PM660L analog codec is untouched — needed for a wired-headphone or earpiece output, separate from the TERT_MI2S_RX loudspeaker path.
- **Mic capture**: TERT_MI2S_TX exists in the DT (`tas2560.2-004c`'s capture side) but nothing exercises it; not needed for the bike-computer's core use case.
- **Power cost of a resident ADSP**: no unload path once booted (reboot is the only way down) — measure its idle draw like the BT/Wi-Fi off-vs-on measurements elsewhere in this document, since audio may only be needed for alerts.
- **Alert integration for the ride app**: once the [bike-computer UI](ui-and-ride-app.md#bike-computer-application) exists, a small resident mmap player for turn/lap/low-battery tones, replacing the vibrator as the only feedback channel; `audio-up` promoted from manual opt-in to inittab at the same time.

## Strava

After a ride, upload the recording ([bike-computer application](ui-and-ride-app.md#bike-computer-application)) from the phone itself over [Wi-Fi](#wi-fi): Strava's v3 API takes a FIT/GPX/TCX file at `POST /api/v3/uploads` with an OAuth2 token, so we need our own API application (client id/secret), a one-time browser authorisation done on the host, and the refresh token kept on writable storage ([persistent storage](storage-and-boot.md#persistent-storage)). Queue uploads while offline and flush when associated; show the result (activity id/URL) on the panel. Fallback plan: pull a finished GPX/FIT recording over USB/HTTP and upload from the host. USB/HTTP extraction is already verified for raw GPS logs; a Strava-ready recording/exporter remains to be built.

## SLPI sensors

Accelerometer/gyro/magnetometer/ALS/proximity sit behind the sensor DSP (SLPI/SSC) — another subsystem bring-up like the modem (PIL, firmware, its own QMI services). Would give wake-on-tap, a compass for the map, and auto-brightness. Not before the app works without them.
