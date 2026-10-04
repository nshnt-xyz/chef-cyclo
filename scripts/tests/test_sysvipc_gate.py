import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class SysvipcGateTests(unittest.TestCase):
    """chrony.conf's GPS SHM refclock needs CONFIG_SYSVIPC=y: chronyd exits when
    it cannot attach the segment, so the build refuses a kernel without it."""

    def test_config_fragment_backs_the_shm_refclock(self):
        chrony = (REPO / "initramfs/etc/chrony/chrony.conf").read_text()
        fragment = (REPO / "kernel-config/chef-cyclo.config").read_text().splitlines()
        self.assertIn("refclock SHM 0 ", chrony)
        self.assertIn("CONFIG_SYSVIPC=y", fragment)

    def test_mkinitramfs_refuses_kconfig_without_sysvipc(self):
        if not (REPO / "out/rootfs/bin/busybox").exists():
            self.skipTest("out/rootfs not built")
        # Run a copy in a scratch tree (rootfs and overlay linked read-only), so a
        # regressed gate builds into the scratch tree, never into out/.
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            (tree / "scripts").mkdir()
            (tree / "scripts/mkinitramfs.sh").write_text((REPO / "scripts/mkinitramfs.sh").read_text())
            (tree / "out").mkdir()
            (tree / "out/rootfs").symlink_to(REPO / "out/rootfs")
            (tree / "initramfs").symlink_to(REPO / "initramfs")
            kconfig = tree / ".config"
            kconfig.write_text("CONFIG_RD_LZMA=y\n# CONFIG_SYSVIPC is not set\n")
            for gz in ("", "1"):
                with self.subTest(GZIP=gz):
                    result = subprocess.run(
                        ["sh", "scripts/mkinitramfs.sh"], cwd=tree,
                        env={**os.environ, "KCONFIG": str(kconfig), "GZIP": gz},
                        capture_output=True, text=True, timeout=60,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("lacks CONFIG_SYSVIPC=y", result.stderr)
                    self.assertEqual(sorted(p.name for p in (tree / "out").iterdir()), ["rootfs"])


if __name__ == "__main__":
    unittest.main()
