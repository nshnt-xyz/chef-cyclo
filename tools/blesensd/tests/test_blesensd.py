#!/usr/bin/env python3
"""End-to-end tests for blesensd: the real binary (host build, -T: timers
divided by 10) against tests/fake_bluez.py on a private dbus-daemon and a
stub chef-state, driven through its socket like the settings app and the
CLI. Skipped without dbus-daemon or python3 dbus + gi."""
import json
import os
import queue
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "build", "host", "blesensd")
FAKE = os.path.join(HERE, "fake_bluez.py")

HR = "AA:BB:CC:00:00:01"
HR2 = "AA:BB:CC:00:00:02"
COMBO = "AA:BB:CC:00:00:03"
BOND = "AA:BB:CC:00:00:04"

try:
    import dbus  # noqa: F401
    from gi.repository import GLib  # noqa: F401
    HAVE = shutil.which("dbus-daemon") is not None
except ImportError:
    HAVE = False

STUB = r"""#!/bin/sh
# stub chef-state for blesensd: records in $D/records, every call logged
D=$(dirname "$0")
echo "$1" >> "$D/calls"
case "$1" in
blesens-load)
	if [ -f "$D/load-hang" ]; then
		sleep 30 & echo $! > "$D/desc.pid"; wait
	fi
	if [ -f "$D/load-desc" ]; then
		# a descendant keeps stdout open after the leader exits
		(sleep 30) & echo $! > "$D/desc.pid"
	fi
	cat "$D/records" 2>/dev/null
	exit 0;;
blesens-save)
	while [ -f "$D/save-gate" ]; do sleep 0.01; done
	if [ -f "$D/save-hang" ]; then
		rm -f "$D/save-hang"
		sleep 30 & echo $! > "$D/desc.pid"; wait
	fi
	if [ -f "$D/save-desc" ]; then
		rm -f "$D/save-desc"
		(sleep 30 <&- >&-) & echo $! > "$D/desc.pid"
	fi
	cat > "$D/records.new" || exit 1
	if ! grep -q ' forget ' "$D/records.new"; then
		if [ -f "$D/save-clean-fail" ]; then rm -f "$D/save-clean-fail"; exit 1; fi
		while [ -f "$D/save-clean-gate" ]; do sleep 0.01; done
	fi
	mv "$D/records.new" "$D/records"
	echo "---" >> "$D/saves"; cat "$D/records" >> "$D/saves"
	exit 0;;
esac
exit 1
"""


class Lines:
    """Lines from a stream, read on a thread; wait() scans from a cursor."""

    def __init__(self, stream, decode=None):
        self.items = []
        self.cv = threading.Condition()
        self.pos = 0
        self.decode = decode
        self.t = threading.Thread(target=self.run, args=(stream,), daemon=True)
        self.t.start()

    def run(self, stream):
        for raw in stream:
            line = raw.decode() if isinstance(raw, bytes) else raw
            line = line.rstrip("\n")
            item = self.decode(line) if self.decode else line
            with self.cv:
                self.items.append(item)
                self.cv.notify_all()

    def wait(self, pred, timeout=5.0, what=""):
        end = time.monotonic() + timeout
        with self.cv:
            while True:
                while self.pos < len(self.items):
                    it = self.items[self.pos]
                    self.pos += 1
                    if pred(it):
                        return it
                left = end - time.monotonic()
                if left <= 0:
                    raise AssertionError("timed out waiting for %s; last: %s" %
                                         (what, self.items[-8:]))
                self.cv.wait(left)

    def skip(self):
        with self.cv:
            self.pos = len(self.items)

    def since(self, pos):
        with self.cv:
            return list(self.items[pos:])

    def mark(self):
        with self.cv:
            return len(self.items)


class Client:
    def __init__(self, path, watch=True):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.02)
        self.f = self.s.makefile("rb")
        self.lines = Lines(self.f, json.loads)
        if watch:
            self.send("watch")
            self.wait(lambda m: m.get("type") == "ok" and m.get("cmd") == "watch", what="watch ok")

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def wait(self, pred, timeout=5.0, what=""):
        return self.lines.wait(pred, timeout, what)

    def wait_all(self, mark, preds, timeout=5.0):
        """Every pred matched by some line after mark, in any order."""
        end = time.monotonic() + timeout
        while True:
            got = self.lines.since(mark)
            if all(any(p(m) for m in got) for p in preds):
                self.lines.skip()
                return
            if time.monotonic() > end:
                raise AssertionError("not all seen: %s" % got[-10:])
            time.sleep(0.02)

    def close(self):
        try:
            self.s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.s.close()


def cp(power, crank=None):
    """Cycling Power Measurement hex: power, optional (revs, time)."""
    import struct
    b = struct.pack("<Hh", 0x20 if crank else 0, power)
    if crank:
        b += struct.pack("<HH", *crank)
    return b.hex()


def csc(wheel=None, crank=None):
    """CSC Measurement hex: optional wheel (revs, time), crank (revs, time)."""
    import struct
    b = bytes([(1 if wheel else 0) | (2 if crank else 0)])
    if wheel:
        b += struct.pack("<IH", *wheel)
    if crank:
        b += struct.pack("<HH", *crank)
    return b.hex()


def is_(t, **kw):
    return lambda m: m.get("type") == t and all(m.get(k) == v for k, v in kw.items())


@unittest.skipUnless(HAVE and os.path.exists(BIN), "needs dbus-daemon, python3 dbus + gi and build/host/blesensd")
class BlesensdTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="bs", dir="/tmp")
        self.bus = os.path.join(self.tmp, "bus")
        conf = os.path.join(self.tmp, "bus.conf")
        with open(conf, "w") as f:
            f.write('<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"\n'
                    ' "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">\n'
                    "<busconfig><type>session</type><listen>unix:path=%s</listen>"
                    '<policy context="default"><allow send_destination="*" eavesdrop="true"/>'
                    '<allow eavesdrop="true"/><allow own="*"/></policy></busconfig>\n' % self.bus)
        self.procs = []
        self.dbusd = subprocess.Popen(["dbus-daemon", "--nofork", "--config-file", conf],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.procs.append(self.dbusd)
        for _ in range(200):
            if os.path.exists(self.bus):
                break
            time.sleep(0.01)
        self.env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + self.bus)
        self.stub = os.path.join(self.tmp, "chef-state")
        with open(self.stub, "w") as f:
            f.write(STUB)
        os.chmod(self.stub, 0o755)
        self.sock = os.path.join(self.tmp, "s")
        self.fake = subprocess.Popen([sys.executable, FAKE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     env=self.env)
        self.procs.append(self.fake)
        self.fl = Lines(self.fake.stdout)
        self.fl.wait(lambda l: l == "ready", 10, "fake ready")
        self.d = None
        self.clients = []

    def tearDown(self):
        for c in self.clients:
            c.close()
        if self.d and self.d.poll() is None:
            self.d.send_signal(signal.SIGTERM)
            try:
                self.d.wait(5)
            except subprocess.TimeoutExpired:
                self.d.kill()
        for p in reversed(self.procs):
            if p.poll() is None:
                p.kill()
                p.wait()
        try:
            pid = int(Path(os.path.join(self.tmp, "desc.pid")).read_text())
            os.kill(pid, signal.SIGKILL)
        except (OSError, ValueError):
            pass
        if self.d and self.d.stderr:
            self.d.stderr.close()
        if hasattr(self, "dlog"):
            self.dlog.close()
        self.fake.stdin.close()
        self.fake.stdout.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ------------------------------------------------------------ helpers

    def cmd(self, line):
        self.fl.skip()
        self.fake.stdin.write((line + "\n").encode())
        self.fake.stdin.flush()
        if line == "dump":
            return json.loads(self.fl.wait(lambda l: l.startswith("{"), 5, "dump"))
        if line.startswith("agent "):
            return self.fl.wait(lambda l: l.startswith("agent "), 5, line)
        return self.fl.wait(lambda l: l == "ok " + line or l == "dropped " + line, 5, line)

    def start(self, *extra, wait_loaded=True):
        self.dlog = open(os.path.join(self.tmp, "daemon.log"), "a")
        self.d = subprocess.Popen([BIN, "-T", "-v", "-S", self.sock, "-s", self.stub, *extra],
                                  env=self.env, stdout=subprocess.DEVNULL, stderr=self.dlog)
        self.procs.append(self.d)
        for _ in range(200):
            if os.path.exists(self.sock):
                break
            time.sleep(0.01)
        c = self.client()
        if wait_loaded:
            self.eventually(lambda: self.status()["loaded"] and self.status()["agent"], timeout=3, what="records loaded and agent ready")
        return c

    def stop(self):
        self.d.send_signal(signal.SIGTERM)
        rc = self.d.wait(5)
        self.dlog.close()
        for c in self.clients:
            c.close()
        self.clients = []
        return rc

    def client(self, watch=True):
        c = Client(self.sock, watch)
        self.clients.append(c)
        return c

    def log(self):
        return Path(os.path.join(self.tmp, "daemon.log")).read_text()

    def records(self):
        try:
            return Path(os.path.join(self.tmp, "records")).read_text()
        except OSError:
            return ""

    def wait_file(self, name, pred, timeout=5.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                text = Path(os.path.join(self.tmp, name)).read_text()
            except OSError:
                text = ""
            if pred(text):
                return text
            time.sleep(0.02)
        raise AssertionError("%s: %r" % (name, text))

    def eventually(self, fn, timeout=2.0, what=""):
        end = time.monotonic() + timeout
        while not fn():
            if time.monotonic() > end:
                raise AssertionError("never: " + what)
            time.sleep(0.05)

    def status(self):
        c = self.client(watch=False)
        c.send("status")
        st = c.wait(is_("status"))
        c.close()
        self.clients.remove(c)
        return st

    def pair(self, c, addr, rssi=-60):
        c.send("scan on")
        c.wait(is_("found", addr=addr), what="found " + addr)
        c.send("pair " + addr)
        c.wait(is_("ok", cmd="pair"))
        ev = c.wait(lambda m: m.get("type") == "pair" and m.get("state") in ("done", "failed"), 6, "pair end")
        self.assertEqual(ev["state"], "done", ev)
        c.send("scan off")
        c.wait(is_("sensor", addr=addr, state="connected"), what="connected")
        return ev

    def notify_until(self, c, addr, kind, hexdata, pred, what=""):
        """Notify until the value event pred holds (StartNotify may still be
        on its way after a (re)connection)."""
        end = time.monotonic() + 5
        while time.monotonic() < end:
            if self.cmd("notify %s %s %s" % (addr, kind, hexdata)).startswith("ok"):
                try:
                    return c.wait(lambda m: m.get("type") == "value" and m.get("addr") == addr and pred(m),
                                  0.5, what)
                except AssertionError:
                    pass
            time.sleep(0.05)
        raise AssertionError("no value for " + what)

    # ------------------------------------------------------------ tests

    def test_pair_values_stale_and_restart(self):
        self.cmd("dev %s CHEF_HR hr gapname=pc_host" % HR)	# the alias changes once connected
        self.cmd("adv %s -60" % HR)
        c = self.start()
        c.wait(is_("adapter", present=True, powered=True))
        c.send("scan on")
        f = c.wait(is_("found", addr=HR))
        self.assertEqual((f["kinds"], f["name"], f["rssi"]), ("hr", "CHEF HR", -60))
        dump = self.cmd("dump")
        self.assertEqual(dump["filters"][0], {"transport": "le", "uuids": [
            "0000180d-0000-1000-8000-00805f9b34fb", "00001816-0000-1000-8000-00805f9b34fb",
            "00001818-0000-1000-8000-00805f9b34fb"]})
        c.send("pair " + HR)
        states = []
        while not states or states[-1] not in ("done", "failed"):
            states.append(c.wait(is_("pair", addr=HR))["state"])
        self.assertEqual(states, ["connecting", "resolving", "saving", "done"])
        c.wait(is_("lost", addr=HR))	# ours now: no longer offered
        s = c.wait(is_("sensor", addr=HR, state="connected"))
        self.assertEqual((s["kinds"], s["name"], s["rssi"]), ("hr", "CHEF HR", -60))
        dump = self.cmd("dump")
        self.assertTrue(dump["devices"][HR]["trusted"])
        self.assertFalse(dump["devices"][HR]["paired"])	# no bond needed
        self.eventually(lambda: self.cmd("dump")["agents"] == ["NoInputNoOutput"], what="ordinary agent restored")
        self.assertEqual(self.cmd("dump")["default_agents"], [])
        self.assertRegex(self.wait_file("records", lambda t: t != ""), r"^sensor %s hr (\d{4}-\d\d-\d\d|-) - CHEF HR\n$" % HR)
        # GATT battery read once per connection
        c.wait(lambda m: m.get("type") == "sensor" and m.get("battery") == 77)
        # the cached Value (0x99) never shows; notifications do, 8 and 16 bit
        self.notify_until(c, HR, "hr", "0054", lambda m: m["value"] == 84, "84")
        self.cmd("notify %s hr 01f000" % HR)
        c.wait(is_("value", addr=HR, value=240))
        self.cmd("notify %s hr 012d01" % HR)	# 301: implausible, dropped
        self.cmd("notify %s hr 0050" % HR)
        v = c.wait(is_("value", addr=HR))
        self.assertEqual(v["value"], 80)
        self.assertNotIn(153, [m.get("value") for m in c.lines.since(0) if m.get("type") == "value"])
        # 5 s (0.5 s) without one: null once
        c.wait(is_("value", addr=HR, value=None), 2)
        # a restart: loaded, reconnected, notifications started again
        starts = self.cmd("dump")["start_notify_hr"]
        self.assertEqual(self.stop(), 0)
        c = self.start()
        c.wait(is_("sensor", addr=HR, state="connected"), 5)
        self.notify_until(c, HR, "hr", "0048", lambda m: m["value"] == 72, "72 after restart")
        self.assertGreater(self.cmd("dump")["start_notify_hr"], starts)
        st = self.status()
        self.assertEqual((st["sensors"], st["connected"], st["loaded"], st["agent"]), (1, 1, True, True))

    def test_combo_device_kinds_and_rates(self):
        self.cmd("dev %s Combo hr,power,csc battery1=64" % COMBO)
        self.cmd("adv %s -70" % COMBO)
        c = self.start()
        self.pair(c, COMBO)
        s = c.wait(lambda m: m.get("type") == "sensor" and m.get("battery") == 64)
        self.assertEqual(s["kinds"], "hr,power,csc")
        self.assertNotIn("read_battery", self.cmd("dump"))	# Battery1, no GATT read
        self.assertNotIn("start_notify_battery", self.cmd("dump")) # preferred Battery1
        # one record for the device
        rec = self.wait_file("records", lambda t: t != "")
        self.assertEqual(rec.count("\n"), 1)
        self.assertIn(" hr,power,csc ", rec)
        # power: crank revs 42 at 1024, 43 at 1024+683 ticks -> 90 rpm; -10 W is valid
        self.notify_until(c, COMBO, "power", cp(245, (42, 1024)), lambda m: m["kind"] == "power" and
                          m["value"] == 245, "power")
        self.cmd("notify %s power %s" % (COMBO, cp(-10, (43, 1024 + 683))))
        v = c.wait(is_("value", addr=COMBO, kind="power"))
        self.assertEqual((v["value"], v["cadence"]), (-10, 90))
        # csc: wheel 10 -> 12 in 1024 ticks (120 rpm) at 2105 mm, crank 5 -> 6 in 768 (80 rpm)
        self.cmd("notify %s csc %s" % (COMBO, csc((10, 0), (5, 0))))
        v = c.wait(is_("value", addr=COMBO, kind="csc"))
        self.assertEqual((v["speed"], v["cadence"]), (None, None))	# one sample: no rate yet
        self.cmd("notify %s csc %s" % (COMBO, csc((12, 1024), (6, 768))))
        v = c.wait(is_("value", addr=COMBO, kind="csc"))
        self.assertEqual((v["speed"], v["cadence"]), (15.2, 80))
        # Battery1 changes are followed
        self.cmd("battery %s 63" % COMBO)
        c.wait(lambda m: m.get("type") == "sensor" and m.get("battery") == 63)
        # a new watcher gets the fresh values at once
        c2 = self.client()
        c2.wait(is_("sensor", addr=COMBO))
        got = {c2.wait(is_("value", addr=COMBO))["kind"] for _ in range(2)}
        self.assertEqual(got, {"power", "csc"})

    def test_optional_rates_disappear_and_rebase(self):
        self.cmd("dev %s Combo power,csc" % COMBO)
        self.cmd("adv %s -60" % COMBO)
        c = self.start()
        self.pair(c, COMBO)

        def sample(kind, packet):
            c.lines.skip()
            self.assertTrue(self.cmd("notify %s %s %s" % (COMBO, kind, packet)).startswith("ok"))
            return c.wait(is_("value", addr=COMBO, kind=kind))

        self.notify_until(c, COMBO, "power", cp(100, (1, 0)), lambda m: m["kind"] == "power")
        self.assertEqual(sample("power", cp(101, (2, 1024)))["cadence"], 60)
        for _ in range(10):
            time.sleep(0.1)
            v = sample("power", cp(102))
            self.assertEqual((v["value"], v["cadence"]), (102, None))
        watch = self.client()
        v = watch.wait(is_("value", addr=COMBO, kind="power"))
        self.assertEqual((v["value"], v["cadence"]), (102, None))
        self.assertIsNone(sample("power", cp(103, (10, 4096)))["cadence"])
        self.assertEqual(sample("power", cp(104, (11, 5120)))["cadence"], 60)
        self.assertEqual(sample("csc", csc((10, 0), (5, 0)))["speed"], None)
        v = sample("csc", csc((12, 1024), (6, 768)))
        self.assertEqual((v["speed"], v["cadence"]), (15.2, 80))
        for i in range(10):
            time.sleep(0.1)
            v = sample("csc", csc((14 + i * 2, 2048 + i * 1024)))
            self.assertEqual((v["speed"], v["cadence"]), (15.2, None))
        watch = self.client()
        v = watch.wait(is_("value", addr=COMBO, kind="csc"))
        self.assertEqual((v["speed"], v["cadence"]), (15.2, None))
        v = sample("csc", csc(None, (20, 8192)))
        self.assertEqual((v["speed"], v["cadence"]), (None, None))
        for i in range(10):
            time.sleep(0.1)
            v = sample("csc", csc(None, (21 + i, 8960 + i * 768)))
            self.assertEqual((v["speed"], v["cadence"]), (None, 80))
        c2 = self.client()
        v = c2.wait(is_("value", addr=COMBO, kind="csc"))
        self.assertEqual((v["speed"], v["cadence"]), (None, 80))
        # An actual stale gap must not calculate a rate across that gap.
        time.sleep(0.7)
        self.assertIsNone(sample("power", cp(105, (20, 9216)))["cadence"])
        self.assertEqual(sample("power", cp(106, (21, 10240)))["cadence"], 60)
        v = sample("csc", csc((50, 30000), (50, 30000)))
        self.assertEqual((v["speed"], v["cadence"]), (None, None))
        # Repeated valid stopped counters stay fresh and eventually read zero.
        for _ in range(34):
            time.sleep(0.1)
            v = sample("csc", csc((50, 30000), (50, 30000)))
        self.assertEqual((v["speed"], v["cadence"]), (0.0, 0))

    def test_owned_battery_notifications_and_reconnect(self):
        self.cmd("dev %s HR hr gattbat=77" % HR)
        self.cmd("adv %s -60" % HR)
        c = self.start()
        self.pair(c, HR)
        self.eventually(lambda: self.cmd("dump")["devices"][HR]["sessions"]["battery"] == 1,
                        what="our battery session")
        c.lines.skip()
        self.assertTrue(self.cmd("notify %s battery 3c" % HR).startswith("ok"))
        c.wait(is_("sensor", addr=HR, battery=60))
        # Invalid notifications cannot corrupt the known percentage.
        for packet in ("ff", "3c01"):
            self.cmd("notify %s battery %s" % (HR, packet))
        c2 = self.client()
        self.assertEqual(c2.wait(is_("sensor", addr=HR))["battery"], 60)
        starts = self.cmd("dump")["start_notify_battery"]
        self.cmd("away %s" % HR)
        c.wait(lambda m: m.get("type") == "sensor" and m.get("addr") == HR and
               m.get("state") != "connected" and m.get("battery") is None)
        self.cmd("adv %s -60" % HR)
        c.send("hold on")
        c.wait(is_("sensor", addr=HR, state="connected"), 5)
        self.eventually(lambda: self.cmd("dump")["start_notify_battery"] > starts,
                        what="new battery session after disconnect")
        c.lines.skip()
        self.cmd("notify %s battery 3b" % HR)
        c.wait(is_("sensor", addr=HR, battery=59))
        self.assertEqual(self.stop(), 0)
        c = self.start()
        self.eventually(lambda: self.cmd("dump")["start_notify_battery"] > starts + 1,
                        what="new battery session after daemon epoch")
        c.lines.skip()
        self.cmd("notify %s battery 3a" % HR)
        c.wait(is_("sensor", addr=HR, battery=58))

    def test_other_notify_owner_and_object_loss(self):
        import dbus
        self.cmd("dev %s Combo hr,power,csc" % COMBO)
        self.cmd("adv %s -60" % COMBO)
        # Another client owns Notifying before our daemon starts. It must
        # still obtain its own sessions, not trust that global property.
        bus = dbus.bus.BusConnection("unix:path=" + self.bus)
        root = bus.get_object("org.bluez", "/", introspect=False)
        devpath = "/org/bluez/hci0/dev_" + COMBO.replace(":", "_")
        bus.get_object("org.bluez", devpath, introspect=False).Connect(dbus_interface="org.bluez.Device1")
        self.eventually(lambda: len(root.GetManagedObjects(dbus_interface="org.freedesktop.DBus.ObjectManager")) >= 6,
                        what="characteristics exported")
        objects = root.GetManagedObjects(dbus_interface="org.freedesktop.DBus.ObjectManager")
        for path, interfaces in objects.items():
            props = interfaces.get("org.bluez.GattCharacteristic1", {})
            if str(props.get("UUID", ""))[4:8] in ("2a37", "2a63", "2a5b", "2a19"):
                bus.get_object("org.bluez", path, introspect=False).StartNotify(
                    dbus_interface="org.bluez.GattCharacteristic1")
        try:
            c = self.start()
            self.pair(c, COMBO)
            self.notify_until(c, COMBO, "hr", "0050", lambda m: m["kind"] == "hr", "hr")
            self.notify_until(c, COMBO, "power", cp(200), lambda m: m["kind"] == "power", "power")
            self.notify_until(c, COMBO, "csc", csc(crank=(5, 0)), lambda m: m["kind"] == "csc", "csc")
            self.eventually(lambda: all(self.cmd("dump")["devices"][COMBO]["sessions"][kind] == 2
                                        for kind in ("hr", "power", "csc", "battery")), what="our four sessions established")
            sessions = self.cmd("dump")["devices"][COMBO]["sessions"]
            self.assertEqual((sessions["hr"], sessions["power"], sessions["csc"], sessions["battery"]), (2, 2, 2, 2))
            self.cmd("notify %s battery 3c" % COMBO)
            c.wait(is_("sensor", addr=COMBO, battery=60))
            mark = c.lines.mark()
            self.cmd("remove " + COMBO)  # BlueZ loses objects, not our Forget
            c.wait_all(mark, [is_("value", addr=COMBO, kind="hr", value=None),
                              is_("value", addr=COMBO, kind="power", value=None, cadence=None),
                              is_("value", addr=COMBO, kind="csc", speed=None, cadence=None),
                              is_("sensor", addr=COMBO, battery=None)], 2)
            self.wait_file("records", lambda text: COMBO in text)  # still saved for reconnect
        finally:
            bus.close()

    def test_bonding_through_scoped_agent(self):
        self.cmd("dev %s Strap hr bond" % BOND)
        self.cmd("dev %s Other hr" % HR)
        self.cmd("adv %s -50" % BOND)
        c = self.start()
        c.send("scan on")
        c.wait(is_("found", addr=BOND))
        # unrelated agent requests are refused while nothing is pairing
        self.assertEqual(self.cmd("agent RequestAuthorization " + HR), "agent RequestAuthorization rejected")
        self.assertEqual(self.cmd("agent RequestConfirmation " + BOND), "agent RequestConfirmation rejected")
        c.send("pair " + BOND)
        states = []
        while not states or states[-1] not in ("done", "failed"):
            states.append(c.wait(is_("pair", addr=BOND), 6)["state"])
        self.assertEqual(states, ["connecting", "resolving", "bonding", "saving", "done"])
        self.eventually(lambda: self.status()["agent"] and not self.cmd("dump")["default_agents"],
                        what="agent re-registered")
        dump = self.cmd("dump")
        self.assertEqual(dump["default_agents"], [])
        self.assertTrue(dump["devices"][BOND]["paired"])
        self.assertEqual(dump["pair"], 1)
        self.assertIn("agent RequestConfirmation for", self.log())
        # a call from anyone but bluetoothd is refused
        import dbus
        bus = dbus.bus.BusConnection("unix:path=" + self.bus)
        agent = bus.get_object(dump["agent_senders"][0], "/chef/blesensd/agent", introspect=False)
        with self.assertRaises(dbus.exceptions.DBusException) as e:
            agent.RequestAuthorization(dbus.ObjectPath("/org/bluez/hci0/dev_AA_BB_CC_00_00_04"),
                                       dbus_interface="org.bluez.Agent1")
        self.assertIn("Rejected", e.exception.get_dbus_name())
        bus.close()
        # a saved sensor's service authorisation is allowed; others not
        self.assertEqual(self.cmd("agent AuthorizeService " + BOND), "agent AuthorizeService ok")
        self.assertEqual(self.cmd("agent AuthorizeService " + HR), "agent AuthorizeService rejected")

    def test_connect_authentication_uses_our_agent(self):
        self.cmd("dev %s Strap hr connectbond" % BOND)
        self.cmd("adv %s -50" % BOND)
        c = self.start()
        c.send("scan on")
        c.wait(is_("found", addr=BOND))
        c.send("pair " + BOND)
        states = []
        while not states or states[-1] not in ("done", "failed"):
            states.append(c.wait(is_("pair", addr=BOND), 6)["state"])
        self.assertEqual(states, ["connecting", "bonding", "saving", "done"])
        self.assertTrue(self.cmd("dump")["devices"][BOND]["paired"])
        self.notify_until(c, BOND, "hr", "0055", lambda m: m["value"] == 85, "encrypted HR")

    def test_default_agent_release_confirmed_and_retried(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("adv %s -60" % HR)
        c = self.start()
        self.cmd("fail UnregisterAgent 2")
        self.pair(c, HR)
        self.eventually(lambda: not self.cmd("dump")["default_agents"] and self.status()["agent"],
                        timeout=3, what="previous default restored after retries")
        self.assertEqual(self.cmd("dump")["unregister_agent"], 3)
        self.assertIn("pairing agent release", self.log())

    def test_agent_registration_retried(self):
        self.cmd("fail RegisterAgent 2")
        self.start()
        end = time.monotonic() + 5
        while self.cmd("dump")["agents"] != ["NoInputNoOutput"]:
            self.assertLess(time.monotonic(), end)
            time.sleep(0.1)
        self.assertEqual(self.cmd("dump")["register_agent"], 3)

    def test_cancel_timeout_and_late_replies(self):
        self.cmd("dev %s A hr" % HR)
        self.cmd("dev %s B hr" % HR2)
        self.cmd("adv %s -60" % HR)
        self.cmd("adv %s -61" % HR2)
        c = self.start()
        c.send("scan on")
        c.wait(is_("found", addr=HR))
        c.wait(is_("found", addr=HR2))
        # A: Connect answers late; cancelled before; B pairs meanwhile
        self.cmd("delay Connect 1500")
        c.send("pair " + HR)
        c.wait(is_("pair", addr=HR, state="connecting"))
        self.eventually(lambda: self.cmd("dump").get("connect", 0) >= 1, what="A Connect actually sent")
        c.send("pair " + HR2)
        self.assertEqual(c.wait(is_("error", cmd="pair"))["reason"], "busy")
        c.send("cancel " + HR)
        c.wait(is_("pair", addr=HR, state="failed", reason="cancelled"))
        c.send("pair " + HR2)
        c.wait(is_("ok", cmd="pair"))
        self.cmd("delay StartNotify 1500")	# B waits in its notify step past A's reply
        c.wait(is_("pair", addr=HR2, state="resolving"))
        time.sleep(1.6)		# A's Connect reply (an abort) lands now
        ev = c.wait(lambda m: m.get("type") == "pair" and m.get("state") in ("done", "failed"), 5)
        self.assertEqual((ev["addr"], ev["state"]), (HR2, "done"))
        self.assertNotIn(HR, self.records())
        self.eventually(lambda: not self.cmd("dump")["devices"][HR]["connected"], what="A disconnected")
        # a pairing that never resolves times out (20 s, 2 s here) and is undone
        self.cmd("dev %s C hr noresolve" % COMBO)
        self.cmd("adv %s -60" % COMBO)
        c.send("scan on")
        c.wait(is_("found", addr=COMBO))
        c.send("pair " + COMBO)
        ev = c.wait(is_("pair", addr=COMBO, state="failed"), 4)
        self.assertEqual(ev["reason"], "timed out")
        self.eventually(lambda: not self.cmd("dump")["devices"][COMBO]["connected"], what="C disconnected")
        # unknown or invalid requests
        c.send("pair 11:22:33:44:55:66")
        self.assertEqual(c.wait(is_("error", cmd="pair"))["reason"], "not a sensor in range")
        c.send("pair " + HR2)
        self.assertEqual(c.wait(is_("error", cmd="pair"))["reason"], "already paired")
        c.send("frobnicate")
        c.wait(is_("error", cmd="frobnicate"))

    def test_bond_cancel_cancels_pairing(self):
        self.cmd("dev %s Strap hr bond" % BOND)
        self.cmd("adv %s -50" % BOND)
        c = self.start()
        c.send("scan on")
        c.wait(is_("found", addr=BOND))
        self.cmd("delay Pair 3000")
        c.send("pair " + BOND)
        c.wait(is_("pair", addr=BOND, state="bonding"), 4)
        self.assertEqual(len(self.cmd("dump")["default_agents"]), 1)
        c.send("cancel " + BOND)
        c.wait(is_("pair", addr=BOND, state="failed", reason="cancelled"))
        time.sleep(0.3)
        dump = self.cmd("dump")
        self.assertEqual(dump["cancel_pairing"], 1)
        self.assertEqual(dump["default_agents"], [])
        self.assertNotIn(BOND, dump["devices"])	# a half bond is removed
        self.assertEqual(self.records(), "")

    def test_disconnect_reconnect_backoff(self):
        self.cmd("dev %s Combo power,csc" % COMBO)
        self.cmd("adv %s -60" % COMBO)
        c = self.start()
        self.pair(c, COMBO)
        self.notify_until(c, COMBO, "csc", csc(crank=(5, 0)), lambda m: m["kind"] == "csc", "csc 1")
        self.cmd("notify %s csc %s" % (COMBO, csc(crank=(6, 768))))
        v = c.wait(is_("value", addr=COMBO, kind="csc"))
        self.assertEqual(v["cadence"], 80)
        self.notify_until(c, COMBO, "power", cp(200), lambda m: m["kind"] == "power" and m["value"] == 200,
                          "power")
        # the sensor drops the link: both kinds go null at once, state connecting
        m0 = c.lines.mark()
        self.cmd("drop " + COMBO)
        c.wait_all(m0, [is_("sensor", addr=COMBO, state="connecting"),
                        is_("value", addr=COMBO, kind="csc", speed=None, cadence=None),
                        is_("value", addr=COMBO, kind="power", value=None, cadence=None)], 1)
        # it is back in range: reconnected, notifications started again, and
        # the old crank baseline is gone (the first sample gives no cadence)
        c.wait(is_("sensor", addr=COMBO, state="connected"), 5)
        v = self.notify_until(c, COMBO, "csc", csc(crank=(7, 1536)), lambda m: m["kind"] == "csc", "csc after")
        self.assertIsNone(v["cadence"])
        self.assertEqual(self.cmd("dump")["devices"][COMBO]["sessions"], {"power": 1, "csc": 1, "battery": 1})
        # out of range: Connect retried with backoff 0.5 s doubling to 3 s
        self.cmd("away " + COMBO)
        c.wait(is_("sensor", addr=COMBO, state="connecting"))
        n0 = self.cmd("dump")["connect"]
        time.sleep(4.0)
        n = self.cmd("dump")["connect"] - n0
        self.assertTrue(3 <= n <= 5, n)	# 0, 0.5, 1.5, 3.5 s (+ answer times)
        # back: connected again on the next try
        self.cmd("adv %s -60" % COMBO)
        c.wait(is_("sensor", addr=COMBO, state="connected"), 5)

    def test_reconnect_only_while_held_or_soon_after(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("adv %s -60" % HR)
        c = self.start("-r", "20")	# 20 s window, 2 s here
        self.pair(c, HR)
        # the sensor goes away: retried for the window, then quiet
        self.cmd("away " + HR)
        c.wait(is_("sensor", addr=HR, state="connecting"))
        c.wait(is_("sensor", addr=HR, state="waiting"), 4)
        self.assertIn("reconnection paused", self.log())
        st = self.status()
        self.assertEqual((st["held"], st["reconnecting"]), (False, False))
        n0 = self.cmd("dump")["connect"]
        time.sleep(2.0)
        self.assertEqual(self.cmd("dump")["connect"], n0)	# no radio use while nobody holds
        # a hold (the settings page, a ride) tries at once and keeps trying
        h = self.client(watch=False)
        h.send("hold on")
        h.wait(is_("ok", cmd="hold"))
        c.wait(is_("sensor", addr=HR, state="connecting"))
        time.sleep(0.3)
        self.assertGreater(self.cmd("dump")["connect"], n0)
        time.sleep(3.0)		# past the window: still trying while held
        n1 = self.cmd("dump")["connect"]
        self.cmd("adv %s -60" % HR)
        c.wait(is_("sensor", addr=HR, state="connected"), 5)
        self.assertGreater(self.cmd("dump")["connect"], n1)
        # the holder leaves (closing counts): a later loss is retried for the window only
        h.close()
        self.clients.remove(h)
        self.cmd("away " + HR)
        c.wait(is_("sensor", addr=HR, state="waiting"), 4)
        # BT on again opens a new window
        self.cmd("power 0")
        c.wait(is_("sensor", addr=HR, state="off"))
        n2 = self.cmd("dump")["connect"]
        self.cmd("power 1")
        c.wait(is_("sensor", addr=HR, state="connecting"))
        time.sleep(0.3)
        self.assertGreater(self.cmd("dump")["connect"], n2)
        c.wait(is_("sensor", addr=HR, state="waiting"), 4)

    def test_window_expiry_cancels_outstanding_connection(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("adv %s -60" % HR)
        c = self.start("-r", "10")  # one second with -T
        self.pair(c, HR)
        self.cmd("delay Connect 1900")
        self.cmd("drop " + HR)
        c.wait(is_("sensor", addr=HR, state="connecting"))
        c.wait(is_("sensor", addr=HR, state="waiting"), 3)
        self.eventually(lambda: self.cmd("dump").get("disconnect", 0) >= 1,
                        what="outstanding connection cancelled")
        count = self.cmd("dump")["connect"]
        time.sleep(2)
        dump = self.cmd("dump")
        self.assertFalse(dump["devices"][HR]["connected"])
        self.assertEqual(dump["connect"], count)
        c.send("hold on")
        c.wait(is_("sensor", addr=HR, state="connected"), 4)
        self.assertGreater(self.cmd("dump")["connect"], count)

    def test_adapter_off_on(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("adv %s -60" % HR)
        c = self.start()
        self.pair(c, HR)
        self.notify_until(c, HR, "hr", "0050", lambda m: m["value"] == 80, "80")
        c.send("scan on")
        c.wait(is_("adapter", scanning=True))
        m = c.lines.mark()
        self.cmd("power 0")
        c.wait_all(m, [is_("adapter", powered=False, scanning=False), is_("sensor", addr=HR, state="off"),
                       is_("value", addr=HR, value=None)])
        st = self.status()
        self.assertEqual((st["powered"], st["connected"]), (False, 0))
        m = c.lines.mark()
        self.cmd("power 1")
        c.wait_all(m, [is_("adapter", powered=True, scanning=True),	# the scan resumes
                       is_("sensor", addr=HR, state="connected")])
        self.notify_until(c, HR, "hr", "0051", lambda m: m["value"] == 81, "81")

    def test_scan_follows_clients(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("adv %s -60" % HR)
        c1 = self.start()
        c2 = self.client()
        c1.send("scan on")
        c1.wait(is_("adapter", scanning=True))
        c2.send("scan on")
        c2.wait(is_("ok", cmd="scan"))
        c1.close()
        self.clients.remove(c1)
        time.sleep(0.3)
        self.assertEqual(self.cmd("dump")["discovering"], 1)
        m = c2.lines.mark()
        c2.send("scan off")
        c2.wait_all(m, [is_("adapter", scanning=False), is_("lost", addr=HR)])
        self.assertEqual(self.cmd("dump")["discovering"], 0)
        # RSSI goes with the advertisement
        c2.send("scan on")
        c2.wait(is_("found", addr=HR))
        self.cmd("away " + HR)
        c2.wait(is_("lost", addr=HR))

    def test_forget_waits_for_both_durable_saves(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("adv %s -60" % HR)
        c = self.start()
        self.pair(c, HR)
        self.wait_file("records", lambda text: HR in text)
        for name in ("save-gate", "save-clean-fail", "save-clean-gate"):
            Path(self.tmp, name).touch()
        mark = c.lines.mark()
        c.send("forget " + HR)
        c.wait(is_("sensor", addr=HR, state="forgetting"))
        time.sleep(0.2)
        self.assertEqual(self.cmd("dump").get("remove_device", 0), 0)
        self.assertNotIn(" forget ", self.records())
        Path(self.tmp, "save-gate").unlink()
        self.wait_file("records", lambda text: " forget " in text)
        self.eventually(lambda: HR not in self.cmd("dump")["devices"], what="BlueZ removed")
        time.sleep(0.6)  # final save failed once and its retry is blocked
        self.assertIn(" forget ", self.records())
        watcher = self.client()
        watcher.wait(is_("sensor", addr=HR, state="forgetting"))
        self.assertFalse(any(is_("forgotten", addr=HR)(m) for m in c.lines.since(mark)))
        Path(self.tmp, "save-clean-gate").unlink()
        c.wait(is_("forgotten", addr=HR), 4)
        self.assertEqual(self.records(), "")  # saved before event, no polling
        self.assertEqual(self.status()["sensors"], 0)

    def test_forget_confirmed_and_retried(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("adv %s -60" % HR)
        c = self.start()
        self.pair(c, HR)
        self.cmd("fail RemoveDevice 2")
        c.send("forget " + HR)
        c.wait(is_("ok", cmd="forget"))
        c.wait(is_("sensor", addr=HR, state="forgetting"))
        self.wait_file("records", lambda t: " forget " in t)
        c.wait(is_("forgotten", addr=HR), 4)	# after two failures, 0.5 s apart
        self.assertEqual(self.cmd("dump")["remove_device"], 3)
        self.wait_file("records", lambda t: t == "")
        self.assertNotIn(HR, self.cmd("dump")["devices"])
        st = self.status()
        self.assertEqual(st["sensors"], 0)
        c.send("forget " + HR)
        self.assertEqual(c.wait(is_("error", cmd="forget"))["reason"], "not paired")

    def test_forget_survives_bluez_and_daemon_restarts(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("adv %s -60" % HR)
        c = self.start()
        self.pair(c, HR)
        # bluetoothd away: the removal waits for it
        self.cmd("release")
        c.wait(is_("adapter", present=False))
        c.send("forget " + HR)
        c.wait(is_("sensor", addr=HR, state="forgetting"))
        self.wait_file("records", lambda t: " forget " in t)
        # this daemon stops before it could remove; the next one goes on
        self.assertEqual(self.stop(), 0)
        self.cmd("fail RemoveDevice 1")
        self.cmd("acquire")
        c = self.start()
        c.wait(is_("sensor", addr=HR, state="forgetting"))
        c.wait(is_("forgotten", addr=HR), 4)
        self.wait_file("records", lambda t: t == "")
        self.assertNotIn(HR, self.cmd("dump")["devices"])

    def test_bluez_restart(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("adv %s -60" % HR)
        c = self.start()
        self.pair(c, HR)
        self.notify_until(c, HR, "hr", "0050", lambda m: m["value"] == 80, "80")
        self.cmd("release")
        c.wait(is_("adapter", present=False))
        c.wait(lambda m: m.get("type") == "sensor" and m.get("state") != "connected")
        self.cmd("acquire")
        c.wait(is_("sensor", addr=HR, state="connected"), 5)
        self.notify_until(c, HR, "hr", "0051", lambda m: m["value"] == 81, "81")
        dump = self.cmd("dump")
        self.eventually(lambda: self.cmd("dump")["agents"] == ["NoInputNoOutput"], what="ordinary agent restored")
        self.assertEqual(self.cmd("dump")["default_agents"], [])
        self.assertGreaterEqual(dump["register_agent"], 2)

    def test_forget_then_pair_into_the_same_slot(self):
        self.cmd("dev %s A hr" % HR)
        self.cmd("dev %s B hr" % HR2)
        self.cmd("adv %s -60" % HR)
        self.cmd("adv %s -60" % HR2)
        c = self.start()
        self.pair(c, HR)
        # A drops and its reconnect answers late; A is forgotten and B
        # paired into its slot before that answer
        self.cmd("delay Connect 1200")
        self.cmd("drop " + HR)
        c.wait(is_("sensor", addr=HR, state="connecting"))
        time.sleep(0.2)
        c.send("forget " + HR)
        c.wait(is_("forgotten", addr=HR))
        self.pair(c, HR2)
        time.sleep(1.3)
        self.notify_until(c, HR2, "hr", "0050", lambda m: m["value"] == 80, "B 80")
        st = self.status()
        self.assertEqual((st["sensors"], st["connected"]), (1, 1))
        self.assertNotIn(HR, self.records())

    def test_records_load_bounds_and_saves(self):
        with open(os.path.join(self.tmp, "records"), "w") as f:
            f.write("sensor %s hr 2026-10-13 - Strap 1\n"
                    "sensor aa:bb hr - - bad\n"
                    "sensor %s power,csc - - Meter\n" % (HR, COMBO))
        open(os.path.join(self.tmp, "load-desc"), "w").close()
        t0 = time.monotonic()
        c = self.start()
        s = c.wait(is_("sensor", addr=HR))
        self.assertLess(time.monotonic() - t0, 0.45)	# not held by the descendant
        self.assertEqual((s["paired"], s["name"], s["kinds"]), ("2026-10-13", "Strap 1", "hr"))
        c.wait(is_("sensor", addr=COMBO))
        pid = int(Path(os.path.join(self.tmp, "desc.pid")).read_text())
        time.sleep(0.1)
        self.assertFalse(os.path.exists("/proc/%d" % pid) and
                         Path("/proc/%d/stat" % pid).read_text().split()[2] != "Z", "descendant left")
        self.assertEqual(self.stop(), 0)
        # a loader that hangs (with a descendant): 5 s bound (0.5 s here)
        os.unlink(os.path.join(self.tmp, "load-desc"))
        open(os.path.join(self.tmp, "load-hang"), "w").close()
        c = self.start(wait_loaded=False)
        time.sleep(0.8)
        self.assertIn("gave no answer", self.log())
        self.assertEqual(self.status()["sensors"], 0)
        pid = int(Path(os.path.join(self.tmp, "desc.pid")).read_text())
        self.assertFalse(os.path.exists("/proc/%d" % pid) and
                         Path("/proc/%d/stat" % pid).read_text().split()[2] != "Z", "descendant left")
        self.assertEqual(self.stop(), 0)
        os.unlink(os.path.join(self.tmp, "load-hang"))

    def test_save_bounds_and_exit_save(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("dev %s HR2 hr" % HR2)
        self.cmd("adv %s -60" % HR)
        self.cmd("adv %s -60" % HR2)
        c = self.start()
        # a hung save (with a descendant) is killed at 10 s (1 s here) and
        # the newer records are saved after it
        open(os.path.join(self.tmp, "save-hang"), "w").close()
        self.pair(c, HR)
        self.pair(c, HR2)
        self.wait_file("records", lambda t: HR in t and HR2 in t, 4)
        self.assertIn("still running", self.log())
        pid = int(Path(os.path.join(self.tmp, "desc.pid")).read_text())
        self.assertFalse(os.path.exists("/proc/%d" % pid) and
                         Path("/proc/%d/stat" % pid).read_text().split()[2] != "Z", "descendant left")
        # a save whose leader leaves a descendant: the descendant goes too
        open(os.path.join(self.tmp, "save-desc"), "w").close()
        c.send("forget " + HR2)
        c.wait(is_("forgotten", addr=HR2))
        self.wait_file("records", lambda t: HR2 not in t)
        time.sleep(0.2)
        pid = int(Path(os.path.join(self.tmp, "desc.pid")).read_text())
        self.assertFalse(os.path.exists("/proc/%d" % pid) and
                         Path("/proc/%d/stat" % pid).read_text().split()[2] != "Z", "descendant left")
        # SIGTERM right after a change: the last save completes before exit
        c.send("forget " + HR)
        c.wait(is_("ok", cmd="forget"))
        c.wait(is_("forgotten", addr=HR))
        self.assertEqual(self.stop(), 0)
        self.assertEqual(self.records(), "")

    def test_cli_fragmented_json_lines(self):
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(self.sock)
        listener.listen(1)
        payload = (json.dumps({"type": "sensor", "addr": HR, "name": 'A "quoted" sensor'}) + "\n" +
                   '{"type":"ok","cmd":"list"}\n').encode()
        request = []

        def serve():
            with listener.accept()[0] as peer:
                data = b""
                while b"\n" not in data:
                    data += peer.recv(64)
                request.append(data)
                for off in range(0, len(payload), 7):
                    peer.sendall(payload[off:off + 7])
                    time.sleep(0.005)

        server = threading.Thread(target=serve)
        server.start()
        try:
            result = subprocess.run([BIN, "-S", self.sock, "list"], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, payload)
            self.assertEqual(json.loads(result.stdout.splitlines()[0])["name"], 'A "quoted" sensor')
        finally:
            server.join(5)
            listener.close()
        self.assertEqual(request, [b"list\n"])

    def test_cli(self):
        self.cmd("dev %s HR hr" % HR)
        self.cmd("adv %s -60" % HR)
        c = self.start()
        c.send("scan on")	# the device must be found to pair
        c.wait(is_("found", addr=HR))
        r = subprocess.run([BIN, "-S", self.sock, "pair", HR], capture_output=True, text=True, timeout=10)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        last = json.loads(r.stdout.strip().splitlines()[-1])
        self.assertEqual((last["type"], last["state"]), ("pair", "done"))
        r = subprocess.run([BIN, "-S", self.sock, "status"], capture_output=True, text=True, timeout=5)
        self.assertEqual(json.loads(r.stdout)["sensors"], 1)
        r = subprocess.run([BIN, "-S", self.sock, "list"], capture_output=True, text=True, timeout=5)
        lines = [json.loads(l) for l in r.stdout.splitlines()]
        self.assertEqual([l["type"] for l in lines], ["sensor", "ok"])
        r = subprocess.run([BIN, "-S", self.sock, "pair", HR], capture_output=True, text=True, timeout=5)
        self.assertEqual(json.loads(r.stdout.splitlines()[-1])["reason"], "already paired")
        self.assertEqual(r.returncode, 1)
        r = subprocess.run([BIN, "-S", self.sock, "forget", HR], capture_output=True, text=True, timeout=10)
        self.assertEqual(json.loads(r.stdout.splitlines()[-1]), {"type": "forgotten", "addr": HR})
        r = subprocess.run([BIN, "-S", self.sock, "pair", "nonsense"], capture_output=True, text=True, timeout=5)
        self.assertEqual(r.returncode, 64)


if __name__ == "__main__":
    unittest.main(verbosity=2)
