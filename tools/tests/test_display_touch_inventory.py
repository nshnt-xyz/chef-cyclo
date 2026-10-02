"""Saved metadata fixture: verify allowlist, missing fields and no helper opens."""
import pathlib
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "initramfs/usr/bin/display-touch-inventory"


class InventoryTest(unittest.TestCase):
    def test_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fields = {
                "sys/class/graphics/fb0/virtual_size": "1080,2246",
                "sys/class/graphics/fb0/bits_per_pixel": "32",
                "sys/class/graphics/fb0/stride": "4352",
                "sys/class/input/event7/device/name": "NT36xxx",
                "sys/class/input/event7/device/properties": "2",
                "sys/class/input/event7/device/capabilities/abs": "260800000000000",
                "sys/class/input/event3/device/name": "power button",
            }
            for name, value in fields.items():
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(value + "\n")
            # FIFOs hang if accidentally read. Guard every hazardous path.
            import os
            for name in ("sys/class/graphics/fb0/msm_fb_panel_status",
                         "sys/class/input/event7/device/buildid",
                         "sys/class/input/event7/device/ic_ver",
                         "proc/nvt_fw_version", "proc/NVTflash", "dev/fb0"):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                os.mkfifo(target)
            for shell in (["sh"], ["busybox", "sh"]):
                result = subprocess.run(shell + [str(SCRIPT), "--root", directory],
                                        text=True, capture_output=True, timeout=3, check=True)
                self.assertIn("virtual_size: 1080,2246", result.stdout)
                self.assertIn("stride: 4352", result.stdout)
                self.assertIn("input event3", result.stdout)
                self.assertIn("name: NT36xxx", result.stdout)
                self.assertIn("properties: 2", result.stdout)
                self.assertIn("id/vendor: unavailable", result.stdout)
                self.assertEqual(result.stdout.count("ABS ranges: skipped"), 2)
                self.assertNotIn("panel_status", result.stdout)

    def test_empty_and_invalid(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(["sh", str(SCRIPT), "--root", directory],
                                    text=True, capture_output=True, check=True)
            self.assertIn("framebuffer: none found", result.stdout)
            self.assertIn("input: none found", result.stdout)
        for args in (["--root"], ["--root", ""], ["--unknown"]):
            self.assertEqual(subprocess.run(["sh", str(SCRIPT)] + args,
                             capture_output=True).returncode, 64)


if __name__ == "__main__":
    unittest.main()
