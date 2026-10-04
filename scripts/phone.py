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
import sys
import tarfile

try:
    import pexpect
except ImportError:
    sys.exit("phone.py needs pexpect (python3-pexpect)")

HOST = os.environ.get("PHONE_HOST", "172.16.42.1")
PROMPT = "PROBE_SHELL> "


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


def cmd_run(a):
    line = " ".join(a.cmd) if a.cmd else sys.stdin.read().strip()
    if not line:
        sys.exit("run: no command")
    p = Phone(a.log, "run")
    rc, out = p.run(line, a.timeout)
    p.close()
    sys.stdout.write(out)
    return rc


def cmd_push(a):
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
