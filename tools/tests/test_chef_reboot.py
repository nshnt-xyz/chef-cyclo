#!/usr/bin/env python3
"""Host tests for initramfs/usr/bin/chef-reboot (bash and busybox ash).

chef-state, chef-storage and btprobe are stubs on PATH; a successful btprobe
SIGKILLs the helper, as reboot(2) would (no exit trap runs). Signals to PID 1
never leave the fixture: `kill SIG 1` is a shell function that only records
it. setsid (the CONT watchdog) only records, sleep 0.1 is shortened and
other sleeps only record, timeout behaves like BusyBox's (it execs the
command), and flock is the host's with BusyBox's options. Host BusyBox ash
may prefer its own applets over PATH, so those stubs are also bound as shell
functions, as in test_storage.py. Covered: the order (pause, chef-state
under timeout -k 1 5, plain chef-storage shutdown, sync, btprobe), one pause,
the bootloader reached when chef-state, chef-storage or sync fail or hang,
init resumed when btprobe returns, on TERM/INT and on a refused pause,
SIGHUP ignored, the lock, stage 1 (no chef-state or chef-storage), usage,
a missing kmsg, the watchdog budget, and the callers (phone-boot.sh,
mkinitramfs.sh).
"""
import fcntl
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "initramfs/usr/bin/chef-reboot"

STUB = r'''#!/bin/sh
name=${0##*/}
case "$name $1" in "sleep 0.1") exec /bin/sleep "${TICK:-0.01}";; esac
echo "$name $*" >> "$TRACE"
hang() { echo $$ >> "$TRACE.pids"; exec /bin/sleep 30 </dev/null >/dev/null 2>&1; }
case "$name" in
chef-state) [ "${STATE:-ok}" != hang ] || hang; [ "${STATE:-ok}" = ok ] || exit 1;;
chef-storage) [ "${STORAGE:-ok}" != hang ] || hang; [ "${STORAGE:-ok}" = ok ] || exit 1;;
sync) [ "${SYNC:-ok}" != hang ] || hang;;
btprobe) [ "${BTPROBE:-ok}" = ok ] || exit 1; kill -KILL "$PPID";;
flock)
    case " $* " in *" -w"*|*" --timeout"*|*" -E"*) echo "flock: invalid option" >&2; exit 1;; esac
    exec "$REAL_FLOCK" "$@";;
timeout)
    while [ "$#" -gt 0 ]; do case "$1" in -k|-s) shift 2;; -*) shift;; *) break;; esac; done
    shift
    exec "$@";;
esac
exit 0
'''

# Busybox applets the helper uses, bound as functions; the chef tools and
# btprobe stay PATH lookups so `command -v` sees whether they exist.
APPLETS = ["sleep", "sync", "setsid", "timeout", "flock"]
CHEF = ["chef-state", "chef-storage"]


class ChefReboot(unittest.TestCase):
    shell = ["bash"]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        p = self.p = Path(self.tmp.name)
        for d in ["bin", "chef", "probe", "run"]:
            (p / d).mkdir()
        (p / "kmsg").write_text("")
        stub = p / "stub"
        stub.write_text(STUB)
        stub.chmod(0o755)
        for n in APPLETS:
            (p / "bin" / n).symlink_to(stub)
        for n in CHEF:
            (p / "chef" / n).symlink_to(stub)
        (p / "probe/btprobe").symlink_to(stub)
        wrapper = "".join(f'{n}() {{ "$TEST_BIN/{n}" "$@"; }}\n' for n in APPLETS)
        # Signals to PID 1 are only recorded; a plain `kill PID` (the
        # watchdog) is recorded with the watchdog's PID as $! saw it.
        wrapper += ('kill() { if [ "$#" = 2 ] && [ "$2" = 1 ]; then echo "kill $*" >> "$TRACE";'
                    ' return "${INIT_KILL_RC:-0}"; fi;'
                    ' [ "$#" != 1 ] || echo "kill $1 watchdog=${watchdog:-}" >> "$TRACE"; command kill "$@"; }\n')
        text = SCRIPT.read_text()
        # Short step bounds; the production values are checked separately.
        text = text.replace("\nSTATE_SECS=8\t", "\nSTATE_SECS=1\t", 1)
        text = text.replace("\nSTORAGE_SECS=100\t", "\nSTORAGE_SECS=1\t", 1)
        text = text.replace("\nSYNC_SECS=20\n", "\nSYNC_SECS=1\n", 1)
        self.script = p / "chef-reboot"
        self.script.write_text(wrapper + text)
        self.trace = p / "trace"
        self.env = {**os.environ, "TEST_BIN": str(p / "bin"), "TRACE": str(self.trace),
                    "REAL_FLOCK": "/usr/bin/flock",
                    "CHEF_REBOOT_RUN": str(p / "run"), "CHEF_REBOOT_KMSG": str(p / "kmsg")}
        self.addCleanup(self.kill_hung)

    def kill_hung(self):
        pids = self.p / "trace.pids"
        if pids.exists():
            for pid in pids.read_text().split():
                try:
                    os.kill(int(pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
            pids.unlink()

    def path(self, chef=True, probe=True):
        dirs = [str(self.p / "chef")] if chef else []
        dirs += [str(self.p / "probe")] if probe else []
        return ":".join(dirs + [os.environ["PATH"]])

    def popen(self, *args, chef=True, probe=True, **env):
        # Files, not pipes: a step abandoned after its bound or an abort may
        # keep the helper's stdio open after it exited.
        self.trace.write_text("")
        with open(self.p / "err", "w") as err:
            return subprocess.Popen([*self.shell, str(self.script), *args], text=True,
                                    stdout=subprocess.DEVNULL, stderr=err,
                                    env={**self.env, "PATH": self.path(chef, probe), **env})

    def finish(self, proc):
        proc.wait(timeout=30)
        return (self.p / "err").read_text()

    def call(self, *args, **kw):
        proc = self.popen(*args, **kw)
        err = self.finish(proc)
        return proc.returncode, err, self.lines()

    def lines(self):
        return [l for l in self.trace.read_text().splitlines()]

    def log(self):
        return (self.p / "run/chef-reboot.log").read_text()

    def index(self, trace, prefix):
        hits = [i for i, l in enumerate(trace) if l.startswith(prefix)]
        self.assertEqual(len(hits), 1, (prefix, trace))
        return hits[0]

    def assert_rebooted(self, rc, trace):
        self.assertEqual(rc, -signal.SIGKILL, trace)
        self.assertEqual(trace[-1], "btprobe restart bootloader", trace)
        self.assertNotIn("kill -CONT 1", trace)

    def test_order_and_one_pause(self):
        rc, err, t = self.call("bootloader")
        self.assert_rebooted(rc, t)
        # the watchdog is forked before the pause (its exec may land after it)
        self.index(t, "setsid sh -c sleep 150; kill -CONT 1")
        order = [self.index(t, s) for s in ["kill -TSTP 1",
                                            "timeout -k 1 5 chef-state shutdown", "chef-state shutdown",
                                            "chef-storage shutdown", "sync", "sleep 2", "btprobe restart bootloader"]]
        self.assertEqual(order, sorted(order), t)
        self.assertIn("chef-storage shutdown", t)
        self.assertFalse(any("--pause-init" in l for l in t), t)
        self.assertEqual(t.count("kill -TSTP 1"), 1)
        for line in ["init paused", "chef-state shutdown done", "chef-storage shutdown done", "btprobe restart bootloader"]:
            self.assertIn(line, self.log())
            self.assertIn(line, (self.p / "kmsg").read_text())

    def test_bootloader_reached_when_a_step_fails_or_hangs(self):
        cases = [({"STATE": "fail"}, "chef-state shutdown failed (exit 1)"),
                 ({"STATE": "hang"}, "chef-state shutdown still running after 1 s"),
                 ({"STORAGE": "fail"}, "chef-storage shutdown failed (exit 1); /data may still be writable"),
                 ({"STORAGE": "hang"}, "chef-storage shutdown still running after 1 s"),
                 ({"SYNC": "hang"}, "sync still running after 1 s"),
                 ({"STATE": "hang", "STORAGE": "hang", "SYNC": "hang"}, "sync did not finish")]
        for env, message in cases:
            with self.subTest(env=env):
                start = time.monotonic()
                rc, err, t = self.call("bootloader", **env)
                self.assert_rebooted(rc, t)
                self.assertLess(time.monotonic() - start, 15)
                self.assertIn(message, self.log())
                self.assertIn(message, (self.p / "kmsg").read_text())
                self.assertIn(message, err)
                self.assertLess(self.index(t, "chef-state shutdown"), self.index(t, "chef-storage shutdown"))
                self.assertEqual(t.count("kill -TSTP 1"), 1)

    def test_btprobe_returning_resumes_init(self):
        rc, err, t = self.call("bootloader", BTPROBE="fail")
        self.assertEqual(rc, 1, err)
        self.assertLess(self.index(t, "btprobe restart bootloader"), self.index(t, "kill -CONT 1"))
        self.assertIn("btprobe returned (exit 1)", self.log())
        self.assertIn("bootloader not reached; init resumed (exit 1)", self.log())
        self.assert_watchdog_killed(t)

    def assert_watchdog_killed(self, t):
        kills = [l.split() for l in t if l.startswith("kill ") and " watchdog=" in l]
        self.assertEqual(len(kills), 1, t)
        self.assertRegex(kills[0][1], r"^[0-9]+$")
        self.assertEqual(kills[0][2], "watchdog=" + kills[0][1])
        self.assertLess(t.index(" ".join(kills[0])), t.index("kill -CONT 1"))

    def test_refused_pause_still_reaches_bootloader(self):
        rc, err, t = self.call("bootloader", INIT_KILL_RC="1")
        self.assert_rebooted(rc, t)
        self.assertIn("could not pause init; going on", self.log())
        self.assertIn("chef-state shutdown", t)

    def test_term_and_int_resume_init(self):
        for sig in (signal.SIGTERM, signal.SIGINT):
            with self.subTest(sig=sig):
                proc = self.popen("bootloader", STATE="hang", TICK="0.1")
                self.wait_trace("chef-state shutdown")
                proc.send_signal(sig)
                err = self.finish(proc)
                t = self.lines()
                # the abandoned step (and, with ash's function wrappers in
                # this fixture, a copy of the lock fd) goes before the next run
                self.kill_hung()
                time.sleep(0.2)
                self.assertEqual(proc.returncode, 1, err)
                self.assertIn("kill -CONT 1", t)
                self.assertNotIn("btprobe restart bootloader", t)
                self.assertNotIn("chef-storage shutdown", t)
                self.assert_watchdog_killed(t)

    def test_hup_ignored(self):
        proc = self.popen("bootloader", STATE="hang", TICK="0.1")
        self.wait_trace("chef-state shutdown")
        proc.send_signal(signal.SIGHUP)
        self.finish(proc)
        self.assert_rebooted(proc.returncode, self.lines())

    def wait_trace(self, line):
        for _ in range(500):
            if self.trace.exists() and line in self.trace.read_text():
                return
            time.sleep(0.01)
        self.fail(f"{line!r} never ran")

    def test_lock_refuses_second_run(self):
        with open(self.p / "run/chef-reboot.lock", "w") as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            rc, err, t = self.call("bootloader")
        self.assertEqual(rc, 1, err)
        self.assertIn("another chef-reboot is running; nothing done", err)
        self.assertFalse(any(l.startswith(("kill", "chef-", "btprobe", "setsid")) for l in t), t)

    def test_lock_not_held_by_watchdog_or_steps(self):
        # The helper's lock fd (9) is closed for the watchdog and every step.
        text = SCRIPT.read_text()
        self.assertIn('"$@" </dev/null 9>&- &', text)
        self.assertIn('>/dev/null 2>&1 9>&- &\n\twatchdog=$!', text)
        self.assertIn("btprobe restart bootloader </dev/null 9>&-", text)

    def test_stage1_without_chef_tools(self):
        rc, err, t = self.call("bootloader", chef=False)
        self.assert_rebooted(rc, t)
        self.assertFalse(any(l.startswith(("kill", "setsid", "chef-", "timeout")) for l in t), t)
        self.assertIn("sync ", t)

    def test_no_btprobe_does_nothing(self):
        rc, err, t = self.call("bootloader", probe=False)
        self.assertEqual(rc, 1)
        self.assertEqual(t, [])
        self.assertIn("no btprobe; nothing done", err)

    def test_usage(self):
        for args in [(), ("recovery",), ("bootloader", "now")]:
            rc, err, t = self.call(*args)
            self.assertEqual(rc, 2, args)
            self.assertEqual(t, [])
            self.assertIn("usage: chef-reboot bootloader", err)

    def test_missing_kmsg(self):
        rc, err, t = self.call("bootloader", CHEF_REBOOT_KMSG=str(self.p / "absent"), STATE="fail")
        self.assert_rebooted(rc, t)
        self.assertFalse((self.p / "absent").exists())
        self.assertIn("chef-state shutdown failed", self.log())

    def test_closed_stderr(self):
        r = subprocess.run(["bash", "-c", 'exec 2>&-; exec "$@" bootloader', "sh", *self.shell, str(self.script)],
                           env={**self.env, "PATH": self.path()}, stdout=subprocess.DEVNULL)
        self.assertEqual(r.returncode, -signal.SIGKILL)
        self.assertIn("btprobe restart bootloader", self.log())

    def test_watchdog_budget(self):
        text = SCRIPT.read_text()
        v = {k: int(re.search(rf"\n{k}=(\d+)\b", text).group(1))
             for k in ["STATE_SECS", "STORAGE_SECS", "SYNC_SECS", "WATCHDOG_SECS"]}
        # sleep 1 after the pause, the step bounds, sleep 2 before btprobe
        self.assertGreater(v["WATCHDOG_SECS"], 1 + v["STATE_SECS"] + v["STORAGE_SECS"] + v["SYNC_SECS"] + 2 + 10)
        self.assertGreaterEqual(v["STATE_SECS"], 5 + 1 + 1)  # timeout -k 1 5
        self.assertIn("timeout -k 1 5 chef-state shutdown", text)
        inittab = (ROOT / "initramfs/etc/inittab").read_text()
        self.assertIn("::shutdown:/usr/bin/timeout -k 1 5 /usr/bin/chef-state shutdown", inittab)

    def test_callers(self):
        boot = (ROOT / "scripts/phone-boot.sh").read_text()
        self.assertIn("if command -v chef-reboot >/dev/null; then exec chef-reboot bootloader; fi;", boot)
        # the fallback for images without the helper stays after it
        self.assertLess(boot.index("exec chef-reboot bootloader"), boot.index("chef-storage shutdown --pause-init"))
        # phone-boot.sh waits for fastboot longer than the helper's bounded chain
        wait = int(re.search(r"\[ \$i -le (\d+) \] \|\| \{ echo \"no fastboot device", boot).group(1))
        text = SCRIPT.read_text()
        chain = 1 + sum(int(re.search(rf"\n{k}=(\d+)\b", text).group(1)) for k in ["STATE_SECS", "STORAGE_SECS", "SYNC_SECS"]) + 2
        self.assertGreater(wait, chain + 30)
        self.assertIn('"$ROOT"/usr/bin/chef-reboot', (ROOT / "scripts/mkinitramfs.sh").read_text())
        self.assertTrue(os.access(SCRIPT, os.X_OK))


class AshChefReboot(ChefReboot):
    shell = ["busybox", "ash"]


if __name__ == "__main__":
    unittest.main()
