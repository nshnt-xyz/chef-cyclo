# Audio (standard interfaces step 10)

User request 2026-10-25 ("research about audio standardisation ... so that I
can just hand it over to my implementation agents with access to the
device"). The scope is step 10 of the
[standard interfaces](standard-interfaces.md#order) order, section 6 of that
plan. Read that section, the [audio guide](../features/audio.md), the
[alert-player plan](connectivity-and-sensors.md#audio) (superseded here where
they differ), the [ADSP lifecycle](power-and-reliability.md#adsp-lifecycle-and-power),
[services](../features/services.md),
[users and permissions](../features/users-and-permissions.md) and the
[settings contract](../features/system-ui.md#settings) first. This file adds
the research results, the proposed decisions, the questions only the phone
can answer, and the milestones.

The decisions below are **proposed**. The ones marked *(needs the user)* are
not the implementers' to make: the coordinator gets the user's answer before
the milestone that depends on it starts.

No em dashes anywhere (code, comments, docs, commit messages, messages).

## Working rules

- Work on `main` in this checkout. Kernel changes are commits on the kernel
  submodule's branch `chef-cyclo-qpts30.61-18-10`, plus a pointer bump in the
  superproject. Milestones in the order below, each reviewed before the
  next. Do not push either repo. Keep this file and any edit of
  `standard-interfaces.md` out of milestone commits (the coordinator commits
  them at the end).
- The device agent runs L0 (the live research) before M1 starts, and the
  live check after the last milestone. L0 only reads and plays: no
  `boot_adsp=0`, no TERT_MI2S_TX, no `TAS2560_ALGO_*` writes, no amplitude
  above -6 dBFS through today's path, no DAC volume above 15.
- The implementer does not research or decide. Where this file and the code
  disagree, or this file does not settle something, stop and ask the
  coordinator with the exact question and the options you see.
- `make test` at the repo root must pass at every milestone. Host tests run
  as the developer's uid, never touch the host's `/dev/snd`, and never write
  to the host's syslog.
- Third-party config (alsa-lib `asound.conf`, UCM2 files, udev rules) follows
  that project's own syntax and documentation. Cite the upstream file it
  was modelled on in a comment.

## What the research found

### F1. The XRUN loop: the copy path queues every pre-filled period at once

`kernel/sound/soc/msm/qdsp6v2/msm-pcm-q6-v2.c`, at the submodule's pinned
commit `1f0d322`:

- Read/write clients go through `.copy` (`msm_pcm_playback_copy`, line 774).
  Before the stream starts, each period is copied into an ASM buffer and
  counted in `out_needed` (line 861). When the DSP acknowledges
  `ASM_SESSION_CMD_RUN_V2` (line 263), the handler sends **every** queued
  period to the DSP in one loop (line 277).
- The DSP returns `ASM_DATA_EVENT_WRITE_DONE_V2` as soon as it has taken a
  buffer into its own pipeline (line 159). Each one adds a period to
  `pcm_irq_pos` and calls `snd_pcm_period_elapsed()`. `.pointer` (line 1091)
  reports `pcm_irq_pos`.
- So with a full buffer at start (tinyplay's and aplay's default start
  threshold), both WRITE_DONEs come back almost at once, `hw_ptr` jumps a
  whole buffer, `avail` reaches `stop_threshold` (= `buffer_size`) and the
  core declares an XRUN. That matches the live record (`hw_ptr 1024,
  appl_ptr 0`, the `WARN_ON_ONCE` on a pending `CMD_EOS` at line 652).
- The mmap path behaves differently. `.mmap` (line 1104) sets `mmap_flag`.
  RUN then sends **one** period (line 269), and each WRITE_DONE sends the
  next period from the ring (lines 169 to 177). Exactly one period is in
  flight from the host side, so `hw_ptr` advances at the playback rate. That
  is why `tinyplay -M` works.
- `hw_params` (line 1125) already points the ALSA runtime buffer
  (`dma_area`) at the same contiguous ASM buffer that the mmap path streams
  from (`q6asm_audio_client_buf_alloc_contiguous`).

This refines the guide's root cause ("acks each period as soon as the DSP
copies it"). The ack is the same in both modes. The difference is how many
periods are in flight.

**The kernel fix:** stream read/write playback the way mmap playback already
works. Drop `.copy` for playback so the ALSA core writes into `dma_area` at
`hwoff` itself, and set `mmap_flag` for every playback stream (in
`hw_params` or `prepare`, not only in `.mmap`). Capture keeps its copy path.
The change is small, and it reuses a path that has been verified live. Check:

- that nothing else depends on `mmap_flag == 0` for playback (its uses are
  lines 169, 208, 219 and 269, and only 169 and 269 are playback);
- that the ALSA 4.4 core really falls back to `memcpy` into `dma_area` when
  `ops->copy` is NULL (`snd_pcm_lib_write_transfer` in
  `sound/core/pcm_lib.c`);
- cache coherency. The copy path already writes the same kernel pointer
  (`buf[0].data`) with `copy_from_user`, so the core's `memcpy` into the same
  pointer is no worse.

**Fallback without a kernel change:** alsa-lib's `dmix` opens its slave in
mmap mode and sets the slave's `stop_threshold` to the boundary. A
`default` device built on dmix therefore works on today's kernel. Only a
direct `hw:` open by a read/write client keeps XRUN-looping. The ceiling
(F3) needs a kernel change anyway, so both changes go in the same kernel
build.

### F2. `DAC Playback Volume` cannot exceed 15

`kernel/sound/soc/codecs/tas2560-codec.c:455-459`:
`SOC_SINGLE_TLV("DAC Playback Volume", TAS2560_SPK_CTRL_REG, 0, 0x0f, 0, dac_tlv)`
with `DECLARE_TLV_DB_SCALE(dac_tlv, 0, 100, 0)`. The control is 0 to 15 dB of
**gain**, and its maximum is 15. The kernel already enforces "DAC volume at
most 15" for every client. Nothing in userspace has to.

### F3. Nothing below userspace holds the -6 dBFS ceiling, and any route can reach the speaker

- The ASM per-stream volume (`Playback N Volume`, created by
  `msm_pcm_add_volume_control`, lines 1465 to 1486) is linear 0 to 0x2000.
  0x2000 is unity gain, and it is the maximum (`sound/core/pcm_lib.c:2597`).
  So it can attenuate but cannot cap anything by default. It is also applied
  only when someone writes the control; a newly opened stream plays at
  unity.
- `TERT_MI2S_RX Audio Mixer` has 23 frontend inputs
  (`tertiary_mi2s_rx_mixer_controls`, `msm-pcm-routing-v2.c:4706`), served by
  several platform drivers: `msm-pcm-dsp.0/1/2`, `-noirq`, compress, VoIP,
  AFE proxy, hostless. Anyone who can open `/dev/snd/controlC0` can route
  any of them to the speaker. Standard sound stacks need that control
  device: alsaucm, alsactl, PipeWire.
- `wavtone`'s refusal and `speaker-test-tone`'s clamp therefore protect only
  those two tools. With F1 fixed, any `aplay` of a full-scale file through
  `hw:` reaches 0 dBFS.

Two ways to make the ceiling hold for any client (decision D2):

- **(a) Lower the DAC control's maximum to 9.** One line in the codec
  (`0x0f` to `0x09`), at the last stage, covering every frontend and route.
  Full scale at 9 dB of gain is the same acoustic maximum as -6 dBFS at
  15 dB of gain (0 + 9 = -6 + 15). That relies on the TAS2560 gain being
  linear up to full scale, with no clipping or limiter inside the amp at
  9 dB with a 0 dBFS input. L0 and the TAS2560 datasheet have to confirm
  that. It also changes what the limits mean: the "-6 dBFS" figure becomes
  "no more than +9 dB above full scale at 0 dB gain". Existing levels map
  across: today's -20 dBFS at DAC 10 equals -14 dBFS at DAC 4, or
  -19 dBFS at DAC 9.
- **(b) Cap the ASM gain and lock the route.** In `msm-pcm-q6-v2`, apply
  0x1000 (-6.02 dB) at prepare and clamp the control's put and info to
  0x1000. In `msm-pcm-routing-v2`, refuse (`-EPERM`) any
  `TERT_MI2S_RX Audio Mixer` input but MultiMedia1. This keeps the limits'
  current wording, but it is two patches. It depends on the stream's
  topology actually including the ASM volume module (an L0 question). It
  holds after dmix mixing only because dmix sums into the one MultiMedia1
  stream, which the DSP then attenuates.

### F4. Package costs, Alpine 3.24 aarch64 (computed from APKINDEX against the current `PKGS`)

| Set | New packages | Installed |
|---|---|---|
| `alsa-lib` | 1 | +1.3 MiB |
| `alsa-lib alsa-ucm-conf` | 2 | +2.0 MiB |
| `alsa-utils` (aplay, speaker-test, alsaucm, amixer, alsactl; pulls `alsa-ucm-conf`, fftw, dialog, ncurses form/menu/panel) | 8 | +6.3 MiB |
| `pipewire wireplumber` | 45 | **+245.5 MiB** (llvm22-libs 168 MiB, mesa 36 MiB, libcamera 2.9 MiB, X11 libraries) |
| the above + `pipewire-alsa pipewire-pulse pipewire-spa-bluez` | 69 | +255.6 MiB |
| `sound-theme-freedesktop` | 1 | +0.4 MiB |

`pipewire-libs` 1.6.8 depends on `so:libcamera.so.0.7`, which pulls in Mesa
and LLVM. That cost was not known when PipeWire was used by hand for A2DP.
The RAM image has about 24 MB of margin ([boot](../features/boot.md)), so it
cannot carry PipeWire at all. `system_a` (2560 MiB, about 107 MiB used) could,
but only by carrying a GPU stack the device cannot use. Cutting libcamera
would take a stub package that provides the `so:` names, or our own
PipeWire build, and either one is a maintenance cost. `tinyalsa` (264 KiB)
stays for diagnostics.

### F5. Card, frontends and today's wiring

- Card id `sdm660sndcard`, driver and name `sdm660-snd-card`
  (`sdm660-internal.c:3142`). MultiMedia1, MultiMedia2 and MultiMedia9 are
  all `msm-pcm-dsp.0` (the copy and mmap driver above). MultiMedia5 is
  `msm-pcm-dsp.1` (low latency).
- `audio-up` is started by the one-shot `sensors-up` service and stays
  resident only to supervise a `servreg-locator` when `modem-up` has not
  started one. Both scripts carry matching "who mounted /firmware, who runs
  the locator" logic.
- `/etc/group` has `audio:x:18:`, with no members. No udev rule in the
  overlay mentions `sound`. The device nodes' owner and mode are an L0
  question.
- `etc/wireplumber/wireplumber.conf.d/90-chef-cyclo.conf` keeps a
  hand-installed PipeWire to Bluetooth A2DP: no ALSA monitor.
- The settings file has `alert_volume` (0 to 100, default 60),
  `alert_sounds`, `quiet_paused` and `haptics`. There is no switch for
  system sounds.
- busybox init's `::shutdown` lines run `chef-state shutdown`, then
  `log-save`, then `openrc shutdown`. A shutdown tone has to play before the
  sound service stops, and inside the 2 s normal shutdown target
  ([storage](../features/storage.md)).

### F6. Event sounds

The freedesktop sound theme and naming specifications allow WAV files in a
theme directory: `/usr/share/sounds/<theme>/index.theme`, plus
`stereo/<name>.wav`, falling back to the `freedesktop` theme. The names
needed are `system-bootup` and `system-shutdown`, and later
`complete`/`dialog-warning` etc. for the ride. The standard player is
libcanberra's `canberra-gtk-play`, which needs GTK; Alpine has no
`libcanberra-utils`. A small resolver in shell (theme, fallback, name, then
`aplay -q`) is the honest substitute.

## Proposed decisions

### D1. ALSA only: dmix, UCM2, no sound server in the image *(needs the user)*

`alsa-lib` + `alsa-ucm-conf` + `alsa-utils` in `PKGS` (+6.3 MiB; or
+2.0 MiB with `alsa-utils` added only for tests, but then `alsaucm` and
`aplay` are missing at runtime). One `/etc/asound.conf`:

- `pcm.!default` = `plug` → `dmix` on `hw:sdm660sndcard,0` (MultiMedia1;
  confirm the device number in L0), 48000 Hz, S16_LE, 2 ch, `ipc_key`
  fixed, `ipc_perm 0660`, `ipc_gid audio`, period and buffer sizes from L0.
- `ctl.!default` on the same card.
- A `softvol` for the user volume only (D5). The ceiling is in the kernel
  (D2), never in this file: a client can always open `hw:` directly.

PipeWire stays an optional `apk add` for A2DP, as today. When it is
installed, its drop-in may later be allowed to use the board card through
the ALSA `default` device. Not in this step.

Rejected: PipeWire/WirePlumber as the system server (F4: +245 MiB, it cannot
go in the RAM image, and it adds idle cost on a device whose only output is
alert beeps).

### D2. The ceiling in the kernel *(needs the user: (a) or (b))*

Recommended: **(a)**, the DAC maximum at 9, if L0 and the datasheet confirm
linear gain to full scale. Otherwise (b). Either way the kernel also gets the
F1 copy-path fix, in the same build (one kernel number). `wavtone` keeps
refusing levels above -6 dBFS (defence in depth, and its tests stay valid).
`speaker-test-tone` keeps working, but its `-v` range follows the new
control maximum.

### D3. Routes by UCM2

`/usr/share/alsa/ucm2/conf.d/sdm660-snd-card/sdm660-snd-card.conf` (the
exact directory and file name come from `alsaucm listcards` in L0), with
verb `HiFi` and device `Speaker`:

- `EnableSequence`: `TERT_MI2S_RX Audio Mixer MultiMedia1` 1, and `DAC
  Playback Volume` at the fixed production value (D2 decides 9, or 10
  today).
- `DisableSequence`: the route 0.
- `PlaybackPCM "hw:${CardId},0"`.
- No `PlaybackVolume`/`PlaybackMixerElem`: the DAC is fixed, not a user
  volume.

The sound service (D4) runs `alsaucm -c sdm660-snd-card set _verb HiFi set
_enadev Speaker` at start and `set _verb _disabled` (or the DisableSequence)
at stop. No `tinymix` in production. `speaker-test-tone` stays a diagnostic
and keeps its own route handling.

L0 has to show that holding the route enabled while nothing plays costs
nothing at idle. DAPM should keep the AFE port down without an active
frontend, but measure it. If it does cost something, the route is set per
stream instead, through a small wrapper. Ask before choosing that.

### D4. Services: `servreg-locator`, `adsp`, `audio`

- **`servreg-locator`**: its own supervised service. `modem-up` and `adsp`
  `need` it, and both scripts lose their "start a locator only if 0x40 is
  not served" logic.
- **`adsp`**: replaces `audio-up`'s role. It mounts `/firmware` (the
  shared-mount rule with `modem-up` stays), sets the firmware path and
  `restart_level`, writes `boot_adsp`, and then waits, bounded, in
  `start_post` for the card. It is one-shot, with nothing resident. The
  `sensors-up` service `need`s `adsp` and keeps only its own work.
  `audio-up` remains as a thin by-hand wrapper or is removed (the
  implementer asks which; the tests follow).
- **`audio`**: `need adsp`. Start: wait for `/dev/snd/pcmC*D*p` of the card,
  apply the UCM verb and device. Stop: disable the route. One-shot (dmix
  needs no daemon).

Readiness stays bounded `start_post` waits, as in step 7 (no `notify=`).

### D5. Permissions and volume

- udev: `SUBSYSTEM=="sound", GROUP="audio", MODE="0660"`, unless L0 shows
  eudev's default rules already do this.
- `chefui` and the future recorder user join `audio`. Nothing plays as root
  except boot and shutdown hooks that already run as root.
- The user volume (`alert_volume`) is a `softvol` control in front of the
  dmix (D1), the standard ALSA way: `pcm.!default` = `plug` → `softvol`
  (control `Alert Playback Volume`, `max_dB 0.0`, `min_dB -40.0`) → `dmix`.
  It only attenuates, so it can never raise anything above the kernel
  ceiling. The `audio` service applies the stored value at start, and
  chef-settings (as `chefui`, now in `audio`) sets it with `amixer` or
  alsa-lib when the slider moves. The mapping from 0 to 100 % onto dB is the
  implementer's question to the coordinator. The theme files are generated
  at one fixed level: D2's default level, the equivalent of today's
  -20 dBFS at DAC 10. *(Needs the user: whether system sounds follow
  `alert_volume`, or bypass the softvol at one fixed level.)*

### D6. Sound theme `chef-cyclo`, boot and shutdown tones

- `wavtone` gains note sequences: `-n "f:ms,f:ms,..."` with fixed ramps
  (no clicks), still 48 kHz/S16_LE/stereo, still refusing above -6 dBFS.
  The pure parts get host tests (sequence parsing, duration, peak, ramps).
- Built at image build time into
  `/usr/share/sounds/chef-cyclo/{index.theme,stereo/system-bootup.wav,stereo/system-shutdown.wav}`.
  `Inherits=freedesktop` is listed only if `sound-theme-freedesktop` is
  installed (it is not proposed).
- `chef-sound <name>`: resolves the name per the spec and plays it with
  `aplay -q` on `default`. It does nothing (exit 0, one log line) when the
  sound switch is off or the card is missing.
- Boot tone: an OpenRC service `boot-sound` (`need audio`, `after` the
  status bar is lit, if the user wants it tied to the screen). It is
  skipped on a charger-mode boot (`androidboot.mode=charger`, if that is
  what the cmdline shows on this device: an L0 question).
- Shutdown tone: a bounded `::shutdown` line (`timeout -k 1 2 chef-sound
  system-shutdown`) before `openrc shutdown`. Keep it at 0.5 s or less of
  audio: the 2 s shutdown target counts it. *(Needs the user: before or
  after `chef-state shutdown`.)*
- Which switch silences them is decided under D5: `alert_sounds`, or a new
  `system_sounds` key (settings version 4).

### D7. The alert player: no `alertd` now *(needs the user)*

With dmix, alerts can overlap without fighting over the PCM, and the route
belongs to the `audio` service. What is left of `alertd` (semantic events,
priorities, cooldowns, coalescing) belongs to the ride app's own queue, which
plays theme names through the same path. A resident player is reconsidered
only if L0's open-to-sound latency is too high for navigation prompts.
`connectivity-and-sensors.md#audio` is revised accordingly when this step
closes.

## L0: live research on the phone (before M1)

Device agent, current installed image, `audio-up` running. Record every
command and its output in the evidence log.

1. `cat /proc/asound/cards`, `/proc/asound/card0/id`, `/proc/asound/pcm`,
   `ls -l /dev/snd` (owner, group, mode), and eudev's rules for `sound`
   (`grep -rn sound /lib/udev/rules.d /etc/udev/rules.d`).
2. `apk add alsa-utils` (RAM only), then:
   - `alsaucm listcards` (the UCM lookup name);
   - `aplay -D hw:sdm660sndcard,0 -v` of a -20 dBFS `wavtone` file, route
     and DAC set with tinymix as `speaker-test-tone` does. Confirm the XRUN
     loop with the stock alsa-lib client.
   - The same file through an ad hoc dmix `~/.asoundrc` (mmap slave). This
     shows whether the fallback in F1 works on the current kernel. Record
     `status` while it plays, xruns, wall-clock time, and the kernel's
     `payload size` count.
   - Two simultaneous `aplay`s through the dmix (mixing, `ipc_perm` as a
     non-root user in `audio`, using `su` with a test user).
   - `hw_params` of each: period and buffer sizes the driver accepts
     (`PLAYBACK_MIN_PERIOD_SIZE` 128 B to 122880 B, 2 to 8 periods).
3. Open-to-first-sound latency for a 100 ms tone: `time aplay` through dmix,
   with the route held on. Compare the 1.69 s recorded for `tinyplay`.
4. Idle current with the route held enabled and no stream, against the
   route off: A/B per the
   [method](power-and-reliability.md#suspend-and-idle-power), screen off.
5. For D2: at DAC 9 with a 0 dBFS file, against DAC 15 with a -6 dBFS file
   (the same nominal acoustic level), listen for audible clipping or
   distortion at full scale. Measure peak SPL with a phone meter app at a
   fixed distance if one is available. **This plays a full-scale file**, so
   do it only after the user has agreed to D2 (a), keep each tone at 0.5 s
   or less, and play nothing above that level. If (b): write
   `Playback 0 Volume` (the MultiMedia1 ASM volume; find the exact name with
   `amixer contents`) to 0x1000 during a -6 dBFS stream and confirm that the
   level drops audibly. That tests whether the ASM volume module is in the
   topology.
6. `cat /proc/cmdline` on a boot with the charger attached and the phone
   powered off (charger mode), if that boot reaches our image at all.

L0's notes go in the evidence directory and as a dated block at the end of
this file. The coordinator turns them into final decisions before M1.

## Milestones

1. **M1 kernel**: the F1 copy-path change and the D2 ceiling (a or b). Host:
   the kernel builds and boots (`kmake`); nothing else in the tree changes.
   Live (device agent): `aplay -D hw:...` plays without XRUN; a full-scale
   client is held to the ceiling (D2's own check); `tinyplay -M` and
   `speaker-test-tone` unchanged; sensors unaffected.
2. **M2 services**: `servreg-locator`, `adsp`, `audio` (D4); `sensors-up`
   and `modem-up` adjusted; tests updated (`test_audio-up.sh`,
   `test_modem-up.sh`, `test_sensors-up.sh`, `test_sys_sh.py`). `rc-service
   <each> restart` has no side effects on the others; `kill -9` of the
   locator restarts it.
3. **M3 ALSA stack**: packages (D1), `asound.conf`, the UCM2 profile (D3),
   udev rule and group membership (D5), and the WirePlumber drop-in comment
   updated. Host: a test that parses `asound.conf` with the host's
   alsa-lib, if present (`aplay -L` with `ALSA_CONFIG_PATH`), and the UCM
   file with `alsaucm -c` against a dummy card, if feasible. Otherwise a
   syntax test.
4. **M4 wavtone sequences + theme**: D6's builder, files and `chef-sound`.
   Host tests for sequences and the resolver (theme, fallback, missing
   name, switch off, card missing).
5. **M5 boot and shutdown tones**: `boot-sound`, the `::shutdown` line, the
   charger-mode skip, and the settings switch (D5/D6).
6. **M6 docs**: the audio guide rewritten for the standard path (limits
   section updated for D2), services, users and permissions, runtime files,
   building (packages), the repository layout, the alert-player plan
   revised (D7), and standard-interfaces step 10 marked done.

## Live check (device agent, after M6)

On the installed pair:

- `aplay` and `speaker-test -c 2 -t sine -l 1` as a non-root user in
  `audio` play to the speaker, with no XRUNs (`status` shows `RUNNING`,
  the xrun count is 0) and no `payload size` storm.
- A full-scale client is held under the ceiling (D2's check).
- `alsaucm` sets and clears the route; `tinymix` shows the route at 0
  after `rc-service audio stop`.
- The boot tone plays once, after the status bar; it is silent with the
  switch off and on a charger boot. The shutdown tone plays, and shutdown
  stays within its normal time (measure 10 reboots against the current
  image).
- Sensors still stream; GPS lease unaffected; idle current unchanged within
  the A/B method.
- Boot to a lit status bar no slower than the current image.

## Open questions for the user (summary)

1. D1: ALSA only, with PipeWire left as an optional A2DP add-on. Include
   `alsa-utils` (+6.3 MiB) in the image?
2. D2: the ceiling as DAC maximum 9 (one line, reinterprets "-6 dBFS"), or
   an ASM gain cap with a route lock (keeps the wording, two patches)?
3. D5/D6: do the boot and shutdown tones follow `alert_sounds` and
   `alert_volume`, or get their own switch at a fixed level?
4. D6: the shutdown tone before or after `chef-state shutdown`; the boot
   tone tied to the lit status bar or just to the card?
5. D7: drop `alertd` in favour of theme names and the ride app's queue?
