"""Stamp/manifest rules (scripts/chef-stamp.py) and build-time applet links
(scripts/link-applets.sh), on scratch trees only; nothing under out/."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("chef_stamp", REPO / "scripts/chef-stamp.py")
chef_stamp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(chef_stamp)
TOOL = REPO / "scripts/chef-stamp.py"
RELEASE = "4.4.192-cyclo+"


class Stamp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        p = self.p = Path(self.tmp.name)
        self.root = p / "root"
        (self.root / "lib/modules").mkdir(parents=True)
        (self.root / "etc/chef").mkdir(parents=True)
        (self.root / "lib/modules/wlan.ko").write_bytes(
            b"\0vermagic=" + RELEASE.encode() + b" SMP preempt mod_unload modversions aarch64\0")
        (self.root / "init").write_text("#!/bin/busybox sh\n")
        os.symlink("/bin/busybox", self.root / "sh")
        self.kernel = p / "Image.gz-dtb"
        self.kernel.write_bytes(b"kernel")
        self.symvers = p / "Module.symvers"
        self.symvers.write_text("0x1\tsym\tvmlinux\tEXPORT_SYMBOL\n")
        self.manifest = p / "manifest"

    def tool(self, *args):
        return subprocess.run(["python3", str(TOOL), *map(str, args)], capture_output=True, text=True)

    def make_stamp(self):
        self.manifest.write_text(chef_stamp.manifest(str(self.root)))
        r = self.tool("stamp", self.root, self.kernel, self.symvers, RELEASE, self.manifest)
        self.assertEqual(r.returncode, 0, r.stderr)
        stamp = self.p / "stamp"
        stamp.write_text(r.stdout)
        return stamp

    def check(self, stamp):
        return self.tool("check", stamp, self.root, self.kernel, self.symvers, RELEASE).returncode

    def test_manifest_excludes_stamp_and_is_stable(self):
        before = chef_stamp.manifest(str(self.root))
        (self.root / "etc/chef/build-stamp").write_text("anything\n")
        self.assertEqual(chef_stamp.manifest(str(self.root)), before)
        lines = before.splitlines()
        self.assertEqual(lines[0].split("\t")[:2], ["/", "d"])
        self.assertIn("/sh\tl\t0777", before)
        self.assertTrue(any(l.startswith("/init\tf\t") for l in lines))
        (self.root / "init").chmod(0o700)
        self.assertNotEqual(chef_stamp.manifest(str(self.root)), before)

    def test_stamp_fields_and_match(self):
        stamp = self.make_stamp()
        text = stamp.read_text().splitlines()
        self.assertEqual(text[0], "chef-cyclo-root 1")
        self.assertEqual(text[1], f"kernel-release {RELEASE}")
        self.assertEqual(self.check(stamp), 0)
        uuid, seed = self.tool("ids", stamp).stdout.split()
        self.assertNotEqual(uuid, seed)

    def test_refuses_each_mismatch(self):
        stamp = self.make_stamp()
        self.kernel.write_bytes(b"other kernel")
        self.assertNotEqual(self.check(stamp), 0)
        self.kernel.write_bytes(b"kernel")
        self.symvers.write_text("changed\n")
        self.assertNotEqual(self.check(stamp), 0)
        self.symvers.write_text("0x1\tsym\tvmlinux\tEXPORT_SYMBOL\n")
        self.assertEqual(self.check(stamp), 0)
        wlan = self.root / "lib/modules/wlan.ko"
        wlan.write_bytes(wlan.read_bytes() + b"x")
        self.assertNotEqual(self.check(stamp), 0)
        self.assertNotEqual(self.tool("check", stamp, self.root, self.kernel, self.symvers, "4.4.193").returncode, 0)

    def test_refuses_foreign_module_and_bad_stamps(self):
        (self.root / "lib/modules/wlan.ko").write_bytes(b"vermagic=4.4.193 SMP\0")
        self.manifest.write_text(chef_stamp.manifest(str(self.root)))
        self.assertNotEqual(self.tool("stamp", self.root, self.kernel, self.symvers, RELEASE, self.manifest).returncode, 0)
        bad = self.p / "bad"
        for text in ("", "chef-cyclo-root 2\n", "chef-cyclo-root 1\nkernel-release x\n",
                     "chef-cyclo-root 1\nkernel-release x\nkernel-release y\n"):
            bad.write_text(text)
            self.assertNotEqual(self.check(bad), 0, text)


class Applets(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for d in ("bin", "sbin", "usr/bin", "usr/sbin", "etc/busybox-paths.d"):
            (self.root / d).mkdir(parents=True)
        for pkg in ("busybox", "busybox-extras"):
            (self.root / "bin" / pkg).write_text("")
            (self.root / "bin" / pkg).chmod(0o755)
        (self.root / "usr/bin/ip").write_text("real ip\n")

    def run_links(self, busybox, extras):
        (self.root / "etc/busybox-paths.d/busybox").write_text(busybox)
        (self.root / "etc/busybox-paths.d/busybox-extras").write_text(extras)
        return subprocess.run(["sh", str(REPO / "scripts/link-applets.sh"), str(self.root)],
                              capture_output=True, text=True)

    def test_links(self):
        r = self.run_links("bin/ls\nsbin/init\nusr/bin/ip\nusr/sbin/telnetd\n", "usr/sbin/telnetd\nusr/sbin/udhcpd\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(os.readlink(self.root / "bin/ls"), "/bin/busybox")
        self.assertEqual(os.readlink(self.root / "sbin/init"), "/bin/busybox")
        self.assertEqual((self.root / "usr/bin/ip").read_text(), "real ip\n")   # never replaced
        self.assertEqual(os.readlink(self.root / "usr/sbin/telnetd"), "/bin/busybox")  # busybox first
        self.assertEqual(os.readlink(self.root / "usr/sbin/udhcpd"), "/bin/busybox-extras")

    def test_bad_lists(self):
        for listing in ("/bin/ls\n", "../etc/x\n", "bin/a b\n", ""):
            self.assertNotEqual(self.run_links(listing, "usr/sbin/udhcpd\n").returncode, 0, listing)


if __name__ == "__main__":
    unittest.main()
