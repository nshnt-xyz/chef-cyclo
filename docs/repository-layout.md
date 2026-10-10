# Repository layout

[Project overview](../README.md)

Paths below are relative to the repository root.

| Path | Purpose |
|---|---|
| `kernel/` | Motorola Linux 4.4.192 submodule, project fork branch `chef-cyclo`; device tree `sdm636-chef-evt.dts`. |
| `kernel-config/chef-cyclo.config` | Project fragment appended to Motorola's three-part config stack. |
| `stock/partitions/` | Verified stock backups; includes irreplaceable device-unique partitions. See [device reference](device.md). |
| `stock/twrp-3.7.0_9-0-chef.img`, `stock/twrp-*.txt` | Verified recovery image and stock-kernel observations. |
| `initramfs/` | Alpine overlay: `/init`, users, service configuration, `bt-up`, `gps-up`, [`audio-up`/`speaker-test-tone`](features/audio.md) and the speaker-protection experiment scripts `spk-protect-probe`/`afe-debug`. |
| `tools/btprobe.c` | Freestanding power/UART/HCI helper for [Bluetooth](features/bluetooth.md). |
| `tools/fbdev.h` | Shared framebuffer lifecycle, advisory screen-lock and screen-off flag contract. |
| `tools/chefui/`, `third_party/lvgl/` | [UI platform](next-steps/ui-platform.md): `libchefui` (LVGL v9.6 on the MDSS fbdev: shadow copy and page flips, multitouch reader, buttond client, screen lock/flag/handoff), the `chefui-demo` verification app, the SDL2 host build and host tests; LVGL is a submodule pinned to v9.6.0. |
| `tools/fbtouch.c`, `tools/fblog/` | [Display/touch probe and on-device log screen](features/display-and-touch.md); generated public-domain X11 font included. |
| `tools/buttond.c` | [Button gesture daemon](features/buttons-and-power-off.md) and socket client. |
| `tools/powerd.c` | [Battery daemon](features/battery-and-charging.md): low-battery/over-temperature shutdown, charge throttle, `/run/power` state and log, `powerd status`. |
| `tools/rmtfs/` | BSD upstream port serving EFS with RAM-shadow writes. |
| `tools/msmipc.{c,h}`, `tools/irsc.c`, `tools/qrtr/` | AF_MSM_IPC transport adapter, IRSC gate, and shared QMI support (`qrtr/qmi.c` carries local decoder bounds checks). |
| `tools/qmux.{c,h}`, `tools/qmuxd-lite.c` | libqmi/QMUX bridge to the modem. |
| `tools/servreg-locator/` | Modem service-registry lookup server, QMI service 0x40 instance 0x101. |
| `tools/sensord/`, `initramfs/usr/bin/sensors-up` | [On-board sensors](features/sensors.md): REG2 registry server (RAM copy of persist's `sns.reg`), ADSP SMGR client and `/run/sensord.sock` claim/release API; the bring-up script (persist `blockdev --setro`, copy, `audio-up` if needed). Host tests in `tools/sensord/tests/`. |
| `tools/sensord/compass.{c,h}`, `tools/sensord/compass-replay.c`, `tools/compass-check.py`, `initramfs/usr/bin/sensors-compass-run` | [Compass](features/sensors.md#compass): the tilt-compensated heading filter behind `sensord`'s `heading` channel (pure C, host-tested on synthetic motion), a host tool that replays recorded captures through it, the guided live capture and the host acceptance tool. |
| `tools/mag-cal-check.py`, `initramfs/usr/bin/sensors-magcal-run` | [Magnetometer calibration check](features/sensors.md#magnetometer-calibration-check): guided capture on the phone (full/factory/raw calibration selects and QMAG_CAL through extra `sensord -R` instances) and the host fit/acceptance tool. |
| `tools/sns-idl-dump.py`, `tools/sns-reg-map.py` | Read-only decoders for the stock Sensors1 QMI IDL tables (dumps in `logs/sns-idl-dump-*.txt`) and for the `sns.reg` layout compiled into `sensors.qti` (run by `mkinitramfs.sh` to generate `/usr/share/sensord/sns_reg.map`). |
| `tools/tftp/` | Bounded TFTP/RFS server; read-only firmware and RAM-shadow writable data. |
| `tools/nmea-broker.c` | Checksum-filtered UDP feed to gpsd, optional logging/tee, initial GPS clock step. See [GPS](features/gps.md). |
| `tools/wavtone.c` | Deterministic PCM WAV sine-tone generator for the speaker test. See [Audio](features/audio.md). |
| `tools/tas2560-send-cal.c` | Atomic writer for the TAS2560's write-only five-integer calibration control. |
| `tools/afe-topology-cal.c`, `tools/tert-tx-hold.c`, `tools/acdb-afe-topology.py` | Speaker-protection experiment helpers: resident AFE topology installer, resident TERT_MI2S_TX hostless holder, read-only stock ACDB topology decoder. See [Audio](features/audio.md). |
| `tools/tests/`, component Makefiles | Host verification for helpers and lifecycle scripts. |
| `scripts/` | Toolchain setup, kernel environment, rootfs/initramfs/boot packing, Chef loader-budget check, live-test drivers (`phone-boot.sh`, `phone.py`, see [live testing](live-testing.md)), SSH key setup and gates (`ssh-setup.sh`, `ssh-keys.py`, see [SSH](features/ssh.md)). |
| `secrets/` | Gitignored host-specific inputs: `secrets/ssh/authorized_keys`, the public keys baked into every image ([SSH](features/ssh.md)). |
| `logs/` | Captured device evidence; GPS trace archives may be intentionally gitignored. |
| `toolchain/` | Gitignored GCC 4.9, musl cross compiler, boot tools, Alpine keys and package cache. |
| `out/` | Generated kernel, rootfs, initramfs, and boot images. |
| `docs/` | Current guides, future work, and chronological build log. |
