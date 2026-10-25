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

The decisions below are final: D1 by the user, the rest chosen on the
user's behalf on 2026-10-25 ("Make the choice for me"). D2 has one
condition, settled by L0 step 5; the fallback is already decided, so no one
needs to go back to the user. Anything else still to settle (period sizes,
UCM file name) is the coordinator's call.

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
| `pulseaudio` 17.0 (alone, or with `-alsa`, `-bluez`, `alsa-plugins-pulse`) | 60 to 63 | **+252 to 254 MiB** (the same Mesa and LLVM, through `gst-plugins-base` → `mesa-egl`; `libpulse` itself also links libX11 and libxcb) |
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

### D1. ALSA only: dmix, UCM2, no sound server in the image (user, 2026-10-25)

Decided by the user on 2026-10-25 ("ok straight ALSA it is"), after the
PipeWire and PulseAudio costs in F4.

`alsa-lib` + `alsa-ucm-conf` + `alsa-utils` in `PKGS` (+6.3 MiB). Not
2.0 MiB without `alsa-utils`: the `audio` service needs `alsaucm`, `chef-sound`
needs `aplay`, and the acceptance tests use `aplay` and `speaker-test`.
Writing our own replacements would cost more than the 4.3 MiB saved.
`tinyalsa` stays for diagnostics. One `/etc/asound.conf`:

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
alert beeps). PulseAudio, for the same reasons (F4: +252 MiB, through its
GStreamer module's link to `libgstapp`). It is also in maintenance mode, and
Alpine's desktop default is now PipeWire. Its timer-based scheduling
(`tsched`) would want a finer pointer than this driver's one update per
period, so `tsched=0` would be needed. Either server would still need the
kernel ceiling (D2), because a client can always open `hw:` directly.
Revisit only with our own build without the GStreamer and libcamera
modules.

### D2. The ceiling in the kernel: DAC maximum 9, else ASM cap and route lock

**(a)**, the `DAC Playback Volume` maximum lowered to 9 in
`tas2560-codec.c`, with 9 as the production DAC value. The condition: L0
step 5 hears no clipping or distortion from a 0 dBFS file at DAC 9 that is
absent from a -6 dBFS file at DAC 15, and the TAS2560 datasheet shows no
limiter or gain change between them. If either fails, the coordinator
switches to **(b)** without going back to the user: the ASM gain capped at
0x1000 and only MultiMedia1 routable to TERT_MI2S_RX, with the DAC fixed at
10. That needs L0 step 5's ASM check to show that the ASM volume works. The
audio guide's limits section is rewritten to match whichever applies. Either way the kernel also gets the
F1 copy-path fix, in the same build (one kernel number). `wavtone` keeps
refusing levels above -6 dBFS (defence in depth, and its tests stay valid).
`speaker-test-tone` keeps working, but its `-v` range follows the new
control maximum.

### D3. Routes by UCM2

`/usr/share/alsa/ucm2/conf.d/sdm660-snd-card/sdm660-snd-card.conf` (the
exact directory and file name come from `alsaucm listcards` in L0), with
verb `HiFi` and device `Speaker`:

- `EnableSequence`: `TERT_MI2S_RX Audio Mixer MultiMedia1` 1, and `DAC
  Playback Volume` at the fixed production value (9 under D2 (a), 10 under
  (b)).
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
  `audio-up` is removed; `rc-service adsp start` replaces it by hand, and
  the guide's telnet section follows.
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
  alsa-lib when the slider moves. Mapping: 1 to 100 % is linear in dB from
  -30 dB to 0 dB (softvol `min_dB -30.0`, `max_dB 0.0`, `resolution 101`).
  At 0 %, players play nothing: no softvol is truly silent at its lowest
  step.
- System sounds follow the same settings as alerts: `alert_sounds` off or
  `alert_volume` 0 silences them. There is no new settings key and no
  version bump. One sounds switch is what section 6 of the plan describes,
  and a rider who turns sounds off means every sound. `quiet_paused` does
  not apply to them (no ride is running at boot or shutdown).
- Theme files peak at -10 dBFS. At the default 60 % (-12 dB), they reach
  -22 dBFS at DAC 9, about today's verified -20 dBFS at DAC 10. Under D2 (b),
  the same files at DAC 10 are 1 dB louder, still under the ceiling.

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
- Boot tone: an OpenRC service `boot-sound` (`need audio`). It waits,
  bounded to 30 s, for `/run/chef/fb0.lit` (the status bar is lit), then
  plays `system-bootup` once. The tone means "ready", not "the card is up".
  If the screen never lights, it plays nothing and logs it. It is skipped
  on a charger-mode boot (`androidboot.mode=charger`, or whatever L0 step 6
  shows; with no charger-mode boot into our image, the check is still
  there and documented as untested).
- Shutdown tone: the first `::shutdown` line, before `chef-state shutdown`,
  as `timeout -k 1 2 chef-sound system-shutdown`. It goes first so it plays
  at once, while the `audio` service and settings are still in place. The
  audio is 0.4 s or less. The live check measures the added shutdown time;
  if open-to-sound latency makes it more than 1 s, it is reported, not
  hidden.
- The switch and volume are D5's.

### D7. The alert player: no `alertd`

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
   fixed distance if one is available. **This plays a full-scale file**
   (allowed here, since D2 (a) is decided): keep each tone at 0.5 s or less,
   use a 1 kHz sine and a sweep, and play nothing above that level. Then,
   so the fallback is ready either way, the ASM check: write
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

## Decisions (summary, 2026-10-25)

1. D1: ALSA only (user), with `alsa-utils` in the image.
2. D2: DAC maximum 9, if L0 step 5 passes; otherwise ASM cap and route
   lock at DAC 10.
3. D4: `audio-up` removed in favour of the `adsp` service.
4. D5: system sounds follow `alert_sounds` and `alert_volume`; 1 to 100 %
   maps to -30 to 0 dB; 0 % plays nothing; files at -10 dBFS.
5. D6: the boot tone after the status bar is lit, silent in charger mode;
   the shutdown tone first in `::shutdown`, 0.4 s or less, bounded at 2 s.
6. D7: no `alertd`; the ride app's queue plays theme names.
