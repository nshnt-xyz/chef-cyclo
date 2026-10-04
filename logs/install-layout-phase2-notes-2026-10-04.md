# Install layout: phase 2 notes (on hold) and related evidence, 2026-10-04

Phase 2 (`/data` on `userdata`) was put on hold by the user before anything was written; `userdata` is untouched. These are gnss_impl's research notes and draft design, the coordinator's phase 3 boot-timing baseline, and the read-only `system_b` hash check from the superseded `system_b` plan. Raw copies: `~/chef-cyclo-evidence/install-layout-20261004/` and `~/chef-cyclo-evidence/storage-20261004/`.

## Phase 2 notes

    Not started on the phone: nothing written, userdata untouched.
    Kernel: EXT4_FS=y, BLK_DEV_LOOP=y (for the feature-proof loop image), MAGIC_SYSRQ=y
    (sysrq=1 on the phone), EXT4_ENCRYPTION=y. Motorola extra: CONFIG_EXT4_USERDATA_BLKNUM=70
    -- check kernel/fs/ext4 for what it does before formatting userdata (mmcblk0p69 here).
    Rootfs has no e2fsprogs yet (add to mkrootfs PKGS; not in the apk cache, needs a fetch).
    busybox has timeout, fuser, blkid, mountpoint, flock.
    Reviewer reminders: exact size 55289298432 bytes; pin mke2fs features (no orphan_file,
    check metadata_csum_seed / large_dir against 4.4) via a dedicated MKE2FS_CONFIG and prove
    with a loop image on the phone; /etc/e2fsck.conf broken_system_clock=1; mount in /init
    before exec init; busybox ::shutdown runs BEFORE SIGTERM, so stop writers (fuser -m /data)
    before remount ro/umount; record the userdata superblock (dumpe2fs -h + raw) before format;
    send the plan to gnss_review before any format.
    Draft design: initramfs/usr/bin/chef-storage {status, format --yes-erase-userdata, boot,
    shutdown}; boot decision = slot _a + PARTNAME=userdata + exact size + label 'chefdata' from
    the superblock + marker via read-only debugfs (host-testable without mounting), then
    timeout e2fsck -p, mount noatime,commit=5,errors=remount-ro, bind /data/bluetooth ->
    /var/lib/bluetooth and /data/networkmanager/system-connections ->
    /run/NetworkManager/system-connections. phone-boot.sh: sync before btprobe restart.
    Consumer tests: NM Wi-Fi profile autoconnects after reboot; BT bond with the PC adapter.
    Power loss: sysrq-b under a write loop x N, PMIC 8.7 s reset with the user, e2fsck -fn
    after unmount.
    Coordinator phase 3 timing notes: PHASE3-NOTES.md.

## Phase 3 boot-timing baseline

    Baseline LZMA boot timing: 'Trying to unpack rootfs image as initramfs' 0.387 s,
    'Freeing initrd memory: 22576K' 5.987 s (5.6 s single-threaded LZMA unpack),
    'Freeing unused kernel memory' (/init start) 9.07 s.
    Phase 3: keep stage-1 small, measure these three before/after, choose stage-1
    compression by measured unpack time (gzip / lz4 likely beat LZMA; check
    CONFIG_RD_LZ4 / CONFIG_RD_GZIP), record in the build log.

## system_b hash check (read-only, before the plan changed)

Host backup:

    06f4d3fdca14b4c0b1f69c039c02423e07529cc0704cb55cfead59ca1b0843ee  stock/partitions/system_b.img

Device (`sha256sum /dev/mmcblk0p68`, then exit status and size in bytes):

    Sun Oct  4 18:12:26 UTC 2026
    06f4d3fdca14b4c0b1f69c039c02423e07529cc0704cb55cfead59ca1b0843ee  /dev/mmcblk0p68
    real    0m 41.51s
    user    0m 37.78s
    sys     0m 2.59s
    0
    2684354560
