# Build and boot

[Project overview](../README.md) · [Feature guides](features/README.md)

Run host commands from the repository root. These instructions build a temporary boot image; `fastboot boot` does not flash a partition.

## Prerequisites

Use a Linux host with the kernel and `third_party/lvgl` submodules checked out (`git submodule update --init`), the verified device-specific backups in `stock/partitions/`, and the host tools required by the build scripts. `libssl-dev` is needed for the kernel's `sign-file`; `debugfs` (e2fsprogs) is required to extract the TFTP RAM-shadow seed from the stock persist backup, and `xz` (XZ Utils) to encode the LZMA ramdisk. `libsdl2-dev` is needed only for the host build of the UI platform (`make -C tools/chefui host`). `fastboot` is required to boot the resulting image.

`scripts/setup-toolchain.sh` fetches AOSP GCC 4.9 for the kernel and freestanding `btprobe`, a Bootlin musl aarch64 cross compiler for libc-based helpers, AOSP boot-image tools, and Alpine bootstrap assets into gitignored `toolchain/`.

## Baseline image

```sh
scripts/setup-toolchain.sh   # once: fetches AOSP GCC 4.9 aarch64 into toolchain/ (gitignored)
. scripts/env.sh             # defines kmake and chef_defconfig
chef_defconfig               # sdm660-perf_defconfig + moto-sdm660.config + moto-sdm660-chef.config + chef-cyclo.config -> out/kernel/.config
kmake -j$(nproc)             # Image.gz-dtb comes out in out/kernel/arch/arm64/boot/
scripts/mkrootfs.sh          # Alpine aarch64 musl/busybox/dbus/BlueZ -> out/rootfs (once, or after changing the package list)
scripts/mkinitramfs.sh       # out/rootfs + initramfs/ overlay + helpers + firmware -> out/initramfs.cpio.lzma
scripts/mkboot.sh            # -> out/boot.img (header values from stock boot_a.img)
fastboot boot out/boot.img   # nothing is flashed; a reboot runs what boot_a holds
```

For test runs, `scripts/phone-boot.sh [IMAGE]` gets the phone into fastboot from any state, boots the image and waits for its USB network, and `scripts/phone.py` runs commands, pushes and pulls files over the USB shell. See [live testing](live-testing.md).

The kernel config combines stock Android's `sdm660-perf_defconfig`, `moto-sdm660.config`, `moto-sdm660-chef.config`, and the project fragment `kernel-config/chef-cyclo.config`. `chef_defconfig` writes `out/kernel/.config`. The stock `/proc/config.gz` matches the first three inputs exactly; the project fragment adds the standalone-Linux requirements.

`mkrootfs.sh` builds an Alpine aarch64 root with musl, BusyBox, D-Bus, BlueZ, QMI tools, gpsd, tinyalsa, wpa_supplicant, iw, e2fsprogs/e2fsprogs-extra and the [Wi-Fi UI/diagnostic utilities](features/wifi-ui.md). Rerun it when the package list changes; roots created before the 2026-09-19 gpsd addition, or before the same-day tinyalsa addition, need rebuilding.

`mkinitramfs.sh` overlays `initramfs/` and cross-builds the device helpers. GPS requires RMTFS, IRSC, SERVREG-LOCATOR, TFTP/RFS, and QMUX support. Missing mandatory sources, failed cross-builds, a missing persist seed, a rootfs without gpsd, or a rootfs without tinymix/tinyplay abort image generation. `fbtouch`, `fblog`, the [UI platform](next-steps/ui-platform.md) demo `chefui-demo` (baseline image only; built by `make -C tools/chefui device` against the LVGL submodule), `nmea-broker`, `buttond`, the [audio helpers](features/audio.md) `audio-up`/`speaker-test-tone`/`wavtone`/`tas2560-send-cal` the speaker-protection experiment `spk-protect-probe`/`afe-debug`/`afe-topology-cal`/`tert-tx-hold`, the [sensors](features/sensors.md) bring-up `sensors-up` and daemon `sensord`, and `abslot`, which inittab runs 30 s into each boot to mark `boot_a` successful in the GPT ([A/B slot flags](next-steps/storage-and-boot.md#ab-slot-flags)), are also packed. The sensors registry map `/usr/share/sensord/sns_reg.map` is generated at build time by `tools/sns-reg-map.py` from `sensors.qti` in `stock/partitions/vendor_a.img` (read with debugfs); if that fails the build only warns, and the image then has no map and `sensors-up` refuses to start.

`mkboot.sh` uses the stock `boot_a.img` header values and the built kernel/initramfs. `fastboot boot` runs an image once; installing one is `fastboot flash boot_a` (procedure in [standalone boot](next-steps/storage-and-boot.md#procedure-as-run)). A normal reboot runs whatever `boot_a` holds; see [device recovery](device.md#stock-backups-and-recovery).

The ramdisk is LZMA-encoded (`xz --format=lzma -6` of the same deterministic
cpio stream) because Chef's loader gives the kernel only the region space the
ramdisk leaves: the gzip baseline left 68 KiB of margin, so any rootfs growth broke
boot, while LZMA leaves about 13 MB (live-booted 2026-10-03), and about 24 MB since libinput's removal on 2026-10-04. The kernel
must have `CONFIG_RD_LZMA=y`; `mkinitramfs.sh` checks `out/kernel/.config` (or
`KCONFIG`) and `mkboot.sh` checks the packed kernel's build-tree `.config` before
anything is written. Both also require `CONFIG_SYSVIPC=y` (`mkboot.sh` for the default kernel) for the [GPS time](features/gps.md#gps-time) SHM refclock, without which chronyd would not start. `GZIP=1` restores the gzip encoding and the old name
`out/initramfs.cpio.gz` for `mkinitramfs.sh` and `mkboot.sh` alike.

Before writing the boot image, `mkboot.sh` runs the Chef loader-budget preflight.
It validates the first gzip member of `Image.gz-dtb` and checks its ARM64
`image_size` against the fixed region budget observed in Chef's loader, including
the page-rounded encoded ramdisk size. This is a Chef-specific check; see the
[loader investigation](research/chef-loader-kernel-budget.md) for its assumptions.
An overflow aborts before opening the output and suggests shrinking the ramdisk.
Another ramdisk can be selected with `RAMDISK=path/to/ramdisk OUT=path/to/boot.img sh scripts/mkboot.sh`.
To check without packaging, run
`python3 scripts/check-chef-loader-budget.py path/to/Image.gz-dtb path/to/ramdisk`.

## Rebuilding and testing changes

- Kernel/config change: regenerate config if needed, run `kmake`, then rebuild the initramfs and the boot image. `mkinitramfs.sh` rebuilds `wlan.ko` (the only module) against the new `Module.symvers`; with `CONFIG_MODVERSIONS=y` a config change can alter many symbol CRCs (enabling `SYSVIPC` changed 208 of the module's 425), and a stale module would not load.
- Helper or baseline overlay change: rebuild the initramfs and boot image.
- Rootfs package change: rebuild rootfs, initramfs, and boot image.

Run the relevant host tests before packing changed helpers:

```sh
make -C tools test
make -C tools/rmtfs test
make -C tools/servreg-locator test
make -C tools/tftp test
make -C tools/chefui test
```

`make -C tools test` includes `tests/test_abslot.py` (synthetic GPT images), the GPS startup, audio-up, speaker-test-tone, afe-debug, spk-protect-probe and acdb-afe-topology shell suites. For a focused script change, run `sh tools/tests/test_gps-up.sh`, `sh tools/tests/test_audio-up.sh`, `sh tools/tests/test_speaker-test-tone.sh`, `sh tools/tests/test_afe-debug.sh`, `sh tools/tests/test_spk-protect-probe.sh` or `sh tools/tests/test_acdb-afe-topology.sh` directly (python3 is needed for the last one). Component-specific tests and live verification expectations are linked from each feature guide. The storage suite additionally requires host BusyBox (ash), e2fsprogs and GNU timeout. Host success does not imply live device verification. Record image hashes, device results, and evidence paths in the [build log](build-log.md).

After boot, connect through [USB networking](features/usb-networking.md). The baseline starts Bluetooth, the [log screen](features/display-and-touch.md), and [button handling](features/buttons-and-power-off.md); [GPS](features/gps.md) remains manual.

## Build and boot troubleshooting

- **fastboot hangs on bulk transfers** on the ASRock B450 Steel Legend's chipset USB controller (bus 1). Use one of the four blue CPU-attached rear USB 3.1 Gen1 ports (bus 3). A hung send leaves the bootloader confused — Power+VolDown to reset.

- **`skip_initramfs` is appended by the bootloader** at runtime (with `root=/dev/mmcblk0p67 rootwait ro init=/init`); it is not in the boot image header, so it can't be stripped. Our kernel ignores it (as TWRP's does).

- **Motorola's production config stack has three parts**: `sdm660-perf_defconfig` + `moto-sdm660.config` + `moto-sdm660-chef.config`, followed by our project fragment. The non-perf `sdm660_defconfig` enables many debug and fault-injection options that stock Android does not use. Missing the middle Motorola fragment silently drops `BOOTINFO` (build breaks in `cpuinfo.c`); the Novatek touch driver comes from the chef fragment.

- **Old tree on a new host:** `scripts/gcc-wrapper.py` is python2-only on its warning path and aborts on *any* warning → bypass with `CC=`; bundled dtc needs `-fcommon`; the Makefile's `PYTHON`/`HOSTCFLAGS` use `=` so they must be overridden on the make command line, not the environment. All in `kmake`.

- **AOSP GCC 4.9 prebuilt:** `gcc`/`g++` are python2 wrapper scripts → symlink to the UUID-named real drivers (`setup-toolchain.sh`).

- **Rootfs from Alpine without qemu:** the host's x86_64 `apk.static` populates an aarch64 root with `--arch aarch64 --usermode --no-scripts`. Skipped post-install scripts mean no busybox applet links (`scripts/link-applets.sh` makes them at build time from the packages' own `etc/busybox-paths.d` lists, since the installed root is read-only), no `messagebus` user (in `initramfs/etc/passwd`), no setuid D-Bus launch helper mode (`scripts/mksystem.sh` sets `root:messagebus 4750` under fakeroot; `/init` sets it on the writable RAM root) and no D-Bus machine-id (`dbus-uuidgen --ensure` in `/init`, on the tmpfs `/var`). `telnetd`/`udhcpd` live in `busybox-extras`.

- **DTB targets** are relative to `arch/arm64/boot/dts`: `kmake qcom/sdm636-chef-evt.dtb`.

- **zsh doesn't word-split unquoted variables** — build scripts use `set --`/`"$@"` for lists so they work under both zsh and bash.

`kmake` wraps `make` in `kernel/` with a separate `out/kernel` output directory and the required cross compiler/host overrides. For a device-tree-only build, use `kmake qcom/sdm636-chef-evt.dtb`.

## Firmware and distribution

Images contain stock firmware and device-specific persist/EFS seed material. Before distributing a repository or image, audit which blobs and device data it contains; extract required blobs from the recipient's own stock backup at build time. The existing build already uses local stock backups. Raw GPS trace archives are local evidence, not public documentation.

## Phase 2 storage image

Rebuild the rootfs after adding `e2fsprogs` and `e2fsprogs-extra`, then build
the full initramfs as usual. The overlay contains `chef-storage`, its dedicated
mke2fs feature configuration and e2fsck broken-clock policy. The build refuses
a rootfs missing its required filesystem tools. Keep the known-good
`out/boot.img` unchanged and package the test separately:

```sh
scripts/mkrootfs.sh
scripts/mkinitramfs.sh
OUT=out/boot-data.img scripts/mkboot.sh
scripts/phone-boot.sh out/boot-data.img
```

`make -C tools test` includes rootless storage refusal/boot decision tests and
a real ext4 image feature/marker/fsck check. Phone kernel feature proof and
pre-format superblock evidence precede any userdata format. See
[storage](features/storage.md) for provisioning, boot fallback and shutdown.
In phase 2 the root remained in the RAM disk and this build did not create or write a
`system_a` image.

## Installed layout (phase 3)

Since phase 3 of the [install layout](next-steps/install-layout-handoff.md) the
OS lives on `system_a` as a read-only ext4 root, and `boot_a` holds the kernel
with a small stage-1 ramdisk. One command builds everything with matching
stamps:

```sh
scripts/mkinstall.sh           # kmake first; SKIP_KERNEL=1 uses the existing kernel build
```

| Output | What |
|---|---|
| `out/system_a.img` (+ `.stamp`, `.manifest`) | raw ext4, exactly 2684354560 bytes (the partition), label `chefroot`, no journal |
| `out/boot-stage1.img` | kernel + `out/stage1.cpio.gz` (about 1 MB gzip), for `boot_a` with that `system_a` |
| `out/boot-ram.img` | kernel + the whole OS in the LZMA ramdisk as before; never reads `system_a` |

All three come from one staged tree, `out/initramfs-root` (`mkinitramfs.sh`),
so the RAM image and `system_a` carry the same files. `out/boot.img` is not
written; promoting an image to the baseline stays a separate decision.

- **`scripts/mksystem.sh`** copies the tree to `out/system-root` under one
  fakeroot session (everything `root:root` as in the cpio, the D-Bus launch
  helper `root:messagebus 4750`, mtimes set to the HEAD commit time), writes
  `/etc/chef/build-stamp` and packs it with the host's `/usr/sbin/mke2fs -d`
  (called by full path: the Android SDK `mke2fs` may come first in `PATH`)
  and the pinned features in `scripts/system-mke2fs.conf`. It then reads the
  image back: label, feature list, block count, `e2fsck -fn`, the stamp and
  the helper's owner and mode. For a given staged tree the image is
  byte-for-byte reproducible (`E2FSPROGS_FAKE_TIME`, UUID and hash seed from
  the manifest); a full rebuild is not, because the Wi-Fi module embeds its
  build time.
- **The stamp** (`scripts/chef-stamp.py`) names the kernel release and the
  SHA-256 of `Image.gz-dtb`, `Module.symvers`, `wlan.ko` (whose vermagic must
  match the release) and the tree manifest. Stage 1 compares it byte for byte
  with its own copy; `mkinstall.sh` refuses to write `boot-stage1.img` unless
  the stamp read back from the image, the stage-1 copy and the kernel being
  packed agree.
- **`scripts/mkstage1.sh`** packs `stage1/init`, busybox and busybox-extras
  with the musl loader and their applet links, `btprobe`, the shared USB
  gadget helper (`initramfs/usr/lib/chef/usb-gadget.sh`) and the expected
  stamp. `STAGE1_EXPECTED_STAMP=FILE OUT=out/stage1-x.cpio.gz` builds a
  ramdisk with another expected stamp (the rescue test image).

Flash with `fastboot flash system_a out/system_a.img` (fastboot resparses the
raw image itself; `max-download-size` is 512 MiB) and
`fastboot flash boot_a out/boot-stage1.img`, always with `current-slot` `a`
checked first. Flashing `system_a` also resets slot `_a` to unsuccessful with
7 retries; `abslot` marks it again 30 s into the next boot. A new `system_a`
needs a stage-1 image with its stamp, so flash both from the same build.

Boot timing, 2026-10-05 (kernel unchanged): `Freeing unused kernel memory` at
about 3.5 s and `exec init` at about 5.4 s, against 9.2 s and 9.7 s with the
23 MB LZMA ramdisk; the USB network answers about 10 s after `fastboot boot`.
`boot-stage1.img` is 13484032 bytes and leaves a Chef loader margin of
46374912 bytes, so the 24 MB ramdisk limit no longer constrains the OS.

Host tests: `tools/tests/test_stage1.py` (every stage-1 refusal and the
switch_root path against fake sysfs and real small ext4 images, in
`make -C tools test`) and `scripts/tests/test_chef_stamp.py`
(`python3 -m unittest discover scripts/tests`).
