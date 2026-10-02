#!/usr/bin/env python3
"""Run the real DHCP hook with fake network commands and private runtime paths."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'initramfs/usr/bin/wifi-dhcp').read_text()
with tempfile.TemporaryDirectory(prefix='wifi-test-') as directory:
    root = Path(directory)
    (root / 'wifi').mkdir()
    commands = root / 'commands'
    commands.mkdir()
    for name in ('ip', 'ifconfig'):
        tool = commands / name
        tool.write_text('#!/bin/sh\nprintf "%s\\n" "' + name + ' $*" >> "$TRACE"\n[ "${FAIL_ROUTE:-0}" != 1 ] || [ "$1" != route ] || exit 1\n')
        tool.chmod(0o755)
    hook = root / 'hook'
    hook.write_text(source.replace('/run/wifi', str(root / 'wifi')).replace('/etc/resolv.conf', str(root / 'resolver')))
    env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'], TRACE=str(root / 'trace'), interface='wlan0', ip='192.168.1.10', subnet='255.255.255.0', router='192.168.1.1', dns='1.1.1.1')
    def run(**changes):
        for path in (root / 'wifi').iterdir():
            path.unlink()
        (root / 'trace').write_text('')
        return subprocess.run(['sh', str(hook), 'bound'], env=dict(env, **changes), capture_output=True).returncode
    assert run() == 0
    lines = (root / 'trace').read_text().splitlines()
    assert lines.index('ip route flush dev wlan0') < lines.index('ifconfig wlan0 192.168.1.10 netmask 255.255.255.0') < lines.index('ip route add default via 192.168.1.1 dev wlan0 metric 100')
    assert (root / 'wifi/dhcp.ready').exists()
    assert (root / 'resolver').read_text() == 'nameserver 1.1.1.1\n'
    for address, mask in [('172.16.42.1', '255.255.255.0'), ('172.16.43.1', '255.255.254.0'), ('10.0.0.1', '0.0.0.0'), ('10.0.0.1', '255.0.255.0')]:
        assert run(ip=address, subnet=mask) != 0
        assert (root / 'trace').read_text() == ''
        assert (root / 'wifi/dhcp.failed').exists()
    assert run(FAIL_ROUTE='1') != 0
    assert (root / 'wifi/dhcp.failed').exists()
    assert not (root / 'wifi/dhcp.ready').exists()
    assert run(interface='usb0') != 0
    assert (root / 'trace').read_text() == ''
print('Wi-Fi DHCP routing, USB overlap, failure propagation: PASS')

# Exercise the actual session resolver save/restore blocks for both boot states.
up = (Path(__file__).resolve().parents[2] / 'initramfs/usr/bin/wifi-up').read_text()
save = up[up.index('if [ -e /etc/resolv.conf ]; then'):up.index("printf 'ctrl_interface=")]
restore = up[up.index('    if [ -f "$RUN/resolv.conf" ]; then'):up.index('    rm -rf "$RUN"')]
with tempfile.TemporaryDirectory(prefix='wifi-resolver-test-') as directory:
    root = Path(directory)
    resolver = root / 'resolv.conf'
    for initial in (None, 'nameserver 192.0.2.1\n'):
        run_dir = root / ('absent' if initial is None else 'present')
        run_dir.mkdir()
        if initial is not None:
            resolver.write_text(initial)
        elif resolver.exists():
            resolver.unlink()
        env = dict(os.environ, RUN=str(run_dir))
        subprocess.run(['sh', '-eu', '-c', save.replace('/etc/resolv.conf', str(resolver))], env=env, check=True)
        resolver.write_text('nameserver 198.51.100.1\n')
        subprocess.run(['sh', '-eu', '-c', restore.replace('/etc/resolv.conf', str(resolver))], env=env, check=True)
        if initial is None:
            assert not resolver.exists()
        else:
            assert resolver.read_text() == initial
print('Wi-Fi resolver present/absent restoration: PASS')
