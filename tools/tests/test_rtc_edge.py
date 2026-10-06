#!/usr/bin/env python3
"""Host tests for tools/rtc-edge (host build) against a fake since_epoch.

A thread bumps the fake since_epoch file at a known CLOCK_REALTIME instant
(rewritten in place, same width, so a read never sees a short file); the
binary must report that instant to within a few milliseconds, set (-n: print)
SINCE*1e9 + OFFSET plus the time since the tick, and refuse a missing or
garbage file, a jump of more than one second and a file that never ticks
(exit 3 after about 1.1 s). floor -n sets only when the clock is behind.
Nothing here sets the host clock: -n is used throughout.
"""
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest

BIN = Path(__file__).resolve().parents[1] / "rtc-edge"
NS = 1_000_000_000


class RtcEdge(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.f = self.dir / "since_epoch"
        self.f.write_text("1611357\n")

    def bump(self, delay, value):
        """Rewrite since_epoch after delay s; returns [realtime_ns of the write]."""
        out = []

        def run():
            time.sleep(delay)
            with open(self.f, "r+") as fh:
                out.append(time.clock_gettime_ns(time.CLOCK_REALTIME))
                fh.write(f"{value}\n")
        t = threading.Thread(target=run)
        t.start()
        self.addCleanup(t.join)
        return out, t

    def run_bin(self, *args):
        return subprocess.run([str(BIN), "-r", str(self.dir), *args], capture_output=True, text=True, timeout=10)

    def test_sample_at_the_tick(self):
        at, t = self.bump(0.3, 1611358)
        r = self.run_bin("sample")
        t.join()
        self.assertEqual(r.returncode, 0, r.stderr)
        since, wall = map(int, r.stdout.split())
        self.assertEqual(since, 1611358)
        self.assertLess(abs(wall - at[0]), 5_000_000)  # within 5 ms

    def test_set_dry_run(self):
        offset = 1_789_000_000 * NS + 370_000_000
        at, t = self.bump(0.2, 1611358)
        r = self.run_bin("-n", "set", str(offset))
        done = time.clock_gettime_ns(time.CLOCK_REALTIME)
        t.join()
        self.assertEqual(r.returncode, 0, r.stderr)
        word, value = r.stdout.split()
        self.assertEqual(word, "set")
        target = int(value)
        base = 1611358 * NS + offset
        # the tick plus the time from the tick to the set
        self.assertGreaterEqual(target, base - 5_000_000)
        self.assertLessEqual(target, base + (done - at[0]) + 5_000_000)

    def test_no_tick(self):
        start = time.monotonic()
        r = self.run_bin("sample")
        self.assertEqual(r.returncode, 3)
        self.assertIn("did not tick", r.stderr)
        self.assertGreater(time.monotonic() - start, 1.0)
        self.assertLess(time.monotonic() - start, 2.0)

    def test_jump(self):
        _, t = self.bump(0.1, 1611360)
        r = self.run_bin("sample")
        t.join()
        self.assertEqual(r.returncode, 3)
        self.assertIn("jumped", r.stderr)

    def test_refusals(self):
        for content in ["", "abc\n", "-5\n", "0\n", "12x\n", "12\njunk", "12\n\n", "12\0junk", "+12\n", " 12\n", "4294967296\n", "9223372036854775807\n", "9" * 40]:
            self.f.write_text(content)
            r = self.run_bin("sample")
            self.assertEqual(r.returncode, 1, content)
        self.f.unlink()
        self.assertEqual(self.run_bin("sample").returncode, 1)
        self.assertEqual(self.run_bin("-n", "set", "1").returncode, 1)
        for args in [[], ["set"], ["set", "x"], ["floor", "0"], ["floor", "-1"], ["nope", "1"], ["sample", "1"]]:
            self.assertEqual(self.run_bin("-n", *args).returncode, 1, args)

    def test_set_overflow(self):
        for offset in [9223372036854775807, -9223372036854775808]:
            self.f.write_text("1611357\n")
            _, t = self.bump(0.1, 1611358)
            r = self.run_bin("-n", "set", str(offset))
            t.join()
            self.assertEqual(r.returncode, 1, r.stderr)
            self.assertIn("out of range", r.stderr)
            self.assertEqual(r.stdout, "")

    def test_floor(self):
        now = time.time()
        r = self.run_bin("-n", "floor", str(int(now) + 3600))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.split(), ["set", str((int(now) + 3600) * NS)])
        r = self.run_bin("-n", "floor", str(int(now) - 3600))
        self.assertEqual(r.returncode, 4)
        self.assertEqual(r.stdout, "")


if __name__ == "__main__":
    unittest.main()
