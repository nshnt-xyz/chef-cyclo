"""Our runtime files live under /run/chef (docs/features/runtime-files.md).

Scans the files that make up the image for /run/<name> (comments count) and
for the defaults of run-directory variables, and fails on a top-level name
that is neither `chef` nor in ALLOWED. Not caught: a path assembled at run
time from a base the scan cannot see (an environment variable set outside
the image's files, a C path built from a non-literal base), and names that
other packages choose for themselves inside their own /run directories.
"""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[2]

# Top-level names under /run other than chef, each with its reason.
ALLOWED = {
    "dbus": "D-Bus's standard socket directory",
    "udev": "eudev's standard runtime directory",
    "NetworkManager": "NetworkManager's standard runtime directory",
    "wpa_supplicant": "wpa_supplicant's standard control directory",
    "chrony": "chronyd's standard pid and command socket directory",
    "user": "the XDG runtime directory /run/user/0 (session bus, PipeWire)",
    "lock": "the standard lock directory",
    ".var": "/init's transient mount point while it moves the volatile /var",
}

# A bare /run as a variable's default: the directory the names go into.
BARE_ALLOWED = {
    "rootfs-overlay/usr/lib/chef/usb-gadget.sh":
        "USB_GADGET_RUN defaults to /run for stage 1, which shares the helper; "
        "the stage-2 /init sets /run/chef",
}

# stage1/ is the boot image's rescue world and keeps /run on purpose; test
# sources, build output and vendored code are not in the image.
SKIP_DIRS = {"build", "tests", "lvgl", "__pycache__"}


def image_files():
    """The overlay, the non-test sources under tools/ and the image builders."""
    roots = [REPO / "rootfs-overlay", REPO / "tools"]
    for root in roots:
        for path in sorted(root.rglob("*")):
            rel = path.relative_to(root)
            if path.is_file() and not path.is_symlink() and not SKIP_DIRS & set(rel.parts[:-1]):
                yield path
    yield from sorted((REPO / "scripts").glob("mk*.sh"))


def text(path):
    data = path.read_bytes()
    return None if b"\0" in data else data.decode("utf-8", "replace")


# /run/<name>, not as the tail of a longer path ($d/run/..., x/run/...).
NAME = re.compile(r"(?<![\w.$})/])/run/([A-Za-z0-9_.-]*[A-Za-z0-9_])")
# Defaults of run-directory variables: ${X:-/run...}, X=/run..., "/run" in C.
DEFAULT = re.compile(r"\$\{\w+:-(/run[^}]*)\}|(?:^|[\s;])\w+=['\"]?(/run[^\s;'\"]*)|\"(/run)\"")


class RunLayoutTests(unittest.TestCase):

    def test_scan_sees_the_image(self):
        files = {str(p.relative_to(REPO)) for p in image_files()}
        for f in ("rootfs-overlay/init", "rootfs-overlay/etc/inittab", "rootfs-overlay/usr/bin/chef-state",
                  "tools/common/chef_paths.h", "tools/buttond/buttond.c", "scripts/mkinitramfs.sh"):
            self.assertIn(f, files)
        self.assertFalse([f for f in files if "/tests/" in f or f.startswith("stage1/")])

    def test_top_level_names(self):
        bad = []
        for path in image_files():
            body = text(path)
            if body is None:
                continue
            for n, line in enumerate(body.splitlines(), 1):
                for m in NAME.finditer(line):
                    if m.group(1) != "chef" and m.group(1) not in ALLOWED:
                        bad.append(f"{path.relative_to(REPO)}:{n}: /run/{m.group(1)}")
        self.assertEqual(bad, [], "names outside /run/chef (see docs/features/runtime-files.md)")

    def test_run_directory_defaults(self):
        bad = []
        for path in image_files():
            body = text(path)
            if body is None:
                continue
            rel = str(path.relative_to(REPO))
            for n, line in enumerate(body.splitlines(), 1):
                for m in DEFAULT.finditer(line):
                    value = next(g for g in m.groups() if g is not None)
                    if value == "/run" and rel in BARE_ALLOWED:
                        continue
                    top = value[len("/run/"):].split("/")[0] if value.startswith("/run/") else ""
                    if top != "chef" and top not in ALLOWED:
                        bad.append(f"{rel}:{n}: {value}")
        self.assertEqual(bad, [], "run-directory defaults outside /run/chef")

    def test_patterns(self):
        hits = lambda s: [m.group(1) for m in NAME.finditer(s)]
        self.assertEqual(hits('x /run/foo.sock. "/run/chef/a" `/run/x` ${A:-/run/y}'), ["foo.sock", "chef", "x", "y"])
        self.assertEqual(hits("$d/run/x captures/run/y {p}/run/z /run copy"), [])
        defaults = lambda s: [next(g for g in m.groups() if g) for m in DEFAULT.finditer(s)]
        self.assertEqual(defaults('RUN=${CHEF_RUN:-/run} X=/run/x; p = "/run";'), ["/run", "/run/x", "/run"])
        self.assertEqual(defaults("A='/run' B=\"/run/b\""), ["/run", "/run/b"])
        self.assertEqual(defaults('RUN=${CHEF_RUN:-/run/chef} "/run/chef"'), ["/run/chef"])


if __name__ == "__main__":
    unittest.main()
