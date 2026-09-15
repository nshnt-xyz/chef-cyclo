#!/bin/sh
# Build out/initramfs.cpio.gz from initramfs/ plus the static busybox.
set -eu
cd "$(dirname "$0")/.."
BB=toolchain/busybox/busybox
[ -x "$BB" ] || { echo "run scripts/setup-toolchain.sh first" >&2; exit 1; }

ROOT=out/initramfs-root
rm -rf "$ROOT"
mkdir -p "$ROOT"/bin "$ROOT"/sbin "$ROOT"/usr/bin "$ROOT"/usr/sbin \
         "$ROOT"/etc "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev "$ROOT"/tmp \
         "$ROOT"/root "$ROOT"/mnt
cp -a initramfs/. "$ROOT"/
install -m 755 "$BB" "$ROOT"/bin/busybox
ln -s busybox "$ROOT"/bin/sh   # /init's #! line; the rest is --install'ed at boot
chmod 755 "$ROOT"/init

# Small libc-free helper used by bt-bringup to exercise /dev/btpower and the
# WCN3990 UART before a full BlueZ userspace exists.
BTCC=toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-gcc
"$BTCC" -Os -static -nostdlib -fno-stack-protector \
    -o "$ROOT/usr/bin/btprobe" tools/btprobe.c
chmod 755 "$ROOT/usr/bin/bt-bringup"

# Pull the device-matched WCN3990 firmware from the verified stock partition.
BTIMG=stock/partitions/bluetooth_a.img
if [ -r "$BTIMG" ] && command -v debugfs >/dev/null 2>&1; then
    mkdir -p "$ROOT/lib/firmware/qca"
    debugfs -R "dump /image/crbtfw21.tlv $ROOT/lib/firmware/qca/crbtfw21.tlv" \
        "$BTIMG" >/dev/null 2>&1
    debugfs -R "dump /image/crnv21.bin $ROOT/lib/firmware/qca/crnv21.bin" \
        "$BTIMG" >/dev/null 2>&1
fi

# newc format, everything owned by root, reproducible ordering.
( cd "$ROOT" && find . -print0 | LC_ALL=C sort -z \
    | cpio -0 -o -H newc --owner=+0:+0 --quiet ) | gzip -9n > out/initramfs.cpio.gz
ls -l out/initramfs.cpio.gz
