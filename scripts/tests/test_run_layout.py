"""Our runtime files live under /run/chef (docs/features/runtime-files.md).

Scans the files that make up the image for /run/<name> (comments count) and
for the defaults of run-directory variables, and fails on a top-level name
that is neither `chef` nor in ALLOWED. Not caught: a path assembled at run
time from a base the scan cannot see (an environment variable set outside
the image's files, a C path built from a non-literal base), and names that
other packages choose for themselves inside their own /run directories.
Also: the paths standard interfaces step 8 moved are gone from the image,
and /init makes /run/chef/state and the screen handover directory.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
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
    "openrc": "OpenRC's state directory (RC_SVCDIR)",
    "supervise-syslog.pid": "supervise-daemon's own pid file for syslog, at its default path: "
                            "the package's /run/syslogd.pid is syslogd's (etc/conf.d/syslog)",
    "udhcpd.pid": "busybox udhcpd's own default pid file (named in etc/conf.d/udhcpd, which keeps "
                  "the supervisor off it)",
    "supervise-udhcpd.pid": "supervise-daemon's own pid file for udhcpd, at its default path: "
                            "the package's /run/udhcpd.pid is udhcpd's (etc/conf.d/udhcpd)",
    "syslogd.pid": "busybox syslogd's own default pid file (CONFIG_PID_FILE_PATH is /run); "
                   "the shell log helper checks it to know syslogd is up",
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

    def test_moved_paths_are_gone(self):
        # step 8 moved each daemon's socket into its own directory, the
        # working registry copy into sensord's and the screen handover into
        # screen/
        old = ("/run/chef/buttond.sock", "/run/chef/sensord.sock", "/run/chef/blesensd.sock",
               "/run/chef/gps-manager.sock", "/run/chef/sensors/sns.reg\"", "/run/chef/fb0.", "/run/chef/screen-off",
               "$ST/mag-last", "$ST/gps-enabled", "$ST/blesens\t")
        bad = []
        for path in image_files():
            body = text(path)
            for n, line in enumerate((body or "").splitlines(), 1):
                bad += [f"{path.relative_to(REPO)}:{n}: {o}" for o in old if o in line]
        self.assertEqual(bad, [])

    def test_init_makes_the_shared_directories(self):
        init = (REPO / "rootfs-overlay/init").read_text()
        self.assertIn("\nmkdir -p -m 0755 /run/chef /run/chef/log /run/chef/lock /run/chef/state\n", init)
        block = "if mkdir -m 0775 /run/chef/screen" + init.split("if mkdir -m 0775 /run/chef/screen", 1)[1]
        block = block[:block.index("\nfi\n") + 4]
        self.assertLess(init.index("/run/chef/state\n"), init.index(block))
        self.assertLess(init.index(block), init.index("exec /sbin/init"))
        with tempfile.TemporaryDirectory() as tmp:
            calls = Path(tmp) / "calls"
            code = ('log() { echo "log $*" >> "$CALLS"; }\n'
                    'chgrp() { echo "chgrp $*" >> "$CALLS"; return "${CHGRP_RC:-0}"; }\n'
                    + block.replace("/run/chef", tmp + "/chef"))
            os.mkdir(tmp + "/chef")
            env = {**os.environ, "CALLS": str(calls)}
            subprocess.run(["sh", "-c", code], env=env, check=True)
            screen = Path(tmp) / "chef/screen"
            self.assertEqual(screen.stat().st_mode & 0o7777, 0o775)
            self.assertEqual((screen / "fb0.lock").stat().st_mode & 0o7777, 0o660)
            self.assertEqual(calls.read_text(), f"chgrp screen {screen}\nchgrp screen {screen}/fb0.lock\n")
            # any failure: one line, and init goes on
            (screen / "fb0.lock").unlink()
            screen.rmdir()
            calls.unlink()
            subprocess.run(["sh", "-c", code], env={**env, "CHGRP_RC": "1"}, check=True)
            self.assertEqual(calls.read_text(), f"chgrp screen {screen}\nlog screen handover directory not set up\n")

    def test_init_protects_links(self):
        # before chef-storage boot hands the daemons their /data directories
        init = (REPO / "rootfs-overlay/init").read_text()
        line = ("{ echo 1 > /proc/sys/fs/protected_hardlinks && echo 1 > /proc/sys/fs/protected_symlinks; } "
                "2>/dev/null ||\n    log \"fs.protected_hardlinks/protected_symlinks not set\"\n")
        self.assertEqual(init.count(line), 1)
        self.assertLess(init.index("log()"), init.index(line))
        self.assertLess(init.index(line), init.index("/usr/bin/chef-storage boot"))
        with tempfile.TemporaryDirectory() as tmp:
            fs = Path(tmp) / "fs"
            fs.mkdir()
            code = 'log() { echo "log $*"; }\n' + line.replace("/proc/sys/fs", str(fs))
            r = subprocess.run(["sh", "-c", code], capture_output=True, text=True, check=True)
            self.assertEqual(((fs / "protected_hardlinks").read_text(), (fs / "protected_symlinks").read_text(),
                              r.stdout), ("1\n", "1\n", ""))
            (fs / "protected_hardlinks").unlink()
            (fs / "protected_hardlinks").mkdir()  # a write that fails
            r = subprocess.run(["sh", "-c", code], capture_output=True, text=True, check=True)
            self.assertEqual((r.stdout, r.stderr), ("log fs.protected_hardlinks/protected_symlinks not set\n", ""))

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
