import gzip
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("budget", REPO / "scripts/check-chef-loader-budget.py")
budget = importlib.util.module_from_spec(spec)
spec.loader.exec_module(budget)


def image(size, payload=b""):
    header = bytearray(64)
    struct.pack_into("<Q", header, 16, size)
    header[56:60] = b"ARM\x64"
    return bytes(header) + payload


class ChefBudgetTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.kernel = Path(self.tmp.name) / "Image.gz-dtb"
        self.ramdisk = Path(self.tmp.name) / "ramdisk"
        self.ramdisk.write_bytes(b"r" * 4096)

    def write_kernel(self, size, payload=b""):
        self.kernel.write_bytes(gzip.compress(image(size, payload), mtime=0) + b"\xd0\x0d\xfe\xedDTB")

    def test_equal_bound_passes_and_one_byte_over_fails(self):
        capacity = budget.kernel_capacity(4096)
        self.write_kernel(capacity)
        self.assertEqual(budget.check_budget(self.kernel, self.ramdisk)[3], 0)
        self.write_kernel(capacity + 1)
        with self.assertRaisesRegex(ValueError, "overflow=1 bytes"):
            budget.check_budget(self.kernel, self.ramdisk)

    def test_one_ramdisk_byte_crosses_page_boundary(self):
        self.write_kernel(budget.kernel_capacity(4096))
        self.ramdisk.write_bytes(b"r" * 4097)
        with self.assertRaisesRegex(ValueError, "overflow=4096 bytes"):
            budget.check_budget(self.kernel, self.ramdisk)

    def test_known_artifact_capacities(self):
        self.assertEqual(budget.kernel_capacity(47381624), 40165376)
        self.assertEqual(budget.kernel_capacity(32613658), 54931456)
        self.assertEqual(budget.kernel_capacity(47381624) - 40095744, 69632)
        self.assertEqual(budget.kernel_capacity(47381624) - 40493056, -327680)
        self.assertEqual(budget.kernel_capacity(47381624) - 41373696, -1208320)

    def test_negative_capacity_rejected(self):
        with self.ramdisk.open("wb") as stream:
            stream.truncate(budget.REGION_SIZE)
        self.write_kernel(64)
        with self.assertRaisesRegex(ValueError, "budget exceeded"):
            budget.check_budget(self.kernel, self.ramdisk)

    def test_invalid_and_short_image_headers(self):
        for raw in [b"short", bytes(64), image(0), image(63)]:
            with self.subTest(raw=raw):
                self.kernel.write_bytes(gzip.compress(raw))
                with self.assertRaises(ValueError):
                    budget.read_image_size(self.kernel)

    def test_invalid_and_truncated_gzip(self):
        valid = gzip.compress(image(65536, b"x" * 65536))
        for raw in [b"not gzip", valid[:8], valid[:-8], valid[:-1]]:
            with self.subTest(length=len(raw)):
                self.kernel.write_bytes(raw)
                with self.assertRaises(ValueError):
                    budget.read_image_size(self.kernel)

    def test_corrupt_crc_rejected_after_valid_header(self):
        raw = bytearray(gzip.compress(image(65536)))
        raw[-8] ^= 1
        self.kernel.write_bytes(raw)
        with self.assertRaisesRegex(ValueError, "CRC/trailer"):
            budget.read_image_size(self.kernel)

    def test_empty_fname_and_appended_dtb(self):
        raw = bytearray(gzip.compress(image(0x263D000, b"x" * 200000)))
        raw[3] |= 8
        raw[10:10] = b"\0"
        raw.extend(b"\xd0\x0d\xfe\xednot-gzip-DTB")
        self.kernel.write_bytes(raw)
        self.assertEqual(budget.read_image_size(self.kernel), 0x263D000)
        self.assertEqual(self.kernel.read_bytes(), raw)

    def test_mkboot_rejection_preserves_existing_output(self):
        self.write_kernel(budget.kernel_capacity(4096) + 1)
        output = Path(self.tmp.name) / "boot.img"
        output.write_bytes(b"protected existing output")
        result = subprocess.run(
            ["sh", "scripts/mkboot.sh"], cwd=REPO,
            env={**os.environ, "KERNEL": str(self.kernel), "RAMDISK": str(self.ramdisk), "OUT": str(output)},
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Shrink the ramdisk", result.stderr)
        self.assertEqual(output.read_bytes(), b"protected existing output")


if __name__ == "__main__":
    unittest.main()
