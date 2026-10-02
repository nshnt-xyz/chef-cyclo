#!/bin/bash
# Matching Motorola WLAN sources, built against this image's kernel ABI.
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/env.sh
WIFI_SRC="$CHEF_ROOT/out/wifi-research"
mkdir -p "$WIFI_SRC"
fetch() {
    local name=$1 commit=$2
    local dir="$WIFI_SRC/$name"
    if [ ! -d "$dir/.git" ]; then
        git clone "https://github.com/MotorolaMobilityLLC/vendor-qcom-opensource-wlan-$name.git" "$dir"
    fi
    [ -z "$(git -C "$dir" status --porcelain --untracked-files=no)" ] || {
        echo "refusing modified $name sources" >&2; exit 1;
    }
    [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] || {
        git -C "$dir" fetch origin "$commit"
        git -C "$dir" checkout --detach "$commit"
    }
}
# Export only the pinned Git trees. Existing untracked/ignored files (including
# old Kbuild products) cannot become build inputs or affect sibling headers.
BUILD_SRC=$(mktemp -d "$CHEF_ROOT/out/wifi-build.XXXXXX")
trap 'rm -rf "$BUILD_SRC"' EXIT
prepare() {
    local name=$1 commit=$2
    fetch "$name" "$commit"
    mkdir "$BUILD_SRC/$name"
    git -C "$WIFI_SRC/$name" archive "$commit" | tar -x -C "$BUILD_SRC/$name"
}
prepare qcacld-3.0 a685f67b37cc311487526dda738301b63b903c9b
prepare qca-wifi-host-cmn a6db9b5df15dd4b86d6e49b8e8788154547ab8eb
prepare fw-api aea65288eb08d96d977b99d9e4b60a05dfecd5f1
[ -s "$KERNEL_OUT/Module.symvers" ] || { echo 'build the kernel first (Module.symvers missing)' >&2; exit 1; }
kmake -j"${JOBS:-$(nproc)}" M="$BUILD_SRC/qcacld-3.0" \
    WLAN_ROOT="$BUILD_SRC/qcacld-3.0" MODNAME=wlan \
    CONFIG_QCA_CLD_WLAN=m CONFIG_QCA_WIFI_ISOC=0 CONFIG_QCA_WIFI_2_0=1 \
    BOARD_PLATFORM=sdm660 modules
mkdir -p out/wifi
cp "$BUILD_SRC/qcacld-3.0/wlan.ko" out/wifi/wlan.ko
"$CHEF_ROOT/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-strip" --strip-debug out/wifi/wlan.ko
sha256sum out/wifi/wlan.ko
