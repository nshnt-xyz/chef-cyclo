#!/bin/sh
# Build out/initramfs.cpio.gz: the Alpine rootfs from scripts/mkrootfs.sh
# with the initramfs/ overlay (init, inittab, users, bt-up, BlueZ config),
# the btprobe helper and the WCN3990 firmware on top.
set -eu
cd "$(dirname "$0")/.."
[ -x out/rootfs/bin/busybox ] || { echo "run scripts/mkrootfs.sh first" >&2; exit 1; }

ROOT=out/initramfs-root
rm -rf "$ROOT"
mkdir -p "$ROOT"
cp -a out/rootfs/. "$ROOT"/
mkdir -p "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev "$ROOT"/tmp "$ROOT"/run \
         "$ROOT"/root "$ROOT"/mnt "$ROOT"/var/lib/dbus
cp -a initramfs/. "$ROOT"/
chmod 755 "$ROOT"/init "$ROOT"/usr/bin/bt-up

# Small libc-free helper used by bt-up to exercise /dev/btpower and the
# WCN3990 UART; also a raw H4/QCA attach and LE scan for debugging without
# BlueZ.
BTCC=toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-gcc
"$BTCC" -Os -static -nostdlib -fno-stack-protector \
    -o "$ROOT/usr/bin/btprobe" tools/btprobe.c

# Pull the device-matched WCN3990 firmware from the verified stock partition;
# hci_qca requests qca/crbtfw21.tlv and qca/crnv21.bin by ROM version.
BTIMG=stock/partitions/bluetooth_a.img
if [ -r "$BTIMG" ] && command -v debugfs >/dev/null 2>&1; then
    mkdir -p "$ROOT/lib/firmware/qca"
    debugfs -R "dump /image/crbtfw21.tlv $ROOT/lib/firmware/qca/crbtfw21.tlv" \
        "$BTIMG" >/dev/null 2>&1
    debugfs -R "dump /image/crnv21.bin $ROOT/lib/firmware/qca/crnv21.bin" \
        "$BTIMG" >/dev/null 2>&1
else
    echo "warning: $BTIMG not readable, no Bluetooth firmware in the image" >&2
fi

# newc format, everything owned by root, reproducible ordering.
( cd "$ROOT" && find . -print0 | LC_ALL=C sort -z \
    | cpio -0 -o -H newc --owner=+0:+0 --quiet ) | gzip -9n > out/initramfs.cpio.gz
ls -l out/initramfs.cpio.gz
