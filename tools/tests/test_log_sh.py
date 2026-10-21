#!/usr/bin/env python3
"""Host tests for rootfs-overlay/usr/lib/chef/log.sh (bash and busybox ash).

chef_log TAG MESSAGE goes through logger only while CHEF_LOG_SYSLOGD_PID
names a live process called syslogd (a shell script named syslogd stands
in); with no pid file, a stale or foreign pid, garbage, CHEF_LOG_KMSG_ONLY=1
or a failing logger the line is "TAG: MESSAGE" in the kmsg file. logger is
always a stub that records its arguments, so nothing reaches the host's
syslog. Never fails, under set -eu too. Also the logging wiring: the
syslogd inittab line and /init setting the host name it stamps.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
LIB = ROOT / "rootfs-overlay/usr/lib/chef"


class LogSh(unittest.TestCase):
    shell = ["bash"]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name)
        self.kmsg = self.p / "kmsg"
        self.kmsg.touch()
        self.calls = self.p / "logger-calls"
        self.logger = self.p / "logger"
        self.logger.write_text('#!/bin/sh\nfor a; do printf "%s|" "$a"; done >> "$CALLS"; echo >> "$CALLS"\n'
                               'exit "${LOGGER_RC:-0}"\n')
        self.logger.chmod(0o755)
        self.pidfile = self.p / "syslogd.pid"
        self.env = {"PATH": os.environ["PATH"], "CHEF_LIB_DIR": str(LIB), "CHEF_LOG_KMSG": str(self.kmsg),
                    "CHEF_LOG_SYSLOGD_PID": str(self.pidfile), "CHEF_LOG_LOGGER": str(self.logger),
                    "CALLS": str(self.calls)}

    def run_sh(self, body, **env):
        script = 'set -eu\n. "$CHEF_LIB_DIR/log.sh"\n' + body + '\necho "rc=$?"\n'
        r = subprocess.run([*self.shell, "-c", script], env={**self.env, **env}, capture_output=True, text=True,
                           timeout=10)
        self.assertEqual((r.returncode, r.stdout, r.stderr), (0, "rc=0\n", ""))
        read = lambda f: f.read_text() if f.exists() else ""
        return read(self.kmsg), read(self.calls)

    def start(self, name):
        # comm comes from the script's file name (a copied sleep may be a
        # multicall binary that refuses another name)
        exe = self.p / name
        exe.write_text("#!/bin/sh\nwhile :; do sleep 1; done\n")
        exe.chmod(0o755)
        proc = subprocess.Popen([str(exe)])
        time.sleep(0.05)
        self.assertIsNone(proc.poll())
        self.addCleanup(proc.wait)
        self.addCleanup(proc.kill)
        return proc

    def test_no_syslogd_writes_kmsg(self):
        self.assertEqual(self.run_sh('chef_log gps-up "modem ready" now'), ("gps-up: modem ready now\n", ""))

    def test_live_syslogd_uses_logger(self):
        self.pidfile.write_text(f"{self.start('syslogd').pid}\n")
        self.assertEqual(self.run_sh('chef_log sensors-up -v starts "a  b"'),
                         ("", "-t|sensors-up|-p|daemon.info|--|-v starts a  b|\n"))

    def test_kmsg_only_wins(self):
        self.pidfile.write_text(f"{self.start('syslogd').pid}\n")
        self.assertEqual(self.run_sh("chef_log chef-state shutdown", CHEF_LOG_KMSG_ONLY="1"),
                         ("chef-state: shutdown\n", ""))
        self.assertEqual(self.run_sh("chef_log chef-state running", CHEF_LOG_KMSG_ONLY="0")[1],
                         "-t|chef-state|-p|daemon.info|--|running|\n")

    def test_syslogd_up_ignores_kmsg_only(self):
        self.run_sh('chef_log_syslogd_up && echo up >> "$CALLS" || echo down >> "$CALLS"', CHEF_LOG_KMSG_ONLY="1")
        self.pidfile.write_text(f"{self.start('syslogd').pid}\n")
        self.run_sh('chef_log_syslogd_up && echo up >> "$CALLS" || echo down >> "$CALLS"', CHEF_LOG_KMSG_ONLY="1")
        self.assertEqual(self.calls.read_text(), "down\nup\n")

    def test_failing_logger_falls_back(self):
        self.pidfile.write_text(f"{self.start('syslogd').pid}\n")
        kmsg, calls = self.run_sh("chef_log bt-up attached", LOGGER_RC="1")
        self.assertEqual(kmsg, "bt-up: attached\n")
        self.assertEqual(calls.count("\n"), 1)

    def test_not_a_live_syslogd(self):
        other = self.start("sleep")
        dead = subprocess.Popen(["true"])
        dead.wait()
        for content in (f"{other.pid}\n", f"{dead.pid}\n", "", "12x\n", "-1\n", "../1\n"):
            self.pidfile.write_text(content)
            self.kmsg.write_text("")
            self.assertEqual(self.run_sh("chef_log chef-sshd hello"), ("chef-sshd: hello\n", ""), content)

    def test_syslog_only(self):
        # chef_log_syslog: logger while syslogd is up, else status 1 and nothing
        self.assertEqual(self.run_sh('chef_log_syslog compass "STEP 1" || echo down >> "$CALLS"'),
                         ("", "down\n"))
        self.pidfile.write_text(f"{self.start('syslogd').pid}\n")
        self.calls.unlink()
        self.assertEqual(self.run_sh('chef_log_syslog compass "STEP 1"'),
                         ("", "-t|compass|-p|daemon.info|--|STEP 1|\n"))

    def test_message_kept_verbatim(self):
        self.assertEqual(self.run_sh(r"chef_log x '-n a\tb\\c %s'"), ("x: -n a\\tb\\\\c %s\n", ""))

    def test_kmsg_missing_or_odd(self):
        self.kmsg.unlink()
        self.run_sh("chef_log x one", CHEF_LOG_KMSG=str(self.p / "none"))
        self.assertFalse((self.p / "none").exists())
        self.run_sh("chef_log x two", CHEF_LOG_KMSG=str(self.p))  # a directory: skipped

    def test_defaults_never_reach_logger(self):
        # no overrides but the stub: on the host /run/syslogd.pid does not
        # name a syslogd, so the stub must not be called
        if Path("/run/syslogd.pid").exists():
            self.skipTest("this host has /run/syslogd.pid")
        env = {k: v for k, v in self.env.items() if k != "CHEF_LOG_SYSLOGD_PID"}
        r = subprocess.run([*self.shell, "-c", '. "$CHEF_LIB_DIR/log.sh"; chef_log x y'], env=env,
                           capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0)
        self.assertFalse(self.calls.exists())


class Wiring(unittest.TestCase):
    def test_syslogd_line(self):
        # first respawn line; -t: every ring time is syslogd's local time,
        # never the UTC stamp musl's syslog(3) puts in its messages
        lines = (ROOT / "rootfs-overlay/etc/inittab").read_text().splitlines()
        respawn = [l for l in lines if l.startswith("::respawn:")]
        self.assertEqual(respawn[0], "::respawn:/sbin/syslogd -n -t -C1024")

    def test_init_sets_the_hostname(self):
        # syslogd reads it once at start: set before init runs anything
        init = (ROOT / "rootfs-overlay/init").read_text()
        line = 'hostname -F /etc/hostname || log "hostname not set from /etc/hostname"'
        self.assertEqual(init.count(line), 1)
        self.assertLess(init.index("log()"), init.index(line))
        self.assertLess(init.index(line), init.index("exec /sbin/init"))
        self.assertEqual((ROOT / "rootfs-overlay/etc/hostname").read_text(), "chef-cyclo\n")


@unittest.skipUnless(shutil.which("busybox"), "needs busybox")
class AshLogSh(LogSh):
    shell = ["busybox", "ash"]


if __name__ == "__main__":
    unittest.main()
