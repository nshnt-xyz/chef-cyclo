#!/usr/bin/env python3
"""Host tests for rootfs-overlay/usr/bin/bt-up (docs/features/bluetooth.md).

bt-up runs under bash and BusyBox ash with stubs for btprobe, stty,
btattach (records its arguments, exits) and pidof (prints the pids a test
names), with /dev/null for the UART and /dev/btpower and a temporary
bt-powered marker and cmdline; sleep, stty and pidof are bound as shell
functions too, since BusyBox ash may prefer its own applets. Covered: a
btattach left by a bt-up killed with kill -9 (a real process named
btattach) is stopped before the power and pulse sequence, with TERM, or
KILL when it ignores TERM, and one log line; a zombie btattach is left
alone; with none, nothing is logged; the running-firmware shutdown path
follows when the marker is there. The log helper never reaches the host's
syslog.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "rootfs-overlay/usr/bin/bt-up"

STUB = r'''#!/bin/sh
name=${0##*/}
echo "$name $*" >> "$TRACE"
case "$name" in
sleep) case "$1" in 0.*) exec /bin/sleep "$1";; esac; exit 0;;
pidof) [ "$1" = btattach ] && [ -s "$PIDS" ] && cat "$PIDS"; exit 0;;
esac
exit 0
'''
APPLETS = ["sleep", "stty", "pidof"]

LEFTOVER = '''#!/bin/sh
if [ "$1" = ignore ]; then trap '' TERM; else trap 'exit 0' TERM; fi
while :; do /bin/sleep 0.1; done
'''


class BtUp(unittest.TestCase):
    shell = ["bash"]

    def setUp(self):
        t = tempfile.TemporaryDirectory()
        self.addCleanup(t.cleanup)
        p = self.p = Path(t.name)
        (p / "bin").mkdir()
        (p / "old").mkdir()
        stub = p / "stub"
        stub.write_text(STUB)
        stub.chmod(0o755)
        for n in APPLETS + ["btprobe", "btattach"]:
            (p / "bin" / n).symlink_to(stub)
        # a real process whose comm is btattach, as the old bt-up's child
        self.leftover = p / "old/btattach"
        self.leftover.write_text(LEFTOVER)
        self.leftover.chmod(0o755)
        wrapper = "".join(f'{n}() {{ "$TEST_BIN/{n}" "$@"; }}\n' for n in APPLETS)
        self.script = p / "bt-up"
        self.script.write_text(wrapper + SCRIPT.read_text())
        (p / "cmdline").write_text("console=ttyMSM0 quiet\n")
        (p / "trace").write_text("")
        (p / "kmsg").write_text("")
        logger = p / "logger"
        logger.write_text('#!/bin/sh\necho "logger $*" >> "$0.out"\n')
        logger.chmod(0o755)
        self.env = {**os.environ, "TEST_BIN": str(p / "bin"), "TRACE": str(p / "trace"), "PIDS": str(p / "pids"),
                    "PATH": f"{p / 'bin'}:{os.environ['PATH']}",
                    "BT_UP_UART": "/dev/null", "BT_UP_BTPOWER": "/dev/null",
                    "BT_UP_POWERED": str(p / "bt-powered"), "BT_UP_CMDLINE": str(p / "cmdline"),
                    "CHEF_LIB_DIR": str(ROOT / "rootfs-overlay/usr/lib/chef"),
                    "CHEF_LOG_KMSG": str(p / "kmsg"), "CHEF_LOG_SYSLOGD_PID": str(p / "syslogd.pid"),
                    "CHEF_LOG_LOGGER": str(logger)}

    def start_leftover(self, *args):
        proc = subprocess.Popen([str(self.leftover), *args], start_new_session=True)
        self.addCleanup(lambda: proc.poll() is not None or proc.kill() or proc.wait())
        time.sleep(0.2)
        self.assertIsNone(proc.poll())
        return proc

    def run_bt_up(self, pids=()):
        (self.p / "pids").write_text("".join(f"{p}\n" for p in pids))
        r = subprocess.run([*self.shell, str(self.script)], env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout.splitlines(), (self.p / "trace").read_text().splitlines()

    def test_none_left(self):
        out, trace = self.run_bt_up()
        self.assertEqual(out[0], "bt-up: enabling WCN3990 power rails")
        self.assertFalse(any("left by" in l for l in out))
        self.assertEqual(trace[0], "pidof btattach")
        self.assertIn("btprobe power 1", trace)
        self.assertIn("btattach -B /dev/null -P qca -S 115200", trace)
        self.assertIn("bt-up: no androidboot.btmacaddr on the cmdline; hci0 stays unconfigured", out)
        self.assertTrue((self.p / "bt-powered").exists())

    def test_leftover_stopped_with_term(self):
        old = self.start_leftover()
        (self.p / "bt-powered").write_text("")
        t0 = time.monotonic()
        out, trace = self.run_bt_up([old.pid])
        self.assertEqual(old.wait(timeout=5), 0)
        self.assertLess(time.monotonic() - t0, 2)
        self.assertEqual(out[0], f"bt-up: stopped btattach {old.pid} left by an earlier bt-up")
        # then the running firmware is shut down as before
        self.assertEqual(out[1], "bt-up: shutting down the running WCN3990 firmware")
        self.assertLess(trace.index("pidof btattach"), trace.index("btprobe baud /dev/null 3200000 1"))
        kmsg = (self.p / "kmsg").read_text()
        self.assertIn(f"bt-up: stopped btattach {old.pid} left by an earlier bt-up", kmsg)

    def test_leftover_ignoring_term_is_killed(self):
        old = self.start_leftover("ignore")
        other = self.start_leftover()
        t0 = time.monotonic()
        out, trace = self.run_bt_up([old.pid, other.pid])
        self.assertGreaterEqual(time.monotonic() - t0, 2)
        self.assertEqual(old.wait(timeout=5), -9)
        self.assertEqual(other.wait(timeout=5), 0)
        self.assertEqual(out[0], f"bt-up: stopped btattach {old.pid} {other.pid} left by an earlier bt-up "
                                 f"(KILL after 2 s: {old.pid})")
        self.assertEqual(out[1], "bt-up: enabling WCN3990 power rails")

    def test_zombie_left_alone(self):
        dead = subprocess.Popen(["true"])
        self.addCleanup(dead.wait)
        time.sleep(0.2)
        self.assertEqual(Path(f"/proc/{dead.pid}/stat").read_text().rsplit(") ", 1)[1][0], "Z")
        out, trace = self.run_bt_up([dead.pid, 999999999])
        self.assertFalse(any("left by" in l for l in out))
        self.assertEqual(out[0], "bt-up: enabling WCN3990 power rails")


class AshBtUp(BtUp):
    shell = ["busybox", "ash"]


if __name__ == "__main__":
    unittest.main()
