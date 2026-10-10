#!/usr/bin/env python3
"""Drive the phone's busybox telnetd (172.16.42.1 over USB) from the host.

    scripts/phone.py run 'cmd; cmd'          run a shell line, print its output,
    echo 'cmd' | scripts/phone.py run        exit with the remote status
    scripts/phone.py push FILE... [-d DIR]   copy files to the phone (default /run/push),
                                             verified by sha256 on both ends
    scripts/phone.py pull PATH... -o OUT.tgz byte-exact tar.gz of phone paths
    scripts/phone.py stream 'cmd' OUTFILE    stream a long-running command's output
                                             to OUTFILE until it ends or the phone goes

Options before the subcommand: -t SECS (run/pull timeout, default 60) and
--log DIR (keep a transcript of each session there; default: none).
PHONE_HOST overrides the address. Needs `telnet` and python3-pexpect.

--ssh ADDR (or PHONE_SSH=ADDR) uses SSH instead of telnet, for example over
Wi-Fi with the USB cable out (docs/features/ssh.md): root@ADDR with the
dedicated key ~/.ssh/chef-cyclo_ed25519 (PHONE_SSH_KEY), BatchMode, only that
key, and its own known_hosts file ~/.ssh/chef-cyclo_known_hosts
(PHONE_SSH_KNOWN_HOSTS) under HostKeyAlias chef-cyclo, so every address of the
phone shares one entry; a new host key is accepted once, a changed one is
refused. ~/.ssh/config is not read. The remote command gets the telnet
shell's PATH, HOME and session bus. --log does not apply to SSH.

Quirks this handles (docs/live-testing.md): echo is turned off and the
terminal widened first, so long lines don't wrap into the output markers;
output carries CRLF, which is stripped; busybox base64 has no -w, so uploads
go as <=1500-char printf chunks (the pty line limit is 4096) and downloads are
whitespace-joined before decoding. Closing the session SIGHUPs foreground
jobs: start daemons with `setsid CMD </dev/null >/run/x.out 2>&1 &`.
"""
import argparse
import base64
import datetime
import hashlib
import io
import os
import pathlib
import shlex
import subprocess
import sys
import tarfile

try:
    import pexpect
except ImportError:
    sys.exit("phone.py needs pexpect (python3-pexpect)")

HOST = os.environ.get("PHONE_HOST", "172.16.42.1")
PROMPT = "PROBE_SHELL> "
SSH_KEY = os.environ.get("PHONE_SSH_KEY", os.path.expanduser("~/.ssh/chef-cyclo_ed25519"))
SSH_KNOWN = os.environ.get("PHONE_SSH_KNOWN_HOSTS", os.path.expanduser("~/.ssh/chef-cyclo_known_hosts"))
SSH_ALIAS = "chef-cyclo"
# What the inittab telnetd line gives its shells (dropbear's default PATH
# lacks the sbin directories).
SSH_ENV = ("export PATH=/sbin:/usr/sbin:/bin:/usr/bin HOME=/root XDG_RUNTIME_DIR=/run/user/0 "
           "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/0/bus; ")
HOSTKEY_HELP = f"""phone.py: the phone's SSH host key does not match {SSH_KNOWN}.
A RAM-only boot (/data not ours) serves an ephemeral key from /run/ssh: check
`cat /run/ssh/host-key-source` over telnet. If the change is expected, drop
the old entry with: ssh-keygen -R {SSH_ALIAS} -f {SSH_KNOWN}"""


class Phone:
    def __init__(self, log_dir=None, tag="session", timeout=30):
        self.c = pexpect.spawn("telnet", [HOST], encoding="utf-8", timeout=timeout,
                               maxread=1 << 20)
        if log_dir:
            d = pathlib.Path(log_dir)
            d.mkdir(parents=True, exist_ok=True)
            stamp = datetime.datetime.now().strftime("%H%M%S%f")
            self.c.logfile_read = open(d / f"{tag}-{stamp}.txt", "w")
        self.c.expect("# ")
        self.c.sendline(f"stty -echo; stty columns 1000; export PS1='{PROMPT}'")
        self.c.expect("\r\n" + PROMPT)

    def run(self, cmd, timeout=60):
        """Run one shell line; return (exit status, output without CRs)."""
        self.c.sendline(cmd + '; echo "RC_TAG=$?"')
        self.c.expect(r"RC_TAG=(\d+)", timeout=timeout)
        out = self.c.before.replace("\r", "")
        rc = int(self.c.match.group(1))
        self.c.expect(PROMPT)
        return rc, out

    def close(self):
        self.c.close()


def ssh_argv(addr, remote, alive=False):
    """The ssh command line for one remote shell command on ADDR."""
    argv = ["ssh", "-F", "none", "-T", "-l", "root", "-i", SSH_KEY,
            "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
            "-o", "PasswordAuthentication=no", "-o", "KbdInteractiveAuthentication=no",
            "-o", f"UserKnownHostsFile={SSH_KNOWN}", "-o", "GlobalKnownHostsFile=/dev/null",
            "-o", "StrictHostKeyChecking=accept-new", "-o", f"HostKeyAlias={SSH_ALIAS}",
            "-o", "ConnectTimeout=10", "-o", "LogLevel=ERROR"]
    if alive:
        argv += ["-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3"]
    return argv + [addr, SSH_ENV + remote]


def ssh(a, remote, data=None, timeout=None):
    """Run REMOTE over SSH; return (status, stdout bytes, stderr text)."""
    try:
        r = subprocess.run(ssh_argv(a.ssh, remote), input=data, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        sys.exit(f"phone.py: no answer from {a.ssh} within {timeout} s")
    err = r.stderr.decode(errors="replace")
    if r.returncode == 255 and ("IDENTIFICATION HAS CHANGED" in err or "Host key verification failed" in err):
        sys.exit(err.rstrip() + "\n" + HOSTKEY_HELP)
    return r.returncode, r.stdout, err


def ssh_run(a, line):
    rc, out, err = ssh(a, line, timeout=a.timeout)
    sys.stdout.write(out.decode(errors="replace"))
    sys.stderr.write(err)
    return rc


def ssh_push(a):
    d = a.dir.rstrip("/") or "/"
    for f in a.files:
        data = pathlib.Path(f).read_bytes()
        dst = f"{d}/{pathlib.Path(f).name}"
        mode = "755" if os.access(f, os.X_OK) else "644"
        q, qt = shlex.quote(dst), shlex.quote(dst + ".part")
        rc, out, err = ssh(a, f"mkdir -p {shlex.quote(d)} && cat > {qt} && chmod {mode} {qt} && mv -f {qt} {q} && sha256sum {q}",
                           data=data, timeout=a.timeout)
        here = hashlib.sha256(data).hexdigest()
        if rc or here not in out.decode(errors="replace"):
            sys.exit(f"{f}: phone copy does not match\n{out.decode(errors='replace')}{err}")
        print(f"{f} -> {dst} sha256 {here}")
    return 0


def ssh_pull(a):
    paths = " ".join(shlex.quote(x) for x in a.paths)
    rc, data, err = ssh(a, f"tar -czf - {paths} 2>/dev/null", timeout=a.timeout)
    if rc == 255:
        sys.exit(f"pull: ssh failed, nothing written\n{err}".rstrip())
    if not data:
        sys.exit(f"pull: empty archive (no such paths?) {err}".rstrip())
    try:
        names = tarfile.open(fileobj=io.BytesIO(data), mode="r:gz").getnames()
    except (tarfile.TarError, EOFError, OSError) as e:
        sys.exit(f"pull: truncated or invalid archive ({e}), nothing written\n{err}".rstrip())
    pathlib.Path(a.out).write_bytes(data)
    print(f"{a.out}: {len(names)} entries, sha256 {hashlib.sha256(data).hexdigest()}")
    return 0


def ssh_stream(a):
    with open(a.outfile, "wb") as out:
        p = subprocess.Popen(ssh_argv(a.ssh, a.cmd, alive=True), stdin=subprocess.DEVNULL,
                             stdout=out, stderr=subprocess.STDOUT)
        try:
            p.wait(timeout=a.max)
        except subprocess.TimeoutExpired:
            p.terminate()
            p.wait()
    return 0


def cmd_run(a):
    line = " ".join(a.cmd) if a.cmd else sys.stdin.read().strip()
    if not line:
        sys.exit("run: no command")
    if a.ssh:
        return ssh_run(a, line)
    p = Phone(a.log, "run")
    rc, out = p.run(line, a.timeout)
    p.close()
    sys.stdout.write(out)
    return rc


def cmd_push(a):
    if a.ssh:
        return ssh_push(a)
    p = Phone(a.log, "push")
    d = a.dir.rstrip("/") or "/"
    rc, out = p.run(f"mkdir -p {shlex.quote(d)}")
    if rc:
        sys.exit(out)
    for f in a.files:
        data = pathlib.Path(f).read_bytes()
        dst = shlex.quote(f"{d}/{pathlib.Path(f).name}")
        b = base64.b64encode(data).decode()
        p.run(f"rm -f {dst}.b64")
        for i in range(0, len(b), 1500):
            rc, out = p.run(f"printf %s '{b[i:i + 1500]}' >> {dst}.b64")
            if rc:
                sys.exit(out)
        mode = "755" if os.access(f, os.X_OK) else "644"
        rc, out = p.run(f"base64 -d {dst}.b64 > {dst} && rm {dst}.b64 && chmod {mode} {dst} && sha256sum {dst}")
        here = hashlib.sha256(data).hexdigest()
        if rc or here not in out:
            sys.exit(f"{f}: phone copy does not match\n{out}")
        print(f"{f} -> {d}/{pathlib.Path(f).name} sha256 {here}")
    p.close()
    return 0


def cmd_pull(a):
    if a.ssh:
        return ssh_pull(a)
    p = Phone(a.log, "pull")
    paths = " ".join(shlex.quote(x) for x in a.paths)
    p.c.sendline(f"tar -czf - {paths} 2>/dev/null | base64; echo END_TAG")
    p.c.expect("END_TAG", timeout=a.timeout)
    blob = "".join(p.c.before.split())
    p.c.expect(PROMPT)
    p.close()
    data = base64.b64decode(blob)
    if not data:
        sys.exit("pull: empty archive (no such paths?)")
    names = tarfile.open(fileobj=io.BytesIO(data), mode="r:gz").getnames()
    pathlib.Path(a.out).write_bytes(data)
    print(f"{a.out}: {len(names)} entries, sha256 {hashlib.sha256(data).hexdigest()}")
    return 0


def cmd_stream(a):
    if a.ssh:
        return ssh_stream(a)
    c = pexpect.spawn("telnet", [HOST], encoding="utf-8", timeout=None, maxread=65536)
    c.logfile_read = open(a.outfile, "w", buffering=1)
    c.expect("# ")
    c.sendline("stty -echo; export PS1=''")
    c.sendline(a.cmd)
    try:
        c.expect(pexpect.EOF, timeout=a.max)
    except pexpect.TIMEOUT:
        pass
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-t", "--timeout", type=int, default=60)
    ap.add_argument("--log", metavar="DIR")
    ap.add_argument("--ssh", metavar="ADDR", default=os.environ.get("PHONE_SSH") or None,
                    help="use SSH to root@ADDR instead of telnet (default: $PHONE_SSH)")
    sub = ap.add_subparsers(dest="op", required=True)
    s = sub.add_parser("run")
    s.add_argument("cmd", nargs="*")
    s.set_defaults(fn=cmd_run)
    s = sub.add_parser("push")
    s.add_argument("files", nargs="+")
    s.add_argument("-d", "--dir", default="/run/push")
    s.set_defaults(fn=cmd_push)
    s = sub.add_parser("pull")
    s.add_argument("paths", nargs="+")
    s.add_argument("-o", "--out", required=True)
    s.set_defaults(fn=cmd_pull)
    s = sub.add_parser("stream")
    s.add_argument("cmd")
    s.add_argument("outfile")
    s.add_argument("--max", type=int, default=1800, help="give up after SECS (default 1800)")
    s.set_defaults(fn=cmd_stream)
    a = ap.parse_args()
    sys.exit(a.fn(a))


if __name__ == "__main__":
    main()
