# chef-cyclo

Non-Android Linux bike-computer OS for the Motorola One Power (codename `chef`, XT1942-2, SDM636).

## Layout
- `kernel/` — Motorola kernel source `kernel-msm-MMI-QPT30.61-18` (Linux 4.4.192). Config = `arch/arm64/configs/sdm660_defconfig` + `ext_config/moto-sdm660.config` + `ext_config/moto-sdm660-chef.config`; DT = `sdm636-chef-evt.dts`.
- `stock/partitions/` — full raw dump of every partition except userdata, taken 2026-09-13 from stock QPTS30.61-18-16-19 (Android 10). `SHA256SUMS` verified against the device. Includes device-unique `persist`, `modemst1/2`, `fsg_*`, `utags`, `cid`, `hw` — do not lose.
- `stock/twrp-3.7.0_9-0-chef.img` — official TWRP, verified. `fastboot boot` it for a root adb shell (nothing flashed).
- `stock/twrp-*.txt` — kernel dmesg, cmdline, input devices and display info captured from the 4.4 kernel running under TWRP.
- `kernel-config/chef-cyclo.config` — our kernel config fragment on top of Motorola's stack.
- `initramfs/` — the boot ramdisk: `/init` (USB NCM gadget + telnet shell), inittab, udhcpd config.
- `scripts/` — `setup-toolchain.sh`, `env.sh` (`kmake`, `chef_defconfig`), `mkinitramfs.sh`, `mkboot.sh`.
- `toolchain/` (gitignored) — GCC 4.9, AOSP `mkbootimg`, Debian static busybox; all fetched by `setup-toolchain.sh`.

## Device facts
- Bootloader already unlocked; A/B, active slot `_a`; no dtbo partition (DTB appended to kernel).
- Panel: Tianma NT36772 1080x2246 DSI video mode (`mdss_dsi_mot_tianma_nt_618_fhd_vid_v0`), selected by the bootloader via `mdss_mdp.panel=` on the cmdline. Touch: Novatek NT36772.
- Wi-Fi/BT MACs passed on cmdline (`androidboot.wifimacaddr`, `androidboot.btmacaddr`).
- Stock cmdline has `skip_initramfs` — strip it for our own boot image.
- No ANT+ (not enabled by Motorola). Plan: BLE via BlueZ; USB ANT stick over OTG as fallback.

## Building
```
scripts/setup-toolchain.sh   # once: fetches AOSP GCC 4.9 aarch64 into toolchain/ (gitignored)
. scripts/env.sh             # defines kmake and chef_defconfig
chef_defconfig               # sdm660_defconfig + moto-sdm660.config + moto-sdm660-chef.config -> out/kernel/.config
kmake -j$(nproc)             # Image.gz-dtb comes out in out/kernel/arch/arm64/boot/
scripts/mkinitramfs.sh       # initramfs/ + busybox -> out/initramfs.cpio.gz
scripts/mkboot.sh            # -> out/boot.img (header values from stock boot_a.img)
fastboot boot out/boot.img   # nothing is flashed; Power+VolDown to get back to Android
```
Once booted, the phone shows up as a USB NCM ethernet device; it hands the host an address by DHCP and listens on `telnet 172.16.42.1` (root, no password). It buzzes once when `/init` starts and twice when the network is up.
`kmake` is `make` in `kernel/` with `O=out/kernel`, the cross prefix, and the command-line overrides a 4.4 tree needs on a modern host (`CC=` to bypass `gcc-wrapper.py`, `HOSTCFLAGS+=-fcommon`). DTB targets are relative to `arch/arm64/boot/dts` (`kmake qcom/sdm636-chef-evt.dtb`). Host deps: `libssl-dev` (for `sign-file`, since stock has `CONFIG_MODULE_SIG=y`).

## Host gotcha
On the ASRock B450 Steel Legend, `fastboot` data transfers hang on the chipset USB controller (bus 1). Use one of the four blue CPU-attached rear USB 3.1 Gen1 ports (shows as bus 3). A hung send leaves the bootloader confused — Power+VolDown to reset.

## Goal

Boot this phone into a plain Linux userspace (no Android) and run a bike-computer UI on it. Why chef: 5000 mAh battery, full kernel source available, unlocked bootloader, good community history (LineageOS official for years). Hardware pieces that must work, roughly in order: boot → USB networking/shell → display → touch → battery/charging → BLE (sensors) → GPS → Wi-Fi → suspend/battery life. GPU stays software-rendered (Adreno needs proprietary blobs; not needed for this UI).

## Log

### 2026-09-13 — identification, backup, first boot of a non-stock image

**Kernel tree.** Identified `kernel-msm-MMI-QPT30.61-18` as Motorola's Android 10 kernel release: Linux 4.4.192 + Qualcomm CAF + Motorola patches, covering sdm660/sdm636 devices (chef, lake, payton, evert, beckham, heart, flay) and msm8998 (nash, messi). Motorola builds chef with `sdm660_defconfig` concatenated with `ext_config/moto-sdm660-chef.config` (see `defconfig.mk`).

**Device.** Moto One Power, serial `ZF6223WZGL`, running stock `QPTS30.61-18-16-19` (Android 10, patch 2021-10-01, kernel `4.4.192-perf+` built with GCC 4.9 on 2021-09-22 — same line as the source tree). Bootloader `MBM-3.0-chef_retail-06369b83c90-210922`, **already unlocked** (`verifiedbootstate=orange`, `flash.locked=0`). A/B, slot `_a`. Battery health "Good".

**Firmware sources.** Stock firmware for chef is no longer on the usual Moto mirror (lolinet purges after 5 years). The Android Dumps GitLab (`dumps.tadiphone.dev/dumps/motorola/chef_sprout`) has extracted dumps up to `18-16-16`, one branch per build. Decided to dump from the device itself instead (exact version, plus device-unique partitions). See memory note / `stock/`.

**TWRP.** Downloaded official TWRP 3.7.0_9-0 for chef from dl.twrp.me. First two downloads were silently truncated by the server (5 MB and 7.5 MB of a 32 MB file) and failed the published sha256; resuming with `curl -C -` completed it and it verified (`2a632fe9…`).

**The fastboot rabbit hole.** `fastboot boot twrp.img` hung forever at `Sending 'boot.img'`. Short commands (`getvar`) worked; any bulk data transfer stalled, even a 1 MB test file. Killed sends leave the bootloader in a confused state where it processes each new command with the length of the previous longer one (`getvar:all` after `download:…` came back as `all0100000`) — only a Power+VolDown hard reset clears it. Tried: two cables, three ports, disabling USB2 LPM (`echo 0 > …/power/usb2_hardware_lpm`, and `22b8:2e80:k` in `usbcore.quirks`), an older fastboot binary (r33). None helped. **Root cause: the AMD 400-series chipset USB controller** (`02:00.0`, bus 1). Moving the phone to a CPU-attached port (`0c:00.3`, bus 3 — the four blue rear USB 3.1 Gen1 ports on the B450 Steel Legend) fixed it instantly: 31 MB sent in 0.7 s.

**Backup.** Booted TWRP from RAM (nothing flashed), got a root adb shell, and dumped all 77 partitions except userdata with `adb exec-out dd`, verifying each against an on-device `sha256sum`. 11 GB in `stock/partitions/`. Two scripting bugs on the way: TWRP's toybox `dd` rejects `bs=4M` (use `bs=4194304`), and `adb shell` inside a `while read` loop eats the loop's stdin (read from fd 3, redirect adb from `/dev/null`). Also captured the running kernel's dmesg, `/proc/cmdline` and input device list, then rebooted back to stock Android — phone is unchanged.

**Findings from the logs.**
- Panel is detected by the bootloader and named on the cmdline (`mdss_mdp.panel=1:dsi:0:qcom,mdss_dsi_mot_tianma_nt_618_fhd_vid_v0…`); the tree has three chef panel variants (Tianma/NT, Tianma/FT, CSOT/NT) so a replacement display of another variant should still work.
- Touch on this unit is Novatek NT36772 (`NVTCapacitiveTouchScreen`), not Focaltech.
- Modem/ADSP/CDSP/Wi-Fi firmware live in the `modem_a` partition, BT firmware in `bluetooth_a`, DSP in `dsp_a` — Android mounts them at `/vendor/firmware_mnt`, `/vendor/bt_firmware`, `/vendor/dsp`. Our rootfs must do the same. GPU/touch firmware and Wi-Fi config are in `vendor_a` under `/vendor/firmware` and `/vendor/etc/wifi`.
- Stock cmdline includes `skip_initramfs` and `root=/dev/mmcblk0p67`; we drop `skip_initramfs` so the kernel uses our initramfs.
- Wi-Fi and BT MAC addresses are passed on the cmdline.
- **No ANT+**: Motorola didn't enable it (no `com.dsi.ant.server`, no `android.hardware.ant` feature). Sensor plan is BLE via BlueZ; USB ANT+ stick over OTG as fallback for ANT-only sensors.

**Status at end of day.** Nothing on the phone has been modified. Paused pending a replacement display (not a blocker for the next steps — they only need USB networking).

### 2026-09-13 (later) — toolchain

Set up AOSP `aarch64-linux-android-4.9` (branch `android-10.0.0_r47`, "GCC 4.9.x 20150123 (prerelease)" — the same compiler line as the stock kernel banner). Gotchas: in that release `gcc`/`g++` are python2 wrapper scripts that only print a deprecation nag and exec a UUID-named real driver — `scripts/setup-toolchain.sh` replaces them with symlinks to the real binaries. The kernel Makefile hardcodes `PYTHON = python` for `scripts/gcc-wrapper.py` (which is python3-clean) and the tree's bundled dtc defines `yylloc` twice (fails to link under GCC ≥ 10's `-fno-common`); both are handled by command-line overrides in `kmake`. Note the submodule tags `MMI-QPT30.61-18` and `MMI-QPW30.61-21` are the same commit.

**Kernel branch.** The `kernel/` submodule now points at the fork `nshnt-xyz/kernel-msm`, branch `chef-cyclo` (Motorola's tree + our patches). Inside `kernel/`, `origin` is the fork and `upstream` is Motorola.

**First full build.** Two problems. (1) `scripts/gcc-wrapper.py` is *not* python3-clean: its warning path uses py2 `print >>` and, by design, it fails the build on any warning at all; `kmake` now passes `CC=` to bypass it. (2) `arch/arm64/kernel/cpuinfo.c` failed on `system_rev` undeclared — it's guarded by `CONFIG_BOOTINFO`, which lives in `ext_config/moto-sdm660.config`. So the real stack per `defconfig.mk` is **three** files: `sdm660_defconfig` + `moto-sdm660.config` (platform, via `moto-$(DEFCONFIG_BASENAME)`) + `moto-sdm660-chef.config` (device, via `KERNEL_EXTRA_CONFIG`). The chef fragment is clearly a delta on the platform one (it un-sets Madera options the platform enables). The platform fragment also turns on `CONFIG_TOUCHSCREEN_NT36xxx`, the Novatek driver our unit needs. With that, `kmake -j12` completes in ~5 min with one harmless host warning (modpost). Output: `Image.gz-dtb` 14.5 MB (Image.gz + `sdm636-chef-evt.dtb` only, thanks to `CONFIG_CHEF_DTB`), `UTS_RELEASE 4.4.192-g6d83ef138`. Stock `boot_a.img` header (v0, 4096-byte pages) carries a 12.4 MB kernel and a 10.4 MB ramdisk.

### 2026-09-13 (later) — initramfs and boot image

**`skip_initramfs` comes from the bootloader, not the boot image.** Neither the stock nor the TWRP boot image header has it; the runtime cmdline TWRP saw does (`… androidboot.slot_suffix=_a skip_initramfs rootwait ro init=/init …`, all appended by the bootloader, together with `root=/dev/mmcblk0p67`). TWRP still booted its ramdisk (`Trying to unpack rootfs image as initramfs…` in its dmesg) because its kernel ignores the parameter. So instead of stripping it, our kernel does the same: `init/initramfs.c` accepts `skip_initramfs` and does nothing (commit `1c67115bc` on the `chef-cyclo` branch). With an initramfs providing `/init`, the bootloader's `root=`/`init=` are ignored by the kernel.

**Config fragment.** `kernel-config/chef-cyclo.config`, appended by `chef_defconfig`: `CONFIG_DEVTMPFS(+_MOUNT)=y` (Android populates `/dev` with ueventd, stock has it off), `ANDROID_PARANOID_NETWORK` off, `LOCALVERSION=-cyclo`. Kernel is now `4.4.192-cyclo-g6d83ef138-00001-g1c67115bc`.

**USB gadget.** Stock config offers configfs functions NCM (`ncm.usb0`), Qualcomm RNDIS (`rndis_bam`), FunctionFS, MTP/PTP etc. — no plain ECM/RNDIS. NCM works with the in-kernel `cdc_ncm` driver on any Linux host, so `/init` builds a single-function NCM gadget (`1d6b:0104`, locally-administered MACs), binds it to whatever appears in `/sys/class/udc` (expected `a800000.dwc3`), gives `usb0` 172.16.42.1/24, runs `udhcpd` (leases .2–.9, no router option) and `telnetd -l /bin/sh`, then `exec`s busybox init with a small inittab that respawns both. Vibrator is `/sys/class/timed_output/vibrator/enable` (ms) — used as a display-less progress signal.

**busybox.** Alpine's `busybox-static` lacks `telnetd`/`udhcpd` (they live in a dynamic `busybox-extras`); Debian's `busybox-static` 1.37 arm64 has both plus `ip`, `mdev`, `cttyhack`, so `setup-toolchain.sh` pulls that `.deb` (`dpkg-deb --fsys-tarfile`). Initramfs is ~1 MB. `mkboot.sh` uses AOSP `mkbootimg.py` with the header values read out of `stock/partitions/boot_a.img` (v0, 4096-byte pages, offsets 0x8000/0x1000000/0xf00000/0x100, os 10.0.0 / 2021-10, stock cmdline). `unpack_bootimg` on the result matches stock field for field. **Not yet booted on the phone.**

## Next steps

1. ~~Toolchain~~ — done, see *Building*.
2. ~~Build the kernel~~ — done, `out/kernel/arch/arm64/boot/Image.gz-dtb`.
3. ~~Minimal initramfs~~ — done: busybox, USB NCM gadget, udhcpd + telnetd (`initramfs/`).
4. ~~Pack a boot image~~ — done (`scripts/mkboot.sh`). **Next: `fastboot boot out/boot.img`** from a blue rear USB port, wait for two buzzes, `telnet 172.16.42.1`. If it doesn't come up: check `lsusb`/`dmesg` on the host for the `1d6b:0104` device, then Power+VolDown. Android remains the fallback; nothing is flashed.
5. Then in order: display (fbdev/DRM), touch (evdev), battery, BT (BlueZ + `bluetooth_a` firmware), GPS (QMI-LOC via libqmi/ModemManager + `modem_a` firmware), Wi-Fi (qcacld + blobs), suspend.
6. Later: rootfs on the userdata partition, flash `boot_a`, retire Android.
