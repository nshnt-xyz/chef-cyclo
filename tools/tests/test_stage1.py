#!/usr/bin/env python3
"""Rootless tests of the stage-1 /init (stage1/init): every refusal ends in
the rescue shell, the happy path reaches switch_root with system_a set
read-only before it is mounted. Device commands are stubs; the superblock
checks read real ext4 images made with the host's mke2fs. The script runs
under bash with PATH pointed at the stubs and the block/char-device
predicates mocked, as in test_storage.py. The rescue loop ends when its
`sleep 5` stub signals the shell.
"""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'stage1/init'
MKE2FS = '/usr/sbin/mke2fs'
BYTES = 2684354560
STAMP = 'chef-cyclo-root 1\nkernel-release test\n'

STUB = r'''#!/bin/sh
name=${0##*/}
echo "$name $*" >> "$TRACE"
case "$name" in
mount)
    case "$*" in *"-t ext4"*) ;; *) exit 0;; esac
    [ "${MOUNT_RC:-0}" = 0 ] || exit "$MOUNT_RC"
    eval "dev=\${$(($# - 1))}"
    echo "$dev $STAGE1_NEWROOT ext4 ${MOUNT_MODE:-ro},relatime 0 0" >> "$STAGE1_MOUNTS";;
umount)
    if [ "$1" = /dev ]; then
        if [ -e /proc/self/fd/0 ] || [ -e /proc/self/fd/1 ] || [ -e /proc/self/fd/2 ]; then
            echo "fds-open" >> "$TRACE"; else echo "fds-closed" >> "$TRACE"; fi
    fi
    [ "$1" != "${UMOUNT_FAIL:-}" ] || exit 1
    if [ "$1" = /dev ] && [ -n "${UMOUNT_BUSY:-}" ]; then
        n=$(cat "$TRACE.busy" 2>/dev/null || echo 0); echo $((n + 1)) > "$TRACE.busy"
        [ "$n" -ge "$UMOUNT_BUSY" ] || exit 1
    fi;;
mountpoint) exit 0;;
blockdev)
    case "$1" in
    --getsize64) echo "${SIZE:-2684354560}"; exit "${SIZE_RC:-0}";;
    --setro) exit "${SETRO_RC:-0}";;
    --getro) echo "${GETRO:-1}";;
    esac;;
stat) echo "${IDENTITY:-103:22}";;
pidof) exit 1;;
sleep) [ "$1" = 5 ] && kill -TERM "$PPID"; exit 0;;
switch_root) exit 0;;
esac
'''

# Busybox timeout leaves closed fds closed; GNU timeout would fill 0-2 with
# /dev/null. Record the bound and run the command in place.
TIMEOUT = '''#!/bin/sh
echo "timeout $1" >> "$TRACE"
shift
exec "$@"
'''

# Mock only the device-type predicates; everything else is the real test.
WRAPPER = r'''[() {
    if command [ "$#" = 3 ] && { command [ "$1" = -b ] || command [ "$1" = -c ]; }; then
        command [ -f "$2" ]
    else
        command [ "$@"
    fi
}
'''


def make_image(path, label='chefroot', journal=False):
    size = '16M' if journal else '4M'
    features = 'has_journal' if journal else '^has_journal'
    env = {**os.environ, 'MKE2FS_CONFIG': str(ROOT / 'scripts/system-mke2fs.conf')}
    subprocess.run([MKE2FS, '-q', '-F', '-t', 'ext4', '-L', label, '-O', features, str(path), size],
                   env=env, check=True, stderr=subprocess.DEVNULL)


class Stage1(unittest.TestCase):
    fixtures = None

    @classmethod
    def setUpClass(cls):
        cls.fixtures = tempfile.TemporaryDirectory()
        f = Path(cls.fixtures.name)
        make_image(f / 'good')
        make_image(f / 'label', label='android')
        make_image(f / 'journal', journal=True)
        recover = bytearray((f / 'good').read_bytes())
        recover[1024 + 0x60] |= 4
        (f / 'recover').write_bytes(recover)
        (f / 'zero').write_bytes(bytes(8192))

    @classmethod
    def tearDownClass(cls):
        cls.fixtures.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        p = self.p = Path(self.tmp.name)
        self.sys = p / 'sys'
        self.entry = self.sys / 'mmcblk0p67'
        self.entry.mkdir(parents=True)
        for name, value in {'uevent': 'MAJOR=259\nMINOR=34\nDEVNAME=mmcblk0p67\nDEVTYPE=partition\nPARTN=67\nPARTNAME=system_a\n',
                            'size': str(BYTES // 512), 'dev': '259:34', 'partition': '67'}.items():
            (self.entry / name).write_text(value + '\n')
        other = self.sys / 'mmcblk0p69'
        other.mkdir()
        (other / 'uevent').write_text('DEVNAME=mmcblk0p69\nDEVTYPE=partition\nPARTN=69\nPARTNAME=userdata\n')
        self.dev = p / 'dev'
        self.dev.mkdir()
        self.use_image('good')
        (p / 'cmdline').write_text('quiet root=/dev/mmcblk0p67 androidboot.slot_suffix=_a skip_initramfs\n')
        (p / 'mounts').write_text('')
        self.newroot = p / 'newroot'
        for rel in ('init', 'bin/busybox'):
            f = self.newroot / rel
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text('#!/bin/sh\n')
            f.chmod(0o755)
        (self.newroot / 'etc/chef').mkdir(parents=True)
        (self.newroot / 'etc/chef/build-stamp').write_text(STAMP)
        (p / 'expected').write_text(STAMP)
        (p / 'run').mkdir()
        (p / 'kmsg').write_text('')
        (p / 'gadget').write_text('usb_gadget_up() { echo usb_gadget_up >> "$TRACE"; }\n')
        self.bin = p / 'bin'
        self.bin.mkdir()
        for name in ('mount', 'umount', 'mountpoint', 'blockdev', 'stat', 'pidof', 'sleep',
                     'switch_root', 'telnetd', 'udhcpd'):
            (self.bin / name).write_text(STUB)
            (self.bin / name).chmod(0o755)
        (self.bin / 'timeout').write_text(TIMEOUT)
        (self.bin / 'timeout').chmod(0o755)
        self.env = {**os.environ, 'TRACE': str(p / 'trace'),
                    'STAGE1_SYS': str(self.sys), 'STAGE1_DEV': str(self.dev),
                    'STAGE1_CMDLINE': str(p / 'cmdline'), 'STAGE1_MOUNTS': str(p / 'mounts'),
                    'STAGE1_NEWROOT': str(self.newroot), 'STAGE1_EXPECTED': str(p / 'expected'),
                    'STAGE1_RUN': str(p / 'run'), 'STAGE1_KMSG': str(p / 'kmsg'),
                    'STAGE1_VIB': str(p / 'vib'), 'STAGE1_GADGET': str(p / 'gadget')}

    def use_image(self, name):
        shutil.copyfile(Path(self.fixtures.name) / name, self.dev / 'mmcblk0p67')

    def script(self, as_pid1=False):
        text = SCRIPT.read_text()
        path_line = 'export PATH=/sbin:/usr/sbin:/bin:/usr/bin\n'
        self.assertIn(path_line, text)
        text = text.replace(path_line, f'export PATH={self.bin}:$PATH\n')
        if as_pid1:
            self.assertEqual(text.count('if [ $$ != 1 ]; then'), 1)
            text = text.replace('if [ $$ != 1 ]; then', 'if false; then')
        s = self.p / 'init'
        s.write_text(WRAPPER + text)
        return s

    def run_init(self, as_pid1=False, **env):
        (self.p / 'trace').write_text('')
        (self.p / 'trace.busy').unlink(missing_ok=True)
        r = subprocess.run(['bash', str(self.script(as_pid1))], env={**self.env, **env},
                           capture_output=True, text=True, timeout=60)
        return r, (self.p / 'trace').read_text()

    def assert_rescue(self, reason, **env):
        r, t = self.run_init(**env)
        self.assertEqual(r.returncode, -signal.SIGTERM, (r, t))
        self.assertNotIn('switch_root', t)
        self.assertIn('telnetd -b 172.16.42.1 -l /bin/sh', t)
        self.assertIn(f'udhcpd {self.p}/run/udhcpd.conf', t)
        self.assertIn('usb_gadget_up', t)
        self.assertIn('sleep 5', t)
        got = (self.p / 'run/rescue-reason').read_text()
        self.assertIn(reason, got)
        self.assertIn('cyclo-stage1: RESCUE: ' + got, (self.p / 'kmsg').read_text())
        return t

    def test_happy_path(self):
        r, t = self.run_init()
        self.assertEqual(r.returncode, 0, (r, t))
        self.assertFalse((self.p / 'run/rescue-reason').exists())
        lines = t.splitlines()
        node = f'{self.dev}/mmcblk0p67'
        setro = lines.index(f'blockdev --setro {node}')
        mount = lines.index(f'mount -t ext4 -o ro,noload {node} {self.newroot}')
        self.assertLess(setro, mount)
        self.assertLess(lines.index(f'blockdev --getro {node}'), mount)
        self.assertEqual(lines[-1], f'switch_root {self.newroot} /init')
        # No timeout around these: its daemonized watchdog would hold /dev/null.
        self.assertEqual(lines[-5:-1], ['umount /dev', 'fds-closed', 'umount /sys', 'umount /proc'])
        self.assertEqual(lines[lines.index(f'mount -t ext4 -o ro,noload {node} {self.newroot}') - 1], 'timeout 10')
        self.assertNotIn('telnetd', t)
        kmsg = [l.split(': ', 1)[1] for l in (self.p / 'kmsg').read_text().splitlines()]
        expected = ['starting (', 'found system_a ', 'system_a set read-only', 'system_a superblock: ',
                    'system_a mounted read-only', 'system_a build stamp matches; switch_root']
        self.assertEqual(len(kmsg), len(expected), kmsg)
        for line, prefix in zip(kmsg, expected):
            self.assertTrue(line.startswith(prefix), (line, prefix))

    def test_slot(self):
        for cmdline in ('quiet\n', 'androidboot.slot_suffix=_b\n',
                        'androidboot.slot_suffix=_a androidboot.slot_suffix=_a\n',
                        'androidboot.slot_suffix=_b androidboot.slot_suffix=_a\n'):
            (self.p / 'cmdline').write_text(cmdline)
            t = self.assert_rescue('active slot is not _a')
            self.assertNotIn('blockdev', t)

    def test_never_appears(self):
        (self.entry / 'uevent').write_text('DEVNAME=mmcblk0p67\nPARTNAME=system_b\n')
        t = self.assert_rescue('system_a did not appear within 10 s')
        self.assertEqual(t.count('sleep 0.1'), 100)

    def test_duplicate(self):
        dup = self.sys / 'mmcblk1p1'
        dup.mkdir()
        (dup / 'uevent').write_text('PARTNAME=system_a\n')
        self.assert_rescue('system_a PARTNAME is ambiguous')

    def test_metadata(self):
        cases = {'DEVNAME=mmcblk0p67\nDEVTYPE=partition\nPARTN=67\nPARTNAME=system_a\nPARTNAME=x\n': 'conflicting PARTNAME',
                 'DEVNAME=mmcblk0p66\nDEVTYPE=partition\nPARTN=67\nPARTNAME=system_a\n': 'device metadata mismatch',
                 'DEVNAME=mmcblk0p67\nDEVTYPE=disk\nPARTN=67\nPARTNAME=system_a\n': 'device metadata mismatch',
                 'DEVNAME=mmcblk0p67\nDEVTYPE=partition\nPARTN=66\nPARTNAME=system_a\n': 'partition number mismatch'}
        for uevent, reason in cases.items():
            (self.entry / 'uevent').write_text(uevent)
            self.assert_rescue(reason)

    def test_sizes_and_identity(self):
        (self.entry / 'size').write_text('5242879\n')
        self.assert_rescue('sysfs size is not')
        (self.entry / 'size').write_text(f'{BYTES // 512}\n')
        self.assert_rescue('live size', SIZE='2684350464')
        self.assert_rescue('size probe failed', SIZE_RC='1')
        self.assert_rescue('is not system_a', IDENTITY='103:23')

    def test_missing_node(self):
        (self.dev / 'mmcblk0p67').unlink()
        self.assert_rescue('no block node')

    def test_setro(self):
        t = self.assert_rescue('blockdev --setro failed', SETRO_RC='1')
        self.assertNotIn('-t ext4', t)
        t = self.assert_rescue('system_a is not read-only', GETRO='0')
        self.assertNotIn('-t ext4', t)

    def test_superblock(self):
        for image, reason in (('zero', 'not ext4'), ('label', 'label is not chefroot'),
                              ('journal', 'has a journal'), ('recover', 'needs journal recovery')):
            self.use_image(image)
            t = self.assert_rescue(reason)
            self.assertNotIn('-t ext4', t)
        (self.dev / 'mmcblk0p67').write_bytes(bytes(1500))
        self.assert_rescue('cannot read the system_a superblock')

    def test_mount(self):
        t = self.assert_rescue('mounting system_a failed', MOUNT_RC='32')
        self.assertNotIn(f'umount {self.newroot}', t)
        t = self.assert_rescue('not mounted read-only', MOUNT_MODE='rw')
        self.assertIn(f'umount {self.newroot}', t)

    def test_root_contents(self):
        (self.newroot / 'init').chmod(0o644)
        t = self.assert_rescue('no executable /init')
        self.assertIn(f'umount {self.newroot}', t)
        (self.newroot / 'init').unlink()
        self.assert_rescue('no executable /init')
        (self.newroot / 'init').write_text('#!/bin/sh\n')
        (self.newroot / 'init').chmod(0o755)
        (self.newroot / 'bin/busybox').unlink()
        self.assert_rescue('no executable /init')

    def test_stamp(self):
        (self.p / 'expected').write_text(STAMP + 'x\n')
        t = self.assert_rescue('build stamp does not match')
        self.assertIn(f'umount {self.newroot}', t)
        (self.newroot / 'etc/chef/build-stamp').unlink()
        self.assert_rescue('has no build stamp')

    def test_dev_briefly_busy(self):
        r, t = self.run_init(UMOUNT_BUSY='3')
        self.assertEqual(r.returncode, 0, (r, t))
        self.assertEqual(t.count('umount /dev'), 4)
        self.assertEqual(t.splitlines()[-1], f'switch_root {self.newroot} /init')
        t = self.assert_rescue('cannot unmount /dev before switch_root', UMOUNT_BUSY='99')
        self.assertEqual(t.count('umount /dev'), 16)
        self.assertEqual(t.count('sleep 0.2'), 15)

    def test_umount_before_switch_root(self):
        t = self.assert_rescue('cannot unmount /sys before switch_root', UMOUNT_FAIL='/sys')
        self.assertIn(f'umount {self.newroot}', t)

    def test_overrides_inert_as_pid1(self):
        # As PID 1 the STAGE1_* environment (valid fixtures here) is ignored;
        # the host's real /proc/cmdline has no slot suffix, so it must rescue
        # without ever touching a block device.
        r, t = self.run_init(as_pid1=True)
        self.assertEqual(r.returncode, -signal.SIGTERM, (r, t))
        self.assertNotIn('blockdev', t)
        self.assertNotIn('switch_root', t)
        self.assertIn('telnetd -b 172.16.42.1 -l /bin/sh', t)
        self.assertIn('udhcpd /run/udhcpd.conf', t)


class Stage2(unittest.TestCase):
    """The stage-2 /init runs on the read-only system_a root: no root writes."""

    def test_no_root_writes(self):
        init = (ROOT / 'initramfs/init').read_text()
        for forbidden in ('--install', 'sed -i', '/etc/udhcpd.conf'):
            self.assertNotIn(forbidden, init)
        # A failed `.` would end PID 1; the gadget helper is guarded.
        self.assertIn('if [ -r /usr/lib/chef/usb-gadget.sh ] && . /usr/lib/chef/usb-gadget.sh; then', init)
        self.assertLess(init.index('mount --move /run/.var /var'), init.index('dbus-uuidgen --ensure'))
        self.assertIn('mount -t tmpfs -o mode=700 tmpfs /root', init)
        inittab = (ROOT / 'initramfs/etc/inittab').read_text()
        self.assertIn('udhcpd -f /run/udhcpd.conf', inittab)
        gadget = (ROOT / 'initramfs/usr/lib/chef/usb-gadget.sh').read_text()
        self.assertIn('/etc/udhcpd.conf > /run/udhcpd.conf', gadget)
        self.assertNotIn('sed -i', gadget)


if __name__ == '__main__':
    unittest.main()
