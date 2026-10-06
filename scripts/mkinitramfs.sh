#!/bin/sh
# Build out/initramfs.cpio.lzma: the Alpine rootfs from scripts/mkrootfs.sh
# with the initramfs/ overlay (init, inittab, users, bt-up, BlueZ config),
# the btprobe helper, the WCN3990 firmware, the GPS/QMI helpers, the
# display/touch probe fbtouch, the on-device log screen fblog, the UI
# platform demo chefui-demo, the gpsd feed nmea-broker, the GNSS demand
# manager gps-manager, the button daemon buttond, the battery daemon powerd
# and the ADSP bring-up +
# speaker test tone (audio-up, speaker-test-tone, wavtone and the atomic
# TAS2560 calibration-control writer), the speaker-protection experiment
# (afe-debug, spk-protect-probe, afe-topology-cal, tert-tx-hold) and the
# sensors bring-up (sensors-up, sensord and its registry map) on top.
#
# Encoding: LZMA (xz --format=lzma -6) of the same deterministic newc cpio
# stream. Chef's loader leaves the kernel only the region space the ramdisk
# does not take (docs/research/chef-loader-kernel-budget.md): with the gzip
# encoding the baseline had 68 KiB of margin, the LZMA encoding of the same
# cpio about 13 MB (live-booted 2026-10-03). The kernel must have
# CONFIG_RD_LZMA=y (checked against KCONFIG, default out/kernel/.config).
# GZIP=1 restores the old gzip -9n encoding (out/initramfs.cpio.gz).
# Either way the kernel needs CONFIG_SYSVIPC=y: chrony.conf has an SHM
# refclock, and chronyd exits when it cannot attach the segment.
#
# The staged tree out/initramfs-root is also the source of the read-only
# system_a root (scripts/mksystem.sh, scripts/mkinstall.sh), so everything
# /init used to create at runtime on / (busybox applet links, mountpoints,
# the resolv.conf link) is made here.
set -eu
cd "$(dirname "$0")/.."
[ -x out/rootfs/bin/busybox ] || { echo "run scripts/mkrootfs.sh first" >&2; exit 1; }

for bin in wpa_supplicant wpa_cli iw ip tc ss rfkill curl jq tcpdump iperf3 NetworkManager nmcli nmtui gdbus udevd udevadm apk iptables-legacy chronyd chronyc mke2fs e2fsck debugfs dumpe2fs e2label blockdev; do
    [ -x "out/rootfs/usr/sbin/$bin" ] || [ -x "out/rootfs/usr/bin/$bin" ] || [ -x "out/rootfs/sbin/$bin" ] || [ -x "out/rootfs/bin/$bin" ] || {
        echo "missing $bin; rerun scripts/mkrootfs.sh" >&2; exit 1;
    }
done

[ -s out/rootfs/etc/ssl/certs/ca-certificates.crt ] || { echo "missing CA bundle; rerun scripts/mkrootfs.sh" >&2; exit 1; }

for asset in usr/lib/libnm.so.0 usr/share/dbus-1/system.d/org.freedesktop.NetworkManager.conf \
             usr/share/dbus-1/system.d/wpa_supplicant.conf \
             usr/share/dbus-1/system-services/fi.w1.wpa_supplicant1.service \
             etc/apk/repositories usr/lib/xtables/libxt_standard.so \
             usr/libexec/dbus-daemon-launch-helper \
             usr/libexec/nm-dispatcher \
             usr/share/dbus-1/system-services/org.freedesktop.nm_dispatcher.service \
             usr/share/dbus-1/system.d/nm-dispatcher.conf; do
    [ -s "out/rootfs/$asset" ] || { echo "missing standard Wi-Fi asset $asset; rerun scripts/mkrootfs.sh" >&2; exit 1; }
done
[ -x out/rootfs/usr/libexec/dbus-daemon-launch-helper ] || { echo "missing executable D-Bus launch helper" >&2; exit 1; }
[ -x out/rootfs/usr/libexec/nm-dispatcher ] || { echo "missing executable NM dispatcher" >&2; exit 1; }
set -- out/rootfs/usr/lib/NetworkManager/*/libnm-device-plugin-wifi.so
[ -s "$1" ] || { echo "missing NetworkManager Wi-Fi plugin" >&2; exit 1; }
set -- out/rootfs/etc/apk/keys/*.pub
[ -s "$1" ] || { echo "missing runtime apk public signing keys" >&2; exit 1; }

# Skipped apk account scripts: chronyc needs the named account even as root.
# Reject a future overlay ID collision rather than ambiguously drop privileges.
awk -F: '$1=="chrony" {n++; if($3!=102 || $4!=102) bad=1}
           $3==102 && $1!="chrony" {bad=1}
           END {exit (n!=1 || bad)}' initramfs/etc/passwd || { echo 'invalid/colliding chrony account' >&2; exit 1; }
awk -F: '$1=="chrony" {n++; if($3!=102) bad=1}
           $3==102 && $1!="chrony" {bad=1}
           END {exit (n!=1 || bad)}' initramfs/etc/group || { echo 'invalid/colliding chrony group' >&2; exit 1; }

# Only the dedicated bus identity may execute the setuid activation helper.
awk -F: '$1=="messagebus" {n++; if($3!=101 || $4!=101) bad=1}
           ($3==101 || $4==101) && $1!="messagebus" {bad=1}
           END {exit (n!=1 || bad)}' initramfs/etc/passwd || { echo 'invalid/colliding bus account' >&2; exit 1; }
awk -F: '$1=="messagebus" {n++; if($3!=101 || $4!="") bad=1}
           $3==101 && $1!="messagebus" {bad=1}
           END {exit (n!=1 || bad)}' initramfs/etc/group || { echo 'invalid/colliding bus group' >&2; exit 1; }

# gzip(1) itself reads $GZIP as options, so take the opt-out and unset it.
USE_GZIP=${GZIP:-}
unset GZIP
case "$USE_GZIP" in
"") OUTCPIO=out/initramfs.cpio.lzma ;;
1)  OUTCPIO=out/initramfs.cpio.gz ;;
*)  echo "GZIP must be empty or 1" >&2; exit 1 ;;
esac
ROOT=out/initramfs-root
KCONFIG=${KCONFIG:-out/kernel/.config}
grep -qx 'CONFIG_SYSVIPC=y' "$KCONFIG" 2>/dev/null || {
    echo "$KCONFIG lacks CONFIG_SYSVIPC=y: chronyd cannot attach the GPS SHM refclock and would not start" >&2
    exit 1
}
if [ -z "$USE_GZIP" ]; then
    grep -qx 'CONFIG_RD_LZMA=y' "$KCONFIG" 2>/dev/null || {
        echo "$KCONFIG lacks CONFIG_RD_LZMA=y: the kernel cannot unpack an LZMA ramdisk (GZIP=1 for gzip)" >&2
        exit 1
    }
    command -v xz >/dev/null 2>&1 || { echo "xz (XZ Utils) is required for the LZMA ramdisk" >&2; exit 1; }
fi
rm -f "$OUTCPIO"
rm -rf "$ROOT"
mkdir -p "$ROOT"
cp -a out/rootfs/. "$ROOT"/
# Mountpoints and directories /init and the helpers would otherwise create
# at runtime: the same tree becomes the read-only system_a root
# (scripts/mksystem.sh). mkdir -p of an existing directory is harmless there.
mkdir -p "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev "$ROOT"/tmp "$ROOT"/run \
         "$ROOT"/root "$ROOT"/mnt "$ROOT"/var/lib/dbus "$ROOT"/var/lib/bluetooth
install -d -m 755 "$ROOT"/data "$ROOT"/firmware "$ROOT"/factory
cp -a initramfs/. "$ROOT"/
# Busybox applet links at build time (read-only root at runtime).
scripts/link-applets.sh "$ROOT"
# NetworkManager (rc-manager=unmanaged) keeps its resolver file in /run.
rm -f "$ROOT/etc/resolv.conf"
ln -s /run/NetworkManager/resolv.conf "$ROOT/etc/resolv.conf"
# cp -a keeps the checkout's modes, which follow the user's umask. NM's
# dispatcher refuses group/other-writable scripts, so a umask-002 checkout
# silently disabled the overlap guard and chrony refresh: strip those bits.
(cd initramfs && find . -mindepth 1 ! -type l -print0) | (cd "$ROOT" && xargs -0 chmod go-w)
chmod 755 "$ROOT"/init "$ROOT"/usr/bin/bt-up "$ROOT"/usr/bin/gps-up "$ROOT"/usr/bin/chef-storage \
    "$ROOT"/usr/bin/chef-state \
    "$ROOT"/usr/bin/audio-up "$ROOT"/usr/bin/speaker-test-tone \
    "$ROOT"/usr/bin/afe-debug "$ROOT"/usr/bin/spk-protect-probe \
    "$ROOT"/usr/bin/sensors-up "$ROOT"/usr/bin/sensors-magcal-run \
    "$ROOT"/usr/bin/sensors-compass-run "$ROOT"/usr/bin/display-touch-inventory

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

# Build the matching source module; never reuse Android's precompiled wlan.ko.
scripts/build-wifi.sh
mkdir -p "$ROOT/lib/modules" "$ROOT/lib/firmware/wlan/qca_cld"
cp out/wifi/wlan.ko "$ROOT/lib/modules/wlan.ko"
extract_wifi() {
    image=$1 source=$2 target=$3
    [ -r "$image" ] || { echo "missing Wi-Fi firmware source $image" >&2; exit 1; }
    debugfs -R "dump $source $target" "$image" >/dev/null 2>&1
    [ -s "$target" ] || { echo "missing required Wi-Fi blob $source" >&2; exit 1; }
}
extract_wifi stock/partitions/modem_a.img /image/wlanmdsp.mbn "$ROOT/lib/firmware/wlanmdsp.mbn"
extract_wifi stock/partitions/modem_a.img /image/bdwlan_chef.bin "$ROOT/lib/firmware/bdwlan_chef.bin"
extract_wifi stock/partitions/vendor_a.img /etc/wifi/WCNSS_qcom_cfg.ini "$ROOT/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini"
# Pack every board-specific file, preserving firmware-provided names.
# debugfs can return success even when a requested directory is absent, so
# require a successful listing AND a nonempty set before emitting an image.
if ! WIFI_LIST=$(debugfs -R 'ls /image' stock/partitions/modem_a.img 2>&1); then
    echo "cannot list required Wi-Fi board firmware" >&2; exit 1
fi
if ! WIFI_BOARDS=$(printf '%s\n' "$WIFI_LIST" | tr ' ' '\n' | grep '^bdwlan\.[A-Za-z0-9]*$'); then
    echo "no board-specific Wi-Fi firmware in stock modem image" >&2; exit 1
fi
if ! printf '%s\n' "$WIFI_BOARDS" | grep -Eq '^bdwlan\.([0-9A-Fa-f]{3}|b[0-9A-Fa-f]{2})$'; then
    echo "stock modem listing contains no exact board-ID BDF" >&2; exit 1
fi
for blob in $WIFI_BOARDS; do
    extract_wifi stock/partitions/modem_a.img "/image/$blob" "$ROOT/lib/firmware/$blob"
done
# The matching Motorola driver reads per-device bootloader Wi-Fi MACs;
# wifi-up validates the bootarg before module load. Do not invent wlan_mac.bin.
# No stock persist writes, generated shared MAC or automatic driver startup.

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
"$MUSLCC" -Wall -Wextra -Werror -O2 -static -o "$ROOT/usr/bin/wifi-psk" tools/wifi-psk.c

"$MUSLCC" -Wall -Wextra -Werror -Wno-cpp -O2 -static $KHDR -I tools -I "$QRTR_DIR" \
    -o "$ROOT/usr/bin/wlan-fw" tools/wlan-fw.c tools/qmux.c tools/msmipc.c

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

# Display + touch probe (tools/fbtouch.c): holds /dev/fb0 open, draws a test
# pattern through mmap + FBIOPAN_DISPLAY, drives lcd-backlight and decodes
# the NT36xxx evdev stream. Manual opt-in from the telnet shell, not in
# inittab, until live-verified (same policy as gps-up). Needs only libc's
# <linux/fb.h>/<linux/input.h> (through the header-only tools/fbdev.h and
# tools/evdev.h), so no kernel include paths.
for f in tools/fbtouch.c tools/fbdev.h tools/evdev.h; do
    [ -f "$f" ] || { echo "missing required source: $f" >&2; exit 1; }
done
"$MUSLCC" -Wall -Wextra -O2 -static -o "$ROOT/usr/bin/fbtouch" tools/fbtouch.c
echo "built $ROOT/usr/bin/fbtouch"

# Read-only boot/probe output screen (tools/fblog/fblog.c): tails /dev/kmsg
# onto the panel through the same fbdev.h contract as fbtouch, never opens
# an input device, yields the screen to `fbtouch show` via the advisory
# /run/fb0.lock. Started from inittab (respawn); `touch /run/fblog.off` +
# kill it to idle the panel. Font table tools/fblog/font9x15.h is generated
# by tools/fblog/mkfont.py and committed, so the build needs no Pillow.
for f in tools/fblog/fblog.c tools/fblog/font9x15.h tools/fbdev.h; do
    [ -f "$f" ] || { echo "missing required source: $f" >&2; exit 1; }
done
"$MUSLCC" -Wall -Wextra -O2 -static -o "$ROOT/usr/bin/fblog" tools/fblog/fblog.c
echo "built $ROOT/usr/bin/fblog"

# gpsd feed (tools/nmea-broker.c): reads the qmicli LOC follower's stdout and
# sends each valid NMEA sentence as one UDP datagram to gpsd on
# 127.0.0.1:20175 (gpsd cannot read a FIFO; README next-steps item 7). The
# rootfs must carry gpsd itself (scripts/mkrootfs.sh's package list) or the
# broker has nothing to feed -- fail closed on either half missing. Manual
# opt-in from the telnet shell like gps-up; not in inittab until live-verified.
[ -f tools/nmea-broker.c ] || { echo "missing required source: tools/nmea-broker.c" >&2; exit 1; }
[ -x "$ROOT/usr/sbin/gpsd" ] || { echo "no gpsd in out/rootfs; rerun scripts/mkrootfs.sh (package list changed)" >&2; exit 1; }
"$MUSLCC" -Wall -Wextra -O2 -static -o "$ROOT/usr/bin/nmea-broker" tools/nmea-broker.c
echo "built $ROOT/usr/bin/nmea-broker"

# GNSS demand manager (tools/gps-manager.c, docs/features/gps.md): leases on
# /run/gps-manager.sock start the LOC session, gpsd and the qmicli follower |
# nmea-broker pipeline on top of gps-up's shared modem, and stop them after
# the last release. Never touches gps-up. Started from inittab (respawn).
[ -f tools/gps-manager.c ] || { echo "missing required source: tools/gps-manager.c" >&2; exit 1; }
"$MUSLCC" -Wall -Wextra -O2 -static -o "$ROOT/usr/bin/gps-manager" tools/gps-manager.c
echo "built $ROOT/usr/bin/gps-manager"

# Side-button gesture daemon (tools/buttond.c): grabs the qpnp_pon and
# gpio-keys evdev nodes, publishes short/double/long/chord gestures on
# /run/buttond.sock to whoever claims them, and by default toggles the panel
# on a short power press through fblog's /run/fblog.off protocol and powers
# off cleanly (sync + busybox `poweroff`) when power is held 3 s and released
# (README next-steps item 14). Started from inittab (respawn). Only libc's
# <linux/input.h> (device discovery through the header-only tools/evdev.h),
# so no kernel include paths.
for f in tools/buttond.c tools/evdev.h; do
    [ -f "$f" ] || { echo "missing required source: $f" >&2; exit 1; }
done
"$MUSLCC" -Wall -Wextra -O2 -static -o "$ROOT/usr/bin/buttond" tools/buttond.c
echo "built $ROOT/usr/bin/buttond"

# UI platform demo (tools/chefui/, docs/next-steps/ui-platform.md): LVGL
# v9.6.0 (the third_party/lvgl submodule) rendering on the MDSS fbdev with
# chefui's screen lock/flag handling, multitouch reader and buttond client;
# chefui-demo is the platform's verification vehicle. tools/chefui's
# Makefile builds it static with the musl cross compiler and strips it.
# Manual opt-in from the telnet shell; inittab does not start it.
[ -f third_party/lvgl/lvgl.h ] || { echo "missing LVGL: git submodule update --init third_party/lvgl" >&2; exit 1; }
make -C tools/chefui -j"$(nproc)" device >/dev/null
install -m 755 tools/chefui/build/device/chefui-demo "$ROOT/usr/bin/chefui-demo"
echo "built $ROOT/usr/bin/chefui-demo"

# A/B slot marker (tools/abslot.c, docs/next-steps/storage-and-boot.md):
# `fastboot flash boot_a` leaves boot_a unsuccessful with 7 retries and abl
# spends one per boot; inittab runs `abslot mark-successful` 30 s into each
# boot, the job Android's boot_control HAL did. Plain libc.
[ -f tools/abslot.c ] || { echo "missing required source: tools/abslot.c" >&2; exit 1; }
"$MUSLCC" -Wall -Wextra -Werror -O2 -static -o "$ROOT/usr/bin/abslot" tools/abslot.c
echo "built $ROOT/usr/bin/abslot"

# Sub-second wall clock against the PMIC RTC (tools/rtc-edge.c,
# docs/features/storage.md): chef-state saves and restores the
# wall-minus-RTC offset with it. Plain libc.
[ -f tools/rtc-edge.c ] || { echo "missing required source: tools/rtc-edge.c" >&2; exit 1; }
"$MUSLCC" -Wall -Wextra -Werror -O2 -static -o "$ROOT/usr/bin/rtc-edge" tools/rtc-edge.c
echo "built $ROOT/usr/bin/rtc-edge"

# Battery daemon (tools/powerd.c, docs/features/battery-and-charging.md):
# low-battery warn/critical and clean shutdown (BatteryService clone),
# power_supply CSV log (/data/v1/power or /run/power) + state file under
# /run/power, and the 44/42 C
# charge throttle on battery/system_temp_level (thermal-engine SS-BATT-BATT
# clone). Started from both inittabs (respawn). Plain libc.
[ -f tools/powerd.c ] || { echo "missing required source: tools/powerd.c" >&2; exit 1; }
"$MUSLCC" -Wall -Wextra -O2 -static -o "$ROOT/usr/bin/powerd" tools/powerd.c
echo "built $ROOT/usr/bin/powerd"

# ADSP bring-up + speaker test tone (docs/features/audio.md): audio-up
# (initramfs/usr/bin/audio-up) and speaker-test-tone
# (initramfs/usr/bin/speaker-test-tone, chmod'd above) drive tinyalsa's
# tinymix/tinyplay from the rootfs's package list -- fail closed on either
# half missing, same pattern as nmea-broker/gpsd above. wavtone
# (tools/wavtone.c) writes the WAV file speaker-test-tone hands to
# tinyplay; only libc's <math.h>, so no kernel include paths, but needs
# -lm explicitly since musl's static libm is a separate archive.
# tas2560-send-cal uses the kernel ALSA control UAPI to atomically write the
# write-only five-integer calibration control; it has no runtime library
# dependency beyond the statically linked musl libc.
[ -f tools/wavtone.c ] || { echo "missing required source: tools/wavtone.c" >&2; exit 1; }
[ -f tools/tas2560-send-cal.c ] || { echo "missing required source: tools/tas2560-send-cal.c" >&2; exit 1; }
[ -x "$ROOT/usr/bin/tinymix" ] && [ -x "$ROOT/usr/bin/tinyplay" ] || {
    echo "no tinymix/tinyplay in out/rootfs; rerun scripts/mkrootfs.sh (package list changed)" >&2
    exit 1
}
"$MUSLCC" -Wall -Wextra -O2 -static -o "$ROOT/usr/bin/wavtone" tools/wavtone.c -lm
echo "built $ROOT/usr/bin/wavtone"

"$MUSLCC" -Wall -Wextra -O2 -static \
    -o "$ROOT/usr/bin/tas2560-send-cal" tools/tas2560-send-cal.c
echo "built $ROOT/usr/bin/tas2560-send-cal"

# Speaker-protection experiment (docs/features/audio.md, "Why FF stays
# DISABLE"): spk-protect-probe (initramfs/usr/bin, chmod'd above, with
# afe-debug) drives speaker-test-tone under two resident helpers --
# afe-topology-cal (tools/afe-topology-cal.c: installs the stock AFE
# topology 0x000112FC for the speaker port through /dev/msm_audio_cal and
# holds the fd, since the kernel frees the block on last close) and
# tert-tx-hold (tools/tert-tx-hold.c: starts the TERT MI2S_TX hostless
# capture so AFE port 0x1005 is up). Both use only libc + kernel UAPI,
# static musl; fail closed on either source missing, same as the block
# above. Neither is started by anything in inittab.
for f in tools/afe-topology-cal.c tools/tert-tx-hold.c \
         initramfs/usr/bin/afe-debug initramfs/usr/bin/spk-protect-probe; do
    [ -f "$f" ] || { echo "missing required source: $f" >&2; exit 1; }
done
"$MUSLCC" -Wall -Wextra -O2 -static \
    -o "$ROOT/usr/bin/afe-topology-cal" tools/afe-topology-cal.c
echo "built $ROOT/usr/bin/afe-topology-cal"
"$MUSLCC" -Wall -Wextra -O2 -static \
    -o "$ROOT/usr/bin/tert-tx-hold" tools/tert-tx-hold.c
echo "built $ROOT/usr/bin/tert-tx-hold"

# Sensors (tools/sensord/sensord.c, docs/next-steps/sensors-plan.md):
# sensord is the REG2 registry server + SMGR client behind
# /run/sensord.sock, started by initramfs/usr/bin/sensors-up (manual
# opt-in, not in inittab). Same transport/codec as servreg-locator.
for f in tools/sensord/sensord.c tools/sensord/sns_msgs.c tools/sensord/sns_reg.c \
         tools/sensord/compass.c initramfs/usr/bin/sensors-up \
         initramfs/usr/bin/sensors-compass-run; do
    [ -f "$f" ] || { echo "missing required source: $f" >&2; exit 1; }
done
"$MUSLCC" -Wall -Wextra -Wno-cpp -O2 -static $KHDR -I tools/sensord -I "$QRTR_DIR" -I tools \
    -o "$ROOT/usr/bin/sensord" \
    tools/sensord/sensord.c tools/sensord/sns_msgs.c tools/sensord/sns_reg.c \
    tools/sensord/compass.c "$QRTR_DIR/qmi.c" "$QRTR_DIR/logging.c" tools/msmipc.c -lm
echo "built $ROOT/usr/bin/sensord"

# sensord's registry map: which sns.reg bytes are which REG2 item/group is
# compiled into the stock sensors.qti, not stored in sns.reg. Extracted at
# build time from the stock vendor image with debugfs (read-only, no
# mount), like the Bluetooth firmware; vendor data, so never committed.
# The registry contents themselves are read on the device from persist by
# sensors-up. Any failure here (no vendor image, no debugfs/python3, a
# vendor build whose tables sns-reg-map.py cannot find) only warns: the
# image is built without a map and sensors-up/sensord refuse to start
# (sensors are opt-in; nothing else depends on them).
VENDOR_IMG=stock/partitions/vendor_a.img
MAPTMP=$(mktemp -d)
MAPOUT="$ROOT/usr/share/sensord/sns_reg.map"
if [ -r "$VENDOR_IMG" ] && command -v debugfs >/dev/null 2>&1 && \
   debugfs -R "dump /bin/sensors.qti $MAPTMP/sensors.qti" "$VENDOR_IMG" >/dev/null 2>&1 && \
   [ -s "$MAPTMP/sensors.qti" ]; then
    mkdir -p "$ROOT/usr/share/sensord"
    if python3 tools/sns-reg-map.py "$MAPTMP/sensors.qti" > "$MAPTMP/sns_reg.map"; then
        mv "$MAPTMP/sns_reg.map" "$MAPOUT"
        echo "wrote $MAPOUT ($(grep -c '^item ' "$MAPOUT") items)"
    else
        echo "warning: sns-reg-map.py failed on $VENDOR_IMG's sensors.qti; image has no sensor registry map (sensors-up will refuse)" >&2
    fi
else
    echo "warning: no sensors.qti from $VENDOR_IMG; image has no sensor registry map (sensors-up will refuse)" >&2
fi
rm -rf "$MAPTMP"

# newc format, everything owned by root, reproducible ordering; the LZMA
# (lzma_alone) container carries no timestamp, gzip -n drops it.
if [ -z "$USE_GZIP" ]; then
    ( cd "$ROOT" && find . -print0 | LC_ALL=C sort -z \
        | cpio -0 -o -H newc --owner=+0:+0 --quiet ) | xz --format=lzma -6 -c > "$OUTCPIO"
else
    ( cd "$ROOT" && find . -print0 | LC_ALL=C sort -z \
        | cpio -0 -o -H newc --owner=+0:+0 --quiet ) | gzip -9n > "$OUTCPIO"
fi
ls -l "$OUTCPIO"
