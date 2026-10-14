import importlib.util
import os
import subprocess
import tempfile
import unittest
import shutil
from pathlib import Path

import test_chef_stamp as ts

SCRIPT = Path(__file__).resolve().parents[1] / "chef-image-info.py"
SPEC = importlib.util.spec_from_file_location("chef_image_info", SCRIPT)
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class ImageInfoTests(unittest.TestCase):
    def repo(self):
        root = Path(tempfile.mkdtemp(prefix="chef-image-info."))
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        subprocess.run(["git", "-C", str(root), "-c", "user.name=test", "-c",
                        "user.email=test@example.invalid", "commit", "--allow-empty", "-qm", "seed"],
                       check=True, env={**os.environ, "GIT_AUTHOR_DATE": "2026-10-01T00:00:00Z",
                                        "GIT_COMMITTER_DATE": "2026-10-01T00:00:00Z"})
        return root

    def test_reproducible_and_dirty(self):
        root = self.repo()
        env = {"SOURCE_DATE_EPOCH": "1790812800"}
        a = module.generate(root, env)
        self.assertEqual(a, module.generate(root, env))
        self.assertEqual(module.parse(a)["dirty"], "0")
        (root / "untracked.txt").write_text("dirty\n")
        self.assertEqual(module.parse(module.generate(root, env))["dirty"], "1")

    def test_epoch_and_validation(self):
        root = self.repo()
        with self.assertRaises(ValueError):
            module.generate(root, {"SOURCE_DATE_EPOCH": "001"})
        good = module.generate(root, {"SOURCE_DATE_EPOCH": "1790812800"})
        for bad in (good.replace("dirty 0\n", "dirty 2\n"),
                    good.replace("commit ", "commit xyz", 1),
                    good + "extra x\n", good[:-1], "x" * 1025):
            with self.subTest(bad=bad[:50]), self.assertRaises(ValueError):
                module.parse(bad)


class Coexist(unittest.TestCase):
    """The display metadata sits in the stamped root next to the strict
    build-stamp: the stamp format and its checks are unchanged (mksystem.sh
    stamps the tree that already holds etc/chef/image-info)."""
    setUp = ts.Stamp.setUp
    tool = ts.Stamp.tool
    make_stamp = ts.Stamp.make_stamp
    check = ts.Stamp.check

    INFO = ("chef-image-info 1\nname chef-cyclo\nversion dev\n"
            "commit 0123456789abcdef0123456789abcdef01234567\ndirty 0\nepoch 1790812800\n")

    def test_stamp_over_a_root_with_image_info(self):
        info = self.root / "etc/chef/image-info"
        info.write_text(self.INFO)
        module.parse(self.INFO)                      # a valid file
        stamp = self.make_stamp()
        self.assertEqual(self.check(stamp), 0)
        fields = ts.chef_stamp.fields(stamp.read_text())
        self.assertEqual(len(fields), 5)             # exactly the five strict fields
        self.assertIn("/etc/chef/image-info\tf\t", ts.chef_stamp.manifest(str(self.root)))

    def test_strict_stamp_still_rejects_extra_lines(self):
        (self.root / "etc/chef/image-info").write_text(self.INFO)
        stamp = self.make_stamp()
        stamp.write_text(stamp.read_text() + "version dev\n")
        self.assertNotEqual(self.check(stamp), 0)
        with self.assertRaises(Exception):
            ts.chef_stamp.fields(stamp.read_text())


if __name__ == "__main__":
    unittest.main()
