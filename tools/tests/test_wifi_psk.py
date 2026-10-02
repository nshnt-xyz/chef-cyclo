#!/usr/bin/env python3
"""Compare standalone PSK derivation with Python's independent crypto backend."""
import hashlib
from pathlib import Path
import subprocess
helper = Path(__file__).resolve().parents[1] / 'wifi-psk'
for ssid, password in [('IEEE', b'password'), ('x' * 32, b'p' * 63), ('a"\\b', b'12345678'), ('network', bytes(range(33, 65)))]:
    result = subprocess.run([str(helper), ssid], input=password+b'\n', capture_output=True, check=True)
    expected = hashlib.pbkdf2_hmac('sha1', password, ssid.encode(), 4096, 32).hex()
    assert result.stdout == f'network={{\n\tssid={ssid.encode().hex()}\n\tpsk={expected}\n}}\n'.encode()
    assert not result.stderr
    assert password not in result.stdout
for ssid, password in [('a', b'short'), ('a', b'x'*64), ('', b'12345678'), ('x'*33, b'12345678')]:
    result = subprocess.run([str(helper), ssid], input=password+b'\n', capture_output=True)
    assert result.returncode != 0 and not result.stdout
    assert password not in result.stderr
print('Wi-Fi stdin PSK vectors, length bounds and config escaping: PASS')
