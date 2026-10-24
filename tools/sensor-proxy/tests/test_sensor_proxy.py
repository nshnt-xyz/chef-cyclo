#!/usr/bin/env python3
"""End-to-end tests for sensor-proxy: the real binary (host build) on a
private dbus-daemon (never the system bus; DBUS_SYSTEM_BUS_ADDRESS points
at it) against a fake sensord (a Unix socket server in this process that
records every request line and sends the lines a test gives it), with
dbus-python clients, each its own bus name. Covered: the objects,
interfaces, properties and methods (introspection, Peer); claim and
release turning into sensord's claim and release lines exactly once, two
clients sharing a kind, a repeated claim, a release without a claim;
kill -9 of a client releasing; the values (orientation and tilt from the
six poses and the hysteresis band, undefined again after the last
accelerometer release, light with the cached first sample, proximity,
heading, -1 before it); PropertiesChanged only on a change; sensord going
away (every Has* false, one signal each) and coming back (list again,
claims sent again); list again every 5 s while SMGR is not ready; error
lines logged with the value kept; the bus going away (exit 1). Skipped
without dbus-daemon or python3 dbus + gi."""
import json
import os
import queue
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
BIN = os.path.join(HERE, "..", "build", "host", "sensor-proxy")
# The daemon logs to kmsg (which a non-root test cannot write) and, with
# -v, stderr; never to the host's /dev/log.
os.environ["CHEF_LOG_SOCKET"] = "/nonexistent/chef-log-socket"

try:
    import dbus
    import dbus.mainloop.glib
    from gi.repository import GLib
    HAVE = shutil.which("dbus-daemon") is not None
except ImportError:
    HAVE = False

NAME = "net.hadess.SensorProxy"
PATH = "/net/hadess/SensorProxy"
CPATH = "/net/hadess/SensorProxy/Compass"
CIFACE = "net.hadess.SensorProxy.Compass"
PROPS = "org.freedesktop.DBus.Properties"
G = 9.81


def list_line(ready=True, present=True, heading=None):
    """sensord's list reply (proto 2), the fields sensor-proxy reads plus a
    few it ignores."""
    sensors = [{"sensor": s, "unit": "x", "present": present, "rate": 0, "claims": 0}
               for s in ("accel", "anglvel", "magn", "illuminance", "proximity")]
    sensors.append({"sensor": "heading", "unit": "deg", "virtual": True,
                    "inputs": "accel 50 Hz, anglvel 50 Hz, magn 20 Hz",
                    "present": present if heading is None else heading})
    sensors.append({"sensor": "rotvec", "virtual": True, "present": False})
    return json.dumps({"type": "list", "smgr": "ready" if ready else "info", "sensors": sensors})


class FakeSensord:
    """sensord's socket: hello on connect, ok for claim/release, the list
    reply set by the test; every request line recorded."""

    def __init__(self, path):
        self.path = path
        self.requests = queue.Queue()
        self.list_reply = list_line()
        self.conn = None
        self.lock = threading.Lock()
        self.srv = None
        self.thread = None
        self.connects = 0

    def start(self):
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(self.path)
        self.srv.listen(4)
        self.running = True
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        srv = self.srv
        while self.running:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            with self.lock:
                self.conn = c
                self.connects += 1
            self.send('{"type":"hello","daemon":"sensord","proto":2}')
            buf = b""
            while True:
                try:
                    data = c.recv(4096)
                except OSError:
                    data = b""
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self.request(line.decode())
            with self.lock:
                if self.conn is c:
                    self.conn = None
            c.close()

    def request(self, line):
        self.requests.put(line)
        words = line.split()
        if words[0] == "list":
            self.send(self.list_reply)
        elif words[0] in ("claim", "release"):
            reply = {"type": "ok", "cmd": words[0], "sensor": words[1]}
            if words[0] == "claim":
                reply["rate"] = int(words[2]) if len(words) > 2 else 5
            self.send(json.dumps(reply))

    def send(self, line):
        with self.lock:
            if self.conn:
                try:
                    self.conn.sendall((line + "\n").encode())
                except OSError:
                    pass

    def stop(self):
        """sensord goes away: the listener and the connection closed."""
        self.running = False
        with self.lock:
            if self.conn:
                self.conn.shutdown(socket.SHUT_RDWR)
        self.srv.close()
        os.unlink(self.path)
        self.thread.join(5)

    def take(self, timeout=0.3):
        """Every request line seen within TIMEOUT."""
        lines, end = [], time.monotonic() + timeout
        while True:
            try:
                lines.append(self.requests.get(timeout=max(0, end - time.monotonic())))
            except queue.Empty:
                return lines

    def wait(self, line, timeout=5):
        end = time.monotonic() + timeout
        seen = []
        while time.monotonic() < end:
            try:
                got = self.requests.get(timeout=max(0, end - time.monotonic()))
            except queue.Empty:
                break
            seen.append(got)
            if got == line:
                return seen
        raise AssertionError(f"no {line!r} from sensor-proxy (saw {seen})")


@unittest.skipUnless(HAVE and os.path.exists(BIN), "needs dbus-daemon, python3 dbus + gi and build/host/sensor-proxy")
class SensorProxyTest(unittest.TestCase):
    def setUp(self):
        # every resource gets its cleanup as soon as it exists, so a setUp
        # that fails half way leaves no process behind (cleanups run last
        # registered first: the daemon, the clients, sensord, the bus)
        self.tmp = tempfile.mkdtemp(prefix="sp", dir="/tmp")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.bus = os.path.join(self.tmp, "bus")
        conf = os.path.join(self.tmp, "bus.conf")
        with open(conf, "w") as f:
            f.write('<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"\n'
                    ' "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">\n'
                    "<busconfig><type>session</type><listen>unix:path=%s</listen>"
                    '<policy context="default"><allow send_destination="*" eavesdrop="true"/>'
                    '<allow eavesdrop="true"/><allow own="*"/></policy></busconfig>\n' % self.bus)
        self.dbusd = subprocess.Popen(["dbus-daemon", "--nofork", "--nosyslog", "--config-file", conf],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop_proc, self.dbusd)
        for _ in range(200):
            if os.path.exists(self.bus):
                break
            time.sleep(0.01)
        self.address = "unix:path=" + self.bus
        self.env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=self.address)
        self.sock = os.path.join(self.tmp, "sensord.sock")
        self.sensord = FakeSensord(self.sock)
        self.sensord.start()
        self.addCleanup(lambda: self.sensord.running and self.sensord.stop())
        self.conns = []
        self.addCleanup(lambda: [c.close() for c in self.conns])
        self.signals = []
        self.loop = dbus.mainloop.glib.DBusGMainLoop()
        self.watcher = self.connect()
        self.watcher.add_signal_receiver(self.on_signal, "PropertiesChanged", PROPS, None, None,
                                         path_keyword="path")
        self.log = []
        self.d = None
        self.start_daemon()

    @staticmethod
    def stop_proc(proc, sig=signal.SIGKILL):
        if proc.poll() is None:
            proc.send_signal(sig)
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    # ------------------------------------------------------------ helpers

    def connect(self):
        c = dbus.bus.BusConnection(self.address, mainloop=self.loop)
        self.conns.append(c)
        return c

    def start_daemon(self):
        self.d = subprocess.Popen([BIN, "-S", self.sock, "-v"], env=self.env, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.d.stderr.close)
        self.addCleanup(self.stop_proc, self.d, signal.SIGTERM)
        threading.Thread(target=self.read_log, args=(self.d.stderr,), daemon=True).start()
        self.sensord.wait("list")
        self.pump(lambda: self.watcher.name_has_owner(NAME), 5, "the name")
        self.pump(lambda: self.get("HasAccelerometer"), 5, "the list reply")
        self.signals.clear()

    def read_log(self, f):
        for line in f:
            self.log.append(line.rstrip("\n"))

    def on_signal(self, iface, changed, invalidated, path=None):
        for k, v in changed.items():
            self.signals.append((str(path), str(iface), str(k), v))

    def pump(self, cond, timeout=5, what="condition"):
        ctx = GLib.MainContext.default()
        end = time.monotonic() + timeout
        while True:
            while ctx.pending():
                ctx.iteration(False)
            if cond():
                return
            if time.monotonic() > end:
                raise AssertionError(f"timed out waiting for {what}")
            time.sleep(0.01)

    def settle(self, secs=0.3):
        """Let everything in flight arrive."""
        end = time.monotonic() + secs
        ctx = GLib.MainContext.default()
        while time.monotonic() < end:
            while ctx.pending():
                ctx.iteration(False)
            time.sleep(0.01)

    def obj(self, conn=None, path=PATH):
        return (conn or self.watcher).get_object(NAME, path, introspect=False)

    def get(self, prop, conn=None):
        compass = prop in ("HasCompass", "CompassHeading")
        return self.obj(conn, CPATH if compass else PATH).Get(CIFACE if compass else NAME, prop,
                                                               dbus_interface=PROPS)

    def call(self, conn, method):
        path, iface = (CPATH, CIFACE) if method.endswith("Compass") else (PATH, NAME)
        return self.obj(conn, path).get_dbus_method(method, iface)()

    def sample(self, sensor, **values):
        self.sensord.send(json.dumps({"type": "sample", "sensor": sensor, "t": 1, **values}))

    def changes(self, settle=0.3):
        self.settle(settle)
        got = [(p, k, v) for p, _, k, v in self.signals]
        self.signals.clear()
        return got

    # -------------------------------------------------------------- tests

    def test_interface(self):
        xml = self.obj().Introspect(dbus_interface="org.freedesktop.DBus.Introspectable")
        for want in ('<interface name="net.hadess.SensorProxy">',
                     '<property name="HasAccelerometer" type="b" access="read">',
                     '<property name="AccelerometerOrientation" type="s" access="read">',
                     '<property name="AccelerometerTilt" type="s" access="read">',
                     '<property name="HasAmbientLight" type="b" access="read">',
                     '<property name="LightLevelUnit" type="s" access="read">',
                     '<property name="LightLevel" type="d" access="read">',
                     '<property name="HasProximity" type="b" access="read">',
                     '<property name="ProximityNear" type="b" access="read">',
                     '<method name="ClaimAccelerometer">', '<method name="ReleaseAccelerometer">',
                     '<method name="ClaimLight">', '<method name="ReleaseLight">',
                     '<method name="ClaimProximity">', '<method name="ReleaseProximity">',
                     '<node name="Compass"/>', '<interface name="org.freedesktop.DBus.Peer">'):
            self.assertIn(want, xml)
        self.assertEqual(xml.count("<property "), 8)
        # every property emits changes: sd-bus writes the annotation only
        # when it is not the default (true)
        self.assertNotIn("EmitsChangedSignal", xml)
        cxml = self.obj(path=CPATH).Introspect(dbus_interface="org.freedesktop.DBus.Introspectable")
        for want in ('<interface name="net.hadess.SensorProxy.Compass">',
                     '<property name="HasCompass" type="b" access="read">',
                     '<property name="CompassHeading" type="d" access="read">',
                     '<method name="ClaimCompass">', '<method name="ReleaseCompass">'):
            self.assertIn(want, cxml)
        self.assertEqual(cxml.count("<property "), 2)
        self.assertNotIn("EmitsChangedSignal", cxml)
        self.obj().Ping(dbus_interface="org.freedesktop.DBus.Peer")
        # the values before any claim
        all_ = self.obj().GetAll(NAME, dbus_interface=PROPS)
        self.assertEqual(dict(all_), {"HasAccelerometer": True, "AccelerometerOrientation": "undefined",
                                      "AccelerometerTilt": "undefined", "HasAmbientLight": True,
                                      "LightLevelUnit": "lux", "LightLevel": 0.0, "HasProximity": True,
                                      "ProximityNear": False})
        self.assertEqual(dict(self.obj(path=CPATH).GetAll(CIFACE, dbus_interface=PROPS)),
                         {"HasCompass": True, "CompassHeading": -1.0})
        # methods take no arguments and return nothing
        a = self.connect()
        self.assertIsNone(self.call(a, "ClaimLight"))
        self.assertIsNone(self.call(a, "ReleaseLight"))

    def test_claims_once_per_kind(self):
        for claim, release, line in (("ClaimAccelerometer", "ReleaseAccelerometer", "accel 10"),
                                     ("ClaimLight", "ReleaseLight", "illuminance"),
                                     ("ClaimProximity", "ReleaseProximity", "proximity"),
                                     ("ClaimCompass", "ReleaseCompass", "heading")):
            with self.subTest(claim=claim):
                a, b = self.connect(), self.connect()
                self.call(a, release)			# no claim: a no-op
                self.assertEqual(self.sensord.take(), [])
                self.call(a, claim)
                self.assertEqual(self.sensord.take(), ["claim " + line])
                self.call(a, claim)			# the same name again: nothing
                self.call(b, claim)			# a second name: nothing
                self.assertEqual(self.sensord.take(), [])
                self.call(a, release)			# b still claims it
                self.call(a, release)			# twice: still a no-op
                self.assertEqual(self.sensord.take(), [])
                self.call(b, release)
                self.assertEqual(self.sensord.take(), ["release " + line.split()[0]])
                # one release ends a doubled claim
                self.call(a, claim)
                self.call(a, claim)
                self.call(a, release)
                self.assertEqual(self.sensord.take(), ["claim " + line, "release " + line.split()[0]])
                # a client closing its connection drops its claims
                self.call(b, claim)
                self.assertEqual(self.sensord.take(), ["claim " + line])
                b.close()
                self.assertEqual(self.sensord.take(0.5), ["release " + line.split()[0]])
        self.assertTrue(any("sent claim accel 10" in l for l in self.log), self.log)
        self.assertTrue(any("sent release heading" in l for l in self.log), self.log)

    def test_kill9_releases(self):
        client = subprocess.Popen([sys.executable, "-c", (
            "import dbus, sys, time\n"
            "c = dbus.bus.BusConnection(sys.argv[1])\n"
            "c.get_object('net.hadess.SensorProxy', '/net/hadess/SensorProxy', introspect=False)"
            ".ClaimAccelerometer(dbus_interface='net.hadess.SensorProxy')\n"
            "print('claimed', flush=True)\n"
            "time.sleep(60)\n"), self.address], stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(client.stdout.readline().strip(), "claimed")
            self.assertEqual(self.sensord.take(), ["claim accel 10"])
            client.kill()
            client.wait()
            self.assertEqual(self.sensord.take(1), ["release accel"])
        finally:
            if client.poll() is None:
                client.kill()
                client.wait()
            client.stdout.close()

    def test_orientation_and_tilt(self):
        a = self.connect()
        self.call(a, "ClaimAccelerometer")
        self.sensord.take()
        # the six poses (sensord's Android frame)
        poses = [((0, G, 0), "normal", "vertical"), ((0, -G, 0), "bottom-up", "vertical"),
                 ((-G, 0, 0), "left-up", "vertical"), ((G, 0, 0), "right-up", "vertical"),
                 ((0, 0, G), "right-up", "face-up"), ((0, 0, -G), "right-up", "face-down")]
        for (x, y, z), orient, tilt in poses:
            self.sample("accel", x=x, y=y, z=z)
            self.pump(lambda: (self.get("AccelerometerOrientation"), self.get("AccelerometerTilt")) == (orient, tilt),
                      2, f"{orient} {tilt}")
        self.changes()
        # the same pose again: no signal at all
        self.sample("accel", x=0, y=0, z=-G)
        self.assertEqual(self.changes(), [])
        # upright: both change, one signal each
        self.sample("accel", x=0, y=G, z=0.1)
        self.assertCountEqual(self.changes(), [(PATH, "AccelerometerOrientation", "normal"),
                                               (PATH, "AccelerometerTilt", "vertical")])
        # within the band (landscape angle 30 degrees, screen tilted up): orientation kept
        self.sample("accel", x=0, y=G * 0.5, z=G * 0.866)
        self.assertEqual(self.changes(), [(PATH, "AccelerometerTilt", "tilted-up")])
        self.assertEqual(self.get("AccelerometerOrientation"), "normal")
        # the last release: both undefined again; later samples ignored
        self.call(a, "ReleaseAccelerometer")
        self.assertCountEqual(self.changes(), [(PATH, "AccelerometerOrientation", "undefined"),
                                               (PATH, "AccelerometerTilt", "undefined")])
        self.sample("accel", x=0, y=G, z=0)
        self.assertEqual(self.changes(), [])
        self.assertEqual(self.get("AccelerometerOrientation"), "undefined")

    def test_light_proximity_compass(self):
        a = self.connect()
        # samples of kinds nobody claims are ignored
        self.sample("illuminance", illuminance=55.0)
        self.sample("heading", heading=12.0)
        self.assertEqual(self.changes(), [])
        self.call(a, "ClaimLight")
        self.call(a, "ClaimProximity")
        self.call(a, "ClaimCompass")
        # the first light sample is sensord's cached reading: published
        self.sample("illuminance", illuminance=250.5)
        self.sample("proximity", near=1, proximity=762)
        self.sample("heading", heading=88.13, pitch=2.9, roll=-5.8, calibrated=True)
        self.assertCountEqual(self.changes(), [(CPATH, "CompassHeading", 88.13), (PATH, "LightLevel", 250.5),
                                               (PATH, "ProximityNear", True)])
        # unchanged values: no signal
        self.sample("illuminance", illuminance=250.5)
        self.sample("proximity", near=1, proximity=748)
        self.sample("heading", heading=88.13)
        self.assertEqual(self.changes(), [])
        self.sample("proximity", near=0, proximity=762)
        self.sample("illuminance", illuminance=0.0)
        self.assertCountEqual(self.changes(), [(PATH, "LightLevel", 0.0), (PATH, "ProximityNear", False)])
        # releases keep the last values
        self.call(a, "ReleaseLight")
        self.call(a, "ReleaseProximity")
        self.call(a, "ReleaseCompass")
        self.assertEqual(self.changes(), [])
        self.assertEqual((self.get("LightLevel"), self.get("ProximityNear"), self.get("CompassHeading")),
                         (0.0, False, 88.13))

    def test_sensord_away_and_back(self):
        a = self.connect()
        self.call(a, "ClaimAccelerometer")
        self.call(a, "ClaimCompass")
        self.assertEqual(self.sensord.take(), ["claim accel 10", "claim heading"])
        self.sensord.stop()
        self.pump(lambda: len(self.signals) >= 4, 3, "Has* false")
        self.assertCountEqual(self.changes(0.5), [(CPATH, "HasCompass", False), (PATH, "HasAccelerometer", False),
                                                  (PATH, "HasAmbientLight", False), (PATH, "HasProximity", False)])
        # claims and releases while it is away stay on the bus side
        self.call(a, "ClaimLight")
        self.call(a, "ReleaseCompass")
        self.sensord = FakeSensord(self.sock)
        self.sensord.start()
        self.assertEqual(self.sensord.take(3), ["list", "claim accel 10", "claim illuminance"])
        self.pump(lambda: len(self.signals) >= 4, 3, "Has* true")
        self.assertCountEqual(self.changes(), [(CPATH, "HasCompass", True), (PATH, "HasAccelerometer", True),
                                               (PATH, "HasAmbientLight", True), (PATH, "HasProximity", True)])
        self.assertTrue(any("sensord connection lost" in l for l in self.log), self.log)

    def test_list_until_smgr_ready(self):
        self.sensord.stop()
        self.changes(0.5)
        self.sensord = FakeSensord(self.sock)
        self.sensord.list_reply = list_line(ready=False, present=False)
        self.sensord.start()
        self.sensord.wait("list", 3)
        self.settle(0.5)
        self.assertFalse(self.get("HasAccelerometer"))
        self.assertEqual(self.changes(), [])
        # 5 s later it asks again; this time SMGR is ready (heading not present)
        self.sensord.list_reply = list_line(heading=False)
        t0 = time.monotonic()
        self.sensord.wait("list", 7)
        self.assertGreater(time.monotonic() - t0, 3.5)
        self.pump(lambda: self.get("HasAccelerometer"), 2, "present")
        self.assertFalse(self.get("HasCompass"))
        self.assertCountEqual(self.changes(), [(PATH, "HasAccelerometer", True), (PATH, "HasAmbientLight", True),
                                               (PATH, "HasProximity", True)])
        # ready: no more lists
        self.assertEqual(self.sensord.take(6), [])

    def test_error_line_logged(self):
        a = self.connect()
        self.call(a, "ClaimLight")
        self.sample("illuminance", illuminance=40.0)
        self.pump(lambda: self.get("LightLevel") == 40.0, 2, "light")
        self.changes()
        self.sensord.send('{"type":"error","reason":"smgr refused report","sensor":"illuminance"}')
        self.pump(lambda: any("sensord: smgr refused report illuminance" in l for l in self.log), 2, "the log line")
        self.assertEqual(self.get("LightLevel"), 40.0)
        self.assertEqual(self.changes(), [])
        # unknown types and members are ignored
        self.sensord.send('{"type":"dropped","count":3}')
        self.sensord.send('{"type":"whatever","x":1}')
        self.sample("illuminance", illuminance=41.0, extra=[1, 2])
        self.assertEqual(self.changes(), [(PATH, "LightLevel", 41.0)])
        self.assertIsNone(self.d.poll())

    def test_bus_gone_exits(self):
        self.dbusd.kill()
        self.dbusd.wait()
        self.assertEqual(self.d.wait(5), 1)
        self.assertTrue(any("system bus lost" in l for l in self.log), self.log)

    def test_name_taken(self):
        # a second instance does not get the name and exits non-zero
        second = subprocess.run([BIN, "-S", self.sock], env=self.env, capture_output=True, timeout=15)
        self.assertEqual(second.returncode, 1)


if __name__ == "__main__":
    unittest.main()
