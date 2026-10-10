#!/usr/bin/env python3
"""SSH key gates for the image build (scripts/mkinitramfs.sh, docs/features/ssh.md).

    ssh-keys.py check FILE   FILE is a usable authorized_keys for the image:
                             a regular file (not a symlink) owned by the
                             caller, not writable by group or others, in a
                             directory that is not either; at least one key;
                             every other line blank or a # comment; every key
                             line exactly `TYPE BASE64 [COMMENT]` with no
                             options, a known public key type and a blob that
                             decodes and names the same type (an ed25519 blob
                             must hold exactly a 32-byte key); nothing that
                             looks like private key material; and, when the
                             host has ssh-keygen, `ssh-keygen -l` must list
                             every key.
    ssh-keys.py scan DIR     no private key material anywhere in the staged
                             tree DIR: no file named like an SSH host key, and
                             no non-ELF file with a PEM/OpenSSH private key
                             header or starting with a raw (dropbear) key
                             blob. ELF files are skipped: libraries such as
                             libcrypto carry the PEM header strings as code.

Exit 0 when the gate passes, 1 with one line per problem otherwise.
"""
import base64
import os
import re
import shutil
import stat
import struct
import subprocess
import sys

TYPES = {
    "ssh-ed25519",
    "ssh-rsa",
    "ecdsa-sha2-nistp256",
    "ecdsa-sha2-nistp384",
    "ecdsa-sha2-nistp521",
    "sk-ssh-ed25519@openssh.com",
    "sk-ecdsa-sha2-nistp256@openssh.com",
}
PRIVATE_MARKERS = (b"PRIVATE KEY-----", b"openssh-key-v1", b"PuTTY-User-Key-File")
# A dropbear private key file is the bare SSH wire blob: string type, then
# the key fields. Public blobs have no business as a file in the tree either.
BLOB_PREFIXES = tuple(struct.pack(">I", len(t)) + t.encode() for t in ("ssh-ed25519", "ssh-rsa", "ssh-dss",
                                                                       "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
                                                                       "ecdsa-sha2-nistp521"))
HOSTKEY_NAME = re.compile(r"(^ssh_host_.*_key$|^dropbear_.*_host_key|host_key$|^id_(rsa|dsa|ecdsa|ed25519)(_sk)?$)")
B64 = re.compile(r"^[A-Za-z0-9+/]+={0,2}$")


def read_string(blob, off):
    if off + 4 > len(blob):
        raise ValueError("truncated blob")
    n = struct.unpack(">I", blob[off:off + 4])[0]
    if off + 4 + n > len(blob):
        raise ValueError("truncated blob")
    return blob[off + 4:off + 4 + n], off + 4 + n


def check_line(line):
    """Return None for a good key line, else the reason."""
    fields = line.split()
    if len(fields) < 2:
        return "not `TYPE BASE64 [COMMENT]`"
    ktype, b64 = fields[0], fields[1]
    if ktype not in TYPES:
        return f"unknown key type or options before the type ({ktype[:40]!r}); options are not allowed"
    if not B64.match(b64) or len(b64) % 4:
        return "key is not base64"
    try:
        blob = base64.b64decode(b64, validate=True)
        name, off = read_string(blob, 0)
    except (ValueError, base64.binascii.Error) as e:
        return f"key blob does not decode ({e})"
    if name.decode(errors="replace") != ktype:
        return f"blob type {name[:40]!r} does not match {ktype}"
    if ktype == "ssh-ed25519":
        try:
            pub, off = read_string(blob, off)
        except ValueError as e:
            return f"ed25519 blob is {e}"
        if len(pub) != 32 or off != len(blob):
            return "ed25519 blob does not hold exactly one 32-byte key"
    return None


def check(path):
    errs = []
    try:
        st = os.lstat(path)
    except OSError as e:
        return [f"{path}: {e.strerror}; create it with scripts/ssh-setup.sh (or build with SSH=0)"]
    if stat.S_ISLNK(st.st_mode):
        return [f"{path}: is a symlink; use a regular file"]
    if not stat.S_ISREG(st.st_mode):
        return [f"{path}: not a regular file"]
    if st.st_uid != os.getuid():
        errs.append(f"{path}: owned by uid {st.st_uid}, not by the builder (uid {os.getuid()})")
    if st.st_mode & 0o022:
        errs.append(f"{path}: writable by group or others (mode {stat.S_IMODE(st.st_mode):o}); chmod 600 it")
    d = os.path.dirname(os.path.abspath(path))
    dst = os.stat(d)
    if dst.st_mode & 0o022:
        errs.append(f"{d}: writable by group or others (mode {stat.S_IMODE(dst.st_mode):o}); chmod 700 it")
    data = open(path, "rb").read()
    if any(m in data for m in PRIVATE_MARKERS) or data.startswith(BLOB_PREFIXES):
        return errs + [f"{path}: contains private key material; put only public keys (*.pub lines) here"]
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError:
        return errs + [f"{path}: not ASCII text"]
    keys = 0
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        why = check_line(line)
        if why:
            errs.append(f"{path}:{n}: {why}")
        else:
            keys += 1
    if keys == 0 and not errs:
        errs.append(f"{path}: no public key; scripts/ssh-setup.sh adds one (or build with SSH=0)")
    if not errs and shutil.which("ssh-keygen"):
        r = subprocess.run(["ssh-keygen", "-l", "-f", path], capture_output=True, text=True)
        listed = [l for l in r.stdout.splitlines() if l.strip()]
        if r.returncode or len(listed) != keys:
            errs.append(f"{path}: ssh-keygen -l lists {len(listed)} of {keys} keys: {r.stderr.strip()}")
    return errs


def scan(root):
    errs = []
    for dirpath, dirnames, filenames in os.walk(root):
        for f in filenames:
            p = os.path.join(dirpath, f)
            rel = os.path.relpath(p, root)
            if HOSTKEY_NAME.search(f):
                errs.append(f"{rel}: named like a private SSH key")
                continue
            st = os.lstat(p)
            if not stat.S_ISREG(st.st_mode):
                continue
            with open(p, "rb") as fh:
                head = fh.read(4)
                if head == b"\x7fELF":
                    continue
                data = head + fh.read()
            if any(m in data for m in PRIVATE_MARKERS) or data.startswith(BLOB_PREFIXES):
                errs.append(f"{rel}: contains private key material")
    return errs


def main(argv):
    if len(argv) != 3 or argv[1] not in ("check", "scan"):
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    errs = check(argv[2]) if argv[1] == "check" else scan(argv[2])
    for e in errs:
        print(f"ssh-keys: {e}", file=sys.stderr)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
