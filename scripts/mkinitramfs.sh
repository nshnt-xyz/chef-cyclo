#!/bin/sh
# Build out/initramfs.cpio.gz: the Alpine rootfs from scripts/mkrootfs.sh
# with the initramfs/ overlay (init, inittab, users, bt-up, BlueZ config),
# the btprobe helper and the WCN3990 firmware on top.
set -eu
cd "$(dirname "$0")/.."
[ -x out/rootfs/bin/busybox ] || { echo "run scripts/mkrootfs.sh first" >&2; exit 1; }

ROOT=out/initramfs-root
rm -f out/initramfs.cpio.gz
rm -rf "$ROOT"
mkdir -p "$ROOT"
cp -a out/rootfs/. "$ROOT"/
mkdir -p "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev "$ROOT"/tmp "$ROOT"/run \
         "$ROOT"/root "$ROOT"/mnt "$ROOT"/var/lib/dbus
cp -a initramfs/. "$ROOT"/
chmod 755 "$ROOT"/init "$ROOT"/usr/bin/bt-up "$ROOT"/usr/bin/gps-up

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

# GPS/QMI userspace helpers (musl aarch64, real libc unlike btprobe's
# freestanding build): rmtfs and servreg-locator are ours; msmipc.c/irsc.c/
# qmuxd-lite.c/qmux.c and the vendored tools/qrtr/ are the protocol transport
# (see GPS section of README.md for the interface). gps-up is included in
# this image, so all four helpers are mandatory: never emit a bootable GPS
# script without its RMTFS, IRSC, SERVREG-LOCATOR and QMUX counterparts.
# servreg-locator is the userspace SERVREG_LOC (pd-mapper) server that
# answers the modem's boot-time domain-list lookup -- see
# tools/servreg-locator/servreg-locator.c's file header and the README's
# 2026-09-16 (dog timeout) entry for why it exists. Deliberately NOT built
# here: any TFTP/RFS (tqftpserv) server -- out of scope for this probe, see
# that same README entry. msmipc.h pulls in the vendor kernel's verbatim
# <linux/msm_ipc.h>, which needs both uapi/ (for the header itself) and the
# plain include/ (for <linux/compiler.h> etc. it pulls in transitively) --
# using make headers_install output instead would avoid the kernel-headers
# warning, but isn't worth it for two headers.
MUSLCC=toolchain/aarch64-musl/bin/aarch64-buildroot-linux-musl-gcc
QRTR_DIR=tools/qrtr
KHDR="-I kernel/include/uapi -I kernel/include"
[ -x "$MUSLCC" ] || { echo "missing musl cross compiler; run scripts/setup-toolchain.sh" >&2; exit 1; }
for src in tools/msmipc.c tools/irsc.c tools/qmux.c tools/qmuxd-lite.c \
           "$QRTR_DIR/qmi.c" "$QRTR_DIR/logging.c" \
           tools/rmtfs/qmi_rmtfs.c tools/rmtfs/rmtfs.c tools/rmtfs/sharedmem.c \
           tools/rmtfs/storage.c tools/rmtfs/util.c \
           tools/servreg-locator/jsn.c tools/servreg-locator/servreg_loc.c \
           tools/servreg-locator/servreg-locator.c; do
    [ -f "$src" ] || { echo "missing required GPS source: $src" >&2; exit 1; }
done

"$MUSLCC" -Wall -O2 -static $KHDR -I tools/rmtfs -I "$QRTR_DIR" -I tools \
    -o "$ROOT/usr/bin/rmtfs" \
    tools/rmtfs/qmi_rmtfs.c tools/rmtfs/rmtfs.c tools/rmtfs/sharedmem.c \
    tools/rmtfs/storage.c tools/rmtfs/util.c \
    "$QRTR_DIR/qmi.c" "$QRTR_DIR/logging.c" tools/msmipc.c -lpthread
echo "built $ROOT/usr/bin/rmtfs"

"$MUSLCC" -Wall -O2 -static $KHDR -I "$QRTR_DIR" -I tools \
    -o "$ROOT/usr/bin/irsc" tools/irsc.c tools/msmipc.c
echo "built $ROOT/usr/bin/irsc"

"$MUSLCC" -Wall -O2 -static $KHDR -I "$QRTR_DIR" -I tools \
    -o "$ROOT/usr/bin/qmuxd-lite" tools/qmuxd-lite.c tools/qmux.c tools/msmipc.c \
    "$QRTR_DIR/qmi.c" "$QRTR_DIR/logging.c" -lpthread
echo "built $ROOT/usr/bin/qmuxd-lite"

"$MUSLCC" -Wall -O2 -static $KHDR -I tools/servreg-locator -I "$QRTR_DIR" -I tools \
    -o "$ROOT/usr/bin/servreg-locator" \
    tools/servreg-locator/jsn.c tools/servreg-locator/servreg_loc.c \
    tools/servreg-locator/servreg-locator.c \
    "$QRTR_DIR/qmi.c" "$QRTR_DIR/logging.c" tools/msmipc.c
echo "built $ROOT/usr/bin/servreg-locator"

# newc format, everything owned by root, reproducible ordering.
( cd "$ROOT" && find . -print0 | LC_ALL=C sort -z \
    | cpio -0 -o -H newc --owner=+0:+0 --quiet ) | gzip -9n > out/initramfs.cpio.gz
ls -l out/initramfs.cpio.gz
