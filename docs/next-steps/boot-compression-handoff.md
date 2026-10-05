# Boot image without compression (handoff)

[Next steps index](README.md) · [Install layout handoff](install-layout-handoff.md) · [Building](../building.md) · [Live testing](../live-testing.md)

**Status:** requested by the user on 2026-10-05 after phase 3. Handed to
Herdr agents `rootfs_impl` (implementation, live runs) and `rootfs_review`
(review), coordinated by `rootfs_research`. Built, booted and measured the same
day ([result](#result-rootfs_impl-2026-10-05)). The uncompressed image works,
but flashed it saves only about 0.2 s of bootloader time. `boot_a` is back on
the gzip `5c509eb2`; installing the uncompressed image is the user's call. An
intermittent shutdown hang turned up during the reboot series, independent of
the image ([shutdown hang](#shutdown-hang-separate-item)); it was a Bluetooth
UART use-after-free, fixed in the kernel the same day. Since then the installed
pair is `system_a` `e72a3039` + `boot_a` `81a5c5ef` (kernel #22), so
`df474390` (old kernel, stamp `f52cfe21`) no longer matches `system_a`: an
uncompressed image now has to be rebuilt with `UNCOMPRESSED=1` from the fixed
kernel.

## Goal

The phase 3 boot image (`out/boot-stage1.img`, `5c509eb2`, installed in
`boot_a`) is 13,484,032 bytes: the gzip kernel `Image.gz-dtb` (12.5 MB,
36.5 MB unpacked) plus a 1.0 MB gzip stage-1 ramdisk. Compression was
needed while the whole OS lived in the ramdisk; it is not needed now.
Build an uncompressed variant, prove it boots, measure whether it is
faster, and install it only if it is at least as good.

## Facts (rootfs_research, 2026-10-05)

- **Ramdisk.** Unpack takes 26 ms today (`Trying to unpack rootfs` 0.387 s,
  `Freeing initrd memory` 0.413 s). The kernel accepts an uncompressed
  newc cpio with no `RD_*` option. Expected gain is a few ms; it is mostly
  a simplification.
- **Kernel.** abl (`out/drm-loader/LinuxLoader.pe`, from `abl_a`) has the
  strings `UNCOMPRESSED_IMG`, `Decompressing kernel image start: %lu ms`,
  `Single appended DTB found`. In Qualcomm's public abl
  (`QcomModulePkg/Library/BootLib/BootLinux.c`) the uncompressed path is a
  "patched kernel" header: the 16 ASCII bytes `UNCOMPRESSED_IMG`, a 4-byte
  little-endian length of the raw `Image`, the raw `Image`, then the
  appended DTB(s). Confirm the exact layout against the disassembly
  (`out/drm-loader/*.txt`, `docs/research/chef-loader-kernel-budget.md`)
  before building; Motorola's abl is not the public source. A plain raw
  `Image` with an appended DTB and no such header is not known to work.
- **Loader budget.** `scripts/check-chef-loader-budget.py` models the gzip
  path only (it reads `Image.image_size` after inflating). The uncompressed
  path may place the kernel differently; extend the check from the
  disassembly or state clearly what is unverified.
- **Timing source.** `MSM_BOOT_STATS=y` prints `KPI:` lines in `dmesg`
  (32768 Hz counts). Only boots from flash give a valid
  `Bootloader load kernel count`; `fastboot boot` shows garbage there.
  Measured on flashed boots: `boot_a` read 6915 counts (211 ms) for the
  36 MB `df856fc3`, 2594 (79 ms) for the 13.5 MB stage-1 image, so about
  170 MB/s. `Bootloader start` to `end` is about 6.0 to 6.2 s on every
  image (stock Android too); the gzip inflate is inside it but not
  separated. `end` to `Kernel MPM timestamp` is a constant 0.50 s.
  Expected cost of an uncompressed 40 MB image: about +0.16 s of eMMC read;
  the gain is whatever the inflate costs. Compare `end - start` over at
  least three flashed boots of each image, plus the kernel-side times
  already tracked (`Freeing unused kernel memory`, `exec init`), and the
  host-side `fastboot boot` to USB network time (USB transfer grows by
  about 27 MB).
- **Charger-mode boot.** The phone came up on 2026-10-05 with
  `androidboot.mode=charger` (`bootreason=charger`, plugged back in while
  off) on the installed stage-1 layout and ran the full OS. Record that
  as the charger-mode check left open by phase 3, and recheck it with the
  new image if it is installed.

## Rules

- The user is **awake and available** for this run: a hard reset (Power
  held about 8.7 s, VolDown for fastboot) is possible, but ask first with
  a countdown and use it only if needed. A wrong abl format most likely
  returns to fastboot; a kernel started without its DTB hangs with no
  panic.
- Kernel config and `system_a` unchanged: the stamp (`f52cfe21`) and
  `wlan.ko` must still match. Only the boot image packaging changes.
- `scripts/mkinstall.sh` and `scripts/mkboot.sh`/`mkstage1.sh` gain an
  explicit uncompressed mode; keep the gzip build available. Test images
  under other names; `out/boot.img` promotion is the coordinator's call.
- Host tests for the new packer (header layout, length field, DTB
  appended, size budget refusal) in the existing style;
  `make -C tools test` and `python3 -m unittest discover scripts/tests`
  green.
- Live order, each step after `rootfs_review` clears the exact hash:
  `fastboot boot` the uncompressed image (twice, short regression:
  USB network, `/` read-only from `system_a`, stamp match, Wi-Fi, BT,
  crash counts 0); then, only if the user agrees with the measured
  numbers, `fastboot flash boot_a` after a fresh `current-slot`/hash
  check, at least three ordinary reboots for the KPI comparison,
  `abslot` successful, `boot_a` read back exactly. If it is not faster,
  leave `boot_a` at `5c509eb2` and record the result.
- Raw evidence in `~/chef-cyclo-evidence/boot-compression-20261005/`
  (private), a credential-free summary in `logs/`, build log entry, docs
  (`building.md`, the handoff status). Stage the diff, do not commit;
  report to `rootfs_research` with the staged diff SHA-256.

## Result (rootfs_impl, 2026-10-05)

Raw evidence: `~/chef-cyclo-evidence/boot-compression-20261005/` (private).
Summary: [logs](../../logs/boot-compression-2026-10-05.txt).

- **Format.** The abl disassembly gives the layout
  `"UNCOMPRESSED_IMG"` | LE32 `len(Image)` | raw `Image` | DTB; the reviewer
  confirmed it independently ([loader investigation](../research/chef-loader-kernel-budget.md#uncompressed-kernel-path)).
  abl copies the whole kernel section before its `image_size` check, so the
  budget is `max(image_size, kernel_size)`. abl finds the DTB through its board-id
  scanner; `androidboot.dtb_idx=-1347440721` proves that on every boot, both
  images.
- **Build.** `PACK_ONLY=1 UNCOMPRESSED=1 scripts/mkinstall.sh`
  ([building](../building.md#uncompressed-boot-image)). Kernel `da2e6c61`,
  `system_a` `9c47c6b8` and stamp `f52cfe21` are unchanged, and
  `out/boot-stage1.img` rebuilt byte-identical to `5c509eb2`.
  `out/boot-stage1-uncompressed.img`
  `df4743903c10b37584f88ebca09bc6b0be6ee6c5262cc9c4832e91571961e9ed` is 38666240
  bytes (`kernel_size` 36812914, ramdisk 1846784, loader margin 45539328).
- **`fastboot boot`, twice.** Both boots passed the short regression: USB network,
  `/` read-only from `system_a`, stamp match, Wi-Fi HTTPS, BT, crash counts 0,
  write sectors 0. Proxy timing, the same method for both images: abl needs
  2.20 s after the download with gzip and 0.96 s uncompressed. Counted from the
  start of `fastboot boot`, the USB network answers 0.48 s sooner, because the
  0.6 s longer upload eats half the saving. The ramdisk unpack drops from 26 to
  12 ms; kernel-side times are otherwise unchanged.
- **Flashed (user approved).** Guarded `fastboot flash boot_a`; `abslot`
  marked the slot and `boot_a` read back exactly.

  | | gzip `5c509eb2` | uncompressed `df474390` |
  |---|---|---|
  | KPI `end - start`, 3 ordinary reboots | 5.980 / 5.983 / 5.983 s (mean 5.982) | 5.779 / 5.786 / 5.789 s (mean 5.785) |
  | `load kernel` (eMMC read) | 2595 counts (79 ms) | 7413 counts (226 ms) |

  The flashed image is faster by 0.197 s, not the 1.1 s predicted from the
  `fastboot boot` numbers: on a flashed boot abl's inflate costs only about 0.34 s.
  The rollback rule set before the flash (stricter than the handoff's "at least as
  good") was to flash `5c509eb2` back unless the flashed mean was lower by more
  than the combined spread. The timing met it: 6450 counts against a spread of
  425.
- **Rolled back, not installed.** The rollback came from the hang
  investigation, not the timing: two of six reboots of the flashed image hung
  in shutdown, so `5c509eb2` was flashed back with the same guards to compare.
  It hung the same way (below), which rules out the packaging. `boot_a` stays
  `5c509eb2` (slot successful, read back exactly). Installing `df474390` for
  0.2 s is the user's call.
- **Charger mode.** The installed stage-1 layout ran the full OS on a
  `androidboot.mode=charger` boot (the phone was plugged back in while off): the
  check phase 3 left open.

### Shutdown hang (separate item)

**Resolved 2026-10-05:** a use-after-free in `hci_qca` when btattach closes at
shutdown, fixed in the kernel; see the
[shutdown hang handoff](shutdown-hang-handoff.md#result-rootfs_impl-2026-10-05).
The notes below are what was known before it was located.

Open item, separate from the boot image. Three of the 15 ordinary reboots
today came back after 32 to 49 s instead of about 21 s, with
`androidboot.bootreason=kernel_panic`, powerup reason `0x00020000` and a 'warm'
PON (normal reboots log 'cold'). It happened on both images: 2 of 6 shutdowns
on `df474390` and 1 of 9 on `5c509eb2`.

- The kernel log streamed over USB ends at the same line on every reboot:
  `chef-storage` has made `/data` read-only and detached it, and `umount -a -r`
  has remounted `mmcblk0p25`.
- In a hang the kernel stops answering ping within a few hundred ms after that
  line; normally it answers for about another 1.7 s. That points to busybox
  init's kill phase (SIGTERM and SIGKILL to the daemons) or the start of
  `reboot(2)`, not the boot image.
- Hangs happened at request uptimes of about 36 s (twice) and 58 s. Six normal
  reboots ran at similar uptimes, and the subsystem state at the request was
  the same as in a hang.
- Earlier evidence shows about 143 reboots without one, so as far as the
  evidence goes it appears only since the phase 3 read-only root layout. Most
  of those earlier reboots also ran at longer uptimes.
- ramoops probes but keeps no console across a warm reset, so pstore cannot
  record this. Locating it needs a netconsole or kill-phase markers; that is
  outside this task. `/data` was already detached in every case, and each hang
  ended in an automatic reset into the installed image (see the
  [live-testing gotcha](../live-testing.md#gotchas)).
