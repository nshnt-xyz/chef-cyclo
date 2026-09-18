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
# freestanding build): rmtfs, servreg-locator and tftp-server are ours;
# msmipc.c/irsc.c/qmuxd-lite.c/qmux.c and the vendored tools/qrtr/ are the
# protocol transport (see GPS section of README.md for the interface).
# gps-up is included in this image, so all five helpers are mandatory:
# never emit a bootable GPS script without its RMTFS, IRSC,
# SERVREG-LOCATOR, TFTP-SERVER and QMUX counterparts. servreg-locator is
# the userspace SERVREG_LOC (pd-mapper) server that answers the modem's
# boot-time domain-list lookup -- see
# tools/servreg-locator/servreg-locator.c's file header and the README's
# 2026-09-16 (dog timeout) entry for why it exists. tftp-server is the
# bounded TFTP/RFS server (service 0x1000) the 2026-09-17 stock-read probe
# found the locator alone doesn't replace -- see tools/tftp/tftpserv.c's
# file header, REPORT.md 8.1 in the stock-read evidence dir, and the
# README's 2026-09-17 (stock read + TFTP/RFS) entry. Its RAM shadow is
# seeded below, at build time, from the persist partition -- see the block
# right after this one. msmipc.h pulls in the vendor kernel's verbatim
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
           tools/servreg-locator/servreg-locator.c \
           tools/tftp/ramfs.c tools/tftp/translate.c tools/tftp/protocol.c \
           tools/tftp/tftpserv.c; do
    [ -f "$src" ] || { echo "missing required GPS source: $src" >&2; exit 1; }
done

# tftp-server's /readwrite RAM shadow is seeded at *build* time (never at
# runtime -- see tools/tftp/translate.c) from the persist partition's own
# rfs/msm/mpss directory, read with debugfs's read-only ext2/3/4 accessor
# (no mount, no loop device, no journal replay -- the same tool and the
# same non-mounting rationale as the Bluetooth firmware pull above). Fails
# closed: the build stops if the saved partition image or any one of the
# eight required files is missing, or if any extracted size doesn't match
# the exact sizes confirmed against the live device in REPORT.md 6 of the
# 2026-09-17 stock-read evidence (`stock-root-read-fable-*`).
PERSIST_IMG=stock/partitions/persist.img
SEED_DIR="$ROOT/usr/share/rfs/msm/mpss"
[ -r "$PERSIST_IMG" ] || { echo "missing $PERSIST_IMG; cannot seed tftp-server's RAM shadow" >&2; exit 1; }
command -v debugfs >/dev/null 2>&1 || { echo "missing debugfs; cannot seed tftp-server's RAM shadow" >&2; exit 1; }
mkdir -p "$SEED_DIR/mot_rfs" "$SEED_DIR/datablock"
set -- shob.bin:37282 dhob.bin:16384 server_check.txt:5 hob_report.txt:4757 \
        dhob_report.txt:138 mot_rfs/imei_sv:1 datablock/id_00:2600 datablock/id_01:2600
for spec in "$@"; do
    f=${spec%:*}
    want=${spec#*:}
    debugfs -R "cat /rfs/msm/mpss/$f" "$PERSIST_IMG" > "$SEED_DIR/$f" 2>/dev/null
    got=$(wc -c < "$SEED_DIR/$f")
    if [ "$got" -ne "$want" ]; then
        echo "seed file $f is $got bytes, expected exactly $want; refusing to build (fail-closed)" >&2
        exit 1
    fi
done
echo "seeded $SEED_DIR from $PERSIST_IMG (8 files, sizes verified)"

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

# tftp-server speaks raw TFTP (RFC 1350), not QMI, so it needs only
# msmipc.c's qrtr_* transport calls -- no qrtr/qmi.c or logging.c, unlike
# rmtfs/servreg-locator above (see tools/tftp/Makefile's file header).
"$MUSLCC" -Wall -O2 -static $KHDR -I tools/tftp -I "$QRTR_DIR" -I tools \
    -o "$ROOT/usr/bin/tftp-server" \
    tools/tftp/ramfs.c tools/tftp/translate.c tools/tftp/protocol.c \
    tools/tftp/tftpserv.c tools/msmipc.c
echo "built $ROOT/usr/bin/tftp-server"

# newc format, everything owned by root, reproducible ordering.
( cd "$ROOT" && find . -print0 | LC_ALL=C sort -z \
    | cpio -0 -o -H newc --owner=+0:+0 --quiet ) | gzip -9n > out/initramfs.cpio.gz
ls -l out/initramfs.cpio.gz
