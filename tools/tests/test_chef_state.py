#!/usr/bin/env python3
"""Host tests for initramfs/usr/bin/chef-state (bash and busybox ash).

Everything device-facing is a stub: mountpoint, mount, umount, chronyc,
rtc-edge, pidof and a busybox-like timeout (its watchdog daemonizes with
stdio on /dev/null and lingers about a second, recording its cwd so the
tests can prove nothing runs under timeout from inside /data). /data, /run,
the RTC's since_epoch, pstore, the sensors run directory and the registry
map are temporary directories. Host BusyBox ash may prefer its own applets
over PATH, so the stubs are also bound as shell functions, as in
test_storage.py. Covered: the `ours` gate for every subcommand, every
restore refusal of the RTC offset and the group 2980 file, the chronyc
source rules, the save skip rule and lock, pstore collection with
de-duplication and retention, boots.log rotation and torn lines, and the
shutdown order.
"""
import hashlib
import fcntl
import signal
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "initramfs/usr/bin/chef-state"
NS = 10**9
BOOT_ID = "58cf29ac-5b1a-4c4e-9d19-5742e1808633"
SYNCED = """Reference ID    : A29FC87B (162.159.200.123)
Stratum         : 4
System time     : 0.000000164 seconds slow of NTP time
Leap status     : Normal
"""

STUB = r'''#!/bin/sh
name=${0##*/}
echo "$name $*" >> "$TRACE"
case "$name" in
mountpoint)
    for a; do p=$a; done
    case "$p" in
    "$CHEF_STATE_DATA") exit "${MP_DATA:-0}";;
    "$CHEF_STATE_PSTORE") exit "${MP_PSTORE:-1}";;
    esac
    exit 1;;
mount) exit "${MOUNT_RC:-0}";;
umount) exit "${UMOUNT_RC:-0}";;
chronyc)
    [ -z "${CHRONYC_SLEEP:-}" ] || exec sleep "$CHRONYC_SLEEP"
    [ -z "${CHRONYD_PID:-}" ] || { kill -0 "$CHRONYD_PID" 2>/dev/null && echo "chronyd alive at chronyc" >> "$TRACE"; }
    cat "$CHRONYC_OUT" 2>/dev/null; exit "${CHRONYC_RC:-0}";;
rtc-edge)
    while [ "$#" -gt 0 ]; do case "$1" in -r) shift 2;; -n) shift;; *) break;; esac; done
    case "$1" in
    sample) [ "${EDGE_SAMPLE_RC:-0}" = 0 ] || exit "$EDGE_SAMPLE_RC"; cat "$EDGE_OUT";;
    set) exit "${EDGE_SET_RC:-0}";;
    floor) exit "${EDGE_FLOOR_RC:-0}";;
    esac;;
pidof) case "$1" in
    chronyd) echo "${CHRONYD_PID:-}";;
    powerd) echo "${POWERD_PID:-}";;
    sensors-up) echo "${SENSORS_PID:-}";;
    esac
    exit 0;;
dd)
    # FAIL_DD on writes: 1 fails outright, partial writes 3 bytes then fails
    case "$*" in *of=*) [ "${FAIL_DD:-0}" = 0 ] || { [ "$FAIL_DD" = partial ] && "$REAL_DD" "$@" count=3; exit 1; };; esac
    exec "$REAL_DD" "$@";;
timeout)
    # busybox: the watchdog is a daemon (stdio /dev/null) that outlives the
    # command by up to a second, in the caller's cwd.
    echo "timeout-cwd $PWD" >> "$TRACE"
    ( setsid sh -c 'sleep 1' </dev/null >/dev/null 2>&1 & )
    while [ "$#" -gt 0 ]; do case "$1" in -k|-s) shift 2;; -*) shift;; *) break;; esac; done
    shift
    "$@";;
esac
'''


def seal(body):
    return body + f"sha256 {hashlib.sha256(body.encode()).hexdigest()}\n"


def time_body(offset, since, wall, boot=BOOT_ID):
    return f"chef-rtc-offset 1\noffset_ns {offset}\nsince_epoch {since}\nwall_s {wall}\nboot_id {boot}\n"


class ChefState(unittest.TestCase):
    shell = ["bash"]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        p = self.p = Path(self.tmp.name)
        for d in ["data/v1", "run", "rtc", "pstore", "sensors", "proc", "bin"]:
            (p / d).mkdir(parents=True)
        (p / "data/chef-layout").write_text("chef-cyclo-data-v1\n")
        (p / "kmsg").write_text("")
        (p / "cmdline").write_text("androidboot.slot_suffix=_a androidboot.bootreason=power_key_press quiet\n")
        (p / "boot_id").write_text(BOOT_ID + "\n")
        (p / "rtc/since_epoch").write_text("1700000\n")
        # Registry map: group 2980 = 20 bytes at 16, bias items at +6/+10/+14.
        (p / "map").write_text("size 64\ngroup 2980 16 20\nitem 3900 16 4\nitem 3903 22 4\n"
                               "item 3904 26 4\nitem 3905 30 4\nitem 3906 34 2\n")
        self.pristine = bytes(range(64))
        (p / "sensors/sns.reg.persist").write_bytes(self.pristine)
        (p / "sensors/sns.reg").write_bytes(self.pristine)
        stub = p / "bin/stub"
        stub.write_text(STUB)
        stub.chmod(0o755)
        names = ["mountpoint", "mount", "umount", "chronyc", "rtc-edge", "pidof", "timeout", "dd"]
        for n in names:
            (p / "bin" / n).symlink_to(stub)
        wrapper = "".join(f'{n}() {{ "$TEST_BIN/{n}" "$@"; }}\n' for n in names)
        self.script = p / "chef-state"
        self.script.write_text(SCRIPT.read_text().replace("\nDATA=", "\n" + wrapper + "DATA=", 1))
        self.data = p / "data"
        self.env = {**os.environ, "PATH": f"{p / 'bin'}:{os.environ['PATH']}", "TEST_BIN": str(p / "bin"),
                    "TRACE": str(p / "trace"),
                    "CHEF_STATE_DATA": str(self.data), "CHEF_STATE_RUN": str(p / "run"),
                    "CHEF_STATE_KMSG": str(p / "kmsg"), "CHEF_STATE_CMDLINE": str(p / "cmdline"),
                    "CHEF_STATE_BOOT_ID": str(p / "boot_id"), "CHEF_STATE_RTC": str(p / "rtc"),
                    "CHEF_STATE_PSTORE": str(p / "pstore"), "CHEF_STATE_SENSORS_RUN": str(p / "sensors"),
                    "CHEF_STATE_MAP": str(p / "map"), "CHEF_STATE_PROC": str(p / "proc"),
                    "CHRONYC_OUT": str(p / "chronyc.out"), "EDGE_OUT": str(p / "edge.out"),
                    "REAL_DD": subprocess.run(["sh", "-c", "command -v dd"], capture_output=True, text=True).stdout.strip()}
        (p / "proc/uptime").write_text("31.03 100.00\n")
        (p / "chronyc.out").write_text(SYNCED)

    def call(self, *args, timeout=20, **env):
        (self.p / "trace").write_text("")
        (self.p / "kmsg").write_text("")
        r = subprocess.run([*self.shell, str(self.script), *args], env={**self.env, **env}, text=True,
                           capture_output=True, timeout=timeout)
        self.trace = (self.p / "trace").read_text()
        self.kmsg = (self.p / "kmsg").read_text()
        for line in self.trace.splitlines():
            if line.startswith("timeout-cwd "):
                self.assertFalse(line.split(" ", 1)[1].startswith(str(self.data)), line)
        return r

    def chef_time(self):
        return dict(l.split("=", 1) for l in (self.p / "run/chef-time").read_text().split())

    def edge(self, since, wall_ns):
        (self.p / "edge.out").write_text(f"{since} {wall_ns}\n")

    # ------------------------------------------------------------ gate

    def test_ours_gate(self):
        r = self.call("ours")
        self.assertEqual(r.returncode, 0)
        self.assertEqual((r.stderr, self.kmsg), ("", ""))
        r = self.call("ours", MP_DATA="1")
        self.assertEqual(r.returncode, 1)
        self.assertEqual((r.stderr, self.kmsg), ("", ""))
        (self.data / "chef-layout").write_text("foreign\n")
        self.assertEqual(self.call("ours").returncode, 1)
        (self.data / "chef-layout").unlink()
        self.assertEqual(self.call("ours").returncode, 1)

    def ram_only_cases(self):
        yield {"MP_DATA": "1"}
        (self.data / "chef-layout").write_text("foreign\n")
        yield {}
        (self.data / "chef-layout").unlink()
        yield {}

    def test_ram_only_everything(self):
        (self.p / "pstore/dmesg-ramoops-0").write_text("oops\n")
        (self.p / "sensors/sns.reg").write_bytes(self.reg_with_bias(65536, -32768, 1000))
        target = self.p / "target"
        for env in self.ram_only_cases():
            before = sorted(str(x) for x in self.data.rglob("*"))
            r = self.call("boot", **env)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(self.chef_time(), {"source": "none", "reason": "ram-only", "saved_age_s": "na"})
            self.assertNotIn("mount -t pstore", self.trace)
            self.assertIn("not ours", self.kmsg)
            self.edge(1700001, 1_791_000_000 * NS)
            self.assertEqual(self.call("time-save", **env).returncode, 1)
            self.assertNotIn("chronyc", self.trace)
            self.assertEqual(self.call("mag-save", **env).returncode, 0)
            target.write_bytes(self.pristine)
            self.assertEqual(self.call("mag-restore", str(target), **env).returncode, 0)
            self.assertEqual(target.read_bytes(), self.pristine)
            r = self.call("shutdown", **env)
            self.assertEqual(r.returncode, 0)
            self.assertNotIn("pidof", self.trace)
            self.assertEqual(self.kmsg, "")
            self.assertEqual(sorted(str(x) for x in self.data.rglob("*")), before)

    # ------------------------------------------------------------ clock

    def write_time(self, body):
        d = self.data / "v1/time"
        d.mkdir(exist_ok=True)
        (d / "rtc-offset").write_text(body)

    def test_restore_rtc_offset(self):
        offset = (1_791_000_000 - 1_600_000) * NS + 250_000_000
        self.write_time(seal(time_body(offset, 1_600_000, 1_791_000_000)))
        r = self.call("boot")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f"rtc-edge -r {self.p / 'rtc'} set {offset}", self.trace)
        self.assertEqual(self.chef_time(), {"source": "rtc-offset", "reason": "ok", "saved_age_s": "100000"})
        self.assertIn("clock set from the saved RTC offset", self.kmsg)

    def test_restore_missing(self):
        self.call("boot")
        self.assertEqual(self.chef_time()["reason"], "missing")
        self.assertNotIn("rtc-edge", self.trace)

    def test_restore_refusals(self):
        offset = (1_791_000_000 - 1_600_000) * NS
        good = time_body(offset, 1_600_000, 1_791_000_000)
        bad = {
            "checksum": good + "sha256 " + "0" * 64 + "\n",
            "no checksum": good,
            "version": seal(good.replace("chef-rtc-offset 1", "chef-rtc-offset 2")),
            "extra line": seal(good + "extra 1\n"),
            "missing line": seal("\n".join(good.splitlines()[:-1]) + "\n"),
            "order": seal(good.replace("offset_ns", "x_ns")),
            "non-numeric": seal(good.replace("since_epoch 1600000", "since_epoch 16e5")),
            "since zero": seal(time_body(offset, 0, 1_791_000_000)),
            "wall before 2026": seal(time_body(1_700_000_000 * NS, 1_600_000, 1_701_600_000)),
            "wall 2100": seal(time_body(4_102_444_800 * NS, 1, 4_102_444_801)),
            "disagree": seal(time_body(offset, 1_600_000, 1_791_000_010)),
            "no final newline": seal(good)[:-1],
            "nul": seal(good).replace("\n", "\0", 1),
            "leading zero": seal(time_body(offset, "01600000", 1_791_000_000)),
            "since overflow": seal(time_body(offset, "9223372036854775807", 1_791_000_000)),
            "offset overflow": seal(time_body("9999999999999999999", 1600000, 1_791_000_000)),
            "huge": seal(good + "pad " + "x" * 5000 + "\n"),
        }
        for why, content in bad.items():
            with self.subTest(why):
                self.write_time(content)
                r = self.call("boot")
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(self.chef_time(), {"source": "none", "reason": "invalid", "saved_age_s": "na"})
                self.assertIn("ignoring", self.kmsg)
                self.assertNotIn("rtc-edge", self.trace)

    def test_restore_battery_disconnect_floor(self):
        offset = (1_791_000_000 - 1_800_000) * NS
        self.write_time(seal(time_body(offset, 1_800_000, 1_791_000_000)))  # RTC now 1700000: reset
        self.call("boot")
        self.assertNotIn(" set ", self.trace)
        self.assertIn("rtc-edge floor 1791000000", self.trace)
        self.assertEqual(self.chef_time(), {"source": "floor", "reason": "rtc-reset", "saved_age_s": "na"})
        self.call("boot", EDGE_FLOOR_RC="4")
        self.assertEqual(self.chef_time()["source"], "none")
        self.assertEqual(self.chef_time()["reason"], "rtc-reset-clock-ahead")
        self.call("boot", EDGE_FLOOR_RC="2")
        self.assertEqual(self.chef_time()["reason"], "rtc-reset-set-failed")

    def test_restore_span_and_range(self):
        # saved 6 years of RTC seconds ago
        (self.p / "rtc/since_epoch").write_text("200000000\n")
        since = 200_000_000 - 189_216_000
        offset = (1_791_000_000 - since) * NS
        self.write_time(seal(time_body(offset, since, 1_791_000_000)))
        self.call("boot")
        self.assertNotIn(" set ", self.trace)
        self.assertEqual(self.chef_time(), {"source": "floor", "reason": "span", "saved_age_s": "na"})
        # target past 2100
        (self.p / "rtc/since_epoch").write_text("1700000\n")
        wall = 4_102_444_800 - 1000
        offset = (wall - 1_690_000) * NS
        self.write_time(seal(time_body(offset, 1_690_000, wall)))
        self.call("boot")
        self.assertNotIn(" set ", self.trace)
        self.assertEqual(self.chef_time()["reason"], "range")

    def test_restore_no_tick_falls_back_to_floor(self):
        offset = (1_791_000_000 - 1_600_000) * NS
        self.write_time(seal(time_body(offset, 1_600_000, 1_791_000_000)))
        self.call("boot", EDGE_SET_RC="3")
        self.assertIn(" set ", self.trace)
        self.assertEqual(self.chef_time(), {"source": "floor", "reason": "no-tick", "saved_age_s": "na"})

    def saved_time(self):
        return (self.data / "v1/time/rtc-offset").read_text()

    def test_save_and_reload(self):
        wall = 1_791_000_123 * NS + 456_000_000
        self.edge(1_700_001, wall)
        r = self.call("time-save")
        self.assertEqual(r.returncode, 0, r.stderr)
        offset = wall - 1_700_001 * NS
        self.assertEqual(self.saved_time(), seal(time_body(offset, 1_700_001, 1_791_000_123)))
        self.assertIn("saved RTC offset", self.kmsg)
        self.assertEqual(list((self.data / "v1/time").iterdir()), [self.data / "v1/time/rtc-offset"])
        # what it saved restores
        (self.p / "rtc/since_epoch").write_text("1700001\n")
        self.call("boot")
        self.assertIn(f" set {offset}", self.trace)

    def test_save_skip_rule(self):
        wall = 1_791_000_000 * NS
        self.edge(1_700_000, wall)
        self.call("time-save")
        ino = (self.data / "v1/time/rtc-offset").stat().st_ino
        # 50 ms later in offset, 1 h later: skipped
        self.edge(1_703_600, wall + 3600 * NS + 50_000_000)
        self.assertEqual(self.call("time-save").returncode, 0)
        self.assertEqual((self.data / "v1/time/rtc-offset").stat().st_ino, ino)
        self.assertEqual(self.kmsg, "")
        # offset moved 150 ms: rewritten
        self.edge(1_703_600, wall + 3600 * NS + 150_000_000)
        self.call("time-save")
        self.assertIn("since_epoch 1703600", self.saved_time())
        # same offset, 6 h later: rewritten
        self.edge(1_703_600 + 21_600, wall + (3600 + 21_600) * NS + 150_000_000)
        self.call("time-save")
        self.assertIn(f"since_epoch {1_703_600 + 21_600}", self.saved_time())

    def test_save_needs_real_source(self):
        self.edge(1_700_000, 1_791_000_000 * NS)
        cases = {
            "unsynchronised": SYNCED.replace("Normal", "Not synchronised"),
            "no reference": SYNCED.replace("A29FC87B (162.159.200.123)", "00000000 ()"),
            "local": SYNCED.replace("A29FC87B (162.159.200.123)", "7F7F0101 ()"),
            "LOCL": SYNCED.replace("A29FC87B (162.159.200.123)", "4C4F434C (LOCL)"),
            "empty": "",
        }
        for why, out in cases.items():
            with self.subTest(why):
                (self.p / "chronyc.out").write_text(out)
                self.assertEqual(self.call("time-save").returncode, 1)
                self.assertFalse((self.data / "v1/time/rtc-offset").exists())
                self.assertNotIn("rtc-edge", self.trace)
        (self.p / "chronyc.out").write_text(SYNCED)
        self.assertEqual(self.call("time-save", CHRONYC_RC="1").returncode, 1)
        self.assertFalse((self.data / "v1/time/rtc-offset").exists())
        (self.p / "chronyc.out").write_text(SYNCED.replace("A29FC87B (162.159.200.123)", "47505300 (GPS)"))
        self.assertEqual(self.call("time-save").returncode, 0)
        self.assertTrue((self.data / "v1/time/rtc-offset").exists())

    def test_save_refusals(self):
        for out in ["", "1 2 3", "x 1791000000000000000", "1700000 abc", "0 1791000000000000000",
                    "1700000 1600000000000000000"]:
            with self.subTest(out):
                (self.p / "edge.out").write_text(out + "\n")
                self.assertEqual(self.call("time-save").returncode, 1)
                self.assertFalse((self.data / "v1/time/rtc-offset").exists())
        self.assertEqual(self.call("time-save", EDGE_SAMPLE_RC="3").returncode, 1)
        self.assertIn("no RTC tick", self.kmsg)

    def test_concurrent_saves_one_valid_file(self):
        stale = self.data / "v1/time"
        stale.mkdir()
        (stale / "rtc-offset.tmp.99999").write_text("stale")
        procs = []
        for i in range(6):
            (self.p / f"edge{i}").write_text(f"{1_700_000 + i * 30_000} {1_791_000_000 * NS + i * 30_000 * NS + i * 200_000_000}\n")
            procs.append(subprocess.Popen([*self.shell, str(self.script), "time-save"],
                                          env={**self.env, "EDGE_OUT": str(self.p / f"edge{i}")},
                                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        for pr in procs:
            self.assertEqual(pr.wait(timeout=30), 0)
        self.assertEqual(sorted(x.name for x in stale.iterdir()), ["rtc-offset"])
        body = self.saved_time()
        self.assertEqual(seal(body[:body.rindex("sha256 ")]), body)

    def test_restore_write_failure_still_writes_chef_time_reason(self):
        self.write_time(seal(time_body(1, 1, 1)))
        self.call("boot")
        self.assertEqual(self.chef_time()["reason"], "invalid")

    # ------------------------------------------------------------ crash

    def boots(self):
        return (self.data / "v1/crash/boots.log").read_text().splitlines()

    def test_crash_collect_and_dedupe(self):
        (self.p / "pstore/dmesg-ramoops-0").write_text("Kernel panic\n")
        (self.p / "pstore/console-ramoops-0").write_text("previous log\n")
        (self.p / "pstore/annotate-ramoops-0").write_text("")  # empty on every boot
        r = self.call("boot")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f"mount -t pstore -o ro pstore {self.p / 'pstore'}", self.trace)
        self.assertIn(f"umount {self.p / 'pstore'}", self.trace)
        d = self.data / "v1/crash/1-power_key_press"
        self.assertEqual((d / "dmesg-ramoops-0").read_text(), "Kernel panic\n")
        self.assertFalse((d / "annotate-ramoops-0").exists())
        self.assertEqual(d.stat().st_mode & 0o777, 0o700)
        info = (d / "info").read_text()
        self.assertIn(f"boot_id {BOOT_ID}\nprevious_boot_id none\nbootreason power_key_press\nsince_epoch 1700000\nwall na\n", info)
        h = hashlib.sha256(b"Kernel panic\n").hexdigest()
        self.assertIn(f"file dmesg-ramoops-0 13 {h}\n", info)
        self.assertIn("crash: saved previous boot's records to", self.kmsg)
        self.assertEqual(self.boots(), [f"seq=1 boot_id={BOOT_ID} bootreason=power_key_press since_epoch=1700000 wall=na pstore=2"])
        # still there next boot (kernel_panic): nothing copied twice
        (self.p / "cmdline").write_text("androidboot.bootreason=kernel_panic\n")
        self.call("boot")
        self.assertEqual(sorted(x.name for x in (self.data / "v1/crash").iterdir()),
                         ["1-power_key_press", "boots.log"])
        self.assertNotIn("saved previous", self.kmsg)
        self.assertTrue(self.boots()[1].startswith("seq=2 ") and self.boots()[1].endswith(" pstore=2"))
        # only empty records: nothing saved, pstore=0
        for f in ("dmesg-ramoops-0", "console-ramoops-0"):
            (self.p / "pstore" / f).unlink()
        self.call("boot")
        self.assertTrue(self.boots()[2].endswith(" pstore=0"))
        (self.p / "pstore/console-ramoops-0").write_text("previous log\n")
        # one new record: only it is copied
        (self.p / "pstore/console-ramoops-0").write_text("another log\n")
        self.call("boot")
        d3 = self.data / "v1/crash/4-kernel_panic"
        self.assertEqual(sorted(x.name for x in d3.iterdir()), ["console-ramoops-0", "info"])
        self.assertFalse(list((self.data / "v1/crash").glob(".tmp-*")))

    def test_crash_dedupe_rechecks_saved_bytes(self):
        record = self.p / "pstore/dmesg-ramoops-0"
        record.write_text("panic\n")
        self.call("boot")
        saved = self.data / "v1/crash/1-power_key_press/dmesg-ramoops-0"
        saved.unlink()
        self.call("boot")
        saved = self.data / "v1/crash/2-power_key_press/dmesg-ramoops-0"
        self.assertEqual(saved.read_text(), "panic\n")
        saved.write_text("torn")
        self.call("boot")
        self.assertEqual((self.data / "v1/crash/3-power_key_press/dmesg-ramoops-0").read_text(), "panic\n")
        (self.data / "v1/crash/08-x").mkdir()
        self.assertEqual(self.call("boot").returncode, 0)

    def test_crash_wall_after_restore(self):
        offset = (1_791_000_000 - 1_600_000) * NS
        self.write_time(seal(time_body(offset, 1_600_000, 1_791_000_000)))
        self.call("boot")
        self.assertRegex(self.boots()[0], r" wall=\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ pstore=0$")

    def test_crash_pstore_mount_failure(self):
        r = self.call("boot", MOUNT_RC="1")
        self.assertEqual(r.returncode, 0)
        self.assertIn("cannot mount pstore", self.kmsg)
        self.assertNotIn("umount", self.trace)
        self.assertTrue(self.boots()[0].endswith(" pstore=0"))

    def test_crash_pstore_already_mounted(self):
        (self.p / "pstore/dmesg-ramoops-0").write_text("x\n")
        self.call("boot", MP_PSTORE="0")
        self.assertNotIn("mount -t", self.trace)
        self.assertNotIn("umount", self.trace)
        self.assertTrue((self.data / "v1/crash/1-power_key_press/dmesg-ramoops-0").exists())

    def test_crash_retention(self):
        c = self.data / "v1/crash"
        c.mkdir()
        for i in range(1, 41):
            (c / f"{i}-x").mkdir()
            (c / f"{i}-x/info").write_text(f"seq {i}\n")
        (self.p / "pstore/dmesg-ramoops-0").write_text("new\n")
        self.call("boot")
        dirs = sorted(int(x.name.split("-")[0]) for x in c.iterdir() if x.is_dir())
        self.assertEqual(len(dirs), 32)
        self.assertEqual(dirs[-1], 41)
        self.assertEqual(dirs[0], 10)
        # size: 3 dirs of 7 MiB -> the oldest go until under 16 MiB
        (self.p / "pstore/dmesg-ramoops-0").unlink()
        for x in c.iterdir():
            if x.is_dir():
                for f in x.iterdir():
                    f.unlink()
                x.rmdir()
        for i in (1, 2, 3):
            (c / f"{i}-x").mkdir()
            (c / f"{i}-x/big").write_bytes(os.urandom(7 * 1024 * 1024))
        self.call("boot")
        self.assertEqual(sorted(x.name for x in c.iterdir() if x.is_dir()), ["2-x", "3-x"])
        # even the newest record is removed if it exceeds the cap
        for x in ("2-x", "3-x"):
            for f in (c / x).iterdir():
                f.unlink()
            (c / x).rmdir()
        (c / "9-x").mkdir()
        (c / "9-x/big").write_bytes(os.urandom(17 * 1024 * 1024))
        self.call("boot")
        self.assertFalse((c / "9-x").exists())

    def line(self, seq, boot=BOOT_ID):
        return f"seq={seq} boot_id={boot} bootreason=reboot since_epoch=5 wall=na pstore=0"

    def test_boots_log_rotation(self):
        c = self.data / "v1/crash"
        c.mkdir()
        lines = [self.line(i) for i in range(1, 4000)]
        text = "\n".join(lines) + "\n"
        self.assertGreater(len(text), 262144)
        (c / "boots.log").write_text(text)
        self.call("boot")
        self.assertEqual((c / "boots.log.1").read_text(), text)
        self.assertEqual(len(self.boots()), 1)
        self.assertTrue(self.boots()[0].startswith("seq=4000 "))
        # only .1 present
        (c / "boots.log").unlink()
        self.call("boot")
        self.assertTrue(self.boots()[0].startswith("seq=4000 "))

    def test_boots_log_torn_lines(self):
        c = self.data / "v1/crash"
        c.mkdir()
        cases = {
            "torn without newline": self.line(1) + "\n" + self.line(2) + "\n" + self.line(3)[:30],
            "nul tail": self.line(1) + "\n" + self.line(2) + "\n" + "\0" * 40,
            "nul inside": self.line(1) + "\n" + self.line(2) + "\n" + self.line(9).replace("=reboot", "=reb\0ot") + "\n",
            "extra field": self.line(1) + "\n" + self.line(2) + "\n" + self.line(9) + " x=1\n",
            "bad uuid": self.line(1) + "\n" + self.line(2) + "\n" + self.line(9, "58cf29ac") + "\n",
        }
        for why, text in cases.items():
            with self.subTest(why):
                (c / "boots.log").write_text(text)
                self.call("boot")
                last = (c / "boots.log").read_text().split("\n")[-2]
                self.assertTrue(last.startswith("seq=3 "), (why, last))
                self.assertTrue((c / "boots.log").read_text().endswith(" pstore=0\n"))
        (c / "boots.log").write_text("")
        self.call("boot")
        self.assertEqual(len(self.boots()), 1)
        self.assertTrue(self.boots()[0].startswith("seq=1 "))
        # previous boot id comes from the last good line
        (self.p / "pstore/dmesg-ramoops-0").write_text("y\n")
        other = "11111111-2222-3333-4444-555555555555"
        (c / "boots.log").write_text(self.line(7, other) + "\n")
        self.call("boot")
        self.assertIn(f"previous_boot_id {other}\n", (c / "8-power_key_press/info").read_text())

    # ------------------------------------------------------------ magnetometer

    def reg_with_bias(self, x, y, z, base=None):
        b = bytearray(base or self.pristine)
        for off, v in zip((22, 26, 30), (x, y, z)):
            b[off:off + 4] = (v & 0xFFFFFFFF).to_bytes(4, "little")
        return bytes(b)

    def mag_file(self):
        return self.data / "v1/sensors/mag-group-2980"

    def mag_body(self, group_bytes, psha=None, msha=None, length=20):
        psha = psha or hashlib.sha256(self.pristine).hexdigest()
        msha = msha or hashlib.sha256((self.p / "map").read_bytes()).hexdigest()
        return (f"chef-mag-group 1\ngroup 2980\nlength {length}\npersist_sha256 {psha}\n"
                f"map_sha256 {msha}\nbytes {group_bytes.hex()}\n")

    def test_mag_save_restore(self):
        reg = self.reg_with_bias(65536, -32768, 1000)
        (self.p / "sensors/sns.reg").write_bytes(reg)
        r = self.call("mag-save")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.mag_file().read_text(), seal(self.mag_body(reg[16:36])))
        self.assertIn("saved group 2980 bias 1.0000 -0.5000 0.0153 gauss", self.kmsg)
        # same bytes: no rewrite
        ino = self.mag_file().stat().st_ino
        self.call("mag-save")
        self.assertEqual(self.mag_file().stat().st_ino, ino)
        self.assertEqual(self.kmsg, "")
        # restore onto a fresh persist copy: only group 2980 changes
        target = self.p / "target"
        target.write_bytes(self.pristine)
        r = self.call("mag-restore", str(target))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(target.read_bytes(), reg)
        self.assertIn("seeded group 2980 bias 1.0000 -0.5000 0.0153 gauss", self.kmsg)

    def test_mag_write_rate_and_final_save(self):
        reg = self.p / "sensors/sns.reg"
        reg.write_bytes(self.reg_with_bias(100, 0, 0))
        self.call("mag-save")
        first = self.mag_file().read_bytes()
        reg.write_bytes(self.reg_with_bias(200, 0, 0))
        self.assertEqual(self.call("mag-save").returncode, 3)
        self.assertEqual(self.mag_file().read_bytes(), first)
        (self.p / "proc/uptime").write_text("91.03 0\n")
        self.call("mag-save")
        self.assertNotEqual(self.mag_file().read_bytes(), first)
        reg.write_bytes(self.reg_with_bias(300, 0, 0))
        self.call("mag-save", CHEF_STATE_MAG_FINAL="1")
        self.assertIn(self.reg_with_bias(300, 0, 0)[16:36].hex(), self.mag_file().read_text())

    def test_mag_restore_overlay_failure_leaves_target(self):
        reg = self.reg_with_bias(65536, -32768, 1000)
        (self.p / "sensors/sns.reg").write_bytes(reg)
        self.call("mag-save")
        target = self.p / "target"
        for mode in ("1", "partial"):
            with self.subTest(mode):
                target.write_bytes(self.pristine)
                r = self.call("mag-restore", str(target), FAIL_DD=mode)
                self.assertEqual(r.returncode, 1)
                self.assertEqual(target.read_bytes(), self.pristine)
                self.assertIn("overlay onto", self.kmsg)
                self.assertEqual(sorted(x.name for x in self.p.glob("target*")), ["target"])
                self.assertEqual(self.trace.count("dd of="), 1)

    def test_mag_zero_never_replaces(self):
        (self.p / "sensors/sns.reg").write_bytes(self.reg_with_bias(0, 0, 0))
        self.call("mag-save")
        self.assertFalse(self.mag_file().exists())
        reg = self.reg_with_bias(100, 200, 300)
        (self.p / "sensors/sns.reg").write_bytes(reg)
        self.call("mag-save")
        saved = self.mag_file().read_text()
        (self.p / "sensors/sns.reg").write_bytes(self.reg_with_bias(0, 0, 0, base=reg))
        self.call("mag-save")
        self.assertEqual(self.mag_file().read_text(), saved)

    def test_mag_implausible_not_saved(self):
        (self.p / "sensors/sns.reg").write_bytes(self.reg_with_bias(131073, 0, 0))
        self.call("mag-save")
        self.assertFalse(self.mag_file().exists())
        self.assertIn("implausible", self.kmsg)
        (self.p / "sensors/sns.reg").write_bytes(self.reg_with_bias(-131072, 131072, 0))
        self.call("mag-save")
        self.assertTrue(self.mag_file().exists())

    def test_mag_save_uses_pristine_hash(self):
        reg = self.reg_with_bias(100, 0, 0)
        (self.p / "sensors/sns.reg").write_bytes(reg)
        self.call("mag-save")
        self.assertIn(f"persist_sha256 {hashlib.sha256(self.pristine).hexdigest()}", self.mag_file().read_text())

    def test_mag_save_fail_closed(self):
        (self.p / "sensors/sns.reg").write_bytes(self.reg_with_bias(100, 0, 0))
        maps = {
            "no group": "size 64\nitem 3903 22 4\nitem 3904 26 4\nitem 3905 30 4\n",
            "no item": "size 64\ngroup 2980 16 20\nitem 3903 22 4\nitem 3904 26 4\n",
            "item outside": "size 64\ngroup 2980 16 20\nitem 3903 22 4\nitem 3904 26 4\nitem 3905 40 4\n",
            "group past size": "size 30\ngroup 2980 16 20\nitem 3903 22 4\nitem 3904 26 4\nitem 3905 30 4\n",
            "missing": None,
        }
        for why, m in maps.items():
            with self.subTest(why):
                if m is None:
                    (self.p / "map").unlink()
                else:
                    (self.p / "map").write_text(m)
                self.assertEqual(self.call("mag-save").returncode, 1)
                self.assertFalse(self.mag_file().exists())
                target = self.p / "target"
                target.write_bytes(self.pristine)
                self.assertEqual(self.call("mag-restore", str(target)).returncode, 0)
                self.assertEqual(target.read_bytes(), self.pristine)
        # registry of the wrong size
        (self.p / "map").write_text("size 64\ngroup 2980 16 20\nitem 3903 22 4\nitem 3904 26 4\nitem 3905 30 4\n")
        (self.p / "sensors/sns.reg").write_bytes(self.reg_with_bias(100, 0, 0)[:60])
        self.assertEqual(self.call("mag-save").returncode, 1)
        self.assertFalse(self.mag_file().exists())

    def test_mag_restore_refusals(self):
        group = self.reg_with_bias(65536, 0, 0)[16:36]
        good = self.mag_body(group)
        bad = {
            "checksum": good + "sha256 " + "0" * 64 + "\n",
            "version": seal(good.replace("chef-mag-group 1", "chef-mag-group 2")),
            "group": seal(good.replace("group 2980", "group 2981")),
            "length": seal(self.mag_body(group + b"\0\0", length=22)),
            "short bytes": seal(self.mag_body(group[:-1])),
            "bad hex": seal(good.replace(group.hex(), "zz" + group.hex()[2:])),
            "persist hash": seal(self.mag_body(group, psha="1" * 64)),
            "map hash": seal(self.mag_body(group, msha="2" * 64)),
            "zero bias": seal(self.mag_body(bytes(20))),
            "bias > 2 G": seal(self.mag_body(self.reg_with_bias(0, 0, -131073)[16:36])),
        }
        target = self.p / "target"
        for why, content in bad.items():
            with self.subTest(why):
                self.mag_file().parent.mkdir(exist_ok=True)
                self.mag_file().write_text(content)
                target.write_bytes(self.pristine)
                r = self.call("mag-restore", str(target))
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertEqual(target.read_bytes(), self.pristine)
                self.assertNotIn("seeded", self.kmsg)
                self.assertTrue(self.kmsg)
        # a target of the wrong size
        self.mag_file().write_text(seal(good))
        target.write_bytes(self.pristine[:40])
        self.call("mag-restore", str(target))
        self.assertEqual(target.read_bytes(), self.pristine[:40])
        target.write_bytes(self.pristine)
        self.call("mag-restore", str(target))
        self.assertEqual(target.read_bytes()[16:36], group)

    # ------------------------------------------------------------ shutdown

    def spawn(self):
        p = subprocess.Popen(["sleep", "30"])
        self.addCleanup(lambda: (p.kill(), p.wait()))
        return p

    def test_shutdown_order(self):
        keeper, chronyd, sensors = self.spawn(), self.spawn(), self.spawn()
        (self.p / "run/chef-state-timekeeper.pid").write_text(f"{keeper.pid}\n")
        (self.p / f"proc/{keeper.pid}").mkdir()
        (self.p / f"proc/{keeper.pid}/cmdline").write_bytes(b"/bin/sh\0/usr/bin/chef-state\0timekeeper\0")
        (self.p / "sensors/sns.reg").write_bytes(self.reg_with_bias(100, 0, 0))
        self.edge(1_700_000, 1_791_000_000 * NS)
        start = time.monotonic()
        r = self.call("shutdown", CHRONYD_PID=str(chronyd.pid), SENSORS_PID=str(sensors.pid))
        took = time.monotonic() - start
        self.assertEqual(r.returncode, 0, r.stderr)
        for pr in (keeper, chronyd, sensors):
            self.assertEqual(pr.wait(timeout=5), -15)
        t = self.trace
        self.assertIn("timeout -k 1 1 chronyc", t)
        self.assertIn("chronyd alive at chronyc", t)
        self.assertLess(t.index("chronyc"), t.index("pidof chronyd"))
        self.assertTrue((self.data / "v1/time/rtc-offset").exists())
        self.assertTrue(self.mag_file().exists())
        self.assertLess(took, 3.5)
        self.assertRegex(self.kmsg, r"shutdown: time_save_rc=0 .*mag_save_rc=0 sync_rc=0 stop_rc=1 \(uptime [0-9.]+ -> [0-9.]+\)")

    def test_shutdown_real_chronyc_timeout_reports_skipped(self):
        # Use the actual BusyBox timeout, not the fast stub. The fixture
        # chronyc execs sleep so there is no untracked grandchild.
        text = self.script.read_text().replace(
            'timeout() { "$TEST_BIN/timeout" "$@"; }',
            'timeout() { busybox timeout "$@"; }')
        self.script.write_text(text)
        start = time.monotonic()
        r = self.call("shutdown", CHRONYC_SLEEP="10")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertGreater(time.monotonic() - start, 0.9)
        self.assertLess(time.monotonic() - start, 3.5)
        self.assertIn("time_save_rc=1", self.kmsg)
        self.assertIn("stop_rc=0", self.kmsg)
        self.assertNotIn("state saved", self.kmsg)

    def test_shutdown_actual_outer_hook_bound_on_blocked_lock(self):
        # Hold the time-save lock beyond the entire outer hook budget.
        # Real BusyBox timeout is the same 5s TERM +1s KILL as inittab.
        with open(self.p / "run/chef-state-time.lock", "w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            with open(self.p / "outer.out", "w") as output:
                start = time.monotonic()
                pr = subprocess.Popen(
                    ["busybox", "timeout", "-k", "1", "5", *self.shell,
                     str(self.script), "shutdown"], env=self.env,
                    stdout=output, stderr=output, start_new_session=True)
                try:
                    pr.wait(timeout=7.5)
                    took = time.monotonic() - start
                    self.assertGreaterEqual(took, 4.8)
                    self.assertLess(took, 7.0)
                    self.assertNotEqual(pr.returncode, 0)
                    self.assertNotIn("sync_rc=0", (self.p / "outer.out").read_text())
                finally:
                    # timeout targets the hook pid; clean its descendants in
                    # this host-only isolated session before releasing lock.
                    try:
                        os.killpg(pr.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    pr.wait()

    def test_shutdown_terms_powerd_before_time_save(self):
        pr = self.spawn()
        self.edge(1_700_000, 1_791_000_000 * NS)
        # Fake proc identity identifies a terminated/unreaped child. The
        # chronyc stub records whether it was already terminated on entry.
        self.script.write_text(self.script.read_text().replace(
            'out=$(timeout -k 1 "${CHRONYC_SECS:-3}" chronyc',
            'kill -0 "$POWERD_PID" 2>/dev/null && ' +
            "awk '{ print $3 }' /proc/\"$POWERD_PID\"/stat >> \"$TRACE\"; " +
            'out=$(timeout -k 1 "${CHRONYC_SECS:-3}" chronyc'))
        d = self.p / f"proc/{pr.pid}"
        d.mkdir()
        (d / "stat").write_text(f"{pr.pid} (powerd) Z " + "0 " * 18 + "123\n")
        r = self.call("shutdown", POWERD_PID=str(pr.pid))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(pr.wait(timeout=5), -15)
        self.assertIn("stop_rc=0", self.kmsg)
        self.assertLess(self.trace.index("pidof powerd"), self.trace.index("chronyc"))
        self.assertIn("\nZ\n", self.trace)

    def test_shutdown_zombie_or_dead_state_counts_as_exited(self):
        for state in ("Z", "X"):
            pr = self.spawn()
            d = self.p / f"proc/{pr.pid}"
            d.mkdir()
            # comm may contain spaces or parentheses; parse after the last
            # closing parenthesis, not by the raw third whitespace token.
            (d / "stat").write_text(f"{pr.pid} (chrony (worker)) {state} " + "0 " * 18 + "123\n")
            r = self.call("shutdown", CHRONYD_PID=str(pr.pid))
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("stop_rc=0", self.kmsg)
            self.assertNotIn("not confirmed stopped", self.kmsg)
            pr.wait(timeout=5)

    def test_wait_tracks_original_start_time_and_unknown_state(self):
        pr = self.spawn()
        d = self.p / f"proc/{pr.pid}"
        d.mkdir()
        self.script.write_text(self.script.read_text().replace(
            'ours) ours;;', 'test-wait) wait_gone 1 "$2";;\nours) ours;;'))
        stat = d / "stat"
        stat.write_text(f"{pr.pid} (worker (name)) S " + "0 " * 18 + "101\n")
        self.assertEqual(self.call("test-wait", f"{pr.pid}:100").returncode, 0)
        stat.write_text(f"{pr.pid} (worker (name)) S " + "0 " * 18 + "100\n")
        self.assertEqual(self.call("test-wait", f"{pr.pid}:100").returncode, 1)
        self.assertEqual(self.call("test-wait", f"{pr.pid}:unknown").returncode, 1)
        for content in ["malformed\n",
                        f"{pr.pid} (worker) bogus " + "0 " * 18 + "101\n",
                        f"{pr.pid + 1} (worker) S " + "0 " * 18 + "101\n",
                        f"{pr.pid} no-parentheses S " + "0 " * 18 + "101\n"]:
            stat.write_text(content)
            self.assertEqual(self.call("test-wait", f"{pr.pid}:100").returncode, 1, content)
        stat.unlink()
        self.assertEqual(self.call("test-wait", f"{pr.pid}:100").returncode, 1)

    def test_shutdown_stubborn_daemon_reports_unconfirmed(self):
        pr = subprocess.Popen(["sh", "-c", "trap '' TERM; exec sleep 30"])
        self.addCleanup(lambda: (pr.kill(), pr.wait()))
        time.sleep(0.05)
        r = self.call("shutdown", CHRONYD_PID=str(pr.pid))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIsNone(pr.poll())
        self.assertIn("final flush not confirmed", self.kmsg)
        self.assertIn("stop_rc=1", self.kmsg)

    def test_shutdown_foreign_pidfile_not_killed(self):
        other = self.spawn()
        (self.p / "run/chef-state-timekeeper.pid").write_text(f"{other.pid}\n")
        (self.p / f"proc/{other.pid}").mkdir()
        (self.p / f"proc/{other.pid}/cmdline").write_bytes(b"sleep\0" + b"30\0")
        self.call("shutdown")
        self.assertIsNone(other.poll())
        (self.p / "run/chef-state-timekeeper.pid").write_text("12x\n")
        self.assertEqual(self.call("shutdown").returncode, 0)

    def test_shutdown_after_detach_is_quiet(self):
        # ::restart runs after the ::shutdown lines: /data is gone by then.
        r = self.call("shutdown", MP_DATA="1")
        self.assertEqual(r.returncode, 0)
        self.assertEqual((r.stderr, self.kmsg), ("", ""))
        self.assertEqual(self.trace.splitlines(), [f"mountpoint -q {self.data}"])

    def test_shutdown_unsynced_still_stops_chronyd(self):
        chronyd = self.spawn()
        (self.p / "chronyc.out").write_text("")
        r = self.call("shutdown", CHRONYD_PID=str(chronyd.pid))
        self.assertEqual(r.returncode, 0)
        self.assertEqual(chronyd.wait(timeout=5), -15)

    def test_usage(self):
        self.assertEqual(self.call("nope").returncode, 1)
        self.assertEqual(self.call("mag-restore").returncode, 1)


class AshChefState(ChefState):
    shell = ["busybox", "ash"]


if __name__ == "__main__":
    unittest.main()
