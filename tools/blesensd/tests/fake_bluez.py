#!/usr/bin/env python3
"""A fake bluetoothd for tests/test_blesensd.py, on the bus that
DBUS_SYSTEM_BUS_ADDRESS names (a private dbus-daemon, never the real
system bus). It implements the part of BlueZ 5.8x's D-Bus API that
blesensd uses: hci0 (Powered, Discovering, SetDiscoveryFilter, Start/
StopDiscovery per client, RemoveDevice), LE devices (Connect, Disconnect,
Pair through the caller's agent, CancelPairing, Trusted, RSSI only while
discovering, Battery1), GATT characteristics with per-client notification
sessions that survive a disconnect and end when the client leaves the bus
(as BlueZ does), and AgentManager1.

Control on stdin, one command per line; each answers one line on stdout
("ok CMD" unless noted):
  dev ADDR NAME KINDS [bond] [battery1=N] [gattbat=N] [gapname=NAME] [noresolve]
                         a stored LE device (KINDS hr,power,csc); bond: its
                         measurements need a bond; NAME _ for spaces
  adv ADDR RSSI          it advertises (in range); RSSI shows while discovering
  away ADDR              out of range: stops advertising, disconnects
  drop ADDR              the sensor ends the connection (stays in range)
  notify ADDR KIND HEX   a notification on that measurement ("ok" or "dropped")
  battery ADDR N         Battery1 Percentage
  power 0|1              adapter Powered
  delay METHOD MS        the next METHOD call answers MS later
  fail METHOD N          the next N METHOD calls fail
  agent METHOD ADDR      call the registered agent: "agent METHOD ok|rejected"
  release / acquire      give up / take the org.bluez name (a restart)
  dump                   one JSON line of the state and counters
Prints "ready" when it owns the name.
"""
import json
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BLUEZ = "org.bluez"
PROPS = "org.freedesktop.DBus.Properties"
OM = "org.freedesktop.DBus.ObjectManager"
ADAPTER = "/org/bluez/hci0"
AD_IF = BLUEZ + ".Adapter1"
DEV_IF = BLUEZ + ".Device1"
CHR_IF = BLUEZ + ".GattCharacteristic1"
BAT_IF = BLUEZ + ".Battery1"
SVC = {"hr": "180d", "power": "1818", "csc": "1816", "battery": "180f"}
CHR = {"hr": "2a37", "power": "2a63", "csc": "2a5b", "battery": "2a19"}


def uuid(short):
    return "0000%s-0000-1000-8000-00805f9b34fb" % short


def err(name, msg=""):
    return dbus.exceptions.DBusException(msg, name=BLUEZ + ".Error." + name)


counters = {}
delays = {}
fails = {}


def count(name, n=1):
    counters[name] = counters.get(name, 0) + n


def take_delay(method):
    return delays.pop(method, 0)


def take_fail(method):
    if fails.get(method, 0) > 0:
        fails[method] -= 1
        return True
    return False


def later(ms, fn):
    def run():
        fn()
        return False
    if ms:
        GLib.timeout_add(ms, run)
    else:
        GLib.idle_add(run)


class PropObject(dbus.service.Object):
    def __init__(self, bus, path, props):
        super().__init__(bus, path)
        self.path = path
        self.props = props

    def set_prop(self, iface, name, value):
        if self.props[iface].get(name) == value and name in self.props[iface]:
            return
        self.props[iface][name] = value
        self.PropertiesChanged(iface, {name: value}, [])

    def drop_prop(self, iface, name):
        if name in self.props[iface]:
            del self.props[iface][name]
            self.PropertiesChanged(iface, {}, [name])

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, iface, name):
        return self.props[iface][name]

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, iface):
        return dbus.Dictionary(self.props.get(iface, {}), signature="sv")

    @dbus.service.method(PROPS, in_signature="ssv")
    def Set(self, iface, name, value):
        if iface == DEV_IF and name == "Trusted":
            count("set_trusted")
            if take_fail("Trusted"):
                raise err("Failed", "trust")
            self.set_prop(iface, name, dbus.Boolean(value))
            return
        if iface == AD_IF and name == "Powered":
            state.set_powered(bool(value))
            return
        raise err("NotPermitted", name)

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, iface, changed, invalidated):
        pass

    def managed(self):
        return {i: dbus.Dictionary(p, signature="sv") for i, p in self.props.items()}


class Root(dbus.service.Object):
    @dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        count("get_objects")
        out = {ADAPTER: state.adapter.managed()}
        for d in state.devs.values():
            out[d.path] = d.managed()
            for c in d.chrs.values():
                out[c.path] = c.managed()
        return out

    @dbus.service.signal(OM, signature="oa{sa{sv}}")
    def InterfacesAdded(self, path, ifaces):
        pass

    @dbus.service.signal(OM, signature="oas")
    def InterfacesRemoved(self, path, ifaces):
        pass


class AgentManager(dbus.service.Object):
    @dbus.service.method(BLUEZ + ".AgentManager1", in_signature="os", sender_keyword="sender")
    def RegisterAgent(self, path, cap, sender=None):
        count("register_agent")
        if take_fail("RegisterAgent"):
            raise err("Failed", "agent")
        if sender in state.agents:
            raise err("AlreadyExists")
        state.agents[sender] = (str(path), str(cap))

    @dbus.service.method(BLUEZ + ".AgentManager1", in_signature="o", sender_keyword="sender")
    def UnregisterAgent(self, path, sender=None):
        count("unregister_agent")
        if take_fail("UnregisterAgent"):
            raise err("Failed", "agent removal")
        state.agents.pop(sender, None)
        state.defaults = [name for name in state.defaults if name != sender]

    @dbus.service.method(BLUEZ + ".AgentManager1", in_signature="o", sender_keyword="sender")
    def RequestDefaultAgent(self, path, sender=None):
        count("default_agent")
        if sender not in state.agents:
            raise err("DoesNotExist")
        state.defaults = [name for name in state.defaults if name != sender] + [sender]


class Adapter(PropObject):
    def __init__(self, bus):
        super().__init__(bus, ADAPTER, {AD_IF: {
            "Address": "00:11:22:33:44:55", "Powered": dbus.Boolean(True),
            "Discovering": dbus.Boolean(False)}})

    @dbus.service.method(AD_IF, in_signature="a{sv}", sender_keyword="sender")
    def SetDiscoveryFilter(self, f, sender=None):
        count("set_filter")
        state.filters[sender] = {"transport": str(f.get("Transport", "auto")),
                                 "uuids": sorted(str(u) for u in f.get("UUIDs", []))}

    @dbus.service.method(AD_IF, sender_keyword="sender")
    def StartDiscovery(self, sender=None):
        count("start_discovery")
        if not self.props[AD_IF]["Powered"]:
            raise err("NotReady")
        if sender in state.discovering:
            raise err("InProgress")
        state.discovering.add(sender)
        state.sync_discovery()

    @dbus.service.method(AD_IF, sender_keyword="sender")
    def StopDiscovery(self, sender=None):
        count("stop_discovery")
        if sender not in state.discovering:
            raise err("Failed", "No discovery started")
        state.discovering.discard(sender)
        state.sync_discovery()

    @dbus.service.method(AD_IF, in_signature="o", async_callbacks=("ok", "fail"))
    def RemoveDevice(self, path, ok, fail):
        count("remove_device")
        d = state.by_path(str(path))
        ms = take_delay("RemoveDevice")
        if take_fail("RemoveDevice"):
            later(ms, lambda: fail(err("Failed", "busy")))
            return
        if not d:
            later(ms, lambda: fail(err("DoesNotExist")))
            return

        def done():
            if state.by_path(d.path):
                state.remove(d)
            ok()
        later(ms, done)


class Chr(PropObject):
    def __init__(self, bus, dev, kind, idx):
        path = "%s/service%04x/char%04x" % (dev.path, 0x10 + idx * 0x10, 0x11 + idx * 0x10)
        flags = ["read", "notify"] if kind == "battery" else ["notify"]
        super().__init__(bus, path, {CHR_IF: {
            "UUID": uuid(CHR[kind]), "Service": dbus.ObjectPath(path.rsplit("/", 1)[0]),
            "Flags": dbus.Array(flags, signature="s"), "Notifying": dbus.Boolean(False),
            # a stale cached value, as BlueZ keeps one
            "Value": dbus.Array([0x00, 0x99] if kind == "hr" else [0] * 4, signature="y")}})
        self.dev = dev
        self.kind = kind
        self.sessions = set()

    @dbus.service.method(CHR_IF, sender_keyword="sender", async_callbacks=("ok", "fail"))
    def StartNotify(self, sender=None, ok=None, fail=None):
        count("start_notify")
        count("start_notify_" + self.kind)
        ms = take_delay("StartNotify")
        d = self.dev
        if take_fail("StartNotify"):
            later(ms, lambda: fail(err("Failed", "notify")))
            return
        if d.needs_bond and not d.props[DEV_IF]["Paired"] and self.kind != "battery":
            later(ms, lambda: fail(err("NotPermitted", "Insufficient authentication")))
            return

        def done():
            self.sessions.add(sender)
            self.set_prop(CHR_IF, "Notifying", dbus.Boolean(True))
            ok()
        later(ms, done)

    @dbus.service.method(CHR_IF, sender_keyword="sender")
    def StopNotify(self, sender=None):
        self.sessions.discard(sender)
        if not self.sessions:
            self.set_prop(CHR_IF, "Notifying", dbus.Boolean(False))

    @dbus.service.method(CHR_IF, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        count("read_" + self.kind)
        if not self.dev.props[DEV_IF]["Connected"]:
            raise err("Failed", "Not connected")
        if self.kind == "battery":
            return dbus.Array([self.dev.gatt_battery], signature="y")
        return self.props[CHR_IF]["Value"]

    def push(self, data):
        self.props[CHR_IF]["Value"] = dbus.Array(data, signature="y")
        self.PropertiesChanged(CHR_IF, {"Value": dbus.Array(data, signature="y")}, [])


class Device(PropObject):
    def __init__(self, bus, addr, name, kinds, opts):
        path = ADAPTER + "/dev_" + addr.replace(":", "_")
        uuids = [uuid(SVC[k]) for k in kinds] + [uuid(SVC["battery"])]
        props = {DEV_IF: {
            "Address": addr, "AddressType": "public", "Alias": name, "Name": name,
            "Connected": dbus.Boolean(False), "ServicesResolved": dbus.Boolean(False),
            "Paired": dbus.Boolean(False), "Bonded": dbus.Boolean(False),
            "Trusted": dbus.Boolean(False), "UUIDs": dbus.Array(uuids, signature="s"),
            "Adapter": dbus.ObjectPath(ADAPTER)}}
        self.battery1 = None
        for o in opts:
            if o.startswith("battery1="):
                self.battery1 = int(o.split("=")[1])
        if self.battery1 is not None:
            props[BAT_IF] = {"Percentage": dbus.Byte(self.battery1)}
        super().__init__(bus, path, props)
        self.addr = addr
        self.kinds = kinds
        self.needs_bond = "bond" in opts or "connectbond" in opts
        self.connect_bond = "connectbond" in opts
        self.resolve = "noresolve" not in opts
        self.gatt_battery = 77
        self.gapname = None
        for o in opts:
            if o.startswith("gapname="):
                self.gapname = o.split("=", 1)[1].replace("_", " ")
            if o.startswith("gattbat="):
                self.gatt_battery = int(o.split("=")[1])
        self.in_range = False
        self.rssi = None
        self.chrs = {}
        self.pending_pair = None
        self.connecting = False
        self.connect_gen = 0

    def export_chrs(self):
        if self.chrs:
            return
        for i, k in enumerate(self.kinds + ["battery"]):
            if k == "battery" and self.battery1 is not None:
                continue	# BlueZ's battery plugin claims it
            c = Chr(self._connection, self, k, i)
            self.chrs[k] = c
            state.root.InterfacesAdded(c.path, c.managed())

    def disconnect(self):
        self.connecting = False
        self.connect_gen += 1	# a connection being made is abandoned
        self.set_prop(DEV_IF, "ServicesResolved", dbus.Boolean(False))
        self.set_prop(DEV_IF, "Connected", dbus.Boolean(False))

    @dbus.service.method(DEV_IF, sender_keyword="sender", async_callbacks=("ok", "fail"))
    def Connect(self, sender=None, ok=None, fail=None):
        count("connect")
        ms = take_delay("Connect")
        if self.props[DEV_IF]["Connected"]:
            later(ms, ok)
            return
        if self.connecting:
            later(ms, lambda: fail(err("InProgress")))
            return
        self.connecting = True
        gen = self.connect_gen

        def done():
            self.connecting = False
            if gen != self.connect_gen:
                fail(err("Failed", "le-connection-abort-by-local"))
            elif not state.adapter.props[AD_IF]["Powered"]:
                fail(err("NotReady"))
            elif not self.in_range or take_fail("Connect"):
                count("connect_failed")
                fail(err("Failed", "le-connection-abort-by-local"))
            else:
                self.set_prop(DEV_IF, "Connected", dbus.Boolean(True))
                if self.connect_bond and not self.props[DEV_IF]["Paired"]:
                    fail(err("AuthenticationFailed", "GATT profile needs encryption"))
                    return
                ok()
                if self.resolve:
                    later(50, self.resolved)
        later(ms or 50, done)

    def resolved(self):
        if not self.props[DEV_IF]["Connected"]:
            return
        self.export_chrs()
        if self.gapname:	# BlueZ reads the GAP name once connected
            self.set_prop(DEV_IF, "Alias", self.gapname)
        self.set_prop(DEV_IF, "ServicesResolved", dbus.Boolean(True))

    @dbus.service.method(DEV_IF)
    def Disconnect(self):
        count("disconnect")
        self.disconnect()

    @dbus.service.method(DEV_IF, sender_keyword="sender", async_callbacks=("ok", "fail"))
    def Pair(self, sender=None, ok=None, fail=None):
        count("pair")
        if self.props[DEV_IF]["Paired"]:
            fail(err("AlreadyExists"))
            return
        agent = state.agents.get(sender)
        if not agent:
            fail(err("AuthenticationFailed", "no agent"))
            return
        self.pending_pair = fail
        ms = take_delay("Pair")

        def answered():
            if self.pending_pair is None:
                return	# cancelled
            self.pending_pair = None
            self.set_prop(DEV_IF, "Paired", dbus.Boolean(True))
            if self.connect_bond:
                self.resolved()
            self.set_prop(DEV_IF, "Bonded", dbus.Boolean(True))
            ok()

        def refused(e):
            if self.pending_pair is None:
                return
            self.pending_pair = None
            fail(err("AuthenticationRejected", str(e)))

        def ask():
            obj = self._connection.get_object(sender, agent[0], introspect=False)
            obj.RequestConfirmation(dbus.ObjectPath(self.path), dbus.UInt32(123456),
                                    dbus_interface=BLUEZ + ".Agent1",
                                    reply_handler=answered, error_handler=refused)
        later(ms, ask)

    @dbus.service.method(DEV_IF)
    def CancelPairing(self):
        count("cancel_pairing")
        if self.pending_pair:
            f, self.pending_pair = self.pending_pair, None
            f(err("AuthenticationCanceled"))

    def managed(self):
        return {i: dbus.Dictionary(p, signature="sv") for i, p in self.props.items()}


class State:
    def __init__(self, bus):
        self.bus = bus
        self.root = Root(bus, "/")
        self.agentmgr = AgentManager(bus, "/org/bluez")
        self.adapter = Adapter(bus)
        self.devs = {}
        self.agents = {}
        self.defaults = []
        self.discovering = set()
        self.filters = {}
        self.name = None

    def by_path(self, p):
        for d in self.devs.values():
            if d.path == p:
                return d
        return None

    def matches(self, d):
        """Would a discovering client's filter report this device?"""
        for s in self.discovering:
            f = self.filters.get(s, {"transport": "auto", "uuids": []})
            if f["transport"] not in ("le", "auto"):
                continue
            if not f["uuids"] or set(f["uuids"]) & set(str(u) for u in d.props[DEV_IF]["UUIDs"]):
                return True
        return False

    def sync_discovery(self):
        self.adapter.set_prop(AD_IF, "Discovering", dbus.Boolean(bool(self.discovering)))
        for d in self.devs.values():
            self.show_rssi(d)

    def show_rssi(self, d):
        if self.discovering and d.in_range and self.matches(d):
            d.set_prop(DEV_IF, "RSSI", dbus.Int16(d.rssi))
        else:
            d.drop_prop(DEV_IF, "RSSI")

    def remove(self, d):
        d.disconnect()
        for c in d.chrs.values():
            c.sessions.clear()
            self.root.InterfacesRemoved(c.path, [CHR_IF])
            c.remove_from_connection()
        self.root.InterfacesRemoved(d.path, list(d.props))
        d.remove_from_connection()
        del self.devs[d.addr]

    def set_powered(self, on):
        if not on:
            for d in self.devs.values():
                if d.props[DEV_IF]["Connected"]:
                    d.disconnect()
            self.discovering.clear()
            self.sync_discovery()
        self.adapter.set_prop(AD_IF, "Powered", dbus.Boolean(on))

    def client_gone(self, name):
        self.agents.pop(name, None)
        self.defaults = [sender for sender in self.defaults if sender != name]
        if name in self.discovering:
            self.discovering.discard(name)
            self.sync_discovery()
        self.filters.pop(name, None)
        for d in self.devs.values():
            for c in d.chrs.values():
                if name in c.sessions:
                    c.sessions.discard(name)
                    if not c.sessions:
                        c.set_prop(CHR_IF, "Notifying", dbus.Boolean(False))


def owner_changed(name, old, new):
    if old and not new and name == old:
        state.client_gone(name)


def command(line):
    w = line.split()
    if not w:
        return
    c = w[0]
    if c == "dev":
        d = Device(state.bus, w[1], w[2].replace("_", " "), w[3].split(","), w[4:])
        state.devs[d.addr] = d
        state.root.InterfacesAdded(d.path, d.managed())
    elif c == "remove":
        state.remove(state.devs[w[1]])
    elif c == "adv":
        d = state.devs[w[1]]
        d.in_range = True
        d.rssi = int(w[2])
        state.show_rssi(d)
    elif c == "away":
        d = state.devs[w[1]]
        d.in_range = False
        state.show_rssi(d)
        if d.props[DEV_IF]["Connected"]:
            d.disconnect()
    elif c == "drop":
        state.devs[w[1]].disconnect()
    elif c == "notify":
        d = state.devs[w[1]]
        ch = d.chrs.get(w[2])
        if not ch or not ch.sessions or not d.props[DEV_IF]["Connected"]:
            print("dropped " + line, flush=True)
            return
        ch.push(list(bytes.fromhex(w[3])))
    elif c == "battery":
        d = state.devs[w[1]]
        d.set_prop(BAT_IF, "Percentage", dbus.Byte(int(w[2])))
    elif c == "power":
        state.set_powered(w[1] == "1")
    elif c == "delay":
        delays[w[1]] = int(w[2])
    elif c == "fail":
        fails[w[1]] = int(w[2])
    elif c == "agent":
        d = state.devs.get(w[2])
        if not state.agents:
            print("agent %s none" % w[1], flush=True)
            return
        sender, (path, _) = next(iter(state.agents.items()))
        obj = state.bus.get_object(sender, path, introspect=False)
        dpath = dbus.ObjectPath(d.path if d else ADAPTER + "/dev_00_00_00_00_00_00")
        args = {"RequestConfirmation": (dpath, dbus.UInt32(1)), "RequestAuthorization": (dpath,),
                "AuthorizeService": (dpath, uuid("180d"))}[w[1]]
        getattr(obj, w[1])(*args, dbus_interface=BLUEZ + ".Agent1",
                           reply_handler=lambda: print("agent %s ok" % w[1], flush=True),
                           error_handler=lambda e: print("agent %s rejected" % w[1], flush=True))
        return
    elif c == "release":
        # bluetoothd stops: sessions, agents and discovery are gone
        for d in state.devs.values():
            if d.props[DEV_IF]["Connected"]:
                d.disconnect()
        for name in list(state.agents) + list(state.discovering):
            state.client_gone(name)
        for d in state.devs.values():
            for ch in d.chrs.values():
                ch.sessions.clear()
        state.bus.release_name(BLUEZ)
        state.name = None
    elif c == "acquire":
        state.name = dbus.service.BusName(BLUEZ, state.bus)
    elif c == "dump":
        out = dict(counters)
        out["devices"] = {a: {"paired": bool(d.props[DEV_IF]["Paired"]),
                              "trusted": bool(d.props[DEV_IF]["Trusted"]),
                              "connected": bool(d.props[DEV_IF]["Connected"]),
                              "sessions": {k: len(ch.sessions) for k, ch in d.chrs.items()}}
                          for a, d in state.devs.items()}
        out["agents"] = sorted(cap for _, cap in state.agents.values())
        out["agent_senders"] = sorted(state.agents)
        out["default_agents"] = list(state.defaults)
        out["discovering"] = len(state.discovering)
        out["filters"] = list(state.filters.values())
        out["powered"] = bool(state.adapter.props[AD_IF]["Powered"])
        print(json.dumps(out), flush=True)
        return
    print("ok " + line, flush=True)


pending = [b""]


def stdin_ready(fd, cond):
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
system.add_signal_receiver(owner_changed, "NameOwnerChanged", "org.freedesktop.DBus",
                           "org.freedesktop.DBus", "/org/freedesktop/DBus")
state = State(system)
for arg in sys.argv[1:]:
    command(arg)
state.name = dbus.service.BusName(BLUEZ, system)
loop = GLib.MainLoop()
GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN | GLib.IO_HUP, stdin_ready)
print("ready", flush=True)
loop.run()
