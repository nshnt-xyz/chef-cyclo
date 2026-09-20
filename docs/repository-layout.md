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
| `initramfs-ride/` | Temporary ride overlay: `ride-logger`, alternate inittab, HTTP log extraction. |
| `tools/btprobe.c` | Freestanding power/UART/HCI helper for [Bluetooth](features/bluetooth.md). |
| `tools/fbdev.h` | Shared framebuffer lifecycle and advisory screen-lock contract. |
| `tools/fbtouch.c`, `tools/fblog/` | [Display/touch probe and on-device log screen](features/display-and-touch.md); generated public-domain X11 font included. |
| `tools/buttond.c` | [Button gesture daemon](features/buttons-and-power-off.md) and socket client. |
| `tools/rmtfs/` | BSD upstream port serving EFS with RAM-shadow writes. |
| `tools/msmipc.{c,h}`, `tools/irsc.c`, `tools/qrtr/` | AF_MSM_IPC transport adapter, IRSC gate, and shared QMI support. |
| `tools/qmux.{c,h}`, `tools/qmuxd-lite.c` | libqmi/QMUX bridge to the modem. |
| `tools/servreg-locator/` | Modem service-registry lookup server, QMI service 0x40 instance 0x101. |
| `tools/tftp/` | Bounded TFTP/RFS server; read-only firmware and RAM-shadow writable data. |
| `tools/nmea-broker.c` | Checksum-filtered UDP feed to gpsd, optional logging/tee, initial GPS clock step. See [GPS](features/gps.md). |
| `tools/wavtone.c` | Deterministic PCM WAV sine-tone generator for the speaker test. See [Audio](features/audio.md). |
| `tools/tas2560-send-cal.c` | Atomic writer for the TAS2560's write-only five-integer calibration control. |
| `tools/afe-topology-cal.c`, `tools/tert-tx-hold.c`, `tools/acdb-afe-topology.py` | Speaker-protection experiment helpers: resident AFE topology installer, resident TERT_MI2S_TX hostless holder, read-only stock ACDB topology decoder. See [Audio](features/audio.md). |
| `tools/tests/`, component Makefiles | Host verification for helpers and lifecycle scripts. |
| `scripts/` | Toolchain setup, kernel environment, rootfs/initramfs/boot packing, ride image build. |
| `logs/` | Captured device evidence; GPS trace archives may be intentionally gitignored. |
| `toolchain/` | Gitignored GCC 4.9, musl cross compiler, boot tools, Alpine keys and package cache. |
| `out/` | Generated kernel, rootfs, initramfs, and boot images. |
| `docs/` | Current guides, future work, and chronological build log. |
