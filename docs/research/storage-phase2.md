# Phase 2 storage research, 2026-10-05

Research owner: `storage_research`. Implementation and all live device sessions:
`storage_impl`. Independent review: `storage_review`. This records host-source
findings and recommendations, not completed live acceptance.

## Motorola userdata configuration

`CONFIG_EXT4_USERDATA_BLKNUM=70` does not choose a filesystem target. Its only
use is `force_nodiscard()` in `kernel/fs/ext4/super.c`, guarded by
`CONFIG_EXT4_FORCE_NODISCARD`. It matches a MICRON manufacturer and a device
name containing `mmcblk0p70`. The resulting global flag changes discard handling
for ranges of at least 512 blocks in `kernel/fs/ext4/mballoc.c`.

The current `out/kernel/.config` has `CONFIG_EXT4_FORCE_NODISCARD` disabled.
No change to that partition-number setting is needed for phase 2. Resolve the
actual device by unique `PARTNAME=userdata` and validate 55289298432 bytes.
Use explicit `nodiscard` at mount time and `-E nodiscard` at format time.

## Format compatibility

The downstream support masks in `kernel/fs/ext4/ext4.h` include extents,
64bit, flex_bg, metadata checksums and checksum seed. In particular, the
downstream tree supports `CSUM_SEED`; assuming it lacks this solely because
the kernel is 4.4 would be wrong. It lacks the newer large-directory support
bit. Keep the chefdata format conservative and deterministic anyway.

Use a dedicated `MKE2FS_CONFIG`, explicit 4096-byte blocks and 256-byte
inodes, and a positive feature allowlist beginning with `-O none,...`.
Recommended core features: `has_journal,ext_attr,resize_inode,dir_index,filetype,
extent,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum`.
The final list must be proved with the actual phone kernel and packaged
formatter, and recorded from `dumpe2fs -h`. Exclude `orphan_file`,
`orphan_present`, `fast_commit`, `large_dir`, `casefold`, `verity` and
encryption. There is no need for 64bit block addresses at this size.

Implementation selected a different supported allowlist and proved it live:
`has_journal,ext_attr,resize_inode,dir_index,filetype,extent,64bit,flex_bg,
sparse_super,large_file,huge_file,uninit_bg,dir_nlink,extra_isize`.
It uses 64bit descriptors and group-descriptor checksums (`uninit_bg`),
without `metadata_csum` or `metadata_csum_seed`. The earlier core list above
is a research recommendation, not the actual provisioned feature set.

Configuration feature edits accumulate; a dedicated config prevents package
defaults leaking into the layout. `-O none` clears the existing feature set.
Set `lazy_journal_init=0`; lazy inode initialization is a separate choice,
and disabling it makes initialization complete before the first mount.
See the upstream [configuration manual](https://man7.org/linux/man-pages/man5/mke2fs.conf.5.html)
and [formatter manual](https://man7.org/linux/man-pages/man8/mke2fs.8.html).

## Recovery and boot decisions

Validate slot, unique partition, exact size, label and version marker before
running any repairing checker. Read the marker with debugfs without `-w`,
not by mounting the foreign filesystem. The handoff already records this
kernel's orphan-truncation hazard even with `ro,noload`.

Accept checker results 0 (clean) and 1 (repaired). Treat 2 or any combined
result containing it as requiring further action; remain RAM-only rather
than mounting. All other errors and timeout must leave storage unmounted.
Checker statuses are a bitmask, not a simple success/failure boolean. Use a
bounded checker with a tested termination mechanism and log its result.
See the upstream [checker manual](https://man7.org/linux/man-pages/man8/e2fsck.8.html).

Keep `broken_system_clock=1` because early boot does not yet have trusted
wall time. Log to RAM and kmsg so failures never depend on writable `/data`.

## Durability and shutdown

Recommend `noatime,nodiscard,data=ordered,commit=5,errors=remount-ro` with
barriers enabled. Local `kernel/Documentation/filesystems/ext4.txt` documents
ordered data and the default five-second commit interval. This balances
write frequency and recovery, but it is not an application durability
guarantee. Future ride/state consumers need explicit fsync and atomic
replacement, including directory fsync when changing names.

BusyBox init runs shutdown actions before terminating other processes, as
the handoff notes. Stop consumer writers, sync, then remount the filesystem
read-only through an existing alias before detaching any binds or `/data`.
An ordinary ext4 read-only remount changes the shared superblock state, so
surviving bind aliases cannot stay writable if a detach fails. Find aliases
by the validated userdata major/minor even when `/data` is already detached.
Live RAM-loop tests verified EROFS through a surviving alias, including when
`/data` was absent. Avoid killing the shutdown
helper itself through a broad fuser match. Init owns termination of remaining
processes. Verify plain reboot, button/powerd shutdown and the bootloader
restart path; forced sysrq/PMIC resets intentionally bypass the hook.

NetworkManager already has its keyfile path set to
`/run/NetworkManager/system-connections`; bind the persistent directory there
before service startup. BlueZ uses `/var/lib/bluetooth`. Keep these directories
root-only and connection profiles at 0600. No credentials belong in committed
evidence. Physical hard reset testing requires user coordination and a
countdown; implementation owns all device operations.
