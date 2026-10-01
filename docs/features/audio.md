# Audio

[Feature index](README.md) · [Build instructions](../building.md)

## Current status

ADSP bring-up, card registration and audible mmap speaker playback were verified on 2026-09-19 and again during the 2026-09-22 probe. The saved-Rdc write and cleanup were verified on 2026-09-20. Speaker protection was abandoned after the 2026-09-26 FastRPC experiment: the ADSP loaded the TAS module but never instantiated it in the speaker graph. Playback is unprotected and the limits below are permanent.

See the [investigation record](../research/speaker-protection.md) for dated findings and the [build log](../build-log.md) for session evidence.

Every boot without `audio-up`, the kernel logs `pmic_analog_codec …: Adsp is not loaded yet 0`, `sdm660-asoc-snd soc:sound: ASoC: platform (null) not registered` (repeated `EPROBE_DEFER`), and `No soundcards found.` — the ALSA machine driver's codec probe is stuck waiting for the ADSP. `/proc/asound/cards` reads `--- no soundcards ---` and `/dev/snd` holds only `timer`.

## Why audio needs its own subsystem bring-up

Audio on this SoC runs on the ADSP (Hexagon DSP), not the application processor. `audio-up` currently treats it as a one-shot bring-up rather than a supervised lifecycle like `gps-up`. The kernel does expose an unload command: writing `0` to `/sys/kernel/boot_adsp/boot` calls `subsystem_put()` in `kernel/drivers/soc/qcom/qdsp6v2/adsp-loader.c`. That path has not been exercised on this device, and its interaction with the registered ALSA card, APR clients and a later reload is unknown, so the shipped script deliberately leaves the ADSP running until reboot.

## Components and lifecycle

`initramfs/usr/bin/audio-up`:

1. Mounts the active slot's `modem_$SLOT` partition read-only at `/firmware` — the ADSP firmware (`adsp.mdt`/`adsp.b00`..`b21`) lives on the *modem* partition, next to the modem's own firmware, and `gps-up` already knows how to find and mount it. If `gps-up` already has `/firmware` mounted, `audio-up` reuses it and never unmounts it; if `audio-up` mounts it first, `gps-up` does the same in reverse — see both scripts' "audio-up also needs" / "gps-up may already have this mounted" comments for the exact symmetry. Neither script tears down a mount it doesn't own.
2. Sets `/sys/module/firmware_class/parameters/path` to `/firmware/image` with `printf` (no trailing newline — the same `param_set_copystring()` gotcha documented in `gps-up`'s `set_firmware_path`).
3. Runs `irsc` (idempotent) and starts a `servreg-locator` only if QMI service `0x40` isn't already served (by `gps-up` or a previous `audio-up`) — symmetric with `gps-up`'s own check. This answers the kernel audio PDR's boot-time `avs/audio` service lookup (`service_locator.enable=1` is on the bootloader cmdline); adsp-loader itself doesn't strictly need it, but stock always has a locator up by this point.
4. Sets the `adsp` `msm_subsys` device's `restart_level` to `related` (the default `system` panics the whole kernel on a subsystem crash — same reasoning as `gps-up`'s `modem` restart level).
5. Boots the ADSP: `echo 1 > /sys/kernel/boot_adsp/boot` (`CONFIG_MSM_ADSP_LOADER`, DT node `qcom,msm-adsp-loader`; the kernel side is `subsystem_get("adsp")`, which loads `adsp.mdt` via `request_firmware()` from the path set above, then sets the APR state to `LOADED`).
6. Polls `/proc/asound/cards` (up to 60 s; live it took 1.17 s) for the `sdm660-snd-card` machine driver (`kernel/sound/soc/msm/sdm660-internal.c:3142`) to register, then logs `adsp up, card 0 'sdm660sndcard' N.NNs after boot_adsp, MultiMedia1 playback pcm device 0` with the device parsed from `/proc/asound/pcm`. The check is a bare `grep` — **never `[ -s ]`**, since procfs reports size 0 for this file even when it lists the card. On timeout it dumps the last 40 kernel lines matching `adsp|asoc|snd|apr|q6|tas2560|lpass|pil`, and if it had started a locator it says so before stopping it: the ADSP stays up without a locator, but kernel PDR notifications stop.
7. If it started nothing (`gps-up` already had a locator up) it exits 0 — the ADSP needs no resident process. Otherwise (the common case on a fresh boot without GPS) it stays resident supervising the `servreg-locator` it started; `TERM` stops that locator only, the ADSP stays up.

Manual opt-in only (not in inittab), same policy as `gps-up`: run `audio-up &` from the telnet shell.

`initramfs/usr/bin/speaker-test-tone` plays a short sine tone out of the TAS2560 loudspeaker once the card is up. `tools/wavtone.c` (`wavtone`) generates the WAV file it hands to tinyalsa's `tinyplay`.

## Speaker path and mixer controls

The loudspeaker is a **TAS2560 smart amp** on I2C (`tas2560.2-004c`, DT `kernel/arch/arm64/boot/dts/qcom/sdm636-chef-audio.dtsi`), driven by the **TERT_MI2S_RX** backend (`kernel/sound/soc/msm/sdm660-internal.c:2821`, cpu dai `msm-dai-q6-mi2s.2`, codec dai `tas2560 ASI1`). The front-end is **MultiMedia1** (`msm-pcm-dsp.0`, `hw:x,0` — `sdm660-internal.c:1794`); `audio-up`/`speaker-test-tone` discover its PCM device number from `/proc/asound/pcm` rather than hardcoding it, since `msm_int_dai[]`'s ordering isn't a stable ABI. Default format is 48 kHz, S16_LE, 2 ch (`kernel/sound/soc/msm/sdm660-common.c:233`); `wavtone` matches those defaults.

The PM660L analog codec / `INT0_MI2S_RX` path is the earpiece/headphones, **not** the speaker (`qcom,wsa-disable` — there's no WSA881x on this device). Don't route audio there expecting the loudspeaker.

Mixer control names (`speaker-test-tone`, top-of-file variables, cross-checked against stock `mixer_paths.xml` and the kernel source):

| Control | Values | Source |
|---|---|---|
| `TERT_MI2S_RX Audio Mixer MultiMedia1` | 0 / 1 | DAPM mixer route, `kernel/sound/soc/msm/qdsp6v2/msm-pcm-routing-v2.c:15008` |
| `DAC Playback Volume` | 0..15 (1 dB/step) | `SOC_SINGLE_TLV`, `kernel/sound/soc/codecs/tas2560-codec.c:458` |
| `TAS2560_ALGO_CMD_SEND_CAL` | five integers: count, Rdc Q19, 0, 0, 0 | `SOC_SINGLE_MULTI_EXT`, `kernel/sound/soc/msm/tas2560-algo.c:648` |
| `TAS2560_ALGO_FF_MODULE` | `DISABLE` / `ENABLE` | `kernel/sound/soc/msm/tas2560-algo.c:429,652` |

Other TAS2560 controls exist but aren't touched: `TAS2560 Boost load`, `TAS2560 Sampling Rate`, `TAS2560 PowerCtrl`, `TAS2560 EAR Switch`, `TAS2560 PPG` (`kernel/sound/soc/codecs/tas2560-codec.c:460-467`).

## Playback limits and diagnostics

Speaker protection is unavailable and was abandoned on 2026-09-26. The permanent limits are -20 dBFS default, -6 dBFS ceiling, and DAC volume at most 15. Tone duration defaults to 1 s and is capped at 5 s; volume defaults to 10. `wavtone` independently refuses amplitudes above -6 dBFS.

`speaker-test-tone` leaves protection controls alone by default. Its `-p` option remains a best-effort diagnostic: it reads saved Rdc from a read-only persist mount and submits calibration atomically with `tas2560-send-cal` during a running stream. Failure cannot fail otherwise-clean playback. A successful write or `FF_MODULE` readback does not prove protection: the recorded `DISABLE` values came from zeroed DSP error replies.

The probe tools `spk-protect-probe`, `afe-topology-cal`, `tert-tx-hold`, and `afe-debug` remain diagnostics. Their one-boot topology changes and FastRPC hazards are documented in the [investigation record](../research/speaker-protection.md#speaker-protection-abandoned-2026-09-26). Do not start TERT_MI2S_TX from production code.

### tinyplay must use mmap (`-M`)

Without `-M` every play on this card XRUN-loops: live, `state: XRUN`, `hw_ptr 1024, appl_ptr 0` (also with `-p 1920 -n 8` and `-p 4800 -n 4`), a 4 s tone took 7.5–8.5 s wall-clock, dmesg showed `WARNING: at .../msm-pcm-q6-v2.c:652 msm_pcm_trigger` (a `WARN_ON_ONCE` for a pending `CMD_EOS` from the XRUN-triggered stop 40 ms after start) plus ~136 `payload size of 8` lines per run, and the tone sounded broken. Cause: `msm-pcm-q6-v2`'s copy path acks each period as soon as the DSP has copied it, so ALSA's `avail` reaches `buffer_size` immediately and the core declares an XRUN (`stop_threshold` = `buffer_size`); tinyplay then prepare/rewrites in a loop. Android's HAL sidesteps this with `stop_threshold = INT_MAX` / mmap; tinyplay only exposes `-M`. With `-M`: `state: RUNNING`, `hw_ptr` trailing `appl_ptr` by 2048 frames, 4.14 s for the 4 s tone, 0 xruns, 4 `payload size` lines, tone heard properly. **Any future ALSA client on this card must use mmap or raise `stop_threshold` to the boundary.** `speaker-test-tone` always passes `-M`; period settings stay at tinyplay's default 1024×2.

## Run it from the telnet shell

```sh
audio-up > /run/audio-up.log 2>&1 &
while ! grep -q sdm660 /proc/asound/cards; do sleep 1; done
speaker-test-tone -s                   # status: card, MultiMedia1 device, the three control values
speaker-test-tone                      # 1000 Hz, 1 s, -20 dBFS, vol 10, mmap, protection untouched
speaker-test-tone -f 440 -d 2 -a -12 -v 15   # another tone at the live-verified volume, still under the -6 dBFS ceiling
speaker-test-tone -p -d 3              # calibrated best-effort protection probe during a long-enough stream
```

Inspect state directly:

```sh
cat /proc/asound/cards          # card number and name once audio-up succeeds (size 0 on procfs, always grep it)
cat /proc/asound/pcm            # front-end list, including MultiMedia1's device number
tinymix -D 0 contents           # every control's current value (triggers TAS2560_ALGO get-param log lines)
tinymix -D 0 get 'DAC Playback Volume'
cat /proc/asound/card0/pcm0p/sub0/status   # while a tone plays: expect state: RUNNING, not XRUN
```

`audio-up` never tears the ADSP down. The kernel's `boot_adsp=0` unload path exists but remains a live-test item; use a reboot as the known-safe way to return to the pre-audio state until unload, card removal and reload have all been verified together.

## Modify and verify

Run `make -C tools test`. The audio suites cover tone format/amplitude, card discovery, firmware/mount ownership, mmap invocation, atomic calibration, activation ordering, and route cleanup; the [research record](../research/speaker-protection.md#diagnostic-helper-test-coverage) preserves detailed diagnostic-helper coverage.

Rebuild the baseline initramfs and boot image after functional changes. Live verification must establish card registration, audible mmap playback without XRUNs, route cleanup, and coexistence with GPS and sensors. Host tests cannot establish DSP behavior or audibility. Keep permanent playback limits and distinguish playback success from protection.

Alert integration is owned by the [audio roadmap](../next-steps/connectivity-and-sensors.md#audio); unload/reload and shared ADSP power measurements are owned by [power and reliability](../next-steps/power-and-reliability.md#adsp-lifecycle-and-power).
