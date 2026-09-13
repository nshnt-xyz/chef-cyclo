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

# newc format, everything owned by root, reproducible ordering.
( cd "$ROOT" && find . -print0 | LC_ALL=C sort -z \
    | cpio -0 -o -H newc --owner=+0:+0 --quiet ) | gzip -9n > out/initramfs.cpio.gz
ls -l out/initramfs.cpio.gz
