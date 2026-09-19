#!/bin/sh
# Populate out/rootfs with Alpine aarch64 packages: musl, busybox (+extras for
# telnetd/udhcpd), dbus, BlueZ, qmicli and gpsd. apk only unpacks archives, so the host's
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
# gpsd (README next-steps item 7): 3.27.3-r1, /usr/sbin/gpsd + libgps.so.32,
# pulls libcap2 + libstdc++ (+libgcc); 551 KB apk, 1.2 MB installed. Fed over
# loopback UDP by tools/nmea-broker.c. No gpsd-clients (3.7 MB, ncurses):
# verify with busybox nc on 2947 instead.
PKGS="alpine-baselayout musl busybox busybox-extras
      dbus bluez bluez-btmgmt bluez-btmon bluez-deprecated
      qmi-utils gpsd"

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
   "$ROOT/usr/bin/qmicli" "$ROOT/usr/sbin/gpsd" "$ROOT/usr/lib/libgps.so.32" \
   "$ROOT/bin/busybox" "$ROOT/bin/busybox-extras"
