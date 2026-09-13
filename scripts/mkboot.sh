#!/bin/sh
# Pack out/boot.img: our Image.gz-dtb + initramfs, header values copied from
# the stock boot_a.img (QPTS30.61-18-16-19): header v0, 4096-byte pages,
# kernel @ base+0x8000, ramdisk @ +0x1000000, second @ +0xf00000, tags @ +0x100.
# The bootloader appends its own androidboot.* args, root=, and skip_initramfs
# (which our kernel ignores) at boot time.
set -eu
cd "$(dirname "$0")/.."
KERNEL=${KERNEL:-out/kernel/arch/arm64/boot/Image.gz-dtb}
RAMDISK=${RAMDISK:-out/initramfs.cpio.gz}
OUT=${OUT:-out/boot.img}

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
