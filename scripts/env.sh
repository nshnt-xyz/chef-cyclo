# Source this: . scripts/env.sh
# Kernel build environment for chef (Motorola One Power, sdm636).
# Then, from anywhere: kmake <targets...>   (e.g. kmake chef_defconfig; kmake -j12)
CHEF_ROOT=$(cd "$(dirname "${BASH_SOURCE:-$0}")/.." && pwd)
export CHEF_ROOT
export KERNEL_OUT="$CHEF_ROOT/out/kernel"

kmake() {
    # Variables the kernel Makefile assigns with "=" (PYTHON, HOSTCFLAGS) must
    # be overridden on the make command line; the environment is ignored.
    #  - PYTHON=python3   scripts/gcc-wrapper.py is python2/3 clean but the
    #                     Makefile invokes it as "python", which no longer exists
    #  - -fcommon         host dtc in this 4.4 tree defines yylloc twice; GCC 10+
    #                     defaults to -fno-common and refuses to link it
    make -C "$CHEF_ROOT/kernel" O="$KERNEL_OUT" \
        ARCH=arm64 SUBARCH=arm64 \
        CROSS_COMPILE="$CHEF_ROOT/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-" \
        PYTHON=python3 \
        HOSTCFLAGS="-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu89 -fcommon" \
        KBUILD_BUILD_USER=chef-cyclo KBUILD_BUILD_HOST=build \
        "$@"
}

# chef_defconfig: Motorola's build concatenates the base sdm660 defconfig with
# the chef fragment (see kernel/defconfig.mk); do the same and load it.
chef_defconfig() {
    local cfgs="$CHEF_ROOT/kernel/arch/arm64/configs"
    mkdir -p "$KERNEL_OUT"
    { perl -le 'print "# This file was automatically generated from:\n#\t" . join("\n#\t", @ARGV) . "\n"' \
          sdm660_defconfig ext_config/moto-sdm660-chef.config
      cat "$cfgs/sdm660_defconfig" "$cfgs/ext_config/moto-sdm660-chef.config"
    } > "$KERNEL_OUT/chef_defconfig"
    kmake KCONFIG_ALLCONFIG="$KERNEL_OUT/chef_defconfig" alldefconfig
}
