"""Setuid and setgid files (docs/features/users-and-permissions.md): polkit
adds none to the image.

The packages' own tar headers (toolchain/apk/cache) carry the bits; the
staged trees do not (apk --usermode drops them), and the images get only
the D-Bus launch helper's, from mksystem.sh and /init. So two checks:
the packages installed in out/rootfs, less what mkinitramfs.sh's
"privileged helpers" block removes, carry exactly the setuid and setgid
files they did before polkit; and the staged trees, where built, have none
beyond the launch helper in mksystem's tree. Each skips when its inputs
are not there.
"""
import gzip
import io
import os
from pathlib import Path
import re
import stat
import subprocess
import tarfile
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
CACHE = REPO / "toolchain/apk/cache"
INSTALLED = REPO / "out/rootfs/lib/apk/db/installed"
HELPER = "usr/libexec/dbus-daemon-launch-helper"
# The packages' setuid and setgid files before polkit (2026-10-23): the D-Bus
# launch helper (root:messagebus 4750) and util-linux-misc's wall (2755 tty).
BEFORE = {HELPER, "usr/bin/wall"}


def removal_block():
    text = (REPO / "scripts/mkinitramfs.sh").read_text()
    block = text.split("--- privileged helpers ---\n", 1)[1]
    return block.split("# --- end privileged helpers ---", 1)[0]


def removed_paths():
    return {m for m in re.findall(r'"\$ROOT"/(\S+)', removal_block())}


def installed_packages():
    """(name, version) of every package in out/rootfs's apk database that
    installs anything (an empty one, installed size 0, is never cached)."""
    pkgs = []
    for entry in INSTALLED.read_text().split("\n\n"):
        f = dict(line.split(":", 1) for line in entry.splitlines() if line[1:2] == ":")
        if "P" in f and f.get("I") != "0":
            pkgs.append((f["P"], f["V"]))
    return pkgs


def privileged_members(name, version):
    """The setuid/setgid regular files in a cached package, or None if not cached."""
    apks = sorted(CACHE.glob(f"{name}-{version}.*.apk")) + sorted(CACHE.glob(f"{name}-{version}.apk"))
    if not apks:
        return None
    found = set()
    with tarfile.open(fileobj=io.BytesIO(gzip.decompress(apks[0].read_bytes())), ignore_zeros=True) as t:
        for m in t.getmembers():
            if m.isfile() and m.mode & (stat.S_ISUID | stat.S_ISGID):
                found.add(m.name)
    return found


def privileged_in_tree(root):
    found = set()
    for dirpath, dirs, files in os.walk(root):
        for f in files:
            p = Path(dirpath) / f
            st = p.lstat()
            if stat.S_ISREG(st.st_mode) and st.st_mode & (stat.S_ISUID | stat.S_ISGID):
                found.add(str(p.relative_to(root)))
    return found


class Setuid(unittest.TestCase):
    def test_packaged_privileged_files_are_the_ones_before_polkit(self):
        if not INSTALLED.is_file():
            raise unittest.SkipTest("no out/rootfs (scripts/mkrootfs.sh)")
        packaged = {}
        for name, version in installed_packages():
            members = privileged_members(name, version)
            if members is None:
                raise unittest.SkipTest(f"{name}-{version} not in {CACHE}")
            for m in members:
                packaged[m] = name
        removed = removed_paths()
        self.assertEqual(set(packaged) - removed, BEFORE)
        # every removal is one of them, and polkit's and linux-pam's all go
        self.assertLessEqual(removed, set(packaged))
        self.assertEqual({m for m, n in packaged.items() if n.startswith("polkit") or n == "linux-pam"}, removed)

    def test_removal_block_removes_them(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            keep = root / HELPER
            for rel in [*removed_paths(), HELPER]:
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text("x")
            subprocess.run(["sh", "-ec", removal_block()], env={**os.environ, "ROOT": str(root)}, check=True)
            for rel in removed_paths():
                self.assertFalse((root / rel).exists(), rel)
            self.assertTrue(keep.exists())

    def test_staged_trees(self):
        trees = {"out/initramfs-root": set(), "out/system-root": {HELPER}}
        built = [t for t in trees if (REPO / t / "init").is_file()]
        if not built:
            raise unittest.SkipTest("no staged tree (scripts/mkinitramfs.sh, scripts/mksystem.sh)")
        for t in built:
            with self.subTest(tree=t):
                self.assertEqual(privileged_in_tree(REPO / t), trees[t])


if __name__ == "__main__":
    unittest.main()
