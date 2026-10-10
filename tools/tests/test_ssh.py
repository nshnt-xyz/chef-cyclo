#!/usr/bin/env python3
"""Host tests for SSH (docs/features/ssh.md).

- scripts/ssh-keys.py check: good keys (ed25519, ecdsa, rsa, comments) pass;
  a missing, empty or comment-only file, options, unknown types, bad base64,
  a blob naming another type, a truncated ed25519 blob, private keys
  (OpenSSH, PEM, a raw dropbear blob), a symlink, group/other-writable file
  or directory are refused.
- scripts/ssh-keys.py scan: private key text, raw key blobs and host-key
  file names are found; ELF files carrying PEM strings are not flagged.
- initramfs/usr/bin/chef-sshd under bash and BusyBox ash, with stubs for
  dropbearkey (a "key" is a file starting VALID), dropbear (records its
  arguments, writes -E style log lines, exits), chef-state, chown, sync and
  sleep (bound as shell functions too, since BusyBox ash may prefer its own
  applets): host key generated on /data when ours (dir 0700, key 0600, no
  temp or .pub files left), an existing key kept byte for byte, a corrupt or
  zero-length key moved to .bad and replaced, RAM-only (and a /data
  failure) falling back to an ephemeral /run key that a respawn keeps, the
  exact dropbear arguments (-s, no -B or -R, the /run key, -D), SSH=0 images
  idling instead of respawning, failures sleeping before exit, and the kmsg
  rate limit with its suppressed count.
- the NetworkManager dispatcher line with the Wi-Fi address.
- inittab: the telnetd line unchanged, chef-sshd respawned; chef-storage
  makes /data/v1/ssh; the build wiring (mkrootfs, mkinitramfs, mksystem,
  .gitignore).
- scripts/phone.py --ssh: the ssh command line, run/push/pull/stream through
  a fake ssh, and the host key mismatch message.
"""
import base64
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
KEYS = ROOT / "scripts/ssh-keys.py"
SSHD = ROOT / "initramfs/usr/bin/chef-sshd"
DISPATCH = ROOT / "initramfs/etc/NetworkManager/dispatcher.d/30-chef-wifi-ip"
HAVE_KEYGEN = shutil.which("ssh-keygen") is not None


def sshstr(b):
    return struct.pack(">I", len(b)) + b


def ed25519_line(comment="me@host", key=b"\x01" * 32):
    return "ssh-ed25519 " + base64.b64encode(sshstr(b"ssh-ed25519") + sshstr(key)).decode() + " " + comment


class KeyCheck(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory()
        self.addCleanup(t.cleanup)
        self.d = Path(t.name) / "ssh"
        self.d.mkdir(mode=0o700)
        self.f = self.d / "authorized_keys"

    def check(self, text=None, mode=0o600):
        if text is not None:
            self.f.write_text(text)
            self.f.chmod(mode)
        return subprocess.run([sys.executable, str(KEYS), "check", str(self.f)], capture_output=True, text=True)

    def real_keys(self, *types):
        lines = []
        for i, t in enumerate(types):
            k = self.d / f"k{i}"
            subprocess.run(["ssh-keygen", "-q", "-t", t, "-N", "", "-C", f"c{i}", "-f", str(k)], check=True)
            lines.append((k.with_suffix(".pub")).read_text().strip())
        return lines

    @unittest.skipUnless(HAVE_KEYGEN, "no ssh-keygen")
    def test_real_keys_pass(self):
        lines = self.real_keys("ed25519", "ecdsa", "rsa")
        r = self.check("# chef keys\n\n" + "\n".join(lines) + "\n")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_synthetic_ed25519_passes_without_keygen_check(self):
        if HAVE_KEYGEN:
            self.skipTest("ssh-keygen rejects the synthetic key point")
        self.assertEqual(self.check(ed25519_line() + "\n").returncode, 0)

    def test_missing_empty_comment_only(self):
        r = self.check()
        self.assertEqual(r.returncode, 1)
        self.assertIn("ssh-setup.sh", r.stderr)
        for text in ["", "\n\n", "# nothing\n"]:
            r = self.check(text)
            self.assertEqual(r.returncode, 1, text)
            self.assertIn("no public key", r.stderr)

    def test_bad_lines(self):
        good = ed25519_line()
        bad = [
            'command="ls" ' + good,
            'from="1.2.3.4" ' + good,
            "ssh-dss AAAAB3NzaC1kc3MAAACBAP",
            "ssh-ed25519",
            "ssh-ed25519 not*base64",
            "ssh-ed25519 " + base64.b64encode(sshstr(b"ssh-rsa") + sshstr(b"x" * 32)).decode(),
            "ssh-ed25519 " + base64.b64encode(sshstr(b"ssh-ed25519") + sshstr(b"x" * 31)).decode(),
            "ssh-ed25519 " + base64.b64encode(sshstr(b"ssh-ed25519") + sshstr(b"x" * 32) + b"x").decode(),
            "ssh-ed25519 " + base64.b64encode(struct.pack(">I", 99) + b"ssh").decode(),
        ]
        for line in bad:
            r = self.check(line + "\n")
            self.assertEqual(r.returncode, 1, line)
            self.assertIn("authorized_keys:1:", r.stderr, line)

    def test_private_keys(self):
        texts = ["-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXktdjEAAAAA\n-----END OPENSSH PRIVATE KEY-----\n",
                 "-----BEGIN RSA PRIVATE KEY-----\nMIIE\n-----END RSA PRIVATE KEY-----\n",
                 ed25519_line() + "\n-----BEGIN EC PRIVATE KEY-----\n"]
        for text in texts:
            r = self.check(text)
            self.assertEqual(r.returncode, 1)
            self.assertIn("private key material", r.stderr)
        self.f.write_bytes(sshstr(b"ssh-ed25519") + sshstr(b"\x02" * 32) + sshstr(b"\x03" * 64))
        r = self.check()
        self.assertIn("private key material", r.stderr)

    @unittest.skipUnless(HAVE_KEYGEN, "no ssh-keygen")
    def test_permissions(self):
        good = self.real_keys("ed25519")[0] + "\n"
        r = self.check(good, mode=0o620)
        self.assertEqual(r.returncode, 1)
        self.assertIn("writable by group or others", r.stderr)
        self.assertEqual(self.check(good, mode=0o644).returncode, 0)
        self.d.chmod(0o777)
        r = self.check(good)
        self.assertEqual(r.returncode, 1)
        self.assertIn(str(self.d), r.stderr)
        self.d.chmod(0o700)
        link = self.d / "link"
        self.f.rename(link)
        self.f.symlink_to(link)
        r = self.check()
        self.assertIn("symlink", r.stderr)


class Scan(unittest.TestCase):
    def test_scan(self):
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            (d / "etc/chef/ssh").mkdir(parents=True)
            (d / "usr/lib").mkdir(parents=True)
            (d / "etc/chef/ssh/authorized_keys").write_text(ed25519_line() + "\n")
            (d / "usr/lib/libcrypto.so.3").write_bytes(b"\x7fELF" + b"\0" * 64 + b"-----BEGIN PRIVATE KEY-----")
            run = lambda: subprocess.run([sys.executable, str(KEYS), "scan", t], capture_output=True, text=True)
            self.assertEqual(run().returncode, 0, run().stderr)
            cases = {"etc/x.pem": b"-----BEGIN EC PRIVATE KEY-----\n",
                     "etc/y": b"openssh-key-v1\0",
                     "etc/z": sshstr(b"ssh-ed25519") + sshstr(b"\x02" * 64),
                     "etc/dropbear/dropbear_ed25519_host_key": b"",
                     "etc/ssh/ssh_host_rsa_key": b"x"}
            for name, data in cases.items():
                (d / name).parent.mkdir(parents=True, exist_ok=True)
                (d / name).write_bytes(data)
                r = run()
                self.assertEqual(r.returncode, 1, name)
                self.assertIn(name, r.stderr)
                (d / name).unlink()


STUB = r'''#!/bin/sh
name=${0##*/}
echo "$name $*" >> "$TRACE"
case "$name" in
chef-state) [ "${OURS:-1}" = 1 ]; exit;;
chown|sync) exit 0;;
chmod)
    for a; do case "$a" in "${CHMOD_FAIL_UNDER:-/nonexistent}"*) echo "chmod: $a: Read-only file system" >&2; exit 1;; esac; done
    exec /bin/chmod "$@";;
sleep) exit 0;;
dropbearkey)
    [ "${KEYGEN:-ok}" = ok ] || exit 1
    case "$1" in
    -y) f=$3; [ -f "$f" ] || exit 255; read -r first < "$f" || exit 255
        case "$first" in VALID*) echo "Public key portion is:"; echo "ssh-ed25519 AAAA test"; echo "Fingerprint: SHA256:testfp"; exit 0;; esac
        exit 255;;
    -t) f=$4; [ ! -e "$f" ] || exit 1; echo "VALID $(cat /proc/sys/kernel/random/uuid)" > "$f"; chmod 600 "$f"
        echo "ssh-ed25519 AAAA pub" > "$f.pub"; exit 0;;
    esac
    exit 2;;
dropbear)
    printf '%s\n' "$@" > "$TRACE.dropbear"
    i=0
    while [ "$i" -lt "${LINES1:-2}" ]; do echo "[4242] Oct 10 20:01:02 line $i" >&2; i=$((i + 1)); done
    if [ -n "${LINES2:-}" ]; then
        /bin/sleep 0.5; echo "${UP2:-1000.00} 1.00" > "$UPTIME_FILE"
        i=0; while [ "$i" -lt "$LINES2" ]; do echo "[4242] Oct 10 20:02:02 late $i" >&2; i=$((i + 1)); done
    fi
    [ -z "${EXTRA:-}" ] || cat "$EXTRA" >&2
    echo "plain line" >&2
    exit "${DB_RC:-3}";;
esac
exit 0
'''
APPLETS = ["chown", "chmod", "sync", "sleep"]


class ChefSshd(unittest.TestCase):
    shell = ["bash"]

    def setUp(self):
        t = tempfile.TemporaryDirectory()
        self.addCleanup(t.cleanup)
        p = self.p = Path(t.name)
        for d in ["bin", "keys", "data/v1", "run"]:
            (p / d).mkdir(parents=True)
        (p / "keys/authorized_keys").write_text(ed25519_line() + "\n")
        (p / "kmsg").write_text("")
        (p / "uptime").write_text("100.00 1.00\n")
        stub = p / "stub"
        stub.write_text(STUB)
        stub.chmod(0o755)
        for n in APPLETS + ["chef-state", "dropbearkey", "dropbear"]:
            (p / "bin" / n).symlink_to(stub)
        wrapper = "".join(f'{n}() {{ "$TEST_BIN/{n}" "$@"; }}\n' for n in APPLETS)
        self.script = p / "chef-sshd"
        self.script.write_text(wrapper + SSHD.read_text())
        self.trace = p / "trace"
        self.trace.write_text("")
        self.run_dir = p / "run/ssh"
        self.key_dir = p / "data/v1/ssh"
        self.env = {**os.environ, "TEST_BIN": str(p / "bin"), "TRACE": str(self.trace),
                    "PATH": f"{p / 'bin'}:{os.environ['PATH']}",
                    "UPTIME_FILE": str(p / "uptime"),
                    "CHEF_SSHD_KEYS": str(p / "keys"), "CHEF_SSHD_DATA": str(p / "data"),
                    "CHEF_SSHD_RUN": str(self.run_dir), "CHEF_SSHD_KMSG": str(p / "kmsg"),
                    "CHEF_SSHD_UPTIME": str(p / "uptime"), "CHEF_SSHD_DROPBEAR": str(p / "bin/dropbear")}

    def run_sshd(self, **env):
        r = subprocess.run([*self.shell, str(self.script)], env={**self.env, **env},
                           capture_output=True, text=True, timeout=30)
        return r.returncode, self.kmsg(), self.trace.read_text().splitlines()

    def kmsg(self):
        return (self.p / "kmsg").read_text().splitlines()

    def dropbear_args(self):
        return (self.p / "trace.dropbear").read_text().splitlines()

    def mode(self, path):
        return stat.S_IMODE(os.lstat(path).st_mode)

    def test_generates_persistent_key(self):
        rc, kmsg, trace = self.run_sshd()
        self.assertEqual(rc, 1)
        key = self.key_dir / "dropbear_ed25519_host_key"
        self.assertTrue(key.read_text().startswith("VALID"))
        self.assertEqual(self.mode(self.key_dir), 0o700)
        self.assertEqual(self.mode(key), 0o600)
        self.assertEqual(sorted(os.listdir(self.key_dir)), ["dropbear_ed25519_host_key"])
        self.assertEqual((self.run_dir / "dropbear_ed25519_host_key").read_text(), key.read_text())
        self.assertEqual(self.mode(self.run_dir / "dropbear_ed25519_host_key"), 0o600)
        self.assertEqual(self.mode(self.run_dir), 0o700)
        self.assertEqual((self.run_dir / "host-key-source").read_text(), "persistent\n")
        self.assertIn(f"chef-sshd: no host key in {self.key_dir}; generating one", kmsg)
        self.assertIn("chef-sshd: host key persistent SHA256:testfp; listening on port 22, key login only", kmsg)
        self.assertEqual(self.dropbear_args(),
                         ["-F", "-E", "-s", "-m", "-k", "-T", "3", "-K", "30", "-p", "22",
                          "-r", str(self.run_dir / "dropbear_ed25519_host_key"),
                          "-D", str(self.p / "keys"), "-P", str(self.run_dir / "dropbear.pid")])
        # dropbear's exit is logged, then a sleep before init respawns
        self.assertIn("chef-sshd: dropbear exited (3); retrying in 10 s", kmsg)
        self.assertEqual(trace[-1], "sleep 10")
        # the generated key was synced before and after the rename
        self.assertGreaterEqual(trace.count("sync "), 2)

    def test_existing_key_kept(self):
        self.key_dir.mkdir(mode=0o755)
        key = self.key_dir / "dropbear_ed25519_host_key"
        key.write_text("VALID existing\n")
        key.chmod(0o644)
        before = os.stat(key)
        rc, kmsg, trace = self.run_sshd()
        self.assertEqual(key.read_text(), "VALID existing\n")
        self.assertEqual(os.stat(key).st_mtime_ns, before.st_mtime_ns)
        self.assertEqual(self.mode(key), 0o600)
        self.assertEqual(self.mode(self.key_dir), 0o700)
        self.assertNotIn("dropbearkey -t", " ".join(trace))
        self.assertEqual((self.run_dir / "dropbear_ed25519_host_key").read_text(), "VALID existing\n")
        self.assertFalse(any("generating" in l for l in kmsg))

    def test_corrupt_and_empty_keys_replaced(self):
        for content in ["garbage\n", ""]:
            with self.subTest(content=content):
                self.key_dir.mkdir(mode=0o700, exist_ok=True)
                key = self.key_dir / "dropbear_ed25519_host_key"
                key.write_text(content)
                (self.p / "kmsg").write_text("")
                rc, kmsg, trace = self.run_sshd()
                self.assertEqual((self.key_dir / "dropbear_ed25519_host_key.bad").read_text(), content)
                self.assertTrue(key.read_text().startswith("VALID"))
                self.assertIn(f"chef-sshd: host key in {self.key_dir} is unreadable or corrupt; "
                              "moved to dropbear_ed25519_host_key.bad, generating a new one", kmsg)
                self.assertEqual((self.run_dir / "host-key-source").read_text(), "persistent\n")

    def test_ram_only(self):
        rc, kmsg, trace = self.run_sshd(OURS="0")
        self.assertFalse(self.key_dir.exists())
        key = self.run_dir / "dropbear_ed25519_host_key"
        first = key.read_text()
        self.assertTrue(first.startswith("VALID"))
        self.assertEqual((self.run_dir / "host-key-source").read_text(), "ephemeral\n")
        self.assertIn(f"chef-sshd: /data is not ours: host key in {self.run_dir} is ephemeral (this boot only)", kmsg)
        self.assertNotIn("dropbear_ed25519_host_key.tmp", os.listdir(self.run_dir))
        self.assertNotIn("dropbear_ed25519_host_key.tmp.pub", os.listdir(self.run_dir))
        # a respawn in the same boot keeps the key
        self.run_sshd(OURS="0")
        self.assertEqual(key.read_text(), first)

    def test_data_failure_falls_back_to_ram(self):
        (self.p / "data/v1/ssh").write_text("not a directory")
        rc, kmsg, trace = self.run_sshd()
        self.assertIn("chef-sshd: persistent host key unavailable; falling back to RAM", kmsg)
        self.assertEqual((self.run_dir / "host-key-source").read_text(), "ephemeral\n")
        self.assertEqual((self.p / "data/v1/ssh").read_text(), "not a directory")

    def test_readonly_data_keeps_valid_key(self):
        # /data ours but read-only (errors=remount-ro): chmod fails there
        self.key_dir.mkdir(mode=0o700)
        key = self.key_dir / "dropbear_ed25519_host_key"
        key.write_text("VALID existing\n")
        key.chmod(0o600)
        rc, kmsg, trace = self.run_sshd(CHMOD_FAIL_UNDER=str(self.p / "data"))
        self.assertIn(f"chef-sshd: cannot prepare {self.key_dir}; using its existing host key as it is", kmsg)
        self.assertEqual((self.run_dir / "host-key-source").read_text(), "persistent\n")
        self.assertEqual((self.run_dir / "dropbear_ed25519_host_key").read_text(), "VALID existing\n")
        self.assertNotIn("dropbearkey -t", " ".join(trace))
        # an open directory, or no valid key, is not trusted: ephemeral
        for mode, content in [(0o755, "VALID existing\n"), (0o700, "garbage\n")]:
            with self.subTest(mode=oct(mode), content=content):
                self.key_dir.chmod(mode)
                key.write_text(content)
                shutil.rmtree(self.run_dir)
                (self.p / "kmsg").write_text("")
                rc, kmsg, trace = self.run_sshd(CHMOD_FAIL_UNDER=str(self.p / "data"))
                self.assertIn(f"chef-sshd: cannot prepare {self.key_dir}", kmsg)
                self.assertEqual((self.run_dir / "host-key-source").read_text(), "ephemeral\n")
                self.assertEqual(key.read_text(), content)

    def test_keygen_failure_sleeps(self):
        rc, kmsg, trace = self.run_sshd(OURS="0", KEYGEN="fail")
        self.assertEqual(rc, 1)
        self.assertIn(f"chef-sshd: host key generation in {self.run_dir} failed; retrying in 10 s", kmsg)
        self.assertEqual(trace[-1], "sleep 10")
        self.assertFalse((self.p / "trace.dropbear").exists())

    def test_ssh_disabled_image_idles(self):
        (self.p / "keys/authorized_keys").unlink()
        rc, kmsg, trace = self.run_sshd()
        self.assertEqual(rc, 0)
        self.assertEqual(kmsg, [f"chef-sshd: no {self.p / 'keys'}/authorized_keys in this image (built with SSH=0): SSH disabled"])
        self.assertEqual(trace, ["sleep 2147483647"])
        self.assertFalse(self.run_dir.exists())

    def test_log_format_and_rate_limit(self):
        rc, kmsg, trace = self.run_sshd(LINES1="3")
        self.assertIn("dropbear[4242]: line 0", kmsg)
        self.assertIn("dropbear[4242]: line 2", kmsg)
        self.assertIn("dropbear: plain line", kmsg)
        (self.p / "kmsg").write_text("")
        rc, kmsg, trace = self.run_sshd(LINES1="50")
        db = [l for l in kmsg if l.startswith("dropbear")]
        self.assertEqual(len(db), 20)
        self.assertEqual(db[-1], "dropbear[4242]: line 19")
        self.assertEqual(kmsg.count("chef-sshd: 31 dropbear log lines suppressed"), 1)
        # a new window after LOG_WINDOW s reports the suppressed count first
        (self.p / "kmsg").write_text("")
        (self.p / "uptime").write_text("100.00 1.00\n")
        rc, kmsg, trace = self.run_sshd(LINES1="25", LINES2="2", UP2="161.50")
        i = kmsg.index("chef-sshd: 5 dropbear log lines suppressed")
        self.assertEqual(kmsg[i + 1:i + 4], ["dropbear[4242]: late 0", "dropbear[4242]: late 1", "dropbear: plain line"])

    def test_logins_bypass_the_budget(self):
        extra = self.p / "extra"
        extra.write_text("[4242] Oct 10 20:03:04 Pubkey auth succeeded for 'root' with ssh-ed25519 key SHA256:x from 1.2.3.4:5\n"
                         "[4242] Oct 10 20:03:04 Exit before auth from <1.2.3.4:5>: (user 'x] Oct 10 20:03:04 Pubkey auth succeeded for y', 0 fails)\n"
                         "[42x] Oct 10 20:03:04 Pubkey auth succeeded for 'root'\n"
                         "Pubkey auth succeeded for 'root'\n")
        rc, kmsg, trace = self.run_sshd(LINES1="50", EXTRA=str(extra))
        self.assertIn("dropbear[4242]: Pubkey auth succeeded for 'root' with ssh-ed25519 key SHA256:x from 1.2.3.4:5", kmsg)
        self.assertEqual(len([l for l in kmsg if l.startswith("dropbear")]), 21)
        # the smuggled, malformed and bare lines are ordinary: suppressed, counted
        self.assertEqual(kmsg.count("chef-sshd: 34 dropbear log lines suppressed"), 1)
        self.assertFalse(any("user 'x]" in l or "[42x]" in l for l in kmsg))

    def test_production_values(self):
        text = SSHD.read_text()
        for line in ["LOG_BURST=20\n", "LOG_WINDOW=60\n", "FAIL_SLEEP=10\n", "set -o pipefail\n"]:
            self.assertIn(line, text)
        self.assertNotIn(" -B", text)
        self.assertNotIn(" -R", text)
        self.assertTrue(os.access(SSHD, os.X_OK))


class AshChefSshd(ChefSshd):
    shell = ["busybox", "ash"]


class Dispatcher(unittest.TestCase):
    def run_hook(self, iface, action, addrs, **env):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t)
            (p / "bin").mkdir()
            ip = p / "bin/ip"
            ip.write_text('#!/bin/sh\nfor a in $ADDRS; do printf "3: wlan0    inet %s brd x scope global wlan0\\n" "$a"; done\n')
            ip.chmod(0o755)
            kmsg = p / "kmsg"
            kmsg.write_text("")
            r = subprocess.run(["sh", str(DISPATCH), iface, action],
                               env={**os.environ, "PATH": f"{p / 'bin'}:{os.environ['PATH']}", "ADDRS": addrs,
                                    "CHEF_WIFI_IP_KMSG": str(kmsg), **env}, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0)
            return kmsg.read_text()

    def test_lines(self):
        self.assertEqual(self.run_hook("wlan0", "up", "192.168.0.116/24", CONNECTION_ID="home"),
                         "chef Wi-Fi: home IPv4 192.168.0.116, ssh root@192.168.0.116\n")
        self.assertEqual(self.run_hook("wlan0", "dhcp4-change", "10.0.0.5/8 10.0.0.6/8"),
                         "chef Wi-Fi: wlan0 IPv4 10.0.0.5 10.0.0.6, ssh root@10.0.0.5\n")
        self.assertEqual(self.run_hook("wlan0", "down", "192.168.0.116/24"), "")
        self.assertEqual(self.run_hook("usb0", "up", "192.168.0.116/24"), "")
        self.assertEqual(self.run_hook("wlan0", "up", ""), "")
        self.assertTrue(os.access(DISPATCH, os.X_OK))


class Wiring(unittest.TestCase):
    def test_inittab(self):
        lines = (ROOT / "initramfs/etc/inittab").read_text().splitlines()
        self.assertEqual(lines[2], "::respawn:/bin/sh -c 'export HOME=/root XDG_RUNTIME_DIR=/run/user/0 "
                         "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus; exec /usr/sbin/telnetd -F -b 172.16.42.1 -l /bin/sh'")
        self.assertEqual(lines.count("::respawn:/usr/bin/chef-sshd"), 1)
        self.assertFalse(any("dropbear" in l for l in lines if not l.startswith("#")))

    def test_only_root_has_a_login_shell(self):
        # -D /etc/chef/ssh applies to every account: the baked keys must only
        # ever admit root, so root is the only account with a listed shell.
        shells_file = ROOT / "out/rootfs/etc/shells"
        if not shells_file.exists():
            self.skipTest("out/rootfs not built")
        shells = {l.strip() for l in shells_file.read_text().splitlines() if l.strip() and not l.startswith("#")}
        users = [l.split(":") for l in (ROOT / "initramfs/etc/passwd").read_text().splitlines() if l.strip()]
        self.assertEqual([u[0] for u in users if u[6] in shells], ["root"])
        self.assertFalse((ROOT / "initramfs/etc/shells").exists())

    def test_storage_makes_ssh_dir(self):
        self.assertIn("for dir in crash time power sensors ssh; do", (ROOT / "initramfs/usr/bin/chef-storage").read_text())

    def test_build(self):
        rootfs = (ROOT / "scripts/mkrootfs.sh").read_text()
        self.assertIn("dropbear openssh-sftp-server", rootfs)
        mk = (ROOT / "scripts/mkinitramfs.sh").read_text()
        self.assertIn('python3 scripts/ssh-keys.py check "$AUTHORIZED_KEYS"', mk)
        self.assertIn('python3 scripts/ssh-keys.py scan "$ROOT"', mk)
        self.assertLess(mk.index('ssh-keys.py scan "$ROOT"'), mk.index("cpio -0 -o"))
        self.assertIn('install -m 644 "$AUTHORIZED_KEYS" "$ROOT/etc/chef/ssh/authorized_keys"', mk)
        self.assertIn('chmod 755 "$ROOT" "$ROOT/etc" "$ROOT/etc/chef"', mk)
        self.assertIn('"$ROOT"/usr/bin/chef-sshd', mk)
        self.assertIn('python3 scripts/ssh-keys.py scan "$SRC"', (ROOT / "scripts/mksystem.sh").read_text())
        r = subprocess.run(["git", "check-ignore", "-q", "secrets/ssh/authorized_keys"], cwd=ROOT)
        self.assertEqual(r.returncode, 0)

    def test_build_gate(self):
        # The SSH gate at the top of mkinitramfs.sh, run on its own.
        mk = (ROOT / "scripts/mkinitramfs.sh").read_text()
        gate = mk.split("AUTHORIZED_KEYS=${AUTHORIZED_KEYS:-", 1)[1].split("\nfor f in usr/sbin/dropbear", 1)[0]
        gate = "AUTHORIZED_KEYS=${AUTHORIZED_KEYS:-" + gate
        with tempfile.TemporaryDirectory() as t:
            env = {**os.environ, "AUTHORIZED_KEYS": str(Path(t) / "missing")}
            r = subprocess.run(["sh", "-c", gate], cwd=ROOT, env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("refusing to build an image with SSH", r.stderr)
            r = subprocess.run(["sh", "-c", gate], cwd=ROOT, env={**env, "SSH": "0"}, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0)
            self.assertIn("WARNING: SSH=0", r.stderr)
            r = subprocess.run(["sh", "-c", gate], cwd=ROOT, env={**env, "SSH": "yes"}, capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)


FAKE_SSH = r'''#!/bin/sh
printf '%s\n' "$@" > "$FAKE_DIR/argv"
for last; do :; done
case "$MODE" in
changed) echo "@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @@@" >&2; exit 255;;
lost) /bin/sh -c "$last" | head -c 100; echo "Connection to h closed by remote host." >&2; exit 255;;
esac
exec /bin/sh -c "$last"
'''


class PhoneSsh(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory()
        self.addCleanup(t.cleanup)
        p = self.p = Path(t.name)
        (p / "bin").mkdir()
        ssh = p / "bin/ssh"
        ssh.write_text(FAKE_SSH)
        ssh.chmod(0o755)
        self.env = {**os.environ, "PATH": f"{p / 'bin'}:{os.environ['PATH']}", "FAKE_DIR": str(p),
                    "PHONE_SSH_KEY": "/k/id", "PHONE_SSH_KNOWN_HOSTS": "/k/known"}
        self.env.pop("PHONE_SSH", None)

    def phone(self, *args, **env):
        return subprocess.run([sys.executable, str(ROOT / "scripts/phone.py"), *args],
                              env={**self.env, **env}, capture_output=True, timeout=30)

    def argv(self):
        return (self.p / "argv").read_text().splitlines()

    def test_run_and_argv(self):
        r = self.phone("--ssh", "10.1.2.3", "run", 'echo "$PATH $HOME"; exit 7')
        self.assertEqual(r.returncode, 7)
        self.assertEqual(r.stdout.decode(), "/sbin:/usr/sbin:/bin:/usr/bin /root\n")
        argv = self.argv()
        for opt in ["BatchMode=yes", "IdentitiesOnly=yes", "PasswordAuthentication=no",
                    "UserKnownHostsFile=/k/known", "StrictHostKeyChecking=accept-new", "HostKeyAlias=chef-cyclo"]:
            self.assertIn(opt, argv)
        self.assertNotIn("StrictHostKeyChecking=no", argv)
        self.assertEqual(argv[:8], ["-F", "none", "-T", "-l", "root", "-i", "/k/id", "-o"])
        self.assertEqual(argv[-2], "10.1.2.3")
        # PHONE_SSH selects SSH too
        r = self.phone("run", "true", PHONE_SSH="10.9.9.9")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(self.argv()[-2], "10.9.9.9")

    def test_push_pull_stream(self):
        src = self.p / "tool"
        src.write_bytes(os.urandom(5000))
        src.chmod(0o755)
        dest = self.p / "dest"
        r = self.phone("--ssh", "h", "push", str(src), "-d", str(dest))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((dest / "tool").read_bytes(), src.read_bytes())
        self.assertTrue(os.access(dest / "tool", os.X_OK))
        self.assertFalse((dest / "tool.part").exists())
        out = self.p / "o.tgz"
        r = self.phone("--ssh", "h", "pull", str(dest), "-o", str(out))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(b"2 entries", r.stdout)
        so = self.p / "s.txt"
        r = self.phone("--ssh", "h", "stream", "echo one; echo two", str(so))
        self.assertEqual(so.read_text(), "one\ntwo\n")
        self.assertIn("ServerAliveInterval=5", self.argv())

    def test_pull_connection_lost(self):
        src = self.p / "big"
        src.write_bytes(os.urandom(100000))
        out = self.p / "o.tgz"
        r = self.phone("--ssh", "h", "pull", str(src), "-o", str(out), MODE="lost")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"Connection to h closed", r.stderr)
        self.assertNotIn(b"Traceback", r.stderr)
        self.assertFalse(out.exists())

    def test_host_key_changed(self):
        r = self.phone("--ssh", "h", "run", "true", MODE="changed")
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"ssh-keygen -R chef-cyclo -f /k/known", r.stderr)
        self.assertIn(b"/run/ssh/host-key-source", r.stderr)


if __name__ == "__main__":
    unittest.main()
