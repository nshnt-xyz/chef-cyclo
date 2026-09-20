# Audio

[Feature index](README.md) · [Build instructions](../building.md)

## Current status

**Live-verified through 2026-09-20**: the 2026-09-19 runs established ADSP boot, card registration and clean mmap playback; run 2 (`2076c85f…`) verified the then-current scripts end to end and the user heard every tone cleanly. The rebuilt calibrated-probe image (`a53a3329…`, initramfs `433c49e2…`) then verified the corrected persist mount/path and atomic TAS2560 calibration write on the phone: `audio-up` reported card 0 in 1.59 s, `speaker-test-tone -p -d 3` loaded the saved Rdc, the kernel accepted the five values and configured the TAS feedback/RX path, mmap playback completed, the route returned to `Off`, and the owned `/factory` mount was removed. The immediate FF-module readback nevertheless remained `DISABLE`, so **speaker protection is not live-verified**. This exact new tone was not user-confirmed audible; only successful playback completion is recorded. See [next steps](../next-steps/connectivity-and-sensors.md#audio) and the [build log](../build-log.md).

Every boot without `audio-up`, the kernel logs `pmic_analog_codec …: Adsp is not loaded yet 0`, `sdm660-asoc-snd soc:sound: ASoC: platform (null) not registered` (repeated `EPROBE_DEFER`), and `No soundcards found.` — the ALSA machine driver's codec probe is stuck waiting for the ADSP. `/proc/asound/cards` reads `--- no soundcards ---` and `/dev/snd` holds only `timer`.

### Live verification 2026-09-19 (run 1)

Boot image `196c1617…` / initramfs `7f7c57de…`, `fastboot boot`, slot `a`, user at the phone. Measured:

- `audio-up` mounted `modem_a` read-only at 41.50 s; `servreg-locator` loaded 9 descriptors including `adspua.jsn` (`avs/audio -> msm/adsp/audio_pd`, instance 74); kernel `servloc: Service locator initialized` at 41.55 s and `audio_notifer_reg_service: service PDR_ADSP is in use` — the kernel asked the locator for `avs/audio` three times, so the locator is genuinely needed and stays useful.
- `boot_adsp` written at 42.11 s → `subsys-pil-tz 15700000.qcom,lpass: adsp: Brought out of reset` 42.53 s, `Power/Clock ready` 42.54 s, `service-notifier: Indication received from msm/adsp/audio_pd, state: 0x1fffffff` 42.63 s, codec/machine probes complete, `msm_asoc_machine_probe: snd_soc_register_card sucessfully` at 43.27 s — **the card is up 1.17 s after `boot_adsp`**.
- `/proc/asound/cards`: ` 0 [sdm660sndcard  ]: sdm660-snd-card - sdm660-snd-card`; `/proc/asound/pcm` first line `00-00: MultiMedia1 (*) :  : playback 1 : capture 1`. `tinypcminfo` device 0: S16_LE..S32_LE, 8 kHz–384 kHz, 1–32 ch.
- All three control names exist with the expected type/range: `TERT_MI2S_RX Audio Mixer MultiMedia1` BOOL, `DAC Playback Volume` INT 0..15 (15 at boot), `TAS2560_ALGO_FF_MODULE` ENUM DISABLE/ENABLE; `TERT_MI2S_RX Channels/Format/SampleRate` default to Two/S16_LE/KHZ_48.
- A 4 s 1 kHz -20 dBFS tone at DAC volume 15 was heard properly through the loudspeaker, with `state: RUNNING`, `hw_ptr` trailing `appl_ptr` by 2048 frames, 4.14 s wall-clock and 0 xruns — **with `tinyplay -M`** (see below). The exact manual sequence that worked, after `audio-up > /run/audio-up.log 2>&1 &` had brought the card up:

```sh
wavtone -f 1000 -d 4 -a -20 -o /run/tone4.wav
tinymix -D 0 set 'TERT_MI2S_RX Audio Mixer MultiMedia1' 1
tinyplay /run/tone4.wav -D 0 -d 0 -M
tinymix -D 0 set 'TERT_MI2S_RX Audio Mixer MultiMedia1' 0
```

Three defects in the first scripts were found by that run and are fixed in the tree (re-verified live in run 2, see the [build log](../build-log.md)): `card_ready` used `[ -s ]` on `/proc/asound/cards`, which has st_size 0 on procfs, so `audio-up` timed out at 60 s with the card up and `speaker-test-tone` refused; `tinyplay` ran without `-M`; and `TAS2560_ALGO_FF_MODULE` was set unconditionally before the stream, where the kernel rejects it. Details in the sections below.

Known kernel-log messages on every stream open include `afe_get_cal_topology_id: [AFE_TOPOLOGY_CAL] not initialized for this port 4100`, `send_afe_cal_type cal_block not found!!`, `q6asm_send_cal: cal_block is NULL`, `q6core_send_get_avcs_fwk_ver_cmd: DSP returned error[ADSP_EUNSUPPORTED]`, and a few `q6asm_callback: payload size of 8 is less than expected.` The `ADSP_EUNSUPPORTED` at 76.000 s in the 2026-09-20 probe was the same harmless q6core AVCS service-version query seen on prior clean mmap tones, and preceded TAS set-cal at 76.166 s; it is not a calibration failure. The missing AFE topology/calibration messages likewise do not prevent PCM playback, but after the FF module stayed disabled they are now relevant evidence rather than noise to dismiss: stock loads AFE topology/calibration before its TAS send-cal path. `tinymix contents` also triggers about ten `TAS2560_ALGO:… get param` lines.

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

### Calibrated write path verified; FF protection still disabled

`TAS2560_ALGO_FF_MODULE` is the ADSP-side speaker-protection module (`kernel/sound/soc/msm/tas2560-algo.c`, AFE ports `0x1004`/`0x1005`). Live findings (2026-09-19 run 1):

- `tinymix -D 0 set 'TAS2560_ALGO_FF_MODULE' ENABLE` (or `DISABLE`, or by index `1`) returns rc=1 `Error: invalid enum value` whenever the `TERT_MI2S_RX` AFE port is not active — the put handler forwards an AFE set-param to port `0x1004` and the DSP rejects it with no active port. So it cannot be part of the mixer setup before the stream.
- Set 1 s into a running stream the write returns rc=0 (kernel logs `Sending Rx-Enable data 1`) but an immediate get reads `Recieving Rx-Enable data 0` — the ADSP reports the module disabled regardless, presumably for want of calibration data (there is no ACDB loader on this build).

`speaker-test-tone` leaves all protection controls alone by default. With `-p`, it makes a **best-effort, never-fatal** calibrated attempt: discover the `persist` partition by `PARTNAME`, reuse it only if it is already the expected read-only mount at `/factory` (never unmounting a mount it does not own), otherwise mount it explicitly as ext4 with `-t ext4 -o ro,noload`, and read `/factory/factory/audio/tas2560_calib_rdc`. Live on 2026-09-20, persist was ext4 `/dev/mmcblk0p38`; the first prototype mounted it safely but looked for `/factory/audio/...`, so it reported the file missing and skipped protection. The corrected image mounted the same partition without any ext3/ext2 fallback diagnostics (only the expected `EXT4-fs … mounted filesystem without journal`), read the nested path, converted saved `7.081085;` to Q19 `3712528`, and unmounted `/factory` after loading it. `noload` prevents ext4 journal replay, so this probe cannot turn a read-only request into recovery writes to persist. The strict grammar and 4..16-ohm range remain host-tested; the per-device value is never embedded in the script.

After `tinyplay -M` has started, `-p` waits for `/proc/asound/cardN/pcmNp/sub0/status` to report `state: RUNNING`, applies its short settling delay, and rechecks `RUNNING` immediately before calibration. It then invokes the static `tas2560-send-cal CARD RDC_Q19` helper, which opens `/dev/snd/controlC<CARD>`, resolves the exact `TAS2560_ALGO_CMD_SEND_CAL` control, verifies integer type/count 5/range, and submits all `[1, Rdc-Q19, 0, 0, 0]` values atomically in one `SNDRV_CTL_IOCTL_ELEM_WRITE`. Only after that succeeds does the script use `tinymix` to enable and read back `TAS2560_ALGO_FF_MODULE`.

The helper is necessary because the initial live probe found Alpine tinyalsa 2.0.0 `tinymix` unsuitable for this control: it printed `Error: invalid value (1) for index 0`, emitted no `tas2560_algo_set_cal` kernel log, yet exited 0. `TAS2560_ALGO_CMD_SEND_CAL` is an INT×5 control with `get = NULL`; tinyalsa writes array elements index-at-a-time through a read-modify-write path, so it cannot read the write-only control before updating it. Parsing tinymix's output would not fix that mechanism. The helper uses only the kernel `<sound/asound.h>` UAPI and static musl, with no Android HAL, alsa-lib, or runtime tinyalsa dependency.

A missing partition/file, an unexpected or writable pre-existing mount, parse/range failure, mount/unmount failure, inactive PCM, helper failure, enable failure, or readback failure is logged but cannot fail otherwise-clean playback. Helper failure skips enable. Paths, helper command and activation polling/settling delays are environment-overridable for host tests. On the corrected live run, the helper ioctl completed and the kernel logged `tas2560_algo_set_cal [0]=1 [1]=3712528`, followed by Tx-FB, Tx-Enable, Rx-Enable, Rx-Cfg, Rdc programming and `FF set data 1`. Playback completed through mmap, the route reset to `Off`, and the mount was cleaned up. Immediate readback still returned `DISABLE` (the kernel received `0`), so the successful calibration/control sequence does not establish that protection is active.

Until device verification proves that the readback stays `ENABLE` and the DSP is actually applying calibrated protection, the amplitude ceiling remains the safety measure: `speaker-test-tone` defaults to **-20 dBFS** and clamps `-a` to **-6 dBFS**, and `wavtone` independently refuses (does not clamp) anything louder than -6 dBFS (`dbfs_to_amplitude`'s `-1` sentinel). Duration defaults to 1 s, clamped to 5 s. `DAC Playback Volume` defaults to 10; stock's initial mixer setup leaves it at 15 and the live run used 15 at -20 dBFS — `-v 15` reproduces that.

The stock images supplied the basis for this minimal path without importing the full Android audio stack. `vendor_a/etc/mixer_paths.xml` only enables `TAS2560_ALGO_FF_MODULE` in the speaker path; it does not reveal another TAS mixer-control sequence to guess. The stock 32-bit `audio.primary.sdm660.so` contains the persist path and TAS send-cal logic, while existing stock playback logs order `platform_send_audio_calibration`/the ACDB loader (AFE topology plus calibration) before `audio_extn_mot_spkr_send_cal`. `vendor_a` also contains `Speaker_cal.acdb` and `libacdbloader.so`, but the latter depends on Bionic/Android libraries and `/dev/msm_audio_cal`, so it is not directly usable in this musl initramfs.

The strongest current inference is therefore that the absent ACDB-installed AFE topology/calibration is why FF refuses to remain enabled, but the live evidence does **not** prove that cause. Next work is to determine a minimal, safe way to load the needed AFE topology/calibration from stock `Speaker_cal.acdb`, or another viable ACDB path, before repeating the FF check. Do not guess extra TAS controls, and do not substitute the unrelated generic INT4 speaker-protected/VI-feedback route. This device's persist dump also contains F0 and Q values, but the kernel's F0/Q calibration start/stop cases say `Not Implemented`; the implemented Rdc path has already been exercised.

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

```sh
make -C tools test
```

covers, among the rest of the suite:

- `tools/tests/test-wavtone` (47 checks): the amplitude-from-dBFS refusal above -6 dBFS, frame-count rounding, the canonical 44-byte WAV header field by field, first-sample-zero, first-quarter-period sign/magnitude, peak amplitude within 1 LSB of the requested dBFS, both channels identical.
- `tools/tests/test_audio-up.sh` (28 checks, via `AUDIO_UP_SELFTEST=1` sourcing the script): slot/partition selection (copied from `gps-up`), `set_firmware_path`'s no-trailing-newline contract, the `0x40` symmetric presence check and its wait/timeout/dead-locator paths, `find_subsys`, `boot_adsp`, `card_ready`/`wait_for_card` — with the "card present" fixture a **size-0 FIFO** like procfs, plus a check that the function body contains no `-s` — `pcm_mm1_device` parsing against a realistic `/proc/asound/pcm` fixture (leading zero stripped, `08` not mis-read as octal), and the `uptime_s`/`elapsed_s` helpers behind the boot_adsp→card timing.
- `tools/tests/test-tas2560-send-cal`: injected open/ioctl/close coverage for exact-name lookup, integer/count/range metadata validation, the single five-value atomic write, open/lookup/write/close failures, and card/Q19 input bounds.
- `tools/tests/test_speaker-test-tone.sh`: argument clamping and FIFO/no-`-s` card detection; strict Rdc grammar, bounds and nearest-Q19 conversion; the corrected mount-relative file path and explicit ext4/read-only/no-journal mount; mount ownership and cleanup; missing/malformed calibration and mount failures; `tinyplay … -D 0 -d 0 -M`; exact post-`RUNNING` helper → enable → readback order; loss of `RUNNING` during the settling delay; helper/enable failures and best-effort playback; protection untouched by default; status mode; route cleanup on playback failure; and the -6 dBFS clamp.
- `tools/tests/test_gps-up.sh` grew two checks for the `server_present()` `DUMP_SERVERS` override this work added to `gps-up` (needed for its half of the `0x40` symmetry with `audio-up`).

What these tests **cannot** cover without a device includes ADSP/ACDB behavior and whether the DSP actually applies protection. The 2026-09-20 run now covers the real persist mount/path, atomic helper write, kernel TAS programming sequence, mmap completion, route cleanup and owned-mount cleanup. It does not establish FF protection or user-confirmed audibility for that exact tone.

Rebuild the baseline initramfs and boot image after any functional change (`scripts/mkinitramfs.sh` fails closed if `tools/wavtone.c` or `tools/tas2560-send-cal.c` is missing, or if the rootfs lacks `tinymix`/`tinyplay`). The calibrated Rdc write path itself no longer needs another proof run: image `a53a3329…` established its mount, parsing, ioctl, kernel sequence and cleanup. The next live experiment should first install the minimal safe stock-derived AFE topology/calibration, then repeat `speaker-test-tone -p -d 3` and require FF readback `ENABLE` plus evidence that protection is actually applied before calling it verified. Preserve the -20 dBFS default, -6 dBFS ceiling and volume maximum 15 throughout. A missing calibration path should still produce clean unprotected playback, and a pre-existing read-only `/factory` mount must still be retained.
