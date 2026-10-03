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
