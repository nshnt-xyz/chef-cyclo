#!/bin/sh
# Build out/stage1.cpio.gz: the small stage-1 ramdisk for the system_a boot
# image (stage1/init). It holds busybox and busybox-extras with the musl
# loader and their applet links, btprobe (`btprobe restart bootloader` from
# the rescue shell), the shared USB gadget helper with the udhcpd template,
# and etc/chef/expected-stamp: a byte-identical copy of the system_a image's
# /etc/chef/build-stamp (out/system_a.stamp from scripts/mksystem.sh), which
# stage 1 compares before it switch_roots.
#
# STAGE1_EXPECTED_STAMP=FILE packs another expected stamp instead; only for
# the rescue test image (a stamp that cannot match). OUT selects the output.
# gzip -9n: the kernel has CONFIG_RD_GZIP=y; the LZMA budget concern of the
# full RAM image does not apply to a ramdisk this small.
#
# OUT=*.cpio writes the uncompressed ramdisk for the uncompressed boot image
# (the kernel needs no RD_* option for that): the gunzipped STAGE1_GZ
# (default out/stage1.cpio.gz, built first), so both ramdisks carry the same
# bytes; the tree is not rebuilt, whose mtimes would differ. Nothing is
# written unless it is a newc archive whose etc/chef/expected-stamp equals
# the stamp.
set -eu
cd "$(dirname "$0")/.."
SRC=out/initramfs-root
STAMP=${STAGE1_EXPECTED_STAMP:-out/system_a.stamp}
OUT=${OUT:-out/stage1.cpio.gz}
[ -s "$STAMP" ] || { echo "missing $STAMP; run scripts/mksystem.sh first" >&2; exit 1; }
case "$OUT" in
*.cpio.gz) TREE=${OUT%.cpio.gz}-root ;;
*.cpio)
    STAGE1_GZ=${STAGE1_GZ:-out/stage1.cpio.gz}
    [ -s "$STAGE1_GZ" ] || { echo "missing $STAGE1_GZ: build the gzip stage-1 ramdisk first" >&2; exit 1; }
    rm -f "$OUT" "$OUT.tmp"
    if ! gzip -dc "$STAGE1_GZ" > "$OUT.tmp" || [ "$(head -c 6 "$OUT.tmp")" != 070701 ]; then
        rm -f "$OUT.tmp"; echo "refusing: $STAGE1_GZ is not a gzip newc cpio" >&2; exit 1
    fi
    cpio -i --quiet --to-stdout etc/chef/expected-stamp ./etc/chef/expected-stamp < "$OUT.tmp" | cmp -s - "$STAMP" ||
        { rm -f "$OUT.tmp"; echo "refusing: the expected stamp in $STAGE1_GZ differs from $STAMP" >&2; exit 1; }
    mv "$OUT.tmp" "$OUT"
    ls -l "$OUT"
    exit 0
    ;;
*) echo "OUT must end in .cpio.gz or .cpio" >&2; exit 1 ;;
esac
KCONFIG=${KCONFIG:-out/kernel/.config}
[ -x "$SRC/bin/busybox" ] && [ -x "$SRC/usr/bin/btprobe" ] || { echo "run scripts/mkinitramfs.sh first" >&2; exit 1; }
grep -qx 'CONFIG_RD_GZIP=y' "$KCONFIG" || { echo "$KCONFIG lacks CONFIG_RD_GZIP=y" >&2; exit 1; }

rm -rf "$TREE"
mkdir -p "$TREE"/bin "$TREE"/sbin "$TREE"/usr/bin "$TREE"/usr/sbin "$TREE"/lib \
         "$TREE"/etc/chef "$TREE"/etc/busybox-paths.d "$TREE"/usr/lib/chef \
         "$TREE"/proc "$TREE"/sys "$TREE"/dev "$TREE"/run "$TREE"/tmp "$TREE"/newroot
install -m 755 stage1/init "$TREE/init"
install -m 755 "$SRC/bin/busybox" "$SRC/bin/busybox-extras" "$TREE/bin/"
install -m 755 "$SRC/lib/ld-musl-aarch64.so.1" "$TREE/lib/"
ln -s ld-musl-aarch64.so.1 "$TREE/lib/libc.musl-aarch64.so.1"
install -m 755 "$SRC/usr/bin/btprobe" "$TREE/usr/bin/btprobe"
install -m 644 "$SRC/etc/busybox-paths.d/busybox" "$SRC/etc/busybox-paths.d/busybox-extras" \
    "$TREE/etc/busybox-paths.d/"
install -m 644 initramfs/etc/udhcpd.conf "$TREE/etc/udhcpd.conf"
install -m 644 initramfs/usr/lib/chef/usb-gadget.sh "$TREE/usr/lib/chef/usb-gadget.sh"
install -m 644 "$STAMP" "$TREE/etc/chef/expected-stamp"
chmod 1777 "$TREE/tmp"
scripts/link-applets.sh "$TREE"
for applet in sh mount umount switch_root blockdev timeout dd od cut tr cmp grep stat \
              mountpoint telnetd udhcpd ip sed pidof setsid sync sleep base64 sha256sum \
              tar gzip stty cat dmesg; do
    [ -x "$TREE/bin/$applet" ] || [ -L "$TREE/bin/$applet" ] || [ -L "$TREE/sbin/$applet" ] ||
        [ -L "$TREE/usr/bin/$applet" ] || [ -L "$TREE/usr/sbin/$applet" ] ||
        { echo "stage 1 lacks $applet" >&2; exit 1; }
done

rm -f "$OUT"
( cd "$TREE" && find . -print0 | LC_ALL=C sort -z \
    | cpio -0 -o -H newc --owner=+0:+0 --reproducible --quiet ) | gzip -9n > "$OUT"
ls -l "$OUT"
