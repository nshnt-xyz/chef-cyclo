#!/usr/bin/env python3
"""A fake NetworkManager for tests/test_wifi.c, on the bus that
DBUS_SYSTEM_BUS_ADDRESS names (a private dbus-daemon, never the real
system bus). It implements the part of NetworkManager 1.52's D-Bus API
that sys/wifi.c and sys/netbus.c use: wlan0 with access points,
RequestScan/LastScan, saved profiles (Settings, GetSettings without
secrets, Delete), AddAndActivateConnection/ActivateConnection with a
device StateChanged sequence that ends ACTIVATED, or FAILED with reason 7
(no secrets) for a wrong passphrase, and IPv4 config.

Control on stdin, one command per line; answers on stdout:
  ap PATHNUM SSID SEC FREQ STRENGTH SECRET   add an AP (SEC open|wpa2|wpa3|mixed|eap)
  strength PATHNUM N                          change an AP's Strength
  saved SSID SECRET [UUID]                    add a saved profile
Commands given as arguments run before the name is taken (profiles that
NetworkManager loads at start).
  fail-next                                   the next activation fails (reason 11)
  sync-next                                   the next activation ends before its reply
  delete-fails N                              the next N Delete calls fail
  swap-on-lookup                              the next GetConnectionByUuid releases the
                                              name, prints "released" and answers only
                                              on "reply"; Delete then fails (this
                                              instance is on its way out)
  reply                                       send that held answer
  dump                                        one JSON line: profiles, counters
Prints "ready" when it owns the name.
"""
import json
import os
import sys

import dbus
import dbus.lowlevel
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

NM = "org.freedesktop.NetworkManager"
P = "/org/freedesktop/NetworkManager"
PROPS = "org.freedesktop.DBus.Properties"
DEV = P + "/Devices/3"

SEC_FLAGS = {  # (Flags, WpaFlags, RsnFlags)
    "open": (0, 0, 0),
    "wpa2": (1, 0, 0x188),
    "wpa3": (1, 0, 0x488),
    "mixed": (1, 0, 0x588),
    "eap": (1, 0, 0x288),
}

counters = {"scans": 0, "calls": 0, "autostart": 0, "add_activate": 0, "activate": 0,
            "deletes": 0, "delete_errors": 0, "delete_wellknown": 0, "delete_unique": 0}
fail_next = [False]
sync_next = [False]
delete_fails = [0]
swap_on_lookup = [False]
held_reply = [None]
frozen = [False]


class PropObject(dbus.service.Object):
    """An object with properties per interface and the Properties API."""

    def __init__(self, bus, path, props):
        super().__init__(bus, path)
        self.path = path
        self.props = props  # {iface: {name: dbus value}}

    def set_prop(self, iface, name, value, emit=True):
        self.props[iface][name] = value
        if emit:
            self.PropertiesChanged(iface, {name: value}, [])

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, iface, name):
        return self.props[iface][name]

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return dbus.Dictionary(self.props.get(iface, {}), signature="sv")

    @dbus.service.method(PROPS, in_signature="ssv")
    def Set(self, iface, name, value):
        if iface == NM and name == "WirelessEnabled":
            self.set_prop(iface, name, dbus.Boolean(value))
            return
        raise dbus.exceptions.DBusException("read-only", name=NM + ".PermissionDenied")

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


def ssid_bytes(s):
    return dbus.ByteArray(s.encode()) if isinstance(s, str) else dbus.ByteArray(bytes(s))


class AccessPoint(PropObject):
    def __init__(self, bus, num, ssid, sec, freq, strength, secret):
        f, w, r = SEC_FLAGS[sec]
        super().__init__(bus, P + "/AccessPoint/%d" % num, {NM + ".AccessPoint": {
            "Ssid": dbus.Array(ssid_bytes(ssid), signature="y"),
            "Flags": dbus.UInt32(f), "WpaFlags": dbus.UInt32(w), "RsnFlags": dbus.UInt32(r),
            "Frequency": dbus.UInt32(freq), "Strength": dbus.Byte(strength),
            "HwAddress": "00:11:22:33:44:%02X" % num, "Mode": dbus.UInt32(2),
        }})
        self.ssid = ssid
        self.secret = secret


class Connection(PropObject):
    def __init__(self, bus, num, settings, secret):
        super().__init__(bus, P + "/Settings/%d" % num, {NM + ".Settings.Connection": {
            "Unsaved": False, "Flags": dbus.UInt32(0), "Filename": "/run/x/%d" % num}})
        settings["connection"].setdefault("uuid", "f00d0000-0000-4000-8000-%012d" % num)
        self.settings = settings
        self.secret = secret

    def uuid(self):
        return str(self.settings["connection"]["uuid"])

    def ssid(self):
        return bytes(self.settings.get("802-11-wireless", {}).get("ssid", b"")).decode(
            errors="replace")

    @dbus.service.method(NM + ".Settings.Connection", out_signature="a{sa{sv}}")
    def GetSettings(self):
        out = {}
        for sec, vals in self.settings.items():
            # GetSettings never carries secrets
            out[sec] = dbus.Dictionary({k: v for k, v in vals.items() if k != "psk"},
                                       signature="sv")
        return dbus.Dictionary(out, signature="sa{sv}")

    @dbus.service.method(NM + ".Settings.Connection")
    def Delete(self):
        if frozen[0]:
            raise dbus.exceptions.DBusException("gone", name="org.freedesktop.DBus.Error.UnknownObject")
        if delete_fails[0] > 0:
            delete_fails[0] -= 1
            counters["delete_errors"] += 1
            raise dbus.exceptions.DBusException("busy", name=NM + ".Settings.Failed")
        counters["deletes"] += 1
        state.delete(self)

    @dbus.service.signal(NM + ".Settings.Connection")
    def Removed(self):
        pass


class Active(PropObject):
    def __init__(self, bus, num, conn, ap):
        super().__init__(bus, P + "/ActiveConnection/%d" % num, {NM + ".Connection.Active": {
            "Id": conn.settings["connection"]["id"], "Connection": dbus.ObjectPath(conn.path),
            "Uuid": conn.uuid(), "SpecificObject": dbus.ObjectPath(ap), "State": dbus.UInt32(1)}})
        self.conn = conn

    @dbus.service.signal(NM + ".Connection.Active", signature="uu")
    def StateChanged(self, st, reason):
        pass

    def set_state(self, st, reason=0):
        self.set_prop(NM + ".Connection.Active", "State", dbus.UInt32(st))
        self.StateChanged(dbus.UInt32(st), dbus.UInt32(reason))


class Ip4(PropObject):
    def __init__(self, bus, num, addr, gw):
        super().__init__(bus, P + "/IP4Config/%d" % num, {NM + ".IP4Config": {
            "AddressData": dbus.Array([dbus.Dictionary({"address": addr, "prefix": dbus.UInt32(24)},
                                                       signature="sv")], signature="a{sv}"),
            "Gateway": gw}})


class Device(PropObject):
    def __init__(self, bus):
        super().__init__(bus, DEV, {
            NM + ".Device": {"State": dbus.UInt32(30), "ActiveConnection": dbus.ObjectPath("/"),
                             "Ip4Config": dbus.ObjectPath("/"), "Interface": "wlan0"},
            NM + ".Device.Wireless": {"LastScan": dbus.Int64(1000),
                                      "ActiveAccessPoint": dbus.ObjectPath("/"),
                                      "AccessPoints": dbus.Array([], signature="o")},
        })

    @dbus.service.method(NM + ".Device.Wireless", out_signature="ao")
    def GetAllAccessPoints(self):
        return dbus.Array([dbus.ObjectPath(a.path) for a in state.aps.values()], signature="o")

    @dbus.service.method(NM + ".Device.Wireless", in_signature="a{sv}")
    def RequestScan(self, options):
        counters["scans"] += 1

        def done():
            last = self.props[NM + ".Device.Wireless"]["LastScan"]
            self.set_prop(NM + ".Device.Wireless", "LastScan", dbus.Int64(last + 1000))
            return False
        GLib.timeout_add(200, done)

    @dbus.service.signal(NM + ".Device", signature="uuu")
    def StateChanged(self, new, old, reason):
        pass

    @dbus.service.signal(NM + ".Device.Wireless", signature="o")
    def AccessPointAdded(self, path):
        pass

    def set_state(self, new, reason=0):
        old = self.props[NM + ".Device"]["State"]
        self.set_prop(NM + ".Device", "State", dbus.UInt32(new))
        self.StateChanged(dbus.UInt32(new), dbus.UInt32(old), dbus.UInt32(reason))


class Manager(PropObject):
    def __init__(self, bus):
        super().__init__(bus, P, {NM: {
            "WirelessEnabled": True, "Connectivity": dbus.UInt32(4),
            "ConnectivityCheckEnabled": False, "ConnectivityCheckAvailable": True,
            "Version": "1.52.2"}})

    @dbus.service.method(NM, in_signature="s", out_signature="o")
    def GetDeviceByIpIface(self, iface):
        if iface != "wlan0":
            raise dbus.exceptions.DBusException("no", name=NM + ".UnknownDevice")
        return dbus.ObjectPath(DEV)

    @dbus.service.method(NM, in_signature="a{sa{sv}}oo", out_signature="oo")
    def AddAndActivateConnection(self, settings, device, specific):
        counters["add_activate"] += 1
        plain = {k: dict(v) for k, v in settings.items()}
        sec = plain.get("802-11-wireless-security", {})
        conn = state.add_connection(plain, str(sec.get("psk", "")))
        state.last_add = {
            "id": str(plain["connection"]["id"]),
            "ssid": bytes(plain["802-11-wireless"]["ssid"]).decode(errors="replace"),
            "uuid": str(plain["connection"].get("uuid", "")),
            "hidden": bool(plain["802-11-wireless"].get("hidden", False)),
            "key_mgmt": str(sec.get("key-mgmt", "")), "psk": str(sec.get("psk", "")),
            "device": str(device), "specific": str(specific)}
        active = state.activate(conn, str(specific))
        return dbus.ObjectPath(conn.path), dbus.ObjectPath(active.path)

    @dbus.service.method(NM, in_signature="ooo", out_signature="o")
    def ActivateConnection(self, conn_path, device, specific):
        counters["activate"] += 1
        conn = state.conns.get(str(conn_path))
        if not conn:
            raise dbus.exceptions.DBusException("no", name=NM + ".UnknownConnection")
        return dbus.ObjectPath(state.activate(conn, str(specific)).path)


class Settings(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, P + "/Settings")

    @dbus.service.method(NM + ".Settings", in_signature="s", out_signature="o",
                         async_callbacks=("reply", "error"))
    def GetConnectionByUuid(self, uuid, reply, error):
        for c in state.conns.values():
            if c.uuid() == str(uuid):
                path = dbus.ObjectPath(c.path)
                if swap_on_lookup[0]:
                    swap_on_lookup[0] = False
                    frozen[0] = True
                    system.release_name(NM)
                    held_reply[0] = lambda: reply(path)
                    print("released", flush=True)
                    return
                reply(path)
                return
        error(dbus.exceptions.DBusException("No connection with the UUID was found.",
                                            name=NM + ".Settings.InvalidConnection"))

    @dbus.service.method(NM + ".Settings", out_signature="ao")
    def ListConnections(self):
        return dbus.Array([dbus.ObjectPath(p) for p in state.conns], signature="o")

    @dbus.service.signal(NM + ".Settings", signature="o")
    def NewConnection(self, path):
        pass

    @dbus.service.signal(NM + ".Settings", signature="o")
    def ConnectionRemoved(self, path):
        pass


class State:
    def __init__(self, bus):
        self.bus = bus
        self.n = 10
        self.aps = {}
        self.conns = {}
        self.active = None
        self.last_add = None
        self.nm = Manager(bus)
        self.dev = Device(bus)
        self.settings = Settings(bus)

    def num(self):
        self.n += 1
        return self.n

    def add_connection(self, settings, secret):
        c = Connection(self.bus, self.num(), settings, secret)
        self.conns[c.path] = c
        self.settings.NewConnection(dbus.ObjectPath(c.path))
        return c

    def delete(self, c):
        if self.active and self.active.conn is c:
            self.deactivate()
        del self.conns[c.path]
        c.Removed()
        self.settings.ConnectionRemoved(dbus.ObjectPath(c.path))
        c.remove_from_connection()

    def deactivate(self):
        a = self.active
        self.active = None
        a.set_state(3)
        a.set_state(4)
        self.dev.set_prop(NM + ".Device", "ActiveConnection", dbus.ObjectPath("/"))
        self.dev.set_prop(NM + ".Device", "Ip4Config", dbus.ObjectPath("/"))
        self.dev.set_prop(NM + ".Device.Wireless", "ActiveAccessPoint", dbus.ObjectPath("/"))
        self.dev.set_state(110)
        self.dev.set_state(30)
        a.remove_from_connection()

    def ap_for(self, ssid):
        for a in self.aps.values():
            if a.ssid == ssid:
                return a
        return None

    def activate(self, conn, specific):
        if self.active:
            self.deactivate()
        ssid = conn.ssid()
        ap = self.ap_for(ssid)
        active = Active(self.bus, self.num(), conn, specific)
        fail = fail_next[0]
        fail_next[0] = False
        sync = sync_next[0]
        sync_next[0] = False
        good = ap is not None and (ap.secret == "" or ap.secret == conn.secret) and not fail

        def steps():
            for st in (40, 50, 60):
                self.dev.set_state(st)
            if not good:
                # a refused handshake: NM asks for new secrets, no agent has any
                self.dev.set_state(120, 11 if fail else 7)
                active.set_state(4, 9 if not fail else 1)
                self.dev.set_state(30, 0)
                active.remove_from_connection()
                return False
            self.active = active
            ip = Ip4(self.bus, self.num(), "192.168.7.20", "192.168.7.1")
            self.dev.set_prop(NM + ".Device", "ActiveConnection", dbus.ObjectPath(active.path))
            self.dev.set_prop(NM + ".Device", "Ip4Config", dbus.ObjectPath(ip.path))
            self.dev.set_prop(NM + ".Device.Wireless", "ActiveAccessPoint",
                              dbus.ObjectPath(ap.path))
            self.dev.set_state(70)
            self.dev.set_state(100)
            active.set_state(2)
            return False
        if sync:
            steps()		# signals before the method's reply
        else:
            GLib.timeout_add(50, steps)
        return active


def message_filter(bus, msg):
    if msg.get_type() == 1 and msg.get_member() == "Delete":
        counters["delete_wellknown" if msg.get_destination() == NM else "delete_unique"] += 1
    if msg.get_type() == 1 and msg.get_destination() == NM:  # method call to us
        counters["calls"] += 1
        if msg.get_auto_start():
            counters["autostart"] += 1
    # let the objects answer it
    return dbus.lowlevel.HANDLER_RESULT_NOT_YET_HANDLED


def command(line):
    w = line.split()
    if not w:
        return
    if w[0] == "ap":
        num = int(w[1])
        ssid = w[2].replace("_", " ")
        secret = w[6] if len(w) > 6 and w[6] != "-" else ""
        state.aps[num] = AccessPoint(state.bus, num, ssid, w[3], int(w[4]), int(w[5]), secret)
        state.dev.AccessPointAdded(dbus.ObjectPath(state.aps[num].path))
    elif w[0] == "strength":
        state.aps[int(w[1])].set_prop(NM + ".AccessPoint", "Strength", dbus.Byte(int(w[2])))
    elif w[0] == "saved":
        ssid = w[1].replace("_", " ")
        con = {"id": ssid, "type": "802-11-wireless"}
        if len(w) > 3:
            con["uuid"] = w[3]
        state.add_connection({"connection": con,
                              "802-11-wireless": {"ssid": ssid_bytes(ssid)},
                              "802-11-wireless-security": {"key-mgmt": "wpa-psk"}}, w[2])
    elif w[0] == "fail-next":
        fail_next[0] = True
    elif w[0] == "sync-next":
        sync_next[0] = True
    elif w[0] == "delete-fails":
        delete_fails[0] = int(w[1])
    elif w[0] == "swap-on-lookup":
        swap_on_lookup[0] = True
    elif w[0] == "reply" and held_reply[0]:
        held_reply[0]()
        held_reply[0] = None
    elif w[0] == "dump":
        out = dict(counters)
        out["profiles"] = sorted(c.ssid() for c in state.conns.values())
        out["last_add"] = state.last_add
        out["wireless"] = bool(state.nm.props[NM]["WirelessEnabled"])
        out["active"] = state.active.conn.ssid() if state.active else ""
        out["paths"] = sorted(state.conns)
        out["uuids"] = sorted("%s=%s=%s" % (c.ssid(), c.uuid(), c.path)
                              for c in state.conns.values())
        print(json.dumps(out), flush=True)


pending = [b""]


def stdin_ready(fd, cond):
    # raw reads: a buffered readline would leave lines GLib never reports
    data = os.read(sys.stdin.fileno(), 4096)
    if not data:
        loop.quit()
        return False
    pending[0] += data
    while b"\n" in pending[0]:
        line, pending[0] = pending[0].split(b"\n", 1)
        command(line.decode())
    return True


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
system = dbus.SystemBus()
system.add_message_filter(message_filter)
state = State(system)
for arg in sys.argv[1:]:
    command(arg)
name = dbus.service.BusName(NM, system)
loop = GLib.MainLoop()
GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN | GLib.IO_HUP, stdin_ready)
print("ready", flush=True)
loop.run()
