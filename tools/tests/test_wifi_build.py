#!/usr/bin/env python3
"""Exercise real packaging listing gates and pinned export isolation."""
import os
from pathlib import Path
import subprocess
import tempfile
repo = Path(__file__).resolve().parents[2]
source = (repo / 'scripts/mkinitramfs.sh').read_text()
block = source[source.index('# Pack every board-specific'):source.index('# The matching Motorola driver')]
with tempfile.TemporaryDirectory(prefix='wifi-build-test-') as directory:
    root = Path(directory)
    debugfs = root / 'debugfs'
    debugfs.write_text('#!/bin/sh\nprintf "%s\\n" "$LISTING"\nexit "${LIST_RC:-0}"\n')
    debugfs.chmod(0o755)
    env = dict(os.environ, PATH=str(root)+':'+os.environ['PATH'], TRACE=str(root / 'trace'))
    harness = 'set -eu\nROOT=/unused\nextract_wifi() { printf "%s\\n" "$2" >> "$TRACE"; }\n' + block
    for listing, code, expected in [
        ('12 (20) bdwlan.b04 13 (20) bdwlan.102 14 (20) bdwlan.bin', '0', True),
        ('12 (20) bdwlan.b04', '1', False),
        ('File not found by ext2_lookup', '0', False),
        ('12 (20) bdwlan.bin', '0', False),
        ('', '0', False),
    ]:
        (root / 'trace').write_text('')
        result = subprocess.run(['sh', '-c', harness], env=dict(env, LISTING=listing, LIST_RC=code), capture_output=True)
        assert (result.returncode == 0) == expected
        paths = (root / 'trace').read_text().splitlines()
        if expected:
            assert paths == ['/image/bdwlan.b04', '/image/bdwlan.102', '/image/bdwlan.bin']
        else:
            assert paths == []

    # Actual prepare() exports a pinned tree, not working-tree untracked inputs.
    script = (repo / 'scripts/build-wifi.sh').read_text()
    prepare = script[script.index('prepare() {'):script.index('prepare qcacld-3.0')]
    cache, output = root / 'cache', root / 'output'
    tree = cache / 'example'
    tree.mkdir(parents=True)
    output.mkdir()
    def git(*args):
        return subprocess.run(['git', '-C', str(tree), *args], check=True, capture_output=True, text=True).stdout.strip()
    git('init')
    (tree / 'pinned.h').write_text('verified\n')
    git('add', 'pinned.h')
    git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-m', 'fixture')
    commit = git('rev-parse', 'HEAD')
    (tree / 'injected.h').write_text('untracked input\n')
    (tree / '.ignored.h').write_text('ignored input\n')
    (tree / '.git/info/exclude').write_text('.ignored.h\n')
    result = subprocess.run(['bash', '-eu', '-o', 'pipefail', '-c', 'fetch() { :; }\n' + prepare + '\nprepare example "$PIN"'], env=dict(env, WIFI_SRC=str(cache), BUILD_SRC=str(output), PIN=commit), capture_output=True)
    assert result.returncode == 0, result.stderr
    assert [p.name for p in (output / 'example').iterdir()] == ['pinned.h']
print('Wi-Fi build listing failure gates and pinned export isolation: PASS')
