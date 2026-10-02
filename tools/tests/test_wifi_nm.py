#!/usr/bin/env python3
"""Host ownership and actual dispatcher regressions without network privileges."""
import os, pathlib, socket, subprocess, tempfile
R = pathlib.Path(__file__).resolve().parents[2]
lib = R / 'initramfs/usr/lib/chef/modem-owner.sh'
dispatch = R / 'initramfs/etc/NetworkManager/dispatcher.d/10-chef-usb-overlap'
with tempfile.TemporaryDirectory() as t:
    p = pathlib.Path(t); (p/'ready').touch()
    sock = socket.socket(socket.AF_UNIX); sock.bind(str(p/'socket'))
    child = subprocess.Popen(['sleep','60'])
    try:
        start = pathlib.Path(f'/proc/{child.pid}/stat').read_text().rsplit(') ',1)[1].split()[19]
        env = dict(os.environ, MODEM_OWNER_LOCK=t, MODEM_QMUX_SOCKET=str(p/'socket'))
        def owner(value):
            (p/'owner').write_text(value)
            return subprocess.run(['sh','-c', '. "$1"; modem_owner_pid','sh',str(lib)],env=env,capture_output=True)
        good = f'{child.pid} {start}\n'
        assert owner(good).stdout.strip() == str(child.pid).encode()
        for bad in (f'{child.pid} 0\n','1 123\n','bad pid\n','99999999 123\n'):
            assert owner(bad).returncode
        (p/'ready').unlink(); assert owner(good).returncode
        (p/'ready').touch(); child.terminate(); child.wait(); assert owner(good).returncode
    finally:
        if child.poll() is None: child.kill(); child.wait()
        sock.close()
    bindir = p/'bin'; bindir.mkdir()
    (bindir/'ip').write_text('#!/bin/sh\n[ "${IP_FAIL:-0}" = 0 ] || exit 1\nprintf "2: wlan0 inet %s scope global wlan0\\n" "$ADDR"\n')
    (bindir/'nmcli').write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$CALLS"\n')
    for f in bindir.iterdir(): f.chmod(0o755)
    env = dict(os.environ, PATH=str(bindir)+':'+os.environ['PATH'],CALLS=str(p/'calls'))
    for addr, reject in [('192.168.0.116/24',False),('172.16.42.1/24',True),('172.16.43.2/23',True),('172.16.43.2/24',False),('10.0.0.1/0',True),('172.16.42.7/32',True)]:
        (p/'calls').unlink(missing_ok=True)
        subprocess.run(['sh',str(dispatch),'wlan0','dhcp4-change'],env=dict(env,ADDR=addr),check=True,capture_output=True)
        assert (p/'calls').exists() == reject, addr
        if reject: assert (p/'calls').read_text() == '--wait 0 device disconnect wlan0\n'
    (p/'calls').unlink(missing_ok=True)
    result=subprocess.run(['sh',str(dispatch),'wlan0','up'],env=dict(env,ADDR='192.168.0.1/24',IP_FAIL='1'))
    assert result.returncode and (p/'calls').exists()
# Mode reservation precedes init concurrency; manual refuses the marker.
init=(R/'initramfs/init').read_text()
assert init.index('touch /run/wifi-nm-mode') < init.index('exec /sbin/init')
manual=(R/'initramfs/usr/bin/wifi-up').read_text()
assert 'if [ -e /etc/NetworkManager/conf.d/90-chef.conf ] || [ -e /run/wifi-nm-mode ] || pidof NetworkManager' in manual
nm=(R/'initramfs/usr/bin/wifi-nm').read_text()
assert nm.index('wifi-usb-guard ||') < nm.index('wifi-prepare ||') < nm.index('exec /usr/sbin/NetworkManager')
print('test_wifi_nm: owner identity, dispatcher overlap/error, boot ownership PASS')
# Exercise production account collision gates before privilege-dropping daemon setup.
source=(R/'scripts/mkinitramfs.sh').read_text()
account_gate=source.split('# Skipped apk account scripts:',1)[1].split('VARIANT=${VARIANT:-}',1)[0]
# Restore the first comment line removed by split.
account_gate='# Skipped apk account scripts:'+account_gate
with tempfile.TemporaryDirectory() as t:
    d=pathlib.Path(t)/'initramfs/etc';d.mkdir(parents=True)
    passwd=(R/'initramfs/etc/passwd').read_text();group=(R/'initramfs/etc/group').read_text()
    def check(pw,gr):
        (d/'passwd').write_text(pw);(d/'group').write_text(gr)
        return subprocess.run(['sh','-c',account_gate],cwd=t,capture_output=True).returncode
    assert check(passwd,group)==0
    assert check(passwd+'collision:x:102:999::/:/bin/false\n',group)!=0
    assert check(passwd,group+'collision:x:102:\n')!=0
    assert check(passwd.replace('chrony:x:102:102','chrony:x:103:103'),group)!=0
    assert check(passwd+'collision:x:101:999::/:/bin/false\n',group)!=0
    assert check(passwd+'collision:x:999:101::/:/bin/false\n',group)!=0
    assert check(passwd,group.replace('messagebus:x:101:','messagebus:x:101:guest'))!=0
print('test_wifi_nm: actual chrony account collision gates PASS')

# Execute the actual boot coldplug block: readiness only on complete net success.
coldplug=init.split('mkdir -p /run/udev\n',1)[1].split('# Reserve WLAN ownership',1)[0]
coldplug='mkdir -p /run/udev\n'+coldplug
with tempfile.TemporaryDirectory() as t:
    p=pathlib.Path(t);b=p/'bin';b.mkdir()
    (b/'udevd').write_text('#!/bin/sh\n[ "${UDEV_FAIL:-}" != daemon ]\n')
    (b/'udevadm').write_text('''#!/bin/sh
printf "%s\\n" "$*" >> "$CALLS"
case "$*" in
    *--subsystem-match=input)
        if [ -n "${INPUT_GATE:-}" ]; then
            while [ ! -e "$INPUT_GATE" ]; do sleep 0.01; done
        fi
        [ "${UDEV_FAIL:-}" != input-trigger ] ;;
    *--timeout=5) [ "${UDEV_FAIL:-}" != input-settle ] ;;
    *) [ "${UDEV_FAIL:-}" != "$1" ] ;;
esac
''')
    for f in b.iterdir():f.chmod(0o755)
    code='log() { :; }; '+coldplug.replace('/sbin/udevd',str(b/'udevd')).replace('/run/udev',str(p/'udev')).replace('/run/input-udev-ready',str(p/'input-ready'))
    # Production has no wait: only the harness waits for the background task.
    assert ') &' in coldplug and 'wait' not in coldplug.split('    (\n',1)[1].split(') &',1)[1]
    code+='\nwait\n'
    for failure in ('','daemon','trigger','settle','input-trigger','input-settle'):
        ready=p/'udev-ready';ready.unlink(missing_ok=True)
        input_ready=p/'input-ready';input_ready.unlink(missing_ok=True)
        calls=p/'calls';calls.unlink(missing_ok=True)
        env=dict(os.environ,PATH=str(b)+':'+os.environ['PATH'],CALLS=str(calls),UDEV_FAIL=failure)
        subprocess.run(['sh','-c',code],env=env,check=True)
        assert ready.exists()==(failure in ('','input-trigger','input-settle'))
        assert input_ready.exists()==(failure in ('','trigger','settle'))
        if failure=='':assert calls.read_text().splitlines()==['trigger --action=add --subsystem-match=net','settle --timeout=20','trigger --action=add --subsystem-match=input','settle --timeout=5']
    # Hold input discovery behind a gate and prove init handoff proceeds.
    import time
    gate=p/'release-input';handoff=p/'handoff'
    ready.unlink(missing_ok=True);input_ready.unlink(missing_ok=True)
    env=dict(env,UDEV_FAIL='',INPUT_GATE=str(gate))
    child=subprocess.Popen(['sh','-c',code.rsplit('\nwait\n',1)[0]+f'\ntouch "{handoff}"\nwait\n'],env=env)
    try:
        deadline=time.monotonic()+2
        while not handoff.exists() and time.monotonic()<deadline:time.sleep(0.01)
        assert handoff.exists() and ready.exists() and not input_ready.exists()
        gate.touch()
        assert child.wait(timeout=2)==0 and input_ready.exists()
    finally:
        gate.touch()
        if child.poll() is None:child.kill();child.wait()
print('test_wifi_nm: independent network/input coldplug and fail-closed readiness PASS')

# Execute real metadata setup without invoking privileged host chown/chmod.
init=(R/'initramfs/init').read_text()
setup=init.split('if chown root:messagebus',1)[1].split('log "starting',1)[0]
setup='if chown root:messagebus'+setup
with tempfile.TemporaryDirectory() as t:
    p=pathlib.Path(t);b=p/'bin';b.mkdir()
    for command in ('chown','chmod'):
        (b/command).write_text('#!/bin/sh\nprintf "%s %s\\n" "'+command+'" "$*" >> "$CALLS"\n[ "${SETUP_FAIL:-}" != "'+command+'" ]\n')
        (b/command).chmod(0o755)
    code='log() { :; }; '+setup.replace('/run/dbus-helper-ready',str(p/'ready'))
    for failure in ('','chown','chmod'):
        (p/'ready').unlink(missing_ok=True);(p/'calls').unlink(missing_ok=True)
        env=dict(os.environ,PATH=str(b)+':'+os.environ['PATH'],CALLS=str(p/'calls'),SETUP_FAIL=failure)
        subprocess.run(['sh','-c',code],env=env,check=True)
        assert (p/'ready').exists()==(failure=='')
        if failure=='':assert (p/'calls').read_text().splitlines()==['chown root:messagebus /usr/libexec/dbus-daemon-launch-helper','chmod 4750 /usr/libexec/dbus-daemon-launch-helper']
print('test_wifi_nm: strict D-Bus helper setup and failure gating PASS')

# Run the real link-time refresh hook with bounded-command and chronyc stubs.
refresh=R/'initramfs/etc/NetworkManager/dispatcher.d/20-chef-chrony-refresh'
with tempfile.TemporaryDirectory() as t:
    p=pathlib.Path(t);b=p/'bin';b.mkdir()
    (b/'busybox').write_text('#!/bin/sh\nprintf "%s\n" "$*" >> "$TIMEOUT_CALLS"\n[ "$1" = timeout ] && [ "$2" = 5 ] || exit 2\nshift 2; exec "$@"\n')
    (b/'chronyc').write_text('#!/bin/sh\nprintf "%s\n" "$*" >> "$CALLS"\nexit "${CHRONY_RESULT:-0}"\n')
    for f in b.iterdir():f.chmod(0o755)
    # Substitute only the absolute target busybox path for safe host execution.
    hook=p/'hook';hook.write_text(refresh.read_text().replace('/bin/busybox',str(b/'busybox')))
    for interface,action,run in [('wlan0','up',True),('wlan0','dhcp4-change',True),('wlan0','dhcp6-change',True),('wlan0','down',False),('usb0','up',False)]:
        for result in ('0','1','124'):
            (p/'calls').unlink(missing_ok=True);(p/'timeout').unlink(missing_ok=True)
            env=dict(os.environ,PATH=str(b)+':'+os.environ['PATH'],CALLS=str(p/'calls'),TIMEOUT_CALLS=str(p/'timeout'),CHRONY_RESULT=result)
            subprocess.run(['sh',str(hook),interface,action],env=env,check=True,capture_output=True)
            assert (p/'calls').exists()==run
            if run:
                assert (p/'calls').read_text()=='-h /run/chrony/chronyd.sock refresh\n'
                assert (p/'timeout').read_text()=='timeout 5 chronyc -h /run/chrony/chronyd.sock refresh\n'
print('test_wifi_nm: chrony link-refresh success/error/timeout remain nonfatal PASS')

# Model real reapply timing: empty/old address then later overlapping address.
with tempfile.TemporaryDirectory() as t:
    p=pathlib.Path(t);b=p/'bin';b.mkdir()
    (b/'ip').write_text('''#!/bin/sh
n=$(cat "$COUNT" 2>/dev/null || echo 0); n=$((n+1)); echo "$n" > "$COUNT"
case "$SCENARIO" in
 late-overlap) [ "$n" -lt 4 ] || echo '2: wlan0 inet 172.16.42.200/24 scope global wlan0';;
 old-to-overlap) if [ "$n" -lt 4 ]; then echo '2: wlan0 inet 192.168.0.116/24 scope global wlan0'; else echo '2: wlan0 inet 172.16.42.200/24 scope global wlan0'; fi;;
 late-safe) [ "$n" -lt 4 ] || echo '2: wlan0 inet 192.168.0.116/24 scope global wlan0';;
 empty) :;;
esac
''')
    (b/'sleep').write_text('#!/bin/sh\nexit 0\n')
    (b/'busybox').write_text('#!/bin/sh\n[ "$1" = timeout ] && [ "$2" = 5 ] || exit 2\nshift 2;exec "$@"\n')
    (b/'nmcli').write_text('''#!/bin/sh
case "$*" in
 '-g ipv4.method connection show uuid test-uuid') [ "$METHOD" != fail ] || exit 1; echo "$METHOD";;
 '--wait 0 device disconnect wlan0') echo disconnect >> "$CALLS";;
 *) exit 2;;
esac
''')
    for f in b.iterdir():f.chmod(0o755)
    hook=p/'hook';hook.write_text(dispatch.read_text().replace('/bin/busybox',str(b/'busybox')))
    cases=[('late-overlap','auto',True,0),('old-to-overlap','manual',True,0),('late-safe','auto',False,0),('empty','auto',True,1),('empty','disabled',False,0),('empty','ignore',False,0),('empty','fail',True,1),('empty','missing-uuid',True,1)]
    for scenario,method,reject,rc in cases:
        (p/'count').unlink(missing_ok=True);(p/'calls').unlink(missing_ok=True)
        env=dict(os.environ,PATH=str(b)+':'+os.environ['PATH'],COUNT=str(p/'count'),CALLS=str(p/'calls'),SCENARIO=scenario,METHOD=method,CONNECTION_UUID='' if method=='missing-uuid' else 'test-uuid')
        result=subprocess.run(['sh',str(hook),'wlan0','reapply'],env=env,capture_output=True)
        assert result.returncode==rc,(scenario,method,result.stderr)
        assert (p/'calls').exists()==reject,(scenario,method)
        count=int((p/'count').read_text())
        assert count<=11
        if scenario in ('late-safe','empty'): assert count==11,(scenario,method,count)
print('test_wifi_nm: bounded reapply transition/late-overlap/empty-profile validation PASS')
