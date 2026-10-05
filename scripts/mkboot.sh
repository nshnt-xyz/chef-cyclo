#!/bin/sh
# Pack out/boot.img: our Image.gz-dtb + initramfs, header values copied from
# the stock boot_a.img (QPTS30.61-18-16-19): header v0, 4096-byte pages,
# kernel @ base+0x8000, ramdisk @ +0x1000000, second @ +0xf00000, tags @ +0x100.
# The bootloader appends its own androidboot.* args, root=, and skip_initramfs
# (which our kernel ignores) at boot time.
#
# UNCOMPRESSED=1 packs the kernel without gzip: KERNEL is still the gzip
# Image.gz-dtb (the file the build stamp names); scripts/mkkernel-uncompressed.py
# turns it into abl's UNCOMPRESSED_IMG form in ${OUT%.img}.kernel, which is
# what goes into the image. A RAMDISK named *.cpio is packed uncompressed and
# must be a newc archive.
set -eu
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-out/kernel/arch/arm64/boot/Image.gz-dtb}
# LZMA ramdisk by default (scripts/mkinitramfs.sh); GZIP=1 for the gzip one.
if [ "${GZIP:-}" = 1 ]; then
    RAMDISK=${RAMDISK:-out/initramfs.cpio.gz}
else
    RAMDISK=${RAMDISK:-out/initramfs.cpio.lzma}
fi
unset GZIP
OUT=${OUT:-out/boot.img}

# An LZMA ramdisk needs CONFIG_RD_LZMA=y in the kernel being packed: its
# build tree's .config (KCONFIG to override).
case "$RAMDISK" in
*.lzma)
    case "$KERNEL" in
    */arch/arm64/boot/Image.gz-dtb) KCONFIG=${KCONFIG:-${KERNEL%/arch/arm64/boot/Image.gz-dtb}/.config} ;;
    *) KCONFIG=${KCONFIG:-} ;;
    esac
    if [ -n "$KCONFIG" ] && ! grep -qx 'CONFIG_RD_LZMA=y' "$KCONFIG" 2>/dev/null; then
        echo "$KCONFIG lacks CONFIG_RD_LZMA=y: $RAMDISK cannot be unpacked by this kernel" >&2
        exit 1
    fi
    ;;
esac

case "$RAMDISK" in
*.cpio)
    [ "$(head -c 6 "$RAMDISK")" = 070701 ] || { echo "$RAMDISK is not an uncompressed newc cpio" >&2; exit 1; }
    ;;
esac

# Config checks below look at the build tree of the gzip kernel.
SRC_KERNEL=$KERNEL
if [ "${UNCOMPRESSED:-}" = 1 ]; then
    case "$OUT" in *.img) ;; *) echo "OUT must end in .img with UNCOMPRESSED=1" >&2; exit 1 ;; esac
    KERNEL=${OUT%.img}.kernel
    python3 scripts/mkkernel-uncompressed.py "$SRC_KERNEL" "$KERNEL"
fi
unset UNCOMPRESSED

# Reject Chef's observed loader-space overflow before mkbootimg opens OUT.
python3 scripts/check-chef-loader-budget.py "$KERNEL" "$RAMDISK"

# alldefconfig silently drops options whose dependencies are unmet; refuse a
# default-kernel image missing the classic Bluetooth / AF_ALG options or the
# System V IPC that chronyd's GPS SHM refclock needs.
if [ "$SRC_KERNEL" = out/kernel/arch/arm64/boot/Image.gz-dtb ]; then
    for opt in BT_RFCOMM BT_RFCOMM_TTY BT_BNEP BT_BNEP_MC_FILTER BT_BNEP_PROTO_FILTER \
               BT_HIDP BT_HCIVHCI CRYPTO_USER_API_HASH CRYPTO_USER_API_SKCIPHER \
               CRYPTO_USER_API_AEAD SYSVIPC; do
        grep -qx "CONFIG_$opt=y" out/kernel/.config || { echo "CONFIG_$opt missing from out/kernel/.config" >&2; exit 1; }
    done
fi

CMDLINE="console=ttyMSM0,115200,n8 androidboot.console=ttyMSM0 earlycon=msm_serial_dm,0xc170000 androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x37 ehci-hcd.park=3 lpm_levels.sleep_disabled=1 sched_enable_hmp=1 sched_enable_power_aware=1 service_locator.enable=1 swiotlb=1 loop.max_part=7 androidboot.hab.csv=39 androidboot.hab.product=chef androidboot.hab.cid=50 buildvariant=user"

python3 toolchain/mkbootimg/mkbootimg.py \
    --header_version 0 \
    --kernel "$KERNEL" \
    --ramdisk "$RAMDISK" \
    --cmdline "$CMDLINE" \
    --base 0x00000000 \
    --kernel_offset 0x00008000 \
    --ramdisk_offset 0x01000000 \
    --second_offset 0x00f00000 \
    --tags_offset 0x00000100 \
    --pagesize 4096 \
    --os_version 10.0.0 \
    --os_patch_level 2021-10 \
    -o "$OUT"
ls -l "$OUT"
