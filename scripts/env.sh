# Source this: . scripts/env.sh
# Kernel build environment for chef (Motorola One Power, sdm636).
# Then, from anywhere: kmake <targets...>   (e.g. kmake chef_defconfig; kmake -j12)
CHEF_ROOT=$(cd "$(dirname "${BASH_SOURCE:-$0}")/.." && pwd)
export CHEF_ROOT
export KERNEL_OUT="$CHEF_ROOT/out/kernel"

kmake() {
    # Variables the kernel Makefile assigns with "=" (PYTHON, HOSTCFLAGS) must
    # be overridden on the make command line; the environment is ignored.
    #  - CC=…gcc          bypass scripts/gcc-wrapper.py: it is python2-only on
    #                     its warning path and treats every warning as an error
    #  - -fcommon         host dtc in this 4.4 tree defines yylloc twice; GCC 10+
    #                     defaults to -fno-common and refuses to link it
    make -C "$CHEF_ROOT/kernel" O="$KERNEL_OUT" \
        ARCH=arm64 SUBARCH=arm64 \
        CROSS_COMPILE="$CHEF_ROOT/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-" \
        CC="$CHEF_ROOT/toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-gcc" \
        HOSTCFLAGS="-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu89 -fcommon" \
        KBUILD_BUILD_USER=chef-cyclo KBUILD_BUILD_HOST=build \
        "$@"
}

# chef_defconfig: Motorola's build (kernel/defconfig.mk) concatenates the base
# sdm660 defconfig, the platform fragment moto-sdm660.config, and the device
# fragment moto-sdm660-chef.config (KERNEL_EXTRA_CONFIG); do the same, append
# our own kernel-config/chef-cyclo.config, and load it.
chef_defconfig() {
    local cfgs="$CHEF_ROOT/kernel/arch/arm64/configs"
    mkdir -p "$KERNEL_OUT"
    ( cd "$cfgs" || exit 1
      set -- sdm660_defconfig ext_config/moto-sdm660.config ext_config/moto-sdm660-chef.config \
             "$CHEF_ROOT/kernel-config/chef-cyclo.config"
      perl -le 'print "# This file was automatically generated from:\n#\t" . join("\n#\t", @ARGV) . "\n"' "$@"
      cat "$@"
    ) > "$KERNEL_OUT/chef_defconfig"
    kmake KCONFIG_ALLCONFIG="$KERNEL_OUT/chef_defconfig" alldefconfig
}
