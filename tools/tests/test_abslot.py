#!/usr/bin/env python3
"""Host tests for tools/abslot (host build) on synthetic GPT disk images.

Each test builds a small disk image with a primary and a backup GPT, a fake
sysfs (class/block/mmcblk0pN uevent/start/size, block/mmcblk0 size and
logical block size) and a fake /proc/cmdline, then runs the real binary with
its -d/-s/-c/-K/-L overrides. Checked: status output, mark-successful
changing exactly header bytes 16-19, 88-91 and bit 6 of boot_a's attribute
byte 6 in each copy (and nothing else in the file), idempotence, the
half-written state (backup marked, primary not) reported and completed,
every refusal rule leaving the image byte-identical, and the kmsg/log lines.
Both copies are re-validated with scripts/gpt-slots.py's parser.
"""

import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parent
BIN = TOOLS / "abslot"
spec = importlib.util.spec_from_file_location("gpt_slots", REPO / "scripts/gpt-slots.py")
gpt_slots = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpt_slots)

SECTOR = 512
DISK = 4096           # sectors
COUNT = 69            # entries, as on the phone: the array ends mid-sector
ARRAY_SECTORS = 32
BACKUP_ARRAY = DISK - 1 - ARRAY_SECTORS
GUID = bytes(range(16))
# boot_a/boot_b as on the phone right after `fastboot flash boot_a` + 1 boot.
BOOT_A = 0x0037000000000048
BOOT_B = 0x00F0000000000048
SUCCESS = 1 << 54

# (name, first LBA, sectors); boot_a is entry 44 (index 43) like mmcblk0p44.
LAYOUT = [(f"part{i}", 100 + 10 * i, 10) for i in range(COUNT)]
LAYOUT[43] = ("boot_a", 100 + 10 * 43, 10)
LAYOUT[44] = ("boot_b", 100 + 10 * 44, 10)


def entry_array(attrs):
    array = bytearray(COUNT * 128)
    for i, (name, first, n) in enumerate(LAYOUT):
        e = memoryview(array)[i * 128:(i + 1) * 128]
        e[0:16] = bytes([0xA0 + i % 16]) * 16
        e[16:32] = bytes([i]) * 16
        struct.pack_into("<QQQ", e, 32, first, first + n - 1, attrs.get(name, 0x48))
        nm = name.encode("utf-16le")
        e[56:56 + len(nm)] = nm
    return bytes(array)


def header(my_lba, alt_lba, array_lba, array, hsize=92):
    h = bytearray(SECTOR)
    h[0:8] = b"EFI PART"
    struct.pack_into("<III", h, 8, 0x00010000, hsize, 0)
    struct.pack_into("<QQQQ", h, 24, my_lba, alt_lba, 34, DISK - 34)
    h[56:72] = GUID
    struct.pack_into("<QIII", h, 72, array_lba, COUNT, 128, zlib.crc32(array))
    struct.pack_into("<I", h, 16, zlib.crc32(h[:hsize]) if 92 <= hsize <= SECTOR else 0)
    return bytes(h)


def build_disk(pri_attrs=None, bak_attrs=None, hsize=92):
    pri = entry_array(pri_attrs or {"boot_a": BOOT_A, "boot_b": BOOT_B})
    bak = entry_array(bak_attrs or pri_attrs or {"boot_a": BOOT_A, "boot_b": BOOT_B})
    d = bytearray(DISK * SECTOR)
    d[SECTOR:2 * SECTOR] = header(1, DISK - 1, 2, pri, hsize)
    d[2 * SECTOR:2 * SECTOR + len(pri)] = pri
    d[BACKUP_ARRAY * SECTOR:BACKUP_ARRAY * SECTOR + len(bak)] = bak
    d[(DISK - 1) * SECTOR:] = header(DISK - 1, 1, BACKUP_ARRAY, bak, hsize)
    # Filler outside the GPT so a stray write would show.
    for lba in (0, 40, DISK - 40):
        d[lba * SECTOR:(lba + 1) * SECTOR] = bytes([0x5A]) * SECTOR
    return bytes(d)


def copies(data):
    """(primary header, primary array, backup header, backup array) bytes."""
    return (data[SECTOR:2 * SECTOR], data[2 * SECTOR:2 * SECTOR + COUNT * 128],
            data[(DISK - 1) * SECTOR:], data[BACKUP_ARRAY * SECTOR:BACKUP_ARRAY * SECTOR + COUNT * 128])


def diff_offsets(a, b):
    return [i for i in range(len(a)) if a[i] != b[i]]


class AbslotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BIN.exists():
            raise unittest.SkipTest("build tools/abslot first (make -C tools abslot)")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        t = Path(self.tmp.name)
        self.disk, self.sys, self.cmdline = t / "disk.img", t / "sys", t / "cmdline"
        self.kmsg, self.log = t / "kmsg", t / "abslot.log"
        self.cmdline.write_text("root=/dev/mmcblk0p67 androidboot.slot_suffix=_a skip_initramfs\n")
        self.sysfs(partno=44, start=LAYOUT[43][1], size=LAYOUT[43][2])

    def sysfs(self, partno, start, size, lbs=SECTOR, extra=None):
        blk = self.sys / "block/mmcblk0"
        (blk / "queue").mkdir(parents=True, exist_ok=True)
        (blk / "size").write_text(f"{DISK}\n")
        (blk / "queue/logical_block_size").write_text(f"{lbs}\n")
        cls = self.sys / "class/block"
        for d in [cls / "mmcblk0", *cls.glob("mmcblk0p*")]:
            if d.exists():
                for f in d.iterdir():
                    f.unlink()
        parts = {partno: ("boot_a", start, size), 25: ("modem_a", 1, 1)}
        parts.update(extra or {})
        for n, (name, s, sz) in parts.items():
            d = cls / f"mmcblk0p{n}"
            d.mkdir(parents=True, exist_ok=True)
            (d / "uevent").write_text(f"MAJOR=179\nMINOR={n}\nDEVNAME=mmcblk0p{n}\n"
                                      f"DEVTYPE=partition\nPARTN={n}\nPARTNAME={name}\n")
            (d / "start").write_text(f"{s}\n")
            (d / "size").write_text(f"{sz}\n")

    def run_abslot(self, cmd, data=None):
        if data is not None:
            self.disk.write_bytes(data)
        return subprocess.run([str(BIN), "-d", str(self.disk), "-s", str(self.sys),
                               "-c", str(self.cmdline), "-K", str(self.kmsg), "-L", str(self.log), cmd],
                              capture_output=True, text=True)

    def assert_refused(self, data, cmd="mark-successful", needle=None):
        r = self.run_abslot(cmd, data)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertEqual(self.disk.read_bytes(), data, "refusal changed the image")
        if needle:
            self.assertIn(needle, r.stdout)
        return r

    def assert_valid_and_marked(self, data):
        alt, array, size = gpt_slots.primary(data)
        self.assertEqual(gpt_slots.backup(data[-33 * SECTOR:], alt), array)
        attrs = {name: attr for _, name, attr in gpt_slots.entries(array, size)}
        self.assertEqual(attrs["boot_a"], BOOT_A | SUCCESS)
        self.assertEqual(attrs["boot_b"], BOOT_B)

    def test_status_reads_without_writing(self):
        data = build_disk()
        r = self.run_abslot("status", data)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("boot_a p44 attr=0x0037000000000048 priority=3 active=1 retry=6 successful=0", r.stdout)
        self.assertIn("boot_b p45", r.stdout)
        self.assertEqual(self.disk.read_bytes(), data)

    def test_mark_changes_exactly_the_abl_bytes(self):
        before = build_disk()
        r = self.run_abslot("mark-successful", before)
        self.assertEqual(r.returncode, 0, r.stdout)
        after = self.disk.read_bytes()
        self.assert_valid_and_marked(after)
        at = 43 * 128 + 48 + 6
        for (hb, ab), (ha, aa) in zip(zip(copies(before)[::2], copies(before)[1::2]),
                                      zip(copies(after)[::2], copies(after)[1::2])):
            self.assertEqual(set(diff_offsets(hb, ha)) - set(range(16, 20)) - set(range(88, 92)), set())
            self.assertEqual(diff_offsets(ab, aa), [at])
            self.assertEqual(ab[at] ^ aa[at], 0x40)
        # Nothing outside the four written sectors moved.
        changed = {i // SECTOR for i in diff_offsets(before, after)}
        self.assertEqual(changed, {1, 2 + at // SECTOR, BACKUP_ARRAY + at // SECTOR, DISK - 1})
        self.assertIn("0x0037000000000048 -> 0x0077000000000048", r.stdout)
        self.assertIn("written and verified", r.stdout)
        self.assertIn("abslot: boot_a attr 0x0037000000000048 -> 0x0077000000000048", self.kmsg.read_text())
        self.assertIn("boot_a marked successful", self.log.read_text())

    def test_mark_is_idempotent(self):
        self.run_abslot("mark-successful", build_disk())
        marked = self.disk.read_bytes()
        r = self.run_abslot("mark-successful")
        self.assertEqual(r.returncode, 0)
        self.assertIn("already marked successful, nothing written", r.stdout)
        self.assertEqual(self.disk.read_bytes(), marked)

    def test_half_marked_state_is_reported_and_completed(self):
        half = build_disk(pri_attrs={"boot_a": BOOT_A, "boot_b": BOOT_B},
                          bak_attrs={"boot_a": BOOT_A | SUCCESS, "boot_b": BOOT_B})
        r = self.run_abslot("status", half)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("half-marked", r.stdout)
        r = self.run_abslot("mark-successful", half)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("completing the primary", r.stdout)
        after = self.disk.read_bytes()
        self.assert_valid_and_marked(after)
        self.assertEqual(after[(DISK - 33) * SECTOR:], half[(DISK - 33) * SECTOR:], "backup rewritten")
        self.run_abslot("mark-successful", build_disk())
        self.assertEqual(after, self.disk.read_bytes(), "differs from an uninterrupted mark")

    def test_reverse_half_state_is_refused(self):
        rev = build_disk(pri_attrs={"boot_a": BOOT_A | SUCCESS, "boot_b": BOOT_B},
                         bak_attrs={"boot_a": BOOT_A, "boot_b": BOOT_B})
        self.assert_refused(rev, needle="primary marked successful, backup not")
        self.assert_refused(rev, "status")

    def test_other_array_differences_are_refused(self):
        self.assert_refused(build_disk(pri_attrs={"boot_a": BOOT_A, "boot_b": BOOT_B},
                                       bak_attrs={"boot_a": BOOT_A, "boot_b": BOOT_B | 1}),
                            needle="entry arrays differ")
        self.assert_refused(build_disk(pri_attrs={"boot_a": BOOT_A, "boot_b": BOOT_B},
                                       bak_attrs={"boot_a": BOOT_A | SUCCESS | 1, "boot_b": BOOT_B}),
                            needle="entry arrays differ")

    def test_wrong_slot_is_refused(self):
        self.cmdline.write_text("androidboot.slot_suffix=_b\n")
        self.assert_refused(build_disk(), needle="did not boot slot _a")
        self.cmdline.write_text("androidboot.slot_suffix=_ab\n")
        self.assert_refused(build_disk(), needle="did not boot slot _a")

    def test_sector_size_is_checked(self):
        self.sysfs(partno=44, start=LAYOUT[43][1], size=LAYOUT[43][2], lbs=4096)
        self.assert_refused(build_disk(), needle="logical block size")

    def test_kernel_partition_must_match_the_entry(self):
        self.sysfs(partno=44, start=LAYOUT[43][1] + 1, size=LAYOUT[43][2])
        self.assert_refused(build_disk(), needle="does not match the kernel")
        self.sysfs(partno=44, start=LAYOUT[43][1], size=LAYOUT[43][2] + 1)
        self.assert_refused(build_disk(), needle="does not match the kernel")
        self.sysfs(partno=45, start=LAYOUT[43][1], size=LAYOUT[43][2])
        self.assert_refused(build_disk(), needle="does not match the kernel")
        self.sysfs(partno=44, start=1, size=1, extra={46: ("boot_a", 1, 1)})
        self.assert_refused(build_disk(), needle="no single PARTNAME=boot_a")

    def test_incomplete_uevent_is_refused(self):
        u = self.sys / "class/block/mmcblk0p44/uevent"
        u.write_text("MAJOR=179\nMINOR=44\nDEVNAME=mmcblk0p44\nPARTNAME=boot_a\n")
        self.assert_refused(build_disk(), needle="no single PARTNAME=boot_a")

    def test_duplicate_or_missing_boot_a_entry_is_refused(self):
        global LAYOUT
        saved = list(LAYOUT)
        try:
            LAYOUT[10] = ("boot_a", LAYOUT[10][1], LAYOUT[10][2])
            self.assert_refused(build_disk(), needle="more than one boot_a")
            LAYOUT[:] = saved
            LAYOUT[43] = ("boot_x", LAYOUT[43][1], LAYOUT[43][2])
            self.assert_refused(build_disk(), needle="no boot_a")
        finally:
            LAYOUT[:] = saved

    def test_slot_state_must_be_the_running_slot(self):
        for attr in (BOOT_A & ~(1 << 50),            # not active
                     BOOT_A | (1 << 55),             # unbootable
                     BOOT_A & ~(3 << 48)):           # priority 0
            self.assert_refused(build_disk(pri_attrs={"boot_a": attr, "boot_b": BOOT_B}),
                                needle="not the active bootable slot")

    def test_corrupt_copies_are_refused(self):
        good = build_disk()
        for off, needle in ((SECTOR + 30, "primary header: CRC"),         # primary header byte
                            (2 * SECTOR + 43 * 128 + 50, "primary entries: CRC"),
                            ((DISK - 1) * SECTOR + 40, "backup header: CRC"),
                            (BACKUP_ARRAY * SECTOR + 5, "backup entries: CRC"),
                            (SECTOR, "primary header: no GPT signature")):
            bad = bytearray(good)
            bad[off] ^= 1
            self.assert_refused(bytes(bad), needle=needle)
            self.assert_refused(bytes(bad), "status")
        for hsize in (91, 513):
            self.assert_refused(build_disk(hsize=hsize), needle="header: size")

    def test_headers_must_agree(self):
        d = bytearray(build_disk())
        h = bytearray(d[(DISK - 1) * SECTOR:])
        h[56] ^= 1                                    # backup disk GUID
        struct.pack_into("<I", h, 16, 0)
        struct.pack_into("<I", h, 16, zlib.crc32(h[:92]))
        d[(DISK - 1) * SECTOR:] = h
        self.assert_refused(bytes(d), needle="headers disagree")

    def test_backup_alternate_must_point_to_lba_1(self):
        d = bytearray(build_disk())
        bak = d[BACKUP_ARRAY * SECTOR:BACKUP_ARRAY * SECTOR + COUNT * 128]
        d[(DISK - 1) * SECTOR:] = header(DISK - 1, 2, BACKUP_ARRAY, bak)
        self.assert_refused(bytes(d), needle="alternate 2 (want 1)")

    def test_direct_io_path_on_a_disk_filesystem(self):
        # /tmp is often tmpfs, which refuses O_DIRECT (the logged fallback);
        # run one full mark where O_DIRECT works, as on the phone.
        out = REPO / "out"
        if not out.is_dir():
            self.skipTest("no out/ directory on a disk filesystem")
        with tempfile.TemporaryDirectory(dir=out) as d:
            self.disk = Path(d) / "disk.img"
            before = build_disk()
            r = self.run_abslot("mark-successful", before)
            if "refuses O_DIRECT" in r.stdout:
                self.skipTest("out/ does not support O_DIRECT either")
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assert_valid_and_marked(self.disk.read_bytes())
            changed = {i // SECTOR for i in diff_offsets(before, self.disk.read_bytes())}
            self.assertEqual(len(changed), 4)

    def test_tmpfs_fallback_is_logged(self):
        r = self.run_abslot("status", build_disk())
        self.assertEqual(r.returncode, 0, r.stdout)
        if "refuses O_DIRECT" in r.stdout:
            self.assertIn("TEST OVERRIDE", r.stdout)

    def test_bad_usage(self):
        r = subprocess.run([str(BIN), "-K", "/dev/null", "-L", "/dev/null", "frobnicate"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 64)


if __name__ == "__main__":
    unittest.main()
