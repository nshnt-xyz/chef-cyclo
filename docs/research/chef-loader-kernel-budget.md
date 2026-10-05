# Chef bootloader kernel-space investigation

The failed DRM boots have no captured bootloader error or fresh retained kernel console. A read-only examination of the phone's actual `abl_a` image identified a kernel-space bound that is a strong candidate, not yet a runtime diagnosis.

The 1 MiB partition capture is `out/drm-loader/abl_a.img`, SHA256 `d884cdd23bd05ec7e2036f4fdea17cd857ad1070506ab15f392352cc0802100c`. UEFI extraction produced `out/drm-loader/LinuxLoader.pe`. The retained [disassembly excerpts](../../logs/drm-bringup-2026-10-03-loader-disassembly.txt) preserve the caller, constants, decompression path and final ARM64 Image check. The [artifact hash manifest](../../logs/drm-bringup-2026-10-03-loader-artifact-hashes.txt) identifies the tested kernels, ramdisks and boot images.

The inspected ARM64 caller uses a fixed `0x80000` kernel-start displacement; this is not a dynamic read of `Image.text_offset`. With the inspected `0x5600000` region extent and 4096-byte pages, the candidate capacity is:

```
0x5600000 - 0x80000 - roundup(ramdisk_bytes, 4096)
          - 2 * 4096 - 0x200000
```

The caller constructs the descriptor around `0x3cb78`–`0x3cbec`. The callee subtracts kernel start from the upper bound and, after decompression, compares the ARM64 header's `image_size` at offset `0x10` against that span around `0x3e43c`–`0x3e4b0`. This size includes the kernel's memory footprint rather than only the compressed or on-disk bytes. The actual queried region and selected runtime branch have not been observed; interpreting the fallback OR as addition requires suitable base alignment.

For the full gzip ramdisk, the candidate capacity is 40,165,376 bytes:

| Kernel | Image header size | Margin |
| --- | ---: | ---: |
| Working exact-config control | 40,095,744 | +69,632 (68 KiB) |
| DRM core only | 40,493,056 | −327,680 (320 KiB) |
| Full DRM/MSM | 41,373,696 | −1,208,320 |

This matches the previous working-control/failing-DRM observations, but does not establish that the bound caused those failures.

## Prepared tests and current outcome

`scripts/build-drm-diagnostic-initramfs.sh` builds a roughly 1 MiB USB-only diagnostic ramdisk. Implementation and review checked its archive and dependency closure. It includes the baseline static `btprobe` for manual `restart bootloader` recovery. Startup does not access display, touch, firmware, WLAN or disk partitions.

The first temporary boot paired this ramdisk with the previously working control kernel. Fastboot reported successful transfer and boot, but the 45-second shell poll failed; subsequent USB and fastboot inventories were empty. Its startup stage was not observable. Consequently this ramdisk has **not** passed control calibration, and no minimal-ramdisk DRM-core boot was performed. After physical recovery into fastboot, the original baseline was temporarily booted and its USB shell, kernel #20, both readiness markers and WLED brightness 1542 were verified. See the [recovery transcript](../../logs/drm-bringup-2026-10-03-minimal-recovery-live.txt). Nothing was flashed. See the [boot command](../../logs/drm-bringup-2026-10-03-minimal-control-boot.txt) and [observation](../../logs/drm-bringup-2026-10-03-minimal-control-observation.txt).

## Full-userspace LZMA boots

The original baseline CPIO was reencoded to LZMA without changing its contents. The 32,621,248-byte archive decodes to SHA256 `3165b3a1f2a397ba41c215c0d985bf042fbe21b576ab35e1472879d9dfdfaa23`, exactly matching the original gzip archive's decoded CPIO. All relevant kernel configurations enable `CONFIG_RD_LZMA=y`.

The [control boot](../../logs/drm-bringup-2026-10-03-lzma-control-live.txt) passed with that archive, followed by [DRM core with the identical archive](../../logs/drm-bringup-2026-10-03-lzma-core-live.txt). The exact kernel payloads are unchanged from their earlier gzip-ramdisk tests. Both boots exposed the USB shell, both readiness markers, WLED1542 and NetworkManager with WLAN available/disconnected. The core boot additionally logged `Initialized drm 1.1.0` and registered character-device major226. Its DRM class contained only `version`, with no card.

The full DRM/MSM experiment uses its separately rebuilt matching WLAN module. Reencoding that experiment's exact CPIO produces a 32,613,658-byte LZMA archive; decompression yields the unchanged CPIO SHA256 `87548e429922f920943d29b2e670cd684ec140115a605b8df07035f5f81ed6a2`. Under the inspected formula this leaves 13,557,760 bytes above the full DRM kernel's Image size.

The [full DRM boot](../../logs/drm-bringup-2026-10-03-lzma-drm-live.txt) also passed with this matching archive: USB shell, DRM initialization/major226, both readiness markers, WLED1542, NVT libinput discovery, loaded WLAN and NetworkManager availability were observed. The [binding inventory](../../logs/drm-bringup-2026-10-03-lzma-drm-bindings.txt) shows the eight audited DRM platform drivers registered without device links, no DRM card, and no `/dev/dri` nodes. The preexisting vendor SDE V4L2 rotator still probes; it is separate from DRM display scanout.

These successful boots establish DRM framework startup with the experimental configuration and strongly corroborate the loader-space explanation. The exact bootloader error, queried region and selected branch remain unobserved. Reencoding also changes the compressed stream, so there is no direct runtime trace of the bound rejection. Native SDE3.2/14nm PHY/panel support, DRM scanout and acceleration remain outstanding. The display continues through legacy MDSS.

The experiments preserve the existing gzip baseline artifacts. To reproduce an isolated LZMA archive, first verify `CONFIG_RD_LZMA=y` in the selected kernel's configuration, then decode the chosen gzip archive to CPIO and encode it with `xz --format=lzma -6 -c`. Verify that decoding the result yields the exact same CPIO hash before packaging it via `RAMDISK=... KERNEL=... OUT=... scripts/mkboot.sh`. Use baseline WLAN only with a CRC-compatible kernel; the full DRM build requires its matching rebuilt module.

After these tests, the original baseline was temporarily booted again. The [final recovery transcript](../../logs/drm-bringup-2026-10-03-lzma-final-recovery-live.txt) confirms kernel #20, both readiness markers, WLED1542, NVT discovery and loaded/available WLAN. Nothing was flashed.

## Uncompressed kernel path

Read on 2026-10-05 from the same `LinuxLoader.pe` (SHA256 `2699ba66…`; capstone over all of `.text`, file offsets equal RVAs), and confirmed independently by the reviewer. The boot parameter block sits at `sp+0xc0` in the caller: image buffer `+0x08`, kernel start `+0x20`, bound `+0x38`, page size `+0x50`, header `kernel_size` `+0x54`, patched-header size `+0x64`, DTB offset `+0x68`, flags `+0x88` (32-bit), `+0x89` (patched), `+0x8a` (gzip).

- **Detection** (`0x3ca00`–`0x3ca5c`): if the kernel section is not gzip (`0x3fbdc`, `1f 8b 08`) and its first 16 bytes equal `UNCOMPRESSED_IMG` (string at `0x8f748`), abl sets the patched flag and moves the kernel pointer 0x14 bytes on. The ARM64 magic must then be at +0x38 of that pointer, otherwise abl takes the 32-bit path (load offset 0x8000 and other region constants).
- **Load** (`0x3e264`–`0x3e2ec`, "Patched kernel detected" at `0x8f8a6`): the DTB offset is the little-endian 32-bit value at section+0x10, the patched-header size is set to 0x14, and `CopyMem` copies the header's `kernel_size` bytes from section+0x14 to the kernel start. The copy covers the whole section, DTB included, and happens before the same `image_size` check as the gzip path (`0x3e43c`). The budget is therefore `max(image_size, kernel_size)`, with the capacity formula above unchanged.
- **DTB** (header v0, `0x3cf64`): the kernel pointer is section+patched-header size; `DeviceTreeAppended` (`0x3fd28`) and the dtbo variant (`0x40e2c`) look for FDTs at kernel+DTB offset up to kernel+`kernel_size` and match `qcom,msm-id`/`board-id`/`pmic-id`. If that match fails, the single-DTB fallback (`0x3d164`) looks at section+DTB offset *without* the 0x14 header: correct for gzip (header size 0) but 20 bytes short for the patched form, which would then fail.
- **Which path today.** The dtb index global (RVA `0xa84d8`, initial `0xffffffff`) is written only at `0x40e18` (`DeviceTreeAppended` success, from the 0xAF pool fill) and `0x40f34` (the dtbo variant, a loop counter); the cmdline appender (`0x464ac`) skips it while it is -1. Every boot of our kernel shows `androidboot.dtb_idx=-1347440721` (`0xAFAFAFAF`), so abl matches our only DTB (`msm-id <0x159 0>`, `board-id <0x48 0xa100>`) through `DeviceTreeAppended`, not the fallback. The uncompressed boots on 2026-10-05 show the same value.
- The second `UNCOMPRESSED_IMG` reference (`0x3d9d4`) belongs to a "ComputeVM" loader and does not concern the Linux boot.

So the layout is `"UNCOMPRESSED_IMG"` | LE32 `len(Image)` | raw `Image` | DTB(s), as in Qualcomm's public `BootLinux.c`. `scripts/mkkernel-uncompressed.py` writes it and `scripts/check-chef-loader-budget.py` checks it. It booted live on 2026-10-05 ([building](../building.md#uncompressed-boot-image)). abl then spent about 1.24 s less after a `fastboot boot` download than with the gzip kernel, which is the cost of its inflate.
