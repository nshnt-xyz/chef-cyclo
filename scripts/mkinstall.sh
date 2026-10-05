#!/bin/sh
# Build the installed layout in one go, with matching stamps:
#   kernel             out/kernel/arch/arm64/boot/Image.gz-dtb (kmake;
#                      SKIP_KERNEL=1 uses the existing build unchanged)
#   full RAM image     out/boot-ram.img (whole OS in the ramdisk, never reads
#                      system_a: the boot_a fallback)
#   system_a image     out/system_a.img (+ .stamp, .manifest)
#   stage-1 boot image out/boot-stage1.img (for boot_a with system_a)
#   UNCOMPRESSED=1     also out/stage1.cpio and out/boot-stage1-uncompressed.img:
#                      the same stage-1 boot image with neither the kernel nor
#                      the ramdisk compressed (scripts/mkboot.sh UNCOMPRESSED=1)
# PACK_ONLY=1 only packs the stage-1 boot images from the existing kernel,
# out/stage1.cpio.gz and system_a image (no kmake, mkinitramfs, boot-ram.img,
# mksystem or mkstage1), with the same stamp checks: out/system_a.img, its
# stamp and the stage-1 ramdisk stay byte for byte as they are.
# Refuses before writing a stage-1 boot image unless the stamp read back
# from the system_a image, the stage-1 copy and the kernel being packed all
# agree. out/boot.img (the known-good baseline) is never written here.
set -eu
cd "$(dirname "$0")/.."
# Not inherited by the gzip mkboot.sh runs below.
WANT_UNCOMPRESSED=${UNCOMPRESSED:-}
unset UNCOMPRESSED
KERNEL=out/kernel/arch/arm64/boot/Image.gz-dtb
SYMVERS=out/kernel/Module.symvers
RELEASE_FILE=out/kernel/include/config/kernel.release
if [ "${SKIP_KERNEL:-}" != 1 ] && [ "${PACK_ONLY:-}" != 1 ]; then
    # env.sh defines kmake for an interactive shell; it tolerates set -u.
    set +u; . scripts/env.sh; set -u
    kmake -j"$(nproc)"
fi
[ -s "$KERNEL" ] && [ -s "$SYMVERS" ] && [ -s "$RELEASE_FILE" ] || { echo "no kernel build in out/kernel" >&2; exit 1; }
echo "kernel $(sha256sum "$KERNEL" | cut -d' ' -f1) $(cat "$RELEASE_FILE")"

if [ "${PACK_ONLY:-}" != 1 ]; then
    scripts/mkinitramfs.sh
    OUT=out/boot-ram.img scripts/mkboot.sh
    scripts/mksystem.sh
    scripts/mkstage1.sh
fi

/usr/sbin/debugfs -R 'cat /etc/chef/build-stamp' out/system_a.img 2>/dev/null > out/system_a.stamp.readback
gzip -dc out/stage1.cpio.gz | cpio -i --quiet --to-stdout etc/chef/expected-stamp ./etc/chef/expected-stamp > out/stage1.stamp.readback
cmp out/system_a.stamp.readback out/system_a.stamp &&
    cmp out/stage1.stamp.readback out/system_a.stamp ||
    { echo "refusing: system_a and stage-1 stamps differ" >&2; exit 1; }
rm -f out/system_a.stamp.readback out/stage1.stamp.readback
python3 scripts/chef-stamp.py check out/system_a.stamp out/system-root "$KERNEL" "$SYMVERS" "$(cat "$RELEASE_FILE")"
RAMDISK=out/stage1.cpio.gz OUT=out/boot-stage1.img KERNEL="$KERNEL" scripts/mkboot.sh
set -- "$KERNEL" out/system_a.img out/system_a.stamp out/stage1.cpio.gz out/boot-stage1.img
[ "${PACK_ONLY:-}" = 1 ] || set -- "$@" out/boot-ram.img
if [ "$WANT_UNCOMPRESSED" = 1 ]; then
    OUT=out/stage1.cpio STAGE1_GZ=out/stage1.cpio.gz scripts/mkstage1.sh
    UNCOMPRESSED=1 RAMDISK=out/stage1.cpio OUT=out/boot-stage1-uncompressed.img KERNEL="$KERNEL" scripts/mkboot.sh
    set -- "$@" out/stage1.cpio out/boot-stage1-uncompressed.kernel out/boot-stage1-uncompressed.img
fi
sha256sum "$@"
