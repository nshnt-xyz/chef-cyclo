#!/usr/bin/env python3
"""Rootless decision tests; block-node builtin mocked only in test shell.
All device commands are stubs. Separate real ext4 image test pins features.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'initramfs/usr/bin/chef-storage'

class Storage(unittest.TestCase):
    shell = ["bash"]
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name)
        self.sys = self.p/'sys'; self.entry = self.sys/'mmcblk0p69'
        (self.entry/'holders').mkdir(parents=True)
        self.dev=self.p/'dev'; self.dev.mkdir(); (self.dev/'mmcblk0p69').touch()
        for name,value in {'uevent':'DEVNAME=mmcblk0p69\nDEVTYPE=partition\nPARTN=69\nPARTNAME=userdata\n', 'size':str(55289298432//512), 'dev':'259:36', 'partition':'69'}.items():
            (self.entry/name).write_text(value+'\n')
        for name,value in {'cmdline':'androidboot.slot_suffix=_a', 'mounts':'', 'swaps':'Filename Type Size Used Priority\n'}.items(): (self.p/name).write_text(value)
        self.bin=self.p/'bin';self.bin.mkdir()
        stub='''#!/bin/sh
name=${0##*/}
echo "$name $*" >> "$TRACE"
case "$name" in
mkdir) case "$*" in */v1/*) [ "${FAIL_CMD:-}" != "$name" ] || exit 1;; esac;;
chmod|chown) [ "${FAIL_CMD:-}" != "$name" ] || exit 1;;
stat) echo "${IDENTITY:-103:24}";;
blockdev) echo "${SIZE:-55289298432}"; exit "${SIZE_RC:-0}";;
e2label) echo "${TEST_LABEL:-chefdata}"; exit "${LABEL_RC:-0}";;
debugfs) echo "${TEST_MARKER:-chef-cyclo-data-v1}"; exit "${MARKER_RC:-0}";;
e2fsck) exit "${FSCK_RC:-0}";;
mount) case "$*" in
*--bind*) n=$(cat "$TRACE.bind" 2>/dev/null || echo 0); n=$((n+1)); echo "$n" > "$TRACE.bind"; [ "${FAIL_BIND:-0}" != "$n" ] || exit 1; [ "${BIND_RC:-0}" = 0 ] || exit "$BIND_RC"; echo "26 0 259:36 / $3 rw - ext4 node rw" >> "$CHEF_STORAGE_MOUNTS";;
*remount,ro*) exit "${RO_RC:-0}";;
*) [ "${MOUNT_RC:-0}" = 0 ] || exit "$MOUNT_RC"; echo "25 0 259:36 / $CHEF_STORAGE_DATA rw - ext4 node rw" >> "$CHEF_STORAGE_MOUNTS";; esac;;
umount) case "$*" in
*/system-connections) [ "${FAIL_DETACH_NM:-0}" = 0 ] || exit 1;;
*/bluetooth) [ "${FAIL_DETACH_BT:-0}" = 0 ] || exit 1;;
*) [ "${FAIL_DETACH_DATA:-0}" = 0 ] || exit 1;; esac
[ "${UMOUNT_RC:-0}" = 0 ] || exit "$UMOUNT_RC"
awk -v path="$1" '$5!=path' "$CHEF_STORAGE_MOUNTS" > "$TRACE.mounts" && cat "$TRACE.mounts" > "$CHEF_STORAGE_MOUNTS";;
fuser) echo 30001;;
setsid|sleep) exit 0;;
mountpoint) exit "${MOUNTPOINT_RC:-1}";;
esac
'''
        for name in ['stat','blockdev','e2label','debugfs','e2fsck','mount','umount','mke2fs','sync','mkdir','chmod','chown','find','fuser','setsid','sleep','mountpoint']:
            f=self.bin/name;f.write_text(stub);f.chmod(0o755)
        self.runpath=self.p/'run';self.runpath.mkdir();self.data=self.p/'data';self.data.mkdir()
        # Mock the block-file predicate; never relax production safeguards.
        wrapper='''kill() { echo "kill $*" >> "$TRACE"; }
[() { if command [ "$#" = 3 ] && command [ "$1" = -b ]; then command [ -f "$2" ]; else command [ "$@"; fi; }
'''
        # Host BusyBox ash may prefer built-in applets over PATH. Explicit
        # function wrappers keep every simulated mutation inside the fixture.
        for f in self.bin.iterdir():
            wrapper += f'{f.name}() {{ "$TEST_BIN/{f.name}" "$@"; }}\n'
        wrapper += 'timeout() { /usr/bin/timeout "$@"; }\n'
        self.script=self.p/'storage';self.script.write_text(wrapper+SCRIPT.read_text())
        self.env={**os.environ,'PATH':str(self.bin)+':'+os.environ['PATH'],'TRACE':str(self.p/'trace'),'TEST_BIN':str(self.bin),
            'CHEF_STORAGE_SYS':str(self.sys),'CHEF_STORAGE_DEV':str(self.dev), 'CHEF_STORAGE_CMDLINE':str(self.p/'cmdline'), 'CHEF_STORAGE_MOUNTS':str(self.p/'mounts'), 'CHEF_STORAGE_SWAPS':str(self.p/'swaps'),'CHEF_STORAGE_RUN':str(self.runpath),'CHEF_STORAGE_DATA':str(self.data)}
    def call(self, *args, **env):
        (self.p/'trace').write_text('')
        (self.p/'trace.bind').write_text('0')
        if env.get('MOUNTPOINT_RC') == '0':
            (self.p/'mounts').write_text(f'25 0 259:36 / {self.data} rw - ext4 node rw\n26 0 259:36 / /var/lib/bluetooth rw - ext4 node rw\n27 0 259:36 / /run/NetworkManager/system-connections rw - ext4 node rw\n')
        if env.pop('DATA_ABSENT', None):
            (self.p/'mounts').write_text('26 0 259:36 / /var/lib/bluetooth rw - ext4 node rw\n')
        if 'LABEL' in env: env['TEST_LABEL']=env.pop('LABEL')
        if 'MARKER' in env: env['TEST_MARKER']=env.pop('MARKER')
        mounts_before=(self.p/'mounts').read_text() if (self.p/'mounts').exists() else None
        r=subprocess.run([*self.shell,str(self.script),*args],env={**self.env,**env},text=True,capture_output=True)
        trace=(self.p/'trace').read_text() if (self.p/'trace').exists() else ''
        if mounts_before is not None: (self.p/'mounts').write_text(mounts_before)
        return r,trace
    def refuse(self):
        r,t=self.call('format','--yes-erase-userdata',LABEL='android')
        self.assertNotEqual(r.returncode,0,r.stderr);self.assertNotIn('mke2fs ',t)
    def test_confirmation(self):
        for args in [('format',),('format','--yes'),('format','--yes-erase-userdata','extra')]:
            r,t=self.call(*args);self.assertNotEqual(r.returncode,0);self.assertNotIn('mke2fs ',t)
    def test_slots(self):
        for value in ['', 'androidboot.slot_suffix=_b', 'androidboot.slot_suffix=_b androidboot.slot_suffix=_a', 'androidboot.slot_suffix=_a androidboot.slot_suffix=_a']:
            (self.p/'cmdline').write_text(value);self.refuse()
    def test_partition(self):
        for value in ['PARTNAME=wrong\n','PARTNAME=userdata\nPARTNAME=foreign\n','DEVNAME=wrong\nDEVTYPE=partition\nPARTN=69\nPARTNAME=userdata\n','DEVNAME=mmcblk0p69\nDEVTYPE=disk\nPARTN=69\nPARTNAME=userdata\n','DEVNAME=mmcblk0p69\nDEVTYPE=partition\nPARTN=68\nPARTNAME=userdata\n']:
            (self.entry/'uevent').write_text(value);self.refuse()
    def test_duplicate_partition(self):
        other=self.sys/'other';other.mkdir();(other/'uevent').write_text('PARTNAME=userdata\n');self.refuse()
    def test_size(self):
        (self.entry/'size').write_text('107987911');self.refuse()
    def test_live_size_identity(self):
        for env in [{'SIZE':'55289298431'},{'IDENTITY':'103:25'},{'SIZE_RC':'124'}]:
            r,t=self.call('format','--yes-erase-userdata',LABEL='android',**env);self.assertNotEqual(r.returncode,0);self.assertNotIn('mke2fs ',t)
    def test_alias_mount(self):
        (self.p/'mounts').write_text('25 0 259:36 / /alias rw - ext4 /dev/other rw\n');self.refuse()
    def test_holders(self):
        (self.entry/'holders'/'dm-0').touch();self.refuse()
    def test_swap(self):
        (self.p/'swaps').write_text(f'Filename Type Size Used Priority\n{self.dev}/mmcblk0p69 partition 1 0 0\n');self.refuse()
    def test_swap_alias(self):
        (self.dev/'alias').touch();(self.p/'swaps').write_text(f'Filename Type Size Used Priority\n{self.dev}/alias partition 1 0 0\n');self.refuse()
    def test_probe_and_already_formatted(self):
        for env in [{'LABEL':'chefdata'},{'LABEL_RC':'124','LABEL':'android'},{'LABEL_RC':'1','LABEL':'android'}]:
            r,t=self.call('format','--yes-erase-userdata',**env);self.assertNotEqual(r.returncode,0);self.assertNotIn('mke2fs ',t)
    def test_boot_foreign(self):
        for env in [{'LABEL':'foreign'},{'MARKER':'foreign'},{'MARKER_RC':'124'},{'LABEL_RC':'124'}]:
            r,t=self.call('boot',**env);self.assertEqual(r.returncode,0,r.stderr);self.assertNotIn('mount -t',t);self.assertNotIn('mke2fs ',t)
    def test_fsck_policy(self):
        for code in [0,1,2,3,4,8,16,32,128,124,137]:
            r,t=self.call('boot',FSCK_RC=str(code));self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual('mount -t' in t,code in [0,1],t);self.assertNotIn('mke2fs ',t)
    def test_boot_mount_failure(self):
        r,t=self.call('boot',MOUNT_RC='1');self.assertEqual(r.returncode,0);self.assertNotIn('mount --bind',t)
    def test_boot_bind_failure(self):
        r,t=self.call('boot',BIND_RC='1');self.assertEqual(r.returncode,0);self.assertIn('umount ',t);self.assertNotIn('fuser',t)
    def test_unreadable_state(self):
        for name in ['mounts','swaps']:
            path=self.p/name; path.unlink(); self.refuse(); path.write_text('')
        (self.entry/'holders').rmdir();self.refuse()
    def test_consumer_operations_fail_closed(self):
        for name in ['mkdir','chmod','chown']:
            r,t=self.call('boot',FAIL_CMD=name)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertNotIn('mount --bind',t)
    def test_second_bind_failure(self):
        r,t=self.call('boot',FAIL_BIND='2');self.assertEqual(r.returncode,0)
        self.assertEqual(t.count('mount --bind'),2);self.assertIn('umount ',t)
    def test_shutdown_default_order(self):
        r,t=self.call('shutdown',MOUNTPOINT_RC='0');self.assertEqual(r.returncode,0,r.stderr)
        self.assertNotIn('kill -TSTP 1',t);self.assertNotIn('kill -CONT 1',t)
        self.assertLess(t.index('kill -TERM 30001'),t.index('sync '))
        self.assertLess(t.index('sync '),t.index('umount '))
    def test_shutdown_manual_resume(self):
        for env in [{},{'UMOUNT_RC':'1'},{'UMOUNT_RC':'1','RO_RC':'1'}]:
            r,t=self.call('shutdown','--pause-init',MOUNTPOINT_RC='0',**env)
            self.assertIn('kill -TSTP 1',t);self.assertIn('kill -CONT 1',t)
            self.assertLess(t.index('kill -TSTP 1'),t.index('fuser '))
            self.assertEqual(r.returncode,1 if env.get('RO_RC')=='1' else 0,r.stderr)
    def test_shutdown_readonly_and_detach_failures(self):
        import itertools
        for nm,bt,data in itertools.product(['0','1'], repeat=3):
            r,t=self.call('shutdown',MOUNTPOINT_RC='0',FAIL_DETACH_NM=nm,FAIL_DETACH_BT=bt,FAIL_DETACH_DATA=data)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertLess(t.index('mount -o remount,ro'),t.index('umount '))
            self.assertIn('aliases remain mounted' if '1' in (nm,bt,data) else 'data fully unmounted',r.stderr)
        r,t=self.call('shutdown','--pause-init',MOUNTPOINT_RC='0',RO_RC='1')
        self.assertNotEqual(r.returncode,0);self.assertNotIn('umount ',t);self.assertIn('kill -CONT 1',t)
    def test_boot_rollback_readonly_failure(self):
        for detach in ['0','1']:
            r,t=self.call('boot',FAIL_BIND='2',RO_RC='1',UMOUNT_RC=detach)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertLess(t.index('mount -o remount,ro'),t.index('umount '))
            self.assertEqual('storage rollback failed:' in r.stderr,detach=='1')
        r,t=self.call('boot',FAIL_BIND='2',FAIL_DETACH_BT='1')
        self.assertEqual(r.returncode,0);self.assertIn('aliases remain mounted',r.stderr)
    def test_shutdown_alias_without_data(self):
        r,t=self.call('shutdown',DATA_ABSENT='1')
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('mount -o remount,ro /var/lib/bluetooth',t)
        self.assertIn('fuser -m /var/lib/bluetooth',t)
        self.assertLess(t.index('remount,ro'),t.index('umount '))
    def test_boot_closed_console(self):
        r=subprocess.run([*self.shell,str(self.script),'boot'],env=self.env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        self.assertEqual(r.returncode,0)
        # Mirror PID1 with stderr closed, rather than writable /dev/null.
        r=subprocess.run(['bash','-c','exec 2>&-; exec "$@" boot','sh',*self.shell,str(self.script)],env=self.env,stdout=subprocess.DEVNULL)
        self.assertEqual(r.returncode,0)
        self.assertIn('layout v1 mounted',(self.runpath/'storage.log').read_text())
    def test_format_features(self):
        r,t=self.call('format','--yes-erase-userdata',LABEL='android');self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('-F -L chefdata',t);self.assertNotIn('-F -F',t);self.assertIn('-O none,has_journal',t);self.assertIn('nodiscard,lazy_itable_init=0,lazy_journal_init=0',t)
        self.assertEqual((self.data/'chef-layout').read_text(),'chef-cyclo-data-v1\n')

class AshStorage(Storage):
    shell = ["busybox", "ash"]

class Ext4(unittest.TestCase):
    def test_timeout_kill_escalation(self):
        import time
        start=time.monotonic()
        r=subprocess.run(['timeout','-k','1','1','sh','-c','trap "" TERM; exec sleep 30'],capture_output=True)
        self.assertEqual(r.returncode,137)
        self.assertLess(time.monotonic()-start,5)

    def test_real_image_features_marker_fsck(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);image=p/'data.img';image.touch()
            with image.open('r+b') as f: f.truncate(64*1024*1024)
            # Extract the actual format invocation's feature string.
            features=SCRIPT.read_text().split('mke2fs -t ext4 -O ')[1].split()[0]
            env={**os.environ,'MKE2FS_CONFIG':str(ROOT/'initramfs/etc/chef/mke2fs.conf')}
            subprocess.run(['mke2fs','-q','-t','ext4','-O',features,'-b','4096','-I','256','-m','1','-F','-L','chefdata','-E','nodiscard,lazy_itable_init=0,lazy_journal_init=0',str(image)],env=env,check=True)
            subprocess.run(['mke2fs','-q','-t','ext4','-O',features,'-b','4096','-I','256','-m','1','-F','-L','chefdata','-E','nodiscard,lazy_itable_init=0,lazy_journal_init=0',str(image)],env=env,stdin=subprocess.DEVNULL,check=True)
            result=subprocess.run(['dumpe2fs','-h',str(image)],text=True,capture_output=True,check=True).stdout
            line=next(x for x in result.splitlines() if x.startswith('Filesystem features:'))
            actual=set(line.split(':',1)[1].split())
            expected=set(features.split(',')[1:]);
            self.assertEqual(actual,expected)
            for forbidden in ['orphan_file','metadata_csum_seed','large_dir','fast_commit','encrypt','sparse_super2']:self.assertNotIn(forbidden,line)
            marker=p/'marker';marker.write_text('chef-cyclo-data-v1\n')
            subprocess.run(['debugfs','-w','-R',f'write {marker} /chef-layout',str(image)],check=True,capture_output=True)
            r=subprocess.run(['debugfs','-R','cat /chef-layout',str(image)],text=True,capture_output=True,check=True)
            self.assertEqual(r.stdout.strip(),'chef-cyclo-data-v1')
            subprocess.run(['e2fsck','-fn',str(image)],check=True,capture_output=True)

if __name__=='__main__': unittest.main()
