#!/bin/sh
# Populate out/rootfs with Alpine aarch64 packages: musl, busybox (+extras for
# telnetd/udhcpd), dbus and BlueZ. apk only unpacks archives, so the host's
# static x86_64 apk does the job with --no-scripts; whatever the skipped
# post-install scripts would have done (users, machine-id) is provided by the
# initramfs/ overlay instead. The package cache lives under toolchain/ so a
# rebuild is offline.
set -eu
cd "$(dirname "$0")/.."
APK=toolchain/apk/apk.static
[ -x "$APK" ] || { echo "run scripts/setup-toolchain.sh first" >&2; exit 1; }

ALPINE=${ALPINE:-https://dl-cdn.alpinelinux.org/alpine/v3.24}
ROOT=out/rootfs
CACHE=$PWD/toolchain/apk/cache
PKGS="alpine-baselayout musl busybox busybox-extras
      dbus bluez bluez-btmgmt bluez-btmon bluez-deprecated
      qmi-utils"

rm -rf "$ROOT"
mkdir -p "$ROOT" "$CACHE"
"$APK" --root "$ROOT" --arch aarch64 --initdb \
       --keys-dir "$PWD/toolchain/apk/keys" \
       --repository "$ALPINE/main" --repository "$ALPINE/community" \
       --cache-dir "$CACHE" --no-scripts --no-interactive --usermode \
       add $PKGS
# Not needed at runtime and only confuses a later apk run on the device.
rm -rf "$ROOT/var/cache/apk" "$ROOT/etc/apk/keys"

du -sh "$ROOT"
ls "$ROOT/usr/lib/bluetooth/bluetoothd" "$ROOT/usr/bin/btattach" "$ROOT/usr/bin/dbus-daemon" \
   "$ROOT/usr/bin/qmicli" "$ROOT/bin/busybox" "$ROOT/bin/busybox-extras"
