# Build and boot

[Project overview](../README.md) · [Feature guides](features/README.md)

Run host commands from the repository root. These instructions build a temporary boot image; `fastboot boot` does not flash a partition.

## Prerequisites

Use a Linux host with the kernel submodule checked out, the verified device-specific backups in `stock/partitions/`, and the host tools required by the build scripts. `libssl-dev` is needed for the kernel's `sign-file`; `debugfs` (e2fsprogs) is required to extract the TFTP RAM-shadow seed from the stock persist backup. `fastboot` is required to boot the resulting image.

`scripts/setup-toolchain.sh` fetches AOSP GCC 4.9 for the kernel and freestanding `btprobe`, a Bootlin musl aarch64 cross compiler for libc-based helpers, AOSP boot-image tools, and Alpine bootstrap assets into gitignored `toolchain/`.

## Baseline image

```sh
scripts/setup-toolchain.sh   # once: fetches AOSP GCC 4.9 aarch64 into toolchain/ (gitignored)
. scripts/env.sh             # defines kmake and chef_defconfig
chef_defconfig               # sdm660_defconfig + moto-sdm660.config + moto-sdm660-chef.config + chef-cyclo.config -> out/kernel/.config
kmake -j$(nproc)             # Image.gz-dtb comes out in out/kernel/arch/arm64/boot/
scripts/mkrootfs.sh          # Alpine aarch64 musl/busybox/dbus/BlueZ -> out/rootfs (once, or after changing the package list)
scripts/mkinitramfs.sh       # out/rootfs + initramfs/ overlay + btprobe + BT firmware -> out/initramfs.cpio.gz
scripts/mkboot.sh            # -> out/boot.img (header values from stock boot_a.img)
fastboot boot out/boot.img   # nothing is flashed; Power+VolDown to get back to Android
```

The kernel config combines `sdm660_defconfig`, `moto-sdm660.config`, `moto-sdm660-chef.config`, and the project fragment `kernel-config/chef-cyclo.config`. `chef_defconfig` writes `out/kernel/.config`.

`mkrootfs.sh` builds an Alpine aarch64 root with musl, BusyBox, D-Bus, BlueZ, QMI tools, gpsd, and tinyalsa. Rerun it when the package list changes; roots created before the 2026-09-19 gpsd addition, or before the same-day tinyalsa addition, need rebuilding.

`mkinitramfs.sh` overlays `initramfs/` and cross-builds the device helpers. GPS requires RMTFS, IRSC, SERVREG-LOCATOR, TFTP/RFS, and QMUX support. Missing mandatory sources, failed cross-builds, a missing persist seed, a rootfs without gpsd, or a rootfs without tinymix/tinyplay abort image generation. `fbtouch`, `fblog`, `nmea-broker`, `buttond`, the [audio helpers](features/audio.md) `audio-up`/`speaker-test-tone`/`wavtone`/`tas2560-send-cal` and the speaker-protection experiment `spk-protect-probe`/`afe-debug`/`afe-topology-cal`/`tert-tx-hold` are also packed.

`mkboot.sh` uses the stock `boot_a.img` header values and the built kernel/initramfs. A normal reboot returns to the flashed OS; see [device recovery](device.md#stock-backups-and-recovery).

## Rebuilding and testing changes

- Kernel/config change: regenerate config if needed, run `kmake`, and repack the boot image.
- Helper or baseline overlay change: rebuild the initramfs and boot image.
- Rootfs package change: rebuild rootfs, initramfs, and boot image.
- Ride overlay change: use [the ride-image build](features/ride-logging.md), which produces separate artifacts.

Run the relevant host tests before packing changed helpers:

```sh
make -C tools test
make -C tools/rmtfs test
make -C tools/servreg-locator test
make -C tools/tftp test
```

`make -C tools test` includes the GPS startup, ride-logger, audio-up, speaker-test-tone, afe-debug, spk-protect-probe and acdb-afe-topology shell suites. For a focused script change, run `sh tools/tests/test_gps-up.sh`, `sh tools/tests/test_ride-logger.sh`, `sh tools/tests/test_audio-up.sh`, `sh tools/tests/test_speaker-test-tone.sh`, `sh tools/tests/test_afe-debug.sh`, `sh tools/tests/test_spk-protect-probe.sh` or `sh tools/tests/test_acdb-afe-topology.sh` directly (python3 is needed for the last one). Component-specific tests and live verification expectations are linked from each feature guide. Host success does not imply live device verification. Record image hashes, device results, and evidence paths in the [build log](build-log.md).

After boot, connect through [USB networking](features/usb-networking.md). The baseline starts Bluetooth, the [log screen](features/display-and-touch.md), and [button handling](features/buttons-and-power-off.md); [GPS](features/gps.md) remains manual.

## Build and boot troubleshooting

- **fastboot hangs on bulk transfers** on the ASRock B450 Steel Legend's chipset USB controller (bus 1). Use one of the four blue CPU-attached rear USB 3.1 Gen1 ports (bus 3). A hung send leaves the bootloader confused — Power+VolDown to reset.

- **`skip_initramfs` is appended by the bootloader** at runtime (with `root=/dev/mmcblk0p67 rootwait ro init=/init`); it is not in the boot image header, so it can't be stripped. Our kernel ignores it (as TWRP's does).

- **Motorola's config stack has three parts**: `sdm660_defconfig` + `moto-sdm660.config` + `moto-sdm660-chef.config`, followed by our project fragment. Missing the middle Motorola fragment silently drops `BOOTINFO` (build breaks in `cpuinfo.c`); the Novatek touch driver comes from the chef fragment.

- **Old tree on a new host:** `scripts/gcc-wrapper.py` is python2-only on its warning path and aborts on *any* warning → bypass with `CC=`; bundled dtc needs `-fcommon`; the Makefile's `PYTHON`/`HOSTCFLAGS` use `=` so they must be overridden on the make command line, not the environment. All in `kmake`.

- **AOSP GCC 4.9 prebuilt:** `gcc`/`g++` are python2 wrapper scripts → symlink to the UUID-named real drivers (`setup-toolchain.sh`).

- **Rootfs from Alpine without qemu:** the host's x86_64 `apk.static` populates an aarch64 root with `--arch aarch64 --usermode --no-scripts`. Skipped post-install scripts mean no busybox applet links (`/init` runs `busybox --install -s` and `busybox-extras --install -s`), no `messagebus` user (in `initramfs/etc/passwd`) and no D-Bus machine-id (`dbus-uuidgen --ensure` in `/init`). `telnetd`/`udhcpd` live in `busybox-extras`.

- **DTB targets** are relative to `arch/arm64/boot/dts`: `kmake qcom/sdm636-chef-evt.dtb`.

- **zsh doesn't word-split unquoted variables** — build scripts use `set --`/`"$@"` for lists so they work under both zsh and bash.

`kmake` wraps `make` in `kernel/` with a separate `out/kernel` output directory and the required cross compiler/host overrides. For a device-tree-only build, use `kmake qcom/sdm636-chef-evt.dtb`.

## Firmware and distribution

Images contain stock firmware and device-specific persist/EFS seed material. Before distributing a repository or image, audit which blobs and device data it contains; extract required blobs from the recipient's own stock backup at build time. The existing build already uses local stock backups. Raw GPS trace archives are local evidence, not public documentation.
