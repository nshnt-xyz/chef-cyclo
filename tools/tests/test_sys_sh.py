#!/usr/bin/env python3
"""Host tests for rootfs-overlay/usr/lib/chef/sys.sh (bash and busybox ash).

Each helper runs against a fake tree in a temporary directory, with the
variable it reads pointed there: part (SYSFS_BLOCK), server_present
(DUMP_SERVERS), find_subsys (SUBSYS_DIR), card_ready (ASOUND_DIR) and
child_running (real /proc, a live child and a zombie). The scripts that
source it keep their own tests (test_modem-up.sh, test_audio-up.sh,
test_speaker-test-tone.sh, test_sensors-up.sh, test_services.py).
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]
LIB = ROOT / "rootfs-overlay/usr/lib/chef"


class SysSh(unittest.TestCase):
    shell = ["bash"]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name)
        self.env = {"PATH": os.environ["PATH"], "CHEF_LIB_DIR": str(LIB),
                    "SYSFS_BLOCK": str(self.p / "block"), "DUMP_SERVERS": str(self.p / "dump"),
                    "SUBSYS_DIR": str(self.p / "subsys"), "ASOUND_DIR": str(self.p / "asound")}

    def run_sh(self, body, **env):
        """BODY after sourcing sys.sh under set -eu: (stdout, stderr)."""
        script = 'set -eu\n. "$CHEF_LIB_DIR/sys.sh"\n' + body + '\n'
        r = subprocess.run([*self.shell, "-c", script], env={**self.env, **env}, capture_output=True, text=True,
                           timeout=10)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout, r.stderr

    def status(self, call, **env):
        """CALL's status and output, with nothing on stderr."""
        out, err = self.run_sh(f'if out=$({call}); then echo "0 $out"; else echo "$? $out"; fi', **env)
        self.assertEqual(err, "")
        return out.rstrip("\n")

    def test_part(self):
        for dev, name in (("mmcblk0p1", "modemst1"), ("mmcblk0p4", "fsg_a"), ("mmcblk0p5", "fsg_b"),
                          ("mmcblk0p38", "persist")):
            (self.p / "block" / dev).mkdir(parents=True)
            (self.p / "block" / dev / "uevent").write_text(
                f"MAJOR=259\nDEVNAME={dev}\nDEVTYPE=partition\nPARTNAME={name}\n")
        (self.p / "block/queue").mkdir()  # a directory with no uevent
        self.assertEqual(self.status("part fsg_a"), "0 mmcblk0p4")
        self.assertEqual(self.status("part persist"), "0 mmcblk0p38")
        # whole names only
        for name in ("fsg", "fsg_", "sg_a", "persist2", ""):
            self.assertEqual(self.status(f"part '{name}'").split(" ", 1)[1:], [""], name)
        self.assertEqual(self.status("part fsg_a", SYSFS_BLOCK=str(self.p / "none")).split(" ", 1)[1:], [""])

    def test_server_present(self):
        # the 4.4 column format, 0x%08x and lower case
        (self.p / "dump").write_text("0x0000010f |0x00000002 |0x00000001 |0x0000003c |\n"
                                     "0x00000100 |0x00003201 |0x00000005 |0x00000059 |\n"
                                     "0x00001000 |0x00000001|\n")
        for hexid, rc in (("10f", "0"), ("100", "0"), ("1000", "0"), ("10", "1"), ("f", "1"), ("118", "1"),
                          ("3201", "1"), ("40", "1")):
            self.assertEqual(self.status(f"server_present {hexid}"), rc + " ", hexid)
        (self.p / "dump").unlink()  # unreadable: fails (grep's 2)
        self.assertRegex(self.status("server_present 10f"), "^[1-9][0-9]* $")

    def test_find_subsys(self):
        for n, name in ((0, "modem"), (1, "adsp"), (3, "adsp2")):
            (self.p / f"subsys/subsys{n}").mkdir(parents=True)
            (self.p / f"subsys/subsys{n}/name").write_text(name + "\n")
        (self.p / "subsys/subsys2").mkdir()  # no name node
        self.assertEqual(self.status("find_subsys adsp"), f"0 {self.p}/subsys/subsys1")
        self.assertEqual(self.status("find_subsys modem"), f"0 {self.p}/subsys/subsys0")
        self.assertEqual(self.status("find_subsys slpi"), "1 ")
        self.assertEqual(self.status("find_subsys adsp", SUBSYS_DIR=str(self.p / "none")), "1 ")

    def test_card_ready(self):
        cards = self.p / "asound/cards"
        cards.parent.mkdir()
        # st_size 0 like procfs (live 2026-09-19 run 1): a FIFO fed once
        os.mkfifo(cards)
        feeder = threading.Thread(target=lambda: cards.write_text(
            " 0 [sdm660sndcard  ]: sdm660-snd-card - sdm660-snd-card\n"))
        feeder.start()
        self.assertEqual(self.status("card_ready"), "0 ")
        feeder.join(5)
        cards.unlink()
        cards.write_text("")
        self.assertEqual(self.status("card_ready"), "1 ")
        cards.write_text(" 0 [SomeOtherCard  ]: some-other - Some Other Card\n")
        self.assertEqual(self.status("card_ready"), "1 ")
        cards.unlink()
        self.assertRegex(self.status("card_ready"), "^[1-9][0-9]* $")
        # the body must never grow an [ -s ] test back
        text = (LIB / "sys.sh").read_text()
        body = text[text.index("card_ready() {"):]
        body = body[:body.index("\n}\n")]
        code = "\n".join(l for l in body.splitlines() if not l.lstrip().startswith("#"))
        self.assertNotIn("-s", code)

    def test_child_running(self):
        live = subprocess.Popen(["sleep", "30"])
        self.addCleanup(live.wait)
        self.addCleanup(live.kill)
        dead = subprocess.Popen(["true"])
        self.addCleanup(dead.wait)
        # unreaped: a zombie until wait(), which kill -0 alone calls alive
        for _ in range(100):
            if Path(f"/proc/{dead.pid}/stat").read_text().split()[2] == "Z":
                break
            threading.Event().wait(0.01)
        self.assertEqual(self.status(f"child_running {live.pid}"), "0 ")
        self.assertEqual(self.status(f"child_running {dead.pid}"), "1 ")
        dead.wait()
        self.assertEqual(self.status(f"child_running {dead.pid}"), "1 ")

    def test_callers_variables_kept(self):
        (self.p / "block/mmcblk0p4").mkdir(parents=True)
        (self.p / "block/mmcblk0p4/uevent").write_text("PARTNAME=fsg_a\n")
        (self.p / "subsys/subsys1").mkdir(parents=True)
        (self.p / "subsys/subsys1/name").write_text("adsp\n")
        out, _ = self.run_sh('u=1 s=2 name=3\npart fsg_a >/dev/null\nfind_subsys adsp >/dev/null\n'
                             'echo "$u $s $name"')
        self.assertEqual(out, "1 2 3\n")


@unittest.skipUnless(shutil.which("busybox"), "needs busybox")
class AshSysSh(SysSh):
    shell = ["busybox", "ash"]


if __name__ == "__main__":
    unittest.main()
