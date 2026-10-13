#!/usr/bin/env python3
"""Emulated BLE cycling sensors on this PC's Bluetooth adapter, for testing
blesensd on the phone without real sensors (docs/features/bluetooth.md,
"BLE sensors"). One BlueZ GATT application and one LE advertisement:

  hr     Heart Rate 0x180D: Heart Rate Measurement 0x2A37 (8-bit, 1 Hz)
  power  Cycling Power 0x1818: Measurement 0x2A63 with crank revolution data
  csc    Cycling Speed and Cadence 0x1816: Measurement 0x2A5B with wheel and
         crank revolution data (25 km/h at a 2105 mm wheel, 85 rpm)
  and Battery 0x180F: Battery Level 0x2A19 (read, notify)

The values change every second (heart rate 70 to 129 and back, power
around 200 W). An adapter is one device identity, so the services the
--kinds option names are all on one device, as on a combo power meter;
blesensd keeps it as one sensor with several kinds.

The advertisement is a discoverable LE instance. Keep the PC adapter
not globally discoverable so BlueZ sets its BR/EDR-not-supported flag.
The phone must discover it over LE only (blesensd's scan does). If the
phone and this PC were ever bonded as classic (BR/EDR) devices, remove that
bond on both sides first, or the phone connects over BR/EDR and fails.
BlueZ can stop advertising after a connection; this script registers the
advertisement again whenever none is active and no central is connected.

Usage: scripts/ble-sensor-emu.py [--kinds hr,power,csc] [--name CHEF-SENSOR]
       [--adapter hci0] [--bond --central PHONE_ADDR]
--bond uses encrypt-notify on measurements so the central must establish
an encrypted LE link; --central scopes its temporary NoInputNoOutput
default agent to that phone. Pairable and the previous default agent are
restored when the emulator exits.
Runs until Ctrl-C or SIGTERM. Needs python3 dbus + gi and a powered BlueZ
adapter with LE peripheral support; no root needed.
"""
import argparse
import math
import re
import signal
import struct
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

BLUEZ = "org.bluez"
GATT_MGR = "org.bluez.GattManager1"
ADV_MGR = "org.bluez.LEAdvertisingManager1"
PROPS = "org.freedesktop.DBus.Properties"
OM = "org.freedesktop.DBus.ObjectManager"
SVC_IF = "org.bluez.GattService1"
CHR_IF = "org.bluez.GattCharacteristic1"
ROOT = "/chef/sensoremu"

SVC = {"hr": "180d", "power": "1818", "csc": "1816", "battery": "180f"}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class BondAgent(dbus.service.Object):
    """Temporary test agent, accepting only BlueZ and the named central."""
    def __init__(self, bus, adapter, central):
        self.owner = str(bus.get_name_owner(BLUEZ))
        self.central = adapter + "/dev_" + central.replace(":", "_")
        self.path = ROOT + "/agent"
        super().__init__(bus, self.path)

    def check(self, device, sender):
        if str(sender) != self.owner or str(device) != self.central:
            raise dbus.exceptions.DBusException("Not the test central", name=BLUEZ + ".Error.Rejected")

    @dbus.service.method(BLUEZ + ".Agent1", in_signature="o", sender_keyword="sender")
    def RequestAuthorization(self, device, sender=None):
        self.check(device, sender)
        log("bond authorization for test central")

    @dbus.service.method(BLUEZ + ".Agent1", in_signature="ou", sender_keyword="sender")
    def RequestConfirmation(self, device, passkey, sender=None):
        self.check(device, sender)
        log("bond confirmation for test central")

    @dbus.service.method(BLUEZ + ".Agent1", in_signature="os", sender_keyword="sender")
    def AuthorizeService(self, device, uuid, sender=None):
        self.check(device, sender)

    @dbus.service.method(BLUEZ + ".Agent1")
    def Cancel(self):
        pass

    @dbus.service.method(BLUEZ + ".Agent1")
    def Release(self):
        pass


class Application(dbus.service.Object):
    def __init__(self, bus):
        self.path = ROOT
        self.services = []
        super().__init__(bus, ROOT)

    @dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        out = {}
        for s in self.services:
            out[s.path] = s.props()
            for c in s.chars:
                out[c.path] = c.props()
        return out


class Service(dbus.service.Object):
    def __init__(self, bus, index, uuid):
        self.path = "%s/service%d" % (ROOT, index)
        self.uuid = uuid
        self.chars = []
        super().__init__(bus, self.path)

    def props(self):
        return {SVC_IF: {"UUID": self.uuid, "Primary": True,
                         "Characteristics": dbus.Array([c.path for c in self.chars], signature="o")}}


class Characteristic(dbus.service.Object):
    def __init__(self, bus, service, index, uuid, flags, value):
        self.path = "%s/char%d" % (service.path, index)
        self.service = service
        self.uuid = uuid
        self.flags = flags
        self.value = value
        self.notifying = False
        service.chars.append(self)
        super().__init__(bus, self.path)

    def props(self):
        return {CHR_IF: {"Service": dbus.ObjectPath(self.service.path), "UUID": self.uuid,
                         "Flags": dbus.Array(self.flags, signature="s")}}

    @dbus.service.method(CHR_IF, in_signature="a{sv}", out_signature="ay")
    def ReadValue(self, options):
        return dbus.Array(self.value, signature="y")

    @dbus.service.method(CHR_IF)
    def StartNotify(self):
        if not self.notifying:
            log("notify on", self.uuid)
        self.notifying = True

    @dbus.service.method(CHR_IF)
    def StopNotify(self):
        if self.notifying:
            log("notify off", self.uuid)
        self.notifying = False

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    def push(self, value):
        self.value = list(value)
        if self.notifying:
            self.PropertiesChanged(CHR_IF, {"Value": dbus.Array(self.value, signature="y")}, [])


class Advertisement(dbus.service.Object):
    def __init__(self, bus, name, uuids):
        self.path = ROOT + "/adv0"
        self.name = name
        self.uuids = uuids
        super().__init__(bus, self.path)

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return {"Type": "peripheral", "Discoverable": dbus.Boolean(True), "ServiceUUIDs": dbus.Array(self.uuids, signature="s"),
                "LocalName": self.name}

    @dbus.service.method("org.bluez.LEAdvertisement1")
    def Release(self):
        log("advertisement released")


class Rider:
    """Cumulative revolutions and 1/1024 s event times, as sensors count."""

    def __init__(self):
        self.t0 = time.monotonic()
        self.crank = 0
        self.crank_t = 0.0
        self.wheel = 0
        self.wheel_t = 0.0

    def step(self, now, cadence_rpm, speed_kmh, circ_m=2.105):
        # whole revolutions up to now, each with its own event time
        period = 60.0 / cadence_rpm
        while self.crank_t + period <= now - self.t0:
            self.crank_t += period
            self.crank += 1
        wperiod = circ_m / (speed_kmh / 3.6)
        while self.wheel_t + wperiod <= now - self.t0:
            self.wheel_t += wperiod
            self.wheel += 1

    def crank_data(self):
        return struct.pack("<HH", self.crank & 0xFFFF, int(self.crank_t * 1024) & 0xFFFF)

    def wheel_data(self):
        return struct.pack("<IH", self.wheel & 0xFFFFFFFF, int(self.wheel_t * 1024) & 0xFFFF)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--kinds", default="hr,power,csc")
    ap.add_argument("--name", default="CHEF-SENSOR")
    ap.add_argument("--bond", action="store_true", help="require encrypted measurement notifications")
    ap.add_argument("--central", help="phone address allowed to bond (required with --bond)")
    ap.add_argument("--adapter", default="hci0")
    args = ap.parse_args()
    if args.bond and (not args.central or not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", args.central)):
        ap.error("--bond requires --central PHONE_ADDR")
    kinds = [k for k in args.kinds.split(",") if k]
    if not kinds or any(k not in ("hr", "power", "csc") for k in kinds):
        ap.error("--kinds: hr, power and/or csc")

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    apath = "/org/bluez/" + args.adapter
    adapter = bus.get_object(BLUEZ, apath)
    aprops = dbus.Interface(adapter, PROPS)
    aprops.Set("org.bluez.Adapter1", "Powered", dbus.Boolean(True))

    agent = None
    original_pairable = None
    if args.bond:
        agent = BondAgent(bus, apath, args.central.upper())
        agentmgr = dbus.Interface(bus.get_object(BLUEZ, "/org/bluez"), BLUEZ + ".AgentManager1")
        original_pairable = aprops.Get("org.bluez.Adapter1", "Pairable")
        agentmgr.RegisterAgent(agent.path, "NoInputNoOutput")
        agentmgr.RequestDefaultAgent(agent.path)
        aprops.Set("org.bluez.Adapter1", "Pairable", dbus.Boolean(True))

    app = Application(bus)
    chars = {}
    for i, k in enumerate(kinds + ["battery"]):
        s = Service(bus, i, SVC[k])
        app.services.append(s)
        if k == "hr":
            chars[k] = Characteristic(bus, s, 0, "2a37", ["encrypt-notify"] if args.bond else ["notify"], [0, 70])
            Characteristic(bus, s, 1, "2a38", ["read"], [1])	# body sensor location: chest
        elif k == "power":
            chars[k] = Characteristic(bus, s, 0, "2a63", ["encrypt-notify"] if args.bond else ["notify"], [0, 0, 0, 0])
            Characteristic(bus, s, 1, "2a65", ["read"], [0x08, 0, 0, 0])	# feature: crank data
            Characteristic(bus, s, 2, "2a5d", ["read"], [6])	# sensor location: left crank
        elif k == "csc":
            chars[k] = Characteristic(bus, s, 0, "2a5b", ["encrypt-notify"] if args.bond else ["notify"], [0])
            Characteristic(bus, s, 1, "2a5c", ["read"], [0x03, 0])	# feature: wheel + crank
        else:
            chars[k] = Characteristic(bus, s, 0, "2a19", ["read", "notify"], [90])
    adv = Advertisement(bus, args.name, [SVC[k] for k in kinds])
    loop = GLib.MainLoop()
    gatt = dbus.Interface(adapter, GATT_MGR)
    advm = dbus.Interface(adapter, ADV_MGR)
    state = {"advertising": False, "registering": False}

    def fatal(e):
        log("error:", e)
        loop.quit()

    def register_adv():
        if state["registering"]:
            return
        state["registering"] = True

        def ok():
            state["registering"] = False
            state["advertising"] = True
            log("advertising %s (%s)" % (args.name, ",".join(kinds)))

        def bad(e):
            state["registering"] = False
            log("advertisement:", e)
        advm.RegisterAdvertisement(adv.path, {}, reply_handler=ok, error_handler=bad)

    def centrals():
        om = dbus.Interface(bus.get_object(BLUEZ, "/"), OM)
        n = 0
        for path, ifaces in om.GetManagedObjects().items():
            d = ifaces.get("org.bluez.Device1")
            if d and path.startswith(apath + "/") and d.get("Connected"):
                n += 1
        return n

    def watchdog():
        # BlueZ may drop the advertisement after a connection: register it
        # again when nothing is advertising and no central is connected
        try:
            active = int(aprops.Get(ADV_MGR, "ActiveInstances"))
            if active == 0 and not state["registering"] and centrals() == 0:
                if state["advertising"]:
                    try:
                        advm.UnregisterAdvertisement(adv.path)
                    except dbus.exceptions.DBusException:
                        pass
                    state["advertising"] = False
                    log("advertisement gone: registering again")
                register_adv()
        except dbus.exceptions.DBusException as e:
            log("watchdog:", e)
        return True

    gatt.RegisterApplication(app.path, {}, reply_handler=lambda: log("GATT application registered"),
                             error_handler=fatal)
    register_adv()

    rider = Rider()
    tick = [0]

    def update():
        tick[0] += 1
        t = tick[0]
        now = time.monotonic()
        cadence = 85 + 5 * math.sin(t / 20.0)
        speed = 25 + 3 * math.sin(t / 30.0)
        rider.step(now, cadence, speed)
        if "hr" in chars:
            bpm = 70 + (t % 120 if t % 120 < 60 else 120 - t % 120)
            chars["hr"].push([0x00, bpm])
        if "power" in chars:
            watts = int(200 + 60 * math.sin(t / 10.0))
            chars["power"].push(struct.pack("<Hh", 0x20, watts) + rider.crank_data())
        if "csc" in chars:
            chars["csc"].push(bytes([0x03]) + rider.wheel_data() + rider.crank_data())
        if t % 60 == 0:
            chars["battery"].push([max(5, 90 - t // 60)])
        return True

    GLib.timeout_add(1000, update)
    GLib.timeout_add(2000, watchdog)
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: loop.quit())
    loop.run()
    try:
        advm.UnregisterAdvertisement(adv.path)
        gatt.UnregisterApplication(app.path)
    except dbus.exceptions.DBusException:
        pass
    if agent:
        try:
            agentmgr.UnregisterAgent(agent.path)
            aprops.Set("org.bluez.Adapter1", "Pairable", original_pairable)
        except dbus.exceptions.DBusException as e:
            log("agent cleanup:", e)
    log("stopped")


if __name__ == "__main__":
    sys.exit(main())
