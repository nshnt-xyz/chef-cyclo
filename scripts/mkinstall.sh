#!/bin/sh
# Build the installed layout in one go, with matching stamps:
#   kernel             out/kernel/arch/arm64/boot/Image.gz-dtb (kmake;
#                      SKIP_KERNEL=1 uses the existing build unchanged)
#   full RAM image     out/boot-ram.img (whole OS in the ramdisk, never reads
#                      system_a: the boot_a fallback)
#   system_a image     out/system_a.img (+ .stamp, .manifest)
#   stage-1 boot image out/boot-stage1.img (for boot_a with system_a)
# Refuses before writing the stage-1 boot image unless the stamp read back
# from the system_a image, the stage-1 copy and the kernel being packed all
# agree. out/boot.img (the known-good baseline) is never written here.
set -eu
cd "$(dirname "$0")/.."
KERNEL=out/kernel/arch/arm64/boot/Image.gz-dtb
SYMVERS=out/kernel/Module.symvers
RELEASE_FILE=out/kernel/include/config/kernel.release
if [ "${SKIP_KERNEL:-}" != 1 ]; then
    # env.sh defines kmake for an interactive shell; it tolerates set -u.
    set +u; . scripts/env.sh; set -u
    kmake -j"$(nproc)"
fi
[ -s "$KERNEL" ] && [ -s "$SYMVERS" ] && [ -s "$RELEASE_FILE" ] || { echo "no kernel build in out/kernel" >&2; exit 1; }
echo "kernel $(sha256sum "$KERNEL" | cut -d' ' -f1) $(cat "$RELEASE_FILE")"

scripts/mkinitramfs.sh
OUT=out/boot-ram.img scripts/mkboot.sh
scripts/mksystem.sh
scripts/mkstage1.sh

/usr/sbin/debugfs -R 'cat /etc/chef/build-stamp' out/system_a.img 2>/dev/null > out/system_a.stamp.readback
cmp out/system_a.stamp.readback out/system_a.stamp &&
    cmp out/stage1-root/etc/chef/expected-stamp out/system_a.stamp ||
    { echo "refusing: system_a and stage-1 stamps differ" >&2; exit 1; }
rm -f out/system_a.stamp.readback
python3 scripts/chef-stamp.py check out/system_a.stamp out/system-root "$KERNEL" "$SYMVERS" "$(cat "$RELEASE_FILE")"
RAMDISK=out/stage1.cpio.gz OUT=out/boot-stage1.img KERNEL="$KERNEL" scripts/mkboot.sh
sha256sum "$KERNEL" out/boot-ram.img out/system_a.img out/system_a.stamp out/stage1.cpio.gz out/boot-stage1.img
