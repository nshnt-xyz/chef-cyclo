#!/usr/bin/env bash
# DRM experiment: build a boot image whose kernel has the SDE KMS driver
# (DRM_SDE_STAGE selects the image name, e2n by default) and the static
# tools/drmtest client. Output only under out/kernel-drm-sde/ and
# out/boot-drm-sde-$STAGE.img; the baseline build and the earlier E1, E2, E2b, E2c, E2d, E2e, E2f, E2g, E2h, E2i, E3, E3b, E3c, E2j, E2k, E2l and E2m
# images are hashed before and after.
# Never boots anything. --rebuild reuses an existing out/kernel-drm-sde.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
baseline="$KERNEL_OUT/.config"
baseline_files=(
    "$KERNEL_OUT/.config"
    "$KERNEL_OUT/arch/arm64/boot/Image.gz-dtb"
    "$CHEF_ROOT/out/boot-stage1.img"
    "$CHEF_ROOT/out/stage1.cpio.gz"
    "$CHEF_ROOT/out/system_a.img"
    "$CHEF_ROOT/out/boot-drm-sde-e1.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2b.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2c.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2d.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2e.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2f.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2g.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2h.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2i.img"
    "$CHEF_ROOT/out/boot-drm-sde-e3.img"
    "$CHEF_ROOT/out/boot-drm-sde-e3b.img"
    "$CHEF_ROOT/out/boot-drm-sde-e3c.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2j.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2k.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2l.img"
    "$CHEF_ROOT/out/boot-drm-sde-e2m.img"
)
export KERNEL_OUT="$CHEF_ROOT/out/kernel-drm-sde"
STAGE=${DRM_SDE_STAGE:-e2n}
BOOT_OUT=out/boot-drm-sde-$STAGE.img
dtb_name=sdm636-chef-evt-drm
rebuild=no
[[ "$STAGE" != e1 && "$STAGE" != e2 && "$STAGE" != e2b && "$STAGE" != e2c && "$STAGE" != e2d && "$STAGE" != e2e && "$STAGE" != e2f && "$STAGE" != e2g && "$STAGE" != e2h && "$STAGE" != e2i && "$STAGE" != e3 && "$STAGE" != e3b && "$STAGE" != e3c && "$STAGE" != e2j && "$STAGE" != e2k && "$STAGE" != e2l && "$STAGE" != e2m ]] || { echo "Refusing to write the earlier image out/boot-drm-sde-$STAGE.img" >&2; exit 1; }
case "${1:-}" in
    '') ;;
    --rebuild) rebuild=yes ;;
    *) echo "Usage: $0 [--rebuild]" >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { echo "Too many arguments" >&2; exit 2; }
if [[ "$rebuild" == no && -e "$KERNEL_OUT/.config" ]]; then
    echo "Refusing to overwrite existing experiment config: $KERNEL_OUT/.config (use --rebuild)" >&2
    exit 1
fi
mkdir -p "$KERNEL_OUT"
if [[ "$rebuild" == no ]]; then
    sha256sum "${baseline_files[@]}" > "$KERNEL_OUT/baseline.sha256"
else
    [[ -f "$KERNEL_OUT/baseline.sha256" ]] || { echo "Missing $KERNEL_OUT/baseline.sha256 for --rebuild" >&2; exit 1; }
fi
sha256sum -c "$KERNEL_OUT/baseline.sha256"
trap 'status=$?; trap - EXIT; sha256sum -c "$KERNEL_OUT/baseline.sha256" > "$KERNEL_OUT/preservation-after.txt" || status=1; cat "$KERNEL_OUT/preservation-after.txt"; exit "$status"' EXIT

check_config() {
    local setting
    for setting in DRM=y DRM_MSM=y SYSVIPC=y RD_GZIP=y QCOM_KGSL=y FB_MSM_MDSS=y DRM_SDE_HDMI=y BACKLIGHT_LCD_SUPPORT=y \
                   "BUILD_ARM64_APPENDED_DTB_IMAGE_NAMES=\"qcom/$dtb_name\""; do
        grep -qxF "CONFIG_$setting" "$KERNEL_OUT/.config" || { echo "Unexpected experiment config: $setting" >&2; exit 1; }
    done
    for setting in DRM_MSM_DSI DRM_MSM_EARLY_CARD DRM_SDE_WB BACKLIGHT_GENERIC LCD_CLASS_DEVICE DRM_FBDEV_EMULATION; do
        [[ "$(kernel/scripts/config --file "$KERNEL_OUT/.config" --state "$setting")" != y && \
           "$(kernel/scripts/config --file "$KERNEL_OUT/.config" --state "$setting")" != m ]] || { echo "Unexpected enabled option: $setting" >&2; exit 1; }
    done
}

if [[ "$rebuild" == yes ]]; then
    check_config
else
    cp "$baseline" "$KERNEL_OUT/.config"
    kernel/scripts/config --file "$KERNEL_OUT/.config" --enable DRM --enable DRM_MSM \
        --enable BACKLIGHT_LCD_SUPPORT --disable LCD_CLASS_DEVICE --disable BACKLIGHT_GENERIC \
        --disable DRM_MSM_EARLY_CARD --enable DRM_SDE_HDMI --disable DRM_SDE_WB \
        --disable DRM_MSM_DSI \
        --set-str BUILD_ARM64_APPENDED_DTB_IMAGE_NAMES "qcom/$dtb_name"
fi
{
    printf 'Invocation: %q ' "$0" "$@"; printf '\n'
    printf 'Build: kmake -j%s (scripts/env.sh, KERNEL_OUT=%s)\n' "$(nproc)" "$KERNEL_OUT"
    declare -f kmake
    git rev-parse HEAD
    git -C kernel rev-parse HEAD
    "$CHEF_ROOT/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-gcc" --version
} > "$KERNEL_OUT/invocation.txt"
kmake olddefconfig > "$KERNEL_OUT/config.log" 2>&1
check_config
cp "$KERNEL_OUT/.config" "$KERNEL_OUT/experiment.config"
python3 kernel/scripts/diffconfig "$baseline" "$KERNEL_OUT/.config" > "$KERNEL_OUT/config.diff"
kmake -j"$(nproc)" > "$KERNEL_OUT/build.log" 2>&1

# abl takes every appended DTB; exactly one must be there, and it must be ours.
image="$KERNEL_OUT/arch/arm64/boot/Image.gz-dtb"
python3 - "$image" "$KERNEL_OUT/arch/arm64/boot/dts/qcom/$dtb_name.dtb" <<'EOF'
import importlib.util
import sys
import zlib
from pathlib import Path

spec = importlib.util.spec_from_file_location("budget", "scripts/check-chef-loader-budget.py")
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)
decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
data = Path(sys.argv[1]).read_bytes()
decoder.decompress(data)
if not decoder.eof:
    sys.exit("Image.gz-dtb: truncated gzip member")
blob = decoder.unused_data
sizes = budget.split_fdts(blob)
if len(sizes) != 1:
    sys.exit(f"Image.gz-dtb has {len(sizes)} appended DTBs, expected 1")
if blob != Path(sys.argv[2]).read_bytes():
    sys.exit("appended DTB differs from " + sys.argv[2])
print(f"Appended DTB check: 1 FDT, {sizes[0]} bytes, identical to {sys.argv[2]}")
EOF

# drmtest: host syntax check, then the static musl build with the shared
# tools/device.mk flag set (not part of tools/Makefile, the image or tests).
gcc -std=gnu11 -Wall -Wextra -Werror -fsyntax-only tools/drmtest.c
"$CHEF_ROOT/toolchain/aarch64-musl/bin/aarch64-buildroot-linux-musl-gcc" \
    -std=gnu11 -Wall -Wextra -Werror -O2 -static -o "$KERNEL_OUT/drmtest" tools/drmtest.c
sha256sum "$KERNEL_OUT/drmtest"

KERNEL="$image" RAMDISK=out/stage1.cpio.gz OUT="$BOOT_OUT" scripts/mkboot.sh
sha256sum "$BOOT_OUT"
