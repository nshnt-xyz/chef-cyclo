#!/bin/sh
# Build the temporary bike-ride GPS logging image: the baseline initramfs plus
# the initramfs-ride/ overlay (ride-logger started once from inittab, busybox
# httpd for log extraction), packed with the same kernel and header values as
# out/boot.img into out/boot-ride.img. The baseline out/boot.img,
# out/initramfs.cpio.gz and out/initramfs-root are not touched. `fastboot boot`
# it like the baseline; never flash it (README, "Temporary bike-ride GPS
# logging image").
set -eu
cd "$(dirname "$0")/.."
sh tools/tests/test_ride-logger.sh
VARIANT=ride sh scripts/mkinitramfs.sh
RAMDISK=out/initramfs-ride.cpio.gz OUT=out/boot-ride.img sh scripts/mkboot.sh
sha256sum out/initramfs-ride.cpio.gz out/boot-ride.img
