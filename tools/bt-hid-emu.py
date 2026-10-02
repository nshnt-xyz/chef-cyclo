#!/usr/bin/env python3
# Host-side test peer: a classic HID keyboard emulated on a desktop BlueZ
# adapter, for checking the phone as an HID host (docs/features/bluetooth.md).
#
# The desktop's own bluetoothd keeps PSM 17/19 (its input plugin listens
# there) and owns UUID 0x1124 as a profile key, so this does what a
# "virtually cabled" keyboard does when it reconnects: publish an HID SDP
# record through a record-only Profile1 under a private UUID (no listener),
# then connect out to the host's PSM 17 (control) and 19 (interrupt). Pair
# the phone with the desktop while this runs, so the phone sees the record,
# and trust the desktop on the phone. Never let the phone initiate the HID
# connection: the desktop's input plugin answers unknown devices with a
# virtual-cable unplug.
#
# usage: bt-hid-emu.py PHONE_MAC
#   Each line on stdin runs one round: connect, type a b c a (press and
#   release), disconnect. EOF unregisters the record and exits.
# Needs python3-dbus and python3-gi; runs as an ordinary desktop user.
import socket
import struct
import sys
import threading
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

PROFILE_UUID = 'c4ef0001-3a1b-4e7c-9d2a-6c79636c6f00'  # key only; the record is HID
# Boot keyboard, no report IDs: 8 modifier bits, reserved byte, 5 LED bits
# (output), 6 key slots.
REPORT_MAP = ('05010906a101050719e029e715002501750195088102950175088101'
              '9505750105081901290591029501750391019506750815002565050719002965'
              '8100c0')
RECORD = f'''<?xml version="1.0" encoding="UTF-8" ?>
<record>
  <attribute id="0x0001"><sequence><uuid value="0x1124" /></sequence></attribute>
  <attribute id="0x0004"><sequence>
    <sequence><uuid value="0x0100" /><uint16 value="0x0011" /></sequence>
    <sequence><uuid value="0x0011" /></sequence></sequence></attribute>
  <attribute id="0x0005"><sequence><uuid value="0x1002" /></sequence></attribute>
  <attribute id="0x0006"><sequence><uint16 value="0x656e" /><uint16 value="0x006a" /><uint16 value="0x0100" /></sequence></attribute>
  <attribute id="0x0009"><sequence><sequence><uuid value="0x1124" /><uint16 value="0x0101" /></sequence></sequence></attribute>
  <attribute id="0x000d"><sequence><sequence>
    <sequence><uuid value="0x0100" /><uint16 value="0x0013" /></sequence>
    <sequence><uuid value="0x0011" /></sequence></sequence></sequence></attribute>
  <attribute id="0x0100"><text value="chef-cyclo test keyboard" /></attribute>
  <attribute id="0x0101"><text value="Emulated HID keyboard" /></attribute>
  <attribute id="0x0102"><text value="chef-cyclo" /></attribute>
  <attribute id="0x0200"><uint16 value="0x0100" /></attribute>
  <attribute id="0x0201"><uint16 value="0x0111" /></attribute>
  <attribute id="0x0202"><uint8 value="0x40" /></attribute>
  <attribute id="0x0203"><uint8 value="0x00" /></attribute>
  <attribute id="0x0204"><boolean value="false" /></attribute>
  <attribute id="0x0205"><boolean value="true" /></attribute>
  <attribute id="0x0206"><sequence><sequence><uint8 value="0x22" /><text encoding="hex" value="{REPORT_MAP}" /></sequence></sequence></attribute>
  <attribute id="0x0207"><sequence><sequence><uint16 value="0x0409" /><uint16 value="0x0100" /></sequence></sequence></attribute>
  <attribute id="0x020b"><uint16 value="0x0100" /></attribute>
  <attribute id="0x020c"><uint16 value="0x0c80" /></attribute>
  <attribute id="0x020d"><boolean value="false" /></attribute>
  <attribute id="0x020e"><boolean value="true" /></attribute>
</record>'''
# HID usages a b c a: evdev KEY_A (30), KEY_B (48), KEY_C (46), KEY_A.
# Nothing a console would act on.
KEYS = [0x04, 0x05, 0x06, 0x04]
SOL_BLUETOOTH, BT_SECURITY, BT_SECURITY_MEDIUM = 274, 4, 2


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


class Profile(dbus.service.Object):
    @dbus.service.method('org.bluez.Profile1', in_signature='', out_signature='')
    def Release(self):
        log('profile released')

    @dbus.service.method('org.bluez.Profile1', in_signature='oha{sv}', out_signature='')
    def NewConnection(self, dev, fd, props):
        log('unexpected NewConnection from', dev)
        socket.socket(fileno=fd.take()).close()

    @dbus.service.method('org.bluez.Profile1', in_signature='o', out_signature='')
    def RequestDisconnection(self, dev):
        log('RequestDisconnection', dev)


def l2cap_connect(local, phone, psm):
    s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_L2CAP)
    # Encrypted link with the bonded key; HID hosts refuse unencrypted input.
    s.setsockopt(SOL_BLUETOOTH, BT_SECURITY, struct.pack('BB', BT_SECURITY_MEDIUM, 0))
    s.bind((local, 0))
    s.settimeout(20)
    s.connect((phone, psm))
    s.settimeout(0.2)
    return s


def serve(name, s, stop):
    # Answer control-channel requests: GET_REPORT with an empty input report,
    # GET_PROTOCOL with "report", SET_REPORT/SET_PROTOCOL/SET_IDLE with
    # HANDSHAKE SUCCESSFUL. HID_CONTROL and DATA need no reply.
    while not stop.is_set():
        try:
            d = s.recv(64)
        except socket.timeout:
            continue
        except OSError as e:
            log(name, 'recv error', e)
            return
        if not d:
            log(name, 'closed by host')
            return
        log(name, 'rx', d.hex())
        if name != 'ctrl':
            continue
        t = d[0] >> 4
        if t == 0x4:
            s.send(bytes([0xa1, 0, 0, 0, 0, 0, 0, 0, 0]))
        elif t == 0x6:
            s.send(bytes([0xa0, 0x01]))
        elif t in (0x5, 0x7, 0x9):
            s.send(bytes([0x00]))


def round_(n, local, phone):
    log(f'round {n}: connecting control (PSM 17)')
    stop = threading.Event()
    socks, threads = [], []
    try:
        socks.append(l2cap_connect(local, phone, 17))
        socks.append(l2cap_connect(local, phone, 19))
        ctrl, intr = socks
        log('control and interrupt connected')
        threads = [threading.Thread(target=serve, args=a, daemon=True)
                   for a in (('ctrl', ctrl, stop), ('intr', intr, stop))]
        for t in threads:
            t.start()
        time.sleep(4)  # the host creates and opens the input device
        for k in KEYS:
            intr.send(bytes([0xa1, 0, 0, k, 0, 0, 0, 0, 0]))
            time.sleep(0.15)
            intr.send(bytes([0xa1, 0, 0, 0, 0, 0, 0, 0, 0]))
            time.sleep(0.6)
            log('typed usage', hex(k))
        time.sleep(2)
    finally:
        # Also on a failed round: never leave the host with a dangling
        # control channel. Interrupt closes first, as a device disconnects.
        stop.set()
        for t in threads:
            t.join(1)
        for s in reversed(socks):
            s.close()
            time.sleep(0.3)
        log(f'round {n}: disconnected')


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: bt-hid-emu.py PHONE_MAC')
    phone = sys.argv[1].upper()
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    local = str(dbus.Interface(bus.get_object('org.bluez', '/org/bluez/hci0'),
                               'org.freedesktop.DBus.Properties').Get('org.bluez.Adapter1', 'Address'))
    Profile(bus, '/chef/hidemu')
    mgr = dbus.Interface(bus.get_object('org.bluez', '/org/bluez'), 'org.bluez.ProfileManager1')
    mgr.RegisterProfile('/chef/hidemu', PROFILE_UUID,
                        {'Name': 'chef-hid-emu', 'Role': 'server', 'ServiceRecord': RECORD,
                         'RequireAuthentication': True, 'RequireAuthorization': False})
    log(f'HID record registered on {local}; one stdin line per round, EOF to quit')
    loop = GLib.MainLoop()
    threading.Thread(target=loop.run, daemon=True).start()
    n = 0
    try:
        for _ in sys.stdin:
            n += 1
            try:
                round_(n, local, phone)
            except OSError as e:
                log(f'round {n} failed: {e!r}')
    finally:
        mgr.UnregisterProfile('/chef/hidemu')
        loop.quit()
        log('record unregistered')


if __name__ == '__main__':
    main()
