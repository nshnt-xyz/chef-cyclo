# Shared phone-build settings, included by every component Makefile
# (tools/, tools/rmtfs, tools/servreg-locator, tools/tftp, tools/sensord,
# tools/blesensd, tools/buttond, tools/gps-manager, tools/chefui) after it
# sets REPO_ROOT. Each component builds its phone binaries into
# build/device/ with `make device` and installs them with `make
# install-device DESTDIR=...`; the repository's top-level Makefile runs
# both for all of them (scripts/mkinitramfs.sh).
#
# DEVICE_CC: the static musl aarch64 cross compiler fetched by
# scripts/setup-toolchain.sh. DEVICE_CFLAGS: the one flag set every musl
# binary is built with; a rule adds only what that binary needs (-I
# paths, -D, libraries). Only chefui strips its binaries.

DEVICE_CC ?= $(REPO_ROOT)/toolchain/aarch64-musl/bin/aarch64-buildroot-linux-musl-gcc
DEVICE_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O2 -static

.PHONY: check-device-cc check-destdir
check-device-cc:
	@test -x $(DEVICE_CC) || { echo "musl cross compiler not found at $(DEVICE_CC): run scripts/setup-toolchain.sh" >&2; exit 1; }

# install-device writes phone binaries into DESTDIR/usr/bin: never into
# the host's own /usr/bin.
check-destdir:
	@test -n "$(DESTDIR)" || { echo "install-device needs DESTDIR=<staged root>" >&2; exit 1; }

# <linux/msm_ipc.h> for msmipc.c/msmipc.h, copied into the build tree:
# build/host/include and build/device/include each hold only this header,
# and -I<that> replaces the vendor kernel's include/uapi + include/. The
# raw kernel/include/uapi/linux/types.h warns ("Attempt to use kernel
# headers from user space") when used from userspace, which -Werror
# rejects; msm_ipc.h itself is an exported UAPI header (header-y), and the
# sanitized <linux/types.h> and <linux/ioctl.h> it includes come from the
# compiler's own headers (the host's, or the musl toolchain's).
MSM_IPC_H := $(REPO_ROOT)/kernel/include/uapi/linux/msm_ipc.h

build/%/include/linux/msm_ipc.h: $(MSM_IPC_H)
	@mkdir -p $(dir $@)
	cp $< $@
