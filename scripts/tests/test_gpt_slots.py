import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gpt_slots", REPO / "scripts/gpt-slots.py")
gpt_slots = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpt_slots)


LAST = 1000  # last LBA of the fake disk


def array_of(parts, count=4, size=128):
    array = bytearray(count * size)
    for i, (name, attr) in enumerate(parts):
        e = memoryview(array)[i * size:(i + 1) * size]
        e[0:16] = bytes([i + 1]) * 16
        struct.pack_into("<Q", e, 48, attr)
        n = name.encode("utf-16le")
        e[56:56 + len(n)] = n
    return bytes(array)


def header(my_lba, alt_lba, entries_lba, array, count=4, size=128, hsize=92):
    h = bytearray(512)
    h[:8] = b"EFI PART"
    struct.pack_into("<I", h, 12, hsize)
    struct.pack_into("<QQ", h, 24, my_lba, alt_lba)
    struct.pack_into("<QIII", h, 72, entries_lba, count, size, zlib.crc32(array))
    struct.pack_into("<I", h, 16, zlib.crc32(h[:hsize]))
    return bytes(h[:512]).ljust(512, b"\0")


def disk(parts):
    """A disk head with a primary GPT whose entries are (name, attributes)."""
    array = array_of(parts)
    return bytes(512) + header(1, LAST, 2, array) + array


def tail(parts, alt=1):
    """The last two sectors of the same disk: backup entry array and header."""
    array = array_of(parts)
    return array[:512] + header(LAST, alt, LAST - 1, array)


class GptSlotsTests(unittest.TestCase):
    def test_flags_of_the_live_boot_a_attributes(self):
        # boot_a and boot_b as read from the phone on 2026-10-04.
        self.assertEqual(gpt_slots.slot_flags(0x0077000000000048),
                         {"priority": 3, "active": 1, "retry": 6, "successful": 1, "unbootable": 0})
        self.assertEqual(gpt_slots.slot_flags(0x00f0000000000048),
                         {"priority": 0, "active": 0, "retry": 6, "successful": 1, "unbootable": 1})

    def test_entries_reads_names_and_attributes(self):
        alt, array, size = gpt_slots.primary(disk([("boot_a", 0x0077 << 48), ("userdata", 0x18)]))
        self.assertEqual(alt, LAST)
        rows = list(gpt_slots.entries(array, size))
        self.assertEqual(rows, [(1, "boot_a", 0x0077 << 48), (2, "userdata", 0x18)])

    def test_corrupt_header_or_array_is_refused(self):
        d = bytearray(disk([("boot_a", 1 << 54)]))
        d[1024 + 48] ^= 1
        with self.assertRaisesRegex(ValueError, "entry array CRC"):
            gpt_slots.primary(bytes(d))
        d = bytearray(disk([("boot_a", 1 << 54)]))
        d[512 + 40] ^= 1
        with self.assertRaisesRegex(ValueError, "header CRC"):
            gpt_slots.primary(bytes(d))
        with self.assertRaisesRegex(ValueError, "no GPT header"):
            gpt_slots.primary(bytes(2048))

    def test_backup_copy_is_checked(self):
        parts = [("boot_a", 0x0077 << 48)]
        self.assertEqual(gpt_slots.backup(tail(parts), LAST), array_of(parts))
        t = bytearray(tail(parts))
        t[48] ^= 1
        with self.assertRaisesRegex(ValueError, "entry array CRC"):
            gpt_slots.backup(bytes(t), LAST)
        with self.assertRaisesRegex(ValueError, "says it is at LBA"):
            gpt_slots.backup(tail(parts), LAST + 1)

    def test_header_size_and_backup_alternate_are_checked(self):
        array = array_of([("boot_a", 1 << 54)])
        for hsize in (91, 513, 0xFFFFFFFF):
            with self.assertRaisesRegex(ValueError, "has size"):
                gpt_slots.parse_header(header(1, LAST, 2, array, hsize=hsize), 1)
        with self.assertRaisesRegex(ValueError, "not 1"):
            gpt_slots.backup(tail([("boot_a", 1 << 54)], alt=2), LAST)

    def test_cli_requires_identical_copies(self):
        with tempfile.TemporaryDirectory() as d:
            head, back = Path(d, "head"), Path(d, "tail")
            head.write_bytes(disk([("boot_a", 0x0077 << 48)]))
            back.write_bytes(tail([("boot_a", 0x0077 << 48)]))
            cmd = [sys.executable, str(REPO / "scripts/gpt-slots.py"), str(head), "--backup", str(back)]
            ok = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            self.assertIn("valid and identical", ok.stdout)
            back.write_bytes(tail([("boot_a", 0x0037 << 48)]))
            bad = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(bad.returncode, 1)
            self.assertIn("arrays differ", bad.stderr)

    def test_cli_filters_to_slotted_partitions(self):
        with tempfile.NamedTemporaryFile() as f:
            f.write(disk([("boot_a", 0x0077 << 48), ("userdata", 0x18)]))
            f.flush()
            out = subprocess.run([sys.executable, str(REPO / "scripts/gpt-slots.py"), f.name],
                                 capture_output=True, text=True, check=True).stdout
        self.assertIn("boot_a", out)
        self.assertIn("retry=6 successful=1", out)
        self.assertNotIn("userdata", out)


if __name__ == "__main__":
    unittest.main()
