#!/bin/sh
# Build out/initramfs.cpio.lzma: the Alpine rootfs from scripts/mkrootfs.sh
# with the rootfs-overlay/ tree (init, inittab, users, the bring-up scripts,
# service configuration), our own binaries (the top-level Makefile's device
# build: btprobe, the GPS/QMI helpers, the daemons, probes and the UI apps;
# what each one is and why it is packed is beside its Makefile rule), the
# WCN3990 Bluetooth and Wi-Fi firmware and the Wi-Fi module, tftp-server's
# RAM-shadow seed and sensord's registry map on top.
#
# SSH (docs/features/ssh.md): the public keys in secrets/ssh/authorized_keys
# (gitignored; AUTHORIZED_KEYS overrides the path) are checked with
# scripts/ssh-keys.py and baked in as /etc/chef/ssh/authorized_keys, where
# chef-sshd's dropbear reads them. SSH=0 builds without them, and chef-sshd
# then leaves SSH off. The staged tree is scanned for private key material
# before it is packed: host keys are only ever made on the phone.
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
           END {exit (n!=1 || bad)}' rootfs-overlay/etc/passwd || { echo 'invalid/colliding chrony account' >&2; exit 1; }
awk -F: '$1=="chrony" {n++; if($3!=102) bad=1}
           $3==102 && $1!="chrony" {bad=1}
           END {exit (n!=1 || bad)}' rootfs-overlay/etc/group || { echo 'invalid/colliding chrony group' >&2; exit 1; }

# Only the dedicated bus identity may execute the setuid activation helper.
awk -F: '$1=="messagebus" {n++; if($3!=101 || $4!=101) bad=1}
           ($3==101 || $4==101) && $1!="messagebus" {bad=1}
           END {exit (n!=1 || bad)}' rootfs-overlay/etc/passwd || { echo 'invalid/colliding bus account' >&2; exit 1; }
awk -F: '$1=="messagebus" {n++; if($3!=101 || $4!="") bad=1}
           $3==101 && $1!="messagebus" {bad=1}
           END {exit (n!=1 || bad)}' rootfs-overlay/etc/group || { echo 'invalid/colliding bus group' >&2; exit 1; }

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
AUTHORIZED_KEYS=${AUTHORIZED_KEYS:-secrets/ssh/authorized_keys}
case "${SSH:-1}" in
1)  python3 scripts/ssh-keys.py check "$AUTHORIZED_KEYS" ||
        { echo "refusing to build an image with SSH and no usable $AUTHORIZED_KEYS (scripts/ssh-setup.sh; SSH=0 builds without SSH)" >&2; exit 1; } ;;
0)  echo "WARNING: SSH=0: building WITHOUT SSH (no authorized keys; chef-sshd leaves dropbear off)" >&2 ;;
*)  echo "SSH must be 1 or 0" >&2; exit 1 ;;
esac
for f in usr/sbin/dropbear usr/bin/dropbearkey usr/lib/ssh/sftp-server; do
    [ -x "out/rootfs/$f" ] || { echo "missing $f; rerun scripts/mkrootfs.sh" >&2; exit 1; }
done
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
cp -a rootfs-overlay/. "$ROOT"/
# Busybox applet links at build time (read-only root at runtime).
scripts/link-applets.sh "$ROOT"
# NetworkManager (rc-manager=unmanaged) keeps its resolver file in /run.
rm -f "$ROOT/etc/resolv.conf"
ln -s /run/NetworkManager/resolv.conf "$ROOT/etc/resolv.conf"
# cp -a keeps the checkout's modes, which follow the user's umask. NM's
# dispatcher refuses group/other-writable scripts, so a umask-002 checkout
# silently disabled the overlap guard and chrony refresh: strip those bits.
(cd rootfs-overlay && find . -mindepth 1 ! -type l -print0) | (cd "$ROOT" && xargs -0 chmod go-w)
chmod 755 "$ROOT"/init "$ROOT"/usr/bin/bt-up "$ROOT"/usr/bin/gps-up "$ROOT"/usr/bin/chef-storage \
    "$ROOT"/usr/bin/chef-state "$ROOT"/usr/bin/chef-reboot "$ROOT"/usr/bin/chef-sshd \
    "$ROOT"/usr/bin/audio-up "$ROOT"/usr/bin/speaker-test-tone \
    "$ROOT"/usr/bin/sensors-up "$ROOT"/usr/bin/sensors-magcal-run \
    "$ROOT"/usr/bin/sensors-compass-run "$ROOT"/usr/bin/display-touch-inventory

# SSH authorized keys. dropbear (-D) refuses them unless the file and every
# directory up to / is root-owned (the cpio and system_a make everything
# root's) and not writable by group or others: the staged / follows the
# builder's umask (775 under 002), so set the modes explicitly.
chmod 755 "$ROOT" "$ROOT/etc" "$ROOT/etc/chef"
if [ "${SSH:-1}" = 1 ]; then
    install -d -m 755 "$ROOT/etc/chef/ssh"
    install -m 644 "$AUTHORIZED_KEYS" "$ROOT/etc/chef/ssh/authorized_keys"
    echo "SSH: $(grep -c '^[^#]' "$ROOT/etc/chef/ssh/authorized_keys") authorized key(s) baked in"
fi

# Our own binaries: every component's device build (tools/device.mk has
# the compilers and flags), installed into $ROOT/usr/bin with mode 755.
make -j"$(nproc)" device
make install-device DESTDIR="$ROOT"

# Runtime partners of those binaries that come from the rootfs package
# list (scripts/mkrootfs.sh): nmea-broker feeds gpsd, and audio-up and
# speaker-test-tone drive tinyalsa's tinymix/tinyplay. Fail closed on
# either half missing.
[ -x "$ROOT/usr/sbin/gpsd" ] || { echo "no gpsd in out/rootfs; rerun scripts/mkrootfs.sh (package list changed)" >&2; exit 1; }
[ -x "$ROOT/usr/bin/tinymix" ] && [ -x "$ROOT/usr/bin/tinyplay" ] || {
    echo "no tinymix/tinyplay in out/rootfs; rerun scripts/mkrootfs.sh (package list changed)" >&2
    exit 1
}

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

# Separate, read-only display metadata. The security build-stamp format is
# intentionally unchanged; this file is not used for boot validation.
python3 scripts/chef-image-info.py > "$ROOT/etc/chef/image-info"
chmod 644 "$ROOT/etc/chef/image-info"

python3 scripts/ssh-keys.py scan "$ROOT" || { echo "refusing: private key material in $ROOT" >&2; exit 1; }

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
