#!/usr/bin/env bash
# Compile-only built-in DRM probe. Never packages or boots an image.
# --modules / --resume-modules reproduce the earlier module experiment.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
baseline="$KERNEL_OUT/.config"
baseline_image="$KERNEL_OUT/arch/arm64/boot/Image.gz-dtb"
baseline_boot="$CHEF_ROOT/out/boot.img"
export KERNEL_OUT="$CHEF_ROOT/out/kernel-drm"
resume=no
scope=builtin
case "${1:-}" in
    '') ;;
    --resume) resume=yes ;;
    --modules) scope=modules ;;
    --resume-modules) resume=yes; scope=modules ;;
    *) echo "Usage: $0 [--resume|--modules|--resume-modules]" >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { echo "Too many arguments" >&2; exit 2; }
mkdir -p "$KERNEL_OUT"
if [[ "$resume" == no && -e "$KERNEL_OUT/.config" ]]; then
    echo "Refusing to overwrite existing experiment config: $KERNEL_OUT/.config" >&2
    exit 1
fi
if [[ "$resume" == no ]]; then
    sha256sum "$baseline" "$baseline_image" "$baseline_boot" > "$KERNEL_OUT/baseline.sha256"
fi
sha256sum -c "$KERNEL_OUT/baseline.sha256"
trap 'status=$?; trap - EXIT; sha256sum -c "$KERNEL_OUT/baseline.sha256" > "$KERNEL_OUT/preservation-after.txt" || status=1; cat "$KERNEL_OUT/preservation-after.txt"; exit "$status"' EXIT
if [[ "$resume" == yes ]]; then
    drm_value=y
    [[ "$scope" != modules ]] || drm_value=m
    for setting in "DRM=$drm_value" "DRM_MSM=$drm_value" QCOM_KGSL=y FB_MSM_MDSS=y; do
        rg -qx "CONFIG_$setting" "$KERNEL_OUT/.config" || { echo "Unexpected experiment config: $setting" >&2; exit 1; }
    done
    archive="$KERNEL_OUT/attempt-$(date -u +%Y%m%dT%H%M%SZ)"
    mkdir "$archive"
    for artifact in config.log experiment.config config.diff build.log invocation.txt workspace.diff kernel-source.diff env.sh.snapshot build-script.snapshot; do
        [[ ! -f "$KERNEL_OUT/$artifact" ]] || cp "$KERNEL_OUT/$artifact" "$archive/"
    done
    if [[ "$scope" == builtin ]]; then
        for option in DRM_MSM_DSI DRM_MSM_DSI_PLL DRM_MSM_DSI_28NM_PHY DRM_MSM_DSI_20NM_PHY DRM_MSM_EARLY_CARD DRM_SDE_WB BACKLIGHT_GENERIC LCD_CLASS_DEVICE; do
            [[ "$(kernel/scripts/config --file "$KERNEL_OUT/.config" --state "$option")" != y && "$(kernel/scripts/config --file "$KERNEL_OUT/.config" --state "$option")" != m ]] || { echo "Unexpected enabled option: $option" >&2; exit 1; }
        done
    fi
else
    cp "$baseline" "$KERNEL_OUT/.config"
    if [[ "$scope" == modules ]]; then
        kernel/scripts/config --file "$KERNEL_OUT/.config" --module DRM --module DRM_MSM
    else
        kernel/scripts/config --file "$KERNEL_OUT/.config" --enable DRM --enable DRM_MSM \
            --enable BACKLIGHT_LCD_SUPPORT --disable LCD_CLASS_DEVICE --disable BACKLIGHT_GENERIC \
            --disable DRM_MSM_EARLY_CARD --enable DRM_SDE_HDMI --disable DRM_SDE_WB \
            --disable DRM_MSM_DSI
    fi
fi
{
    printf 'Invocation: %q ' "$0" "$@"; printf '\nScope: %s\n' "$scope"
    printf 'Build: kmake -j%s (scripts/env.sh, KERNEL_OUT=%s)\n' "${DRM_BUILD_JOBS:-8}" "$KERNEL_OUT"
    declare -f kmake
    git rev-parse HEAD
    git -C kernel rev-parse HEAD
    "$CHEF_ROOT/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-gcc" --version
} > "$KERNEL_OUT/invocation.txt"
git diff > "$KERNEL_OUT/workspace.diff"
git -C kernel diff > "$KERNEL_OUT/kernel-source.diff"
cp scripts/env.sh "$KERNEL_OUT/env.sh.snapshot"
cp "$0" "$KERNEL_OUT/build-script.snapshot"
kmake olddefconfig > "$KERNEL_OUT/config.log" 2>&1
cp "$KERNEL_OUT/.config" "$KERNEL_OUT/experiment.config"
python3 kernel/scripts/diffconfig "$baseline" "$KERNEL_OUT/.config" > "$KERNEL_OUT/config.diff"
# Full build checks built-in linking and modpost for the retained baseline modules.
kmake -j"${DRM_BUILD_JOBS:-8}" > "$KERNEL_OUT/build.log" 2>&1
