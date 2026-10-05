"""Uncompressed boot image: the UNCOMPRESSED_IMG packer, its loader budget,
mkboot.sh UNCOMPRESSED=1, the mkstage1.sh .cpio gate and mkinstall.sh
PACK_ONLY=1 UNCOMPRESSED=1 against a fake repository."""
import gzip
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, REPO / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


budget = load("budget", "scripts/check-chef-loader-budget.py")
packer = load("packer", "scripts/mkkernel-uncompressed.py")
MKBOOTIMG = REPO / "toolchain/mkbootimg/mkbootimg.py"


def image(size, payload=b""):
    header = bytearray(64)
    struct.pack_into("<Q", header, 16, size)
    header[56:60] = b"ARM\x64"
    return bytes(header) + payload


def fdt(total=64, fill=b"\0"):
    return struct.pack(">II", 0xD00DFEED, total) + fill * (total - 8)


def boot_sections(path):
    """Kernel and ramdisk sections of a header v0 boot image."""
    data = Path(path).read_bytes()
    assert data[:8] == b"ANDROID!"
    kernel_size, _, ramdisk_size = struct.unpack_from("<III", data, 8)
    page = struct.unpack_from("<I", data, 36)[0]
    kernel_pages = (kernel_size + page - 1) // page
    ramdisk_at = page * (1 + kernel_pages)
    return data[page:page + kernel_size], data[ramdisk_at:ramdisk_at + ramdisk_size]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class PackerTests(unittest.TestCase):
    def test_layout(self):
        raw = image(0x5000, b"k" * 1000)
        dtbs = fdt(64, b"a") + fdt(128, b"b")
        packed = packer.pack(gzip.compress(raw, mtime=0) + dtbs)
        self.assertEqual(packed[:16], b"UNCOMPRESSED_IMG")
        self.assertEqual(struct.unpack_from("<I", packed, 16)[0], len(raw))
        self.assertEqual(packed[20:20 + len(raw)], raw)
        self.assertEqual(packed[20 + len(raw):], dtbs)
        self.assertEqual(len(packed), 20 + len(raw) + len(dtbs))

    def test_refusals(self):
        good = gzip.compress(image(0x5000, b"k" * 100), mtime=0)
        cases = {
            "no appended DTB": good,
            "no FDT magic": good + fdt() + b"garbage" * 8,
            "bad totalsize": good + fdt()[:4] + struct.pack(">I", 4096) + bytes(56),
            "truncated FDT header": good + fdt()[:20],
            "ARM64 Image magic": gzip.compress(bytes(128)) + fdt(),
            "truncated ARM64": gzip.compress(b"short") + fdt(),
            "not a gzip": image(0x5000) + fdt(),
            "truncated gzip": good[:-4],
        }
        for message, data in cases.items():
            with self.subTest(message):
                with self.assertRaisesRegex(ValueError, message):
                    packer.pack(data)

    def test_cli_output_rereads_through_the_budget_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, out, ramdisk = Path(tmp, "Image.gz-dtb"), Path(tmp, "k"), Path(tmp, "r")
            src.write_bytes(gzip.compress(image(0x5000, b"k" * 100), mtime=0) + fdt())
            ramdisk.write_bytes(b"070701")
            subprocess.run(["python3", REPO / "scripts/mkkernel-uncompressed.py", src, out], check=True,
                           capture_output=True)
            self.assertEqual(budget.check_budget(out, ramdisk)[0], 0x5000)
            bad = subprocess.run(["python3", REPO / "scripts/mkkernel-uncompressed.py", out, Path(tmp, "x")],
                                 capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0)
            self.assertFalse(Path(tmp, "x").exists())


class UncompressedBudgetTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.kernel = Path(self.tmp.name) / "kernel"
        self.ramdisk = Path(self.tmp.name) / "ramdisk"
        self.ramdisk.write_bytes(b"r" * 4096)

    def write(self, size, payload=b"", dtbs=None, length=None):
        raw = image(size, payload)
        dtbs = fdt() if dtbs is None else dtbs
        length = len(raw) if length is None else length
        self.kernel.write_bytes(b"UNCOMPRESSED_IMG" + struct.pack("<I", length) + raw + dtbs)

    def test_image_size_bound(self):
        capacity = budget.kernel_capacity(4096)
        self.write(capacity)
        self.assertEqual(budget.check_budget(self.kernel, self.ramdisk), (capacity, 4096, capacity, 0))
        self.write(capacity + 1)
        with self.assertRaisesRegex(ValueError, "overflow=1 bytes"):
            budget.check_budget(self.kernel, self.ramdisk)

    def test_kernel_size_copy_bound(self):
        # abl copies the whole kernel section before its image_size check.
        self.ramdisk.write_bytes(b"r" * (budget.REGION_SIZE - 0x80000 - budget.LOADER_RESERVE - 2 * 4096))
        capacity = budget.kernel_capacity(self.ramdisk.stat().st_size)
        self.assertEqual(capacity, 8192)
        self.write(64, dtbs=fdt(capacity - 20 - 64))
        self.assertEqual(budget.check_budget(self.kernel, self.ramdisk)[3], 0)
        self.write(64, dtbs=fdt(capacity - 20 - 64 + 1))
        with self.assertRaisesRegex(ValueError, r"kernel_size=8193 bytes.*overflow=1 bytes"):
            budget.check_budget(self.kernel, self.ramdisk)

    def test_length_field_must_end_at_the_dtbs(self):
        raw_len = len(image(4096, b"k" * 64))
        for length in [raw_len - 1, raw_len + 1, raw_len + 64, 1 << 30, 0]:
            with self.subTest(length=length):
                self.write(4096, b"k" * 64, length=length)
                with self.assertRaises(ValueError):
                    budget.check_budget(self.kernel, self.ramdisk)

    def test_bad_images_and_unknown_forms(self):
        cases = {
            "truncated ARM64": b"UNCOMPRESSED_IMG" + struct.pack("<I", 32) + bytes(32) + fdt(),
            "ARM64 Image magic": b"UNCOMPRESSED_IMG" + struct.pack("<I", 64) + bytes(64) + fdt(),
            "no appended DTB": b"UNCOMPRESSED_IMG" + struct.pack("<I", 64) + image(4096),
            "neither gzip": image(4096) + fdt(),
            "neither gzip ": b"UNCOMPRESSED_im" + bytes(100),
        }
        for message, data in cases.items():
            with self.subTest(message):
                self.kernel.write_bytes(data)
                with self.assertRaisesRegex(ValueError, message.strip()):
                    budget.check_budget(self.kernel, self.ramdisk)

    def test_gzip_cli_output_unchanged(self):
        self.kernel.write_bytes(gzip.compress(image(4096), mtime=0) + fdt())
        result = subprocess.run(["python3", REPO / "scripts/check-chef-loader-budget.py", self.kernel, self.ramdisk],
                                capture_output=True, text=True, check=True)
        capacity = budget.kernel_capacity(4096)
        self.assertEqual(result.stdout,
                         f"Chef loader preflight: Image.image_size=4096, ramdisk=4096, capacity={capacity}, "
                         f"margin={capacity - 4096} bytes (Chef-specific observed bound; first gzip member "
                         "validated)\n")


@unittest.skipUnless(MKBOOTIMG.exists(), "toolchain/mkbootimg not fetched")
class MkbootUncompressedTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.kernel = self.dir / "Image.gz-dtb"
        self.ramdisk = self.dir / "stage1.cpio"
        self.ramdisk.write_bytes(b"070701" + b"r" * 5000)
        self.out = self.dir / "boot.img"

    def mkboot(self, **env):
        return subprocess.run(["sh", "scripts/mkboot.sh"], cwd=REPO, capture_output=True, text=True,
                              env={**os.environ, "KERNEL": str(self.kernel), "RAMDISK": str(self.ramdisk),
                                   "OUT": str(self.out), **env})

    def test_packs_the_uncompressed_kernel_and_raw_ramdisk(self):
        raw = image(0x10000, b"k" * 3000)
        self.kernel.write_bytes(gzip.compress(raw, mtime=0) + fdt())
        result = self.mkboot(UNCOMPRESSED="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("uncompressed kernel", result.stdout)
        kernel, ramdisk = boot_sections(self.out)
        self.assertEqual(kernel, b"UNCOMPRESSED_IMG" + struct.pack("<I", len(raw)) + raw + fdt())
        self.assertEqual(kernel, (self.dir / "boot.kernel").read_bytes())
        self.assertEqual(ramdisk, self.ramdisk.read_bytes())
        # Without UNCOMPRESSED=1 the gzip kernel goes in unchanged.
        self.assertEqual(self.mkboot().returncode, 0)
        self.assertEqual(boot_sections(self.out)[0], self.kernel.read_bytes())

    def test_budget_refusal_keeps_existing_output(self):
        self.kernel.write_bytes(gzip.compress(image(budget.kernel_capacity(8192) + 1), mtime=0) + fdt())
        self.out.write_bytes(b"protected existing output")
        result = self.mkboot(UNCOMPRESSED="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overflow=1 bytes", result.stderr)
        self.assertEqual(self.out.read_bytes(), b"protected existing output")

    def test_refusals_keep_existing_output(self):
        self.kernel.write_bytes(gzip.compress(image(4096), mtime=0) + fdt())
        self.out.write_bytes(b"protected existing output")
        self.ramdisk.write_bytes(gzip.compress(b"070701"))
        result = self.mkboot(UNCOMPRESSED="1")
        self.assertIn("not an uncompressed newc cpio", result.stderr)
        self.ramdisk.write_bytes(b"070701")
        self.kernel.write_bytes(gzip.compress(image(4096), mtime=0))
        result = self.mkboot(UNCOMPRESSED="1")
        self.assertIn("no appended DTB", result.stderr)
        self.assertNotEqual(result.returncode, 0)
        self.kernel.write_bytes(gzip.compress(image(4096), mtime=0) + fdt())
        self.out = self.dir / "boot"
        result = self.mkboot(UNCOMPRESSED="1")
        self.assertIn("OUT must end in .img", result.stderr)
        self.assertEqual((self.dir / "boot.img").read_bytes(), b"protected existing output")


CONFIG_OPTS = ["RD_GZIP", "BT_RFCOMM", "BT_RFCOMM_TTY", "BT_BNEP", "BT_BNEP_MC_FILTER", "BT_BNEP_PROTO_FILTER",
               "BT_HIDP", "BT_HCIVHCI", "CRYPTO_USER_API_HASH", "CRYPTO_USER_API_SKCIPHER",
               "CRYPTO_USER_API_AEAD", "SYSVIPC"]
APPLETS = ["sh", "mount", "umount", "switch_root", "blockdev", "timeout", "dd", "od", "cut", "tr", "cmp", "grep",
           "stat", "mountpoint", "ip", "sed", "pidof", "setsid", "sync", "sleep", "base64", "sha256sum", "tar",
           "gzip", "stty", "cat", "dmesg"]
RELEASE = "4.4.0-test"


@unittest.skipUnless(MKBOOTIMG.exists() and shutil.which("cpio") and Path("/usr/sbin/mke2fs").exists()
                     and Path("/usr/sbin/debugfs").exists(), "needs mkbootimg, cpio, e2fsprogs")
class MkinstallPackOnlyTests(unittest.TestCase):
    """A fake repository with the real scripts: kernel, staged tree, system_a."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        r = self.repo = Path(self.tmp.name) / "repo"
        for name in ["mkinstall.sh", "mkstage1.sh", "mkboot.sh", "chef-stamp.py", "check-chef-loader-budget.py",
                     "mkkernel-uncompressed.py", "link-applets.sh"]:
            (r / "scripts").mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / "scripts" / name, r / "scripts" / name)
        for rel in ["stage1/init", "initramfs/etc/udhcpd.conf", "initramfs/usr/lib/chef/usb-gadget.sh"]:
            (r / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / rel, r / rel)
        (r / "toolchain").mkdir()
        (r / "toolchain/mkbootimg").symlink_to(MKBOOTIMG.parent)
        k = r / "out/kernel"
        (k / "arch/arm64/boot").mkdir(parents=True)
        (k / "include/config").mkdir(parents=True)
        self.kernel = k / "arch/arm64/boot/Image.gz-dtb"
        self.kernel.write_bytes(gzip.compress(image(0x20000, b"k" * 5000), mtime=0) + fdt(96, b"d"))
        (k / ".config").write_text("".join(f"CONFIG_{o}=y\n" for o in CONFIG_OPTS))
        (k / "Module.symvers").write_text("0x1\tsym\tvmlinux\tEXPORT_SYMBOL\n")
        (k / "include/config/kernel.release").write_text(RELEASE + "\n")
        t = r / "out/initramfs-root"
        for d in ["bin", "lib", "usr/bin", "usr/sbin", "etc/busybox-paths.d"]:
            (t / d).mkdir(parents=True)
        for exe in ["bin/busybox", "bin/busybox-extras", "lib/ld-musl-aarch64.so.1", "usr/bin/btprobe"]:
            (t / exe).write_text("#!/bin/sh\n")
            (t / exe).chmod(0o755)
        (t / "etc/busybox-paths.d/busybox").write_text("".join(f"bin/{a}\n" for a in APPLETS))
        (t / "etc/busybox-paths.d/busybox-extras").write_text("usr/sbin/telnetd\nusr/sbin/udhcpd\n")
        s = r / "out/system-root"
        (s / "lib/modules").mkdir(parents=True)
        (s / "lib/modules/wlan.ko").write_bytes(b"\0vermagic=" + RELEASE.encode() + b" SMP mod_unload\0")
        self.write_system_a(self.make_stamp())
        self.mkstage1(check=True)

    def mkstage1(self, check=False, **env):
        return subprocess.run(["sh", str(self.repo / "scripts/mkstage1.sh")], capture_output=True, text=True,
                              check=check, env={**self.clean_env(), **env})

    def clean_env(self):
        return {k: v for k, v in os.environ.items()
                if k not in ("OUT", "RAMDISK", "KERNEL", "STAGE1_GZ", "STAGE1_EXPECTED_STAMP", "GZIP",
                             "UNCOMPRESSED", "PACK_ONLY", "SKIP_KERNEL")}

    def make_stamp(self):
        r = self.repo
        manifest = r / "out/system_a.manifest"
        tool = ["python3", str(r / "scripts/chef-stamp.py")]
        manifest.write_text(subprocess.run(tool + ["manifest", str(r / "out/system-root")], check=True,
                                           capture_output=True, text=True).stdout)
        return subprocess.run(tool + ["stamp", str(r / "out/system-root"), str(self.kernel),
                                      str(r / "out/kernel/Module.symvers"), RELEASE, str(manifest)],
                              check=True, capture_output=True, text=True).stdout

    def write_system_a(self, image_stamp, file_stamp=None):
        r = self.repo
        (r / "out/system_a.stamp").write_text(file_stamp or image_stamp)
        root = Path(self.tmp.name) / "sysimg"
        shutil.rmtree(root, ignore_errors=True)
        (root / "etc/chef").mkdir(parents=True)
        (root / "etc/chef/build-stamp").write_text(image_stamp)
        img = r / "out/system_a.img"
        img.unlink(missing_ok=True)
        subprocess.run(["/usr/sbin/mke2fs", "-q", "-F", "-t", "ext4", "-O", "^has_journal", "-d", str(root),
                        str(img), "4M"], check=True, capture_output=True,
                       env={**os.environ, "MKE2FS_CONFIG": "/dev/null", "E2FSPROGS_FAKE_TIME": "1"})

    def mkinstall(self, **env):
        return subprocess.run(["sh", str(self.repo / "scripts/mkinstall.sh")], capture_output=True, text=True,
                              env={**self.clean_env(), "PACK_ONLY": "1", **env})

    def protected(self):
        return {name: sha(self.repo / "out" / name) for name in ["system_a.img", "system_a.stamp", "stage1.cpio.gz"]}

    def test_builds_both_images_from_one_stamp(self):
        before = self.protected()
        result = self.mkinstall(UNCOMPRESSED="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        out = self.repo / "out"
        self.assertEqual(self.protected(), before)
        raw = gzip.decompress(self.kernel.read_bytes().split(fdt(96, b"d"))[0])
        kernel, ramdisk = boot_sections(out / "boot-stage1-uncompressed.img")
        self.assertEqual(kernel, b"UNCOMPRESSED_IMG" + struct.pack("<I", len(raw)) + raw + fdt(96, b"d"))
        self.assertEqual(ramdisk, gzip.decompress((out / "stage1.cpio.gz").read_bytes()))
        self.assertEqual(ramdisk, (out / "stage1.cpio").read_bytes())
        gz_kernel, gz_ramdisk = boot_sections(out / "boot-stage1.img")
        self.assertEqual(gz_kernel, self.kernel.read_bytes())
        self.assertEqual(gz_ramdisk, (out / "stage1.cpio.gz").read_bytes())
        self.assertIn(b"etc/chef/expected-stamp", ramdisk)
        first = {n: sha(out / n) for n in ["boot-stage1.img", "boot-stage1-uncompressed.img"]}
        self.assertEqual(self.mkinstall(UNCOMPRESSED="1").returncode, 0)
        self.assertEqual({n: sha(out / n) for n in first}, first)

    def test_default_is_gzip_only(self):
        result = self.mkinstall()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.repo / "out/boot-stage1-uncompressed.img").exists())
        self.assertEqual(boot_sections(self.repo / "out/boot-stage1.img")[0], self.kernel.read_bytes())

    def refused(self, result, message):
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(message, result.stderr)
        for name in ["boot-stage1.img", "boot-stage1-uncompressed.img", "stage1.cpio"]:
            self.assertFalse((self.repo / "out" / name).exists(), name)

    def test_refuses_system_a_stamp_mismatch(self):
        good = self.make_stamp()
        self.write_system_a(good.replace("tree-manifest-sha256 ", "tree-manifest-sha256 0"), good)
        before = self.protected()
        self.refused(self.mkinstall(UNCOMPRESSED="1"), "stamps differ")
        self.assertEqual(self.protected(), before)

    def test_refuses_a_kernel_the_stamp_does_not_name(self):
        self.kernel.write_bytes(gzip.compress(image(0x20000, b"x" * 5000), mtime=0) + fdt(96, b"d"))
        before = self.protected()
        self.refused(self.mkinstall(UNCOMPRESSED="1"), "kernel-image-sha256")
        self.assertEqual(self.protected(), before)

    def test_raw_stage1_is_the_gunzipped_gzip_ramdisk_with_the_stamp(self):
        out = self.repo / "out/x.cpio"
        gz = self.repo / "out/stage1.cpio.gz"
        self.assertEqual(self.mkstage1(OUT=str(out)).returncode, 0)
        self.assertEqual(out.read_bytes(), gzip.decompress(gz.read_bytes()))
        other_stamp = Path(self.tmp.name) / "other.stamp"
        other_stamp.write_text("chef-cyclo-root 1\nother\n")
        other_gz = self.repo / "out/other.cpio.gz"
        self.mkstage1(check=True, OUT=str(other_gz), STAGE1_EXPECTED_STAMP=str(other_stamp))
        bad_gz = Path(self.tmp.name) / "bad.gz"
        bad_gz.write_bytes(gzip.compress(b"not a cpio"))
        not_gz = Path(self.tmp.name) / "plain"
        not_gz.write_bytes(b"070701 plain")
        cases = [
            ({"STAGE1_GZ": str(other_gz)}, "differs from"),
            ({"STAGE1_GZ": str(bad_gz)}, "not a gzip newc cpio"),
            ({"STAGE1_GZ": str(not_gz)}, "not a gzip newc cpio"),
            ({"STAGE1_GZ": str(self.repo / "out/missing.gz")}, "build the gzip stage-1 ramdisk first"),
        ]
        for env, message in cases:
            with self.subTest(message=message, env=env):
                out.unlink(missing_ok=True)
                result = self.mkstage1(OUT=str(out), **env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertFalse(out.exists())
                self.assertFalse(Path(str(out) + ".tmp").exists())
        # The rescue-test stamp matches its own ramdisk.
        result = self.mkstage1(OUT=str(out), STAGE1_GZ=str(other_gz), STAGE1_EXPECTED_STAMP=str(other_stamp))
        self.assertEqual(result.returncode, 0, result.stderr)

if __name__ == "__main__":
    unittest.main()
