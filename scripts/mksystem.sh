#!/bin/sh
# Build out/system_a.img: the read-only root filesystem for system_a from
# the staged tree out/initramfs-root (scripts/mkinitramfs.sh), as a raw ext4
# image of exactly the partition's 2684354560 bytes.
#
# Under one fakeroot session the tree is copied to out/system-root, made
# root-owned (as the cpio's --owner=+0:+0), the D-Bus launch helper gets
# root:messagebus 4750 (stage 2 cannot chown a read-only root), mtimes are
# set to SOURCE_DATE_EPOCH, the manifest and /etc/chef/build-stamp are
# written (scripts/chef-stamp.py) and mke2fs packs it with the pinned
# features in scripts/system-mke2fs.conf, label chefroot, no journal, UUID
# and hash seed from the manifest hash. Then the image is checked: label,
# feature list, block count, e2fsck -fn, the stamp and the helper's mode
# read back with debugfs. Outputs: out/system_a.img, out/system_a.stamp,
# out/system_a.manifest.
#
# Host /usr/sbin/mke2fs (e2fsprogs >= 1.47) by full path: PATH may find the
# Android SDK's mke2fs first.
set -eu
cd "$(dirname "$0")/.."
SRC=out/initramfs-root
TREE=out/system-root
IMG=out/system_a.img
KERNEL=${KERNEL:-out/kernel/arch/arm64/boot/Image.gz-dtb}
SYMVERS=${SYMVERS:-out/kernel/Module.symvers}
RELEASE=$(cat "${KRELEASE:-out/kernel/include/config/kernel.release}")
BYTES=2684354560
HELPER=usr/libexec/dbus-daemon-launch-helper
E2=/usr/sbin
[ -x "$SRC/init" ] || { echo "run scripts/mkinitramfs.sh first" >&2; exit 1; }
# Host keys are only ever made on the phone (docs/features/ssh.md).
python3 scripts/ssh-keys.py scan "$SRC" || { echo "refusing: private key material in $SRC" >&2; exit 1; }
for t in mke2fs e2fsck debugfs dumpe2fs; do
    [ -x "$E2/$t" ] || { echo "missing $E2/$t (e2fsprogs)" >&2; exit 1; }
done
command -v fakeroot >/dev/null || { echo "fakeroot is required" >&2; exit 1; }
# Account IDs the helper relies on; mkinitramfs.sh gates the same entries.
grep -qx 'messagebus:x:101:101:[^:]*:[^:]*:[^:]*' "$SRC/etc/passwd" &&
    grep -q '^messagebus:x:101:' "$SRC/etc/group" || { echo "messagebus is not 101:101" >&2; exit 1; }

SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct)}
E2FSPROGS_FAKE_TIME=$SOURCE_DATE_EPOCH
MKE2FS_CONFIG=$PWD/scripts/system-mke2fs.conf
export SOURCE_DATE_EPOCH E2FSPROGS_FAKE_TIME MKE2FS_CONFIG KERNEL SYMVERS RELEASE
export SRC TREE IMG BYTES HELPER E2

rm -f "$IMG" out/system_a.stamp out/system_a.manifest
fakeroot -- sh -eu -c '
    rm -rf "$TREE"
    cp -a "$SRC" "$TREE"
    chown -hR 0:0 "$TREE"
    chmod 755 "$TREE"
    chown 0:101 "$TREE/$HELPER"
    chmod 4750 "$TREE/$HELPER"
    rm -f "$TREE/etc/chef/build-stamp"
    find "$TREE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
    python3 scripts/chef-stamp.py manifest "$TREE" > out/system_a.manifest
    python3 scripts/chef-stamp.py stamp "$TREE" "$KERNEL" "$SYMVERS" "$RELEASE" \
        out/system_a.manifest > out/system_a.stamp
    install -m 644 -o 0 -g 0 out/system_a.stamp "$TREE/etc/chef/build-stamp"
    touch -h -d "@$SOURCE_DATE_EPOCH" "$TREE/etc/chef/build-stamp" "$TREE/etc/chef"
    set -- $(python3 scripts/chef-stamp.py ids out/system_a.stamp)
    truncate -s "$BYTES" "$IMG"
    "$E2/mke2fs" -q -F -t ext4 -L chefroot -m 0 -U "$1" \
        -E "hash_seed=$2,nodiscard,lazy_itable_init=0,root_owner=0:0" \
        -d "$TREE" "$IMG"
'

# Read the result back.
[ "$(stat -c %s "$IMG")" = "$BYTES" ] || { echo "$IMG is not $BYTES bytes" >&2; exit 1; }
HDR=$("$E2/dumpe2fs" -h "$IMG" 2>/dev/null)
field() { printf '%s\n' "$HDR" | sed -n "s/^$1: *//p"; }
[ "$(field 'Filesystem volume name')" = chefroot ] || { echo "label is not chefroot" >&2; exit 1; }
[ "$(field 'Block count')" = $((BYTES / 4096)) ] || { echo "block count is not the partition size" >&2; exit 1; }
FEATURES=$(field 'Filesystem features')
[ "$FEATURES" = 'ext_attr dir_index filetype extent flex_bg sparse_super large_file huge_file dir_nlink extra_isize' ] ||
    { echo "unexpected features: $FEATURES" >&2; exit 1; }
"$E2/e2fsck" -fn "$IMG" >/dev/null || { echo "e2fsck -fn reports problems in $IMG" >&2; exit 1; }
"$E2/debugfs" -R 'cat /etc/chef/build-stamp' "$IMG" 2>/dev/null | cmp -s - out/system_a.stamp ||
    { echo "stamp read back from $IMG differs" >&2; exit 1; }
"$E2/debugfs" -R "stat /$HELPER" "$IMG" 2>/dev/null | grep -Eq 'Mode: +04750' &&
    "$E2/debugfs" -R "stat /$HELPER" "$IMG" 2>/dev/null | grep -Eq 'User: +0 +Group: +101 ' ||
    { echo "launch helper is not root:messagebus 4750 in $IMG" >&2; exit 1; }
"$E2/debugfs" -R 'stat /init' "$IMG" 2>/dev/null | grep -Eq 'User: +0 +Group: +0 ' ||
    { echo "/init is not root-owned in $IMG" >&2; exit 1; }
python3 scripts/chef-stamp.py check out/system_a.stamp "$TREE" "$KERNEL" "$SYMVERS" "$RELEASE"
echo "$IMG: $(wc -l < out/system_a.manifest) paths, $(du -k "$IMG" | cut -f1) KiB allocated"
sha256sum "$IMG" out/system_a.stamp
