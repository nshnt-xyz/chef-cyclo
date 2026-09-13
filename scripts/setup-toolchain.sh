#!/bin/sh
# Fetch the AOSP GCC 4.9 aarch64 prebuilt (the compiler line Motorola used for
# the stock 4.4.192 kernel) into toolchain/ and make it usable on a modern host.
set -eu
cd "$(dirname "$0")/.."

TC=toolchain/aarch64-linux-android-4.9
BRANCH=android-10.0.0_r47

if [ ! -x "$TC/bin/aarch64-linux-android-gcc-4.9.x" ]; then
    git clone --depth=1 -b "$BRANCH" \
        https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9 \
        "$TC"
fi

# In this release gcc and g++ are python2 wrapper scripts (#!/usr/bin/python)
# whose only job is to print a deprecation warning and exec the real driver,
# which sits next to them under a UUID name. Replace the wrappers with symlinks
# to the real drivers: no python2 dependency, no warning on every compile.
cd "$TC/bin"
GCC_REAL=a8fe5040-2585-11e9-9684-4b547fbede22
GXX_REAL=c2219de2-2586-11e9-8b3d-0bdcf7ee6101
for w in aarch64-linux-android-gcc aarch64-linux-androidkernel-gcc; do
    [ -L "$w" ] || ln -sf "$GCC_REAL" "$w"
done
[ -L aarch64-linux-android-g++ ] || ln -sf "$GXX_REAL" aarch64-linux-android-g++

./aarch64-linux-android-gcc --version | head -1
