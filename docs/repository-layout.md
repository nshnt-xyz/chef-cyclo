# Repository layout

[Project overview](../README.md)

Paths below are relative to the repository root.

| Path | Purpose |
|---|---|
| `Makefile`, `tools/device.mk` | The one build ([build and boot](building.md#one-build-make-device-and-make-test)): `make device`, `make install-device DESTDIR=...`, `make test` (every suite in parallel) and `make clean` across the component Makefiles; `device.mk` holds the phone compiler and flags. Every component writes only under its own `build/`. |
| `kernel/` | Motorola Linux 4.4.192 submodule, project fork branch `chef-cyclo`; device tree `sdm636-chef-evt.dts`. |
| `kernel-config/chef-cyclo.config` | Project fragment appended to Motorola's three-part config stack. |
| `stock/partitions/` | Verified stock backups; includes irreplaceable device-unique partitions. See [device reference](device.md). |
| `stock/twrp-3.7.0_9-0-chef.img`, `stock/twrp-*.txt` | Verified recovery image and stock-kernel observations. |
| `initramfs/` | Alpine overlay laid over `out/rootfs`: the `system_a` root of the installed layout and the ramdisk of the RAM image. `/init`, `inittab`, users, service configuration (NetworkManager and its dispatcher scripts, BlueZ, chrony, WirePlumber), `bt-up`, `gps-up`, and [`audio-up`/`speaker-test-tone`](features/audio.md). |
| `initramfs/usr/bin/chef-storage`, `initramfs/usr/bin/chef-state`, `initramfs/usr/bin/chef-reboot` | [Persistent data](features/storage.md): explicit `userdata` provisioning (boot falls back to RAM); crash records, boot history, the wall clock against the RTC and the magnetometer bias across reboots; the return to fastboot that runs init's shutdown steps first. |
| `initramfs/usr/bin/chef-sshd` | [SSH](features/ssh.md): dropbear, key login only. |
| `initramfs/usr/bin/wifi-*` | [Wi-Fi](features/wifi.md) and its [NetworkManager mode](features/wifi-ui.md): device readiness (`wifi-prepare`), NetworkManager startup (`wifi-nm`), the USB-only address guard (`wifi-usb-guard`), the manual session `wifi-up`/`wifi-down`/`wifi-status` and its DHCP hook `wifi-dhcp`. |
| `initramfs/usr/bin/display-touch-inventory` | Read-only display and touch metadata inventory. See [display and touch](features/display-and-touch.md). |
| `initramfs/usr/lib/chef/` | Shell libraries: `usb-gadget.sh`, the USB NCM gadget shared with `stage1/init` ([USB networking](features/usb-networking.md)), and `modem-owner.sh`, which finds the resident `gps-up` modem owner ([GPS](features/gps.md)). |
| `stage1/init` | Stage-1 `/init` of the installed boot image (`scripts/mkstage1.sh`): checks and mounts the read-only `system_a` root and switch_roots to it, or stays in a rescue shell on USB networking. See [build and boot](building.md). |
| `tools/btprobe.c` | Freestanding power/UART/HCI helper for [Bluetooth](features/bluetooth.md). |
| `tools/fbdev.h` | Shared framebuffer lifecycle, advisory screen-lock and screen-off flag contract. |
| `tools/evdev.h` | Shared evdev discovery and multitouch slot decoding (header-only), used by chefui, `fbtouch` and `buttond`. |
| `tools/chefui/`, `third_party/lvgl/` | [UI platform](features/ui-platform.md): `libchefui` (LVGL v9.6 on the MDSS fbdev: shadow copy and page flips, multitouch reader, buttond client, screen lock/flag/handoff), the `chefui-demo` verification app, the SDL2 host build and host tests; LVGL is a submodule pinned to v9.6.0. |
| `tools/chefui/apps/settings/`, `tools/chefui/kit/`, `tools/chefui/sys/` | [System UI](features/system-ui.md) app `chef-settings` (status bar, settings hub and pages), the UI kit (tokens, fonts, icons, widgets) and the state sources and settings file. |
| `tools/fbtouch.c`, `tools/fblog/` | [Display/touch probe and on-device log screen](features/display-and-touch.md); generated public-domain X11 font included. |
| `tools/buttond.c` | [Button gesture daemon](features/buttons-and-power-off.md) and socket client. |
| `tools/powerd.c` | [Battery daemon](features/battery-and-charging.md): low-battery/over-temperature shutdown, charge throttle, `/run/power` state and log, `powerd status`. |
| `tools/gps-manager.c` | [GNSS demand manager](features/gps.md#use-gnss-gps-manager): leases on `/run/gps-manager.sock` run the LOC session, gpsd and the NMEA pipeline. |
| `tools/blesensd/`, `scripts/ble-sensor-emu.py` | [BLE sensors](features/bluetooth.md#ble-sensors): `blesensd`, the one owner of heart rate, cycling power and speed/cadence sensors (BlueZ over static basu), and the emulated sensors on the PC used to test it. |
| `tools/bt-hid-emu.py` | Host-side HID keyboard test peer for the phone as an HID host. See [Bluetooth](features/bluetooth.md). |
| `tools/abslot.c` | [A/B slot flags](features/boot.md#ab-slot-flags): marks `boot_a` successful in the GPT 30 s into each boot. |
| `tools/rtc-edge.c` | Sub-second wall clock against the PMIC RTC, for `chef-state` ([persistent data](features/storage.md)). |
| `tools/wlan-fw.c`, `tools/wifi-psk.c` | [Wi-Fi](features/wifi.md): WLFW board-data client on AF_MSM_IPC, and the WPA2 PSK derivation (passphrase from stdin, never printed). |
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
| `tools/tests/`, `scripts/tests/`, component Makefiles | Host verification for helpers and lifecycle scripts; `scripts/tests/` covers the build scripts (stamps, image info, loader budget, GPT slots, the SYSVIPC gate, the uncompressed boot image). |
| `scripts/` | Toolchain setup (`setup-toolchain.sh`), kernel environment (`env.sh`), the image builds (`mkrootfs.sh`, `mkinitramfs.sh` with `link-applets.sh` and `build-wifi.sh`, `mkboot.sh`, and for the installed layout `mksystem.sh` with `system-mke2fs.conf`, `mkstage1.sh`, `mkinstall.sh`, `chef-stamp.py`; see [build and boot](building.md)), image metadata (`chef-image-info.py`), GPT slot dumps (`gpt-slots.py`), the uncompressed kernel packer (`mkkernel-uncompressed.py`), the Chef loader-budget check (`check-chef-loader-budget.py`), live-test drivers (`phone-boot.sh`, `phone.py`, see [live testing](live-testing.md)), SSH key setup and gates (`ssh-setup.sh`, `ssh-keys.py`, see [SSH](features/ssh.md)). |
| `secrets/` | Gitignored host-specific inputs: `secrets/ssh/authorized_keys`, the public keys baked into every image ([SSH](features/ssh.md)). |
| `logs/` | Captured device evidence; GPS trace archives may be intentionally gitignored. |
| `toolchain/` | Gitignored GCC 4.9, musl cross compiler, boot tools, Alpine keys and package cache. |
| `out/` | Generated kernel, rootfs, initramfs, and boot images. |
| Branches `experiment/speaker-protection`, `experiment/drm` | Shelved experiments, removed from `main` on 2026-10-18 and kept where they still build (both at `08e1ec9`): the speaker-protection probe (`spk-protect-probe`, `afe-debug`, `afe-topology-cal`, `tert-tx-hold`, `acdb-afe-topology.py`, see [Audio](features/audio.md)) and the DRM route (`scripts/build-drm-experiment.sh`, `scripts/build-drm-diagnostic-initramfs.sh`, `kernel-config/drm-helper-namespace.patch`, see [DRM bring-up](research/drm-bringup.md)). |
| `docs/` | Current feature/build guides, remaining work in `next-steps/`, completed plans in `archive/`, investigations in `research/`, and chronological build log. |
