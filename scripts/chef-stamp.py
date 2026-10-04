#!/usr/bin/env python3
"""Build stamp tying the system_a root to the kernel in the boot image.

    chef-stamp.py manifest ROOT                 tree manifest on stdout
    chef-stamp.py stamp ROOT KERNEL SYMVERS RELEASE MANIFEST
                                                stamp text on stdout
    chef-stamp.py check STAMP ROOT KERNEL SYMVERS RELEASE
                                                exit 1 unless STAMP matches
    chef-stamp.py ids STAMP                     "UUID HASH_SEED" for mke2fs

The stamp (/etc/chef/build-stamp on system_a, a byte-identical copy in the
stage-1 ramdisk) names the kernel release, the SHA-256 of the kernel image,
of its Module.symvers and of wlan.ko (the only module, built against that
Module.symvers), and of the tree manifest. The manifest lists every path of
the root with type, mode, owner and content hash; it leaves out the stamp
itself. Run `manifest` and `stamp` under the same fakeroot session as
mke2fs so owners are the ones packed into the image. mke2fs adds
/lost+found (root, 0700), which is not in the manifest.
"""
import hashlib
import os
import stat
import sys
import uuid

STAMP_PATH = "/etc/chef/build-stamp"
WLAN = "lib/modules/wlan.ko"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def manifest(root):
    """One line per path below ROOT, sorted bytewise, '/' first."""
    entries = ["/"]
    for top, dirs, files in os.walk(root):
        for name in dirs + files:
            full = os.path.join(top, name)
            rel = "/" + os.path.relpath(full, root)
            if rel != STAMP_PATH:
                entries.append(rel)
    lines = []
    for rel in sorted(set(entries), key=os.fsencode):
        full = root if rel == "/" else os.path.join(root, rel[1:])
        st = os.lstat(full)
        mode = stat.S_IMODE(st.st_mode)
        head = f"{rel}\t{{}}\t{mode:04o}\t{st.st_uid}\t{st.st_gid}"
        if stat.S_ISREG(st.st_mode):
            lines.append(head.format("f") + f"\t{st.st_size}\t{sha256_file(full)}")
        elif stat.S_ISDIR(st.st_mode):
            lines.append(head.format("d"))
        elif stat.S_ISLNK(st.st_mode):
            lines.append(head.format("l") + f"\t{os.readlink(full)}")
        else:
            sys.exit(f"chef-stamp: unexpected file type in the root: {rel}")
    return "".join(line + "\n" for line in lines)


def stamp(root, kernel, symvers, release, manifest_file):
    wlan = os.path.join(root, WLAN)
    with open(wlan, "rb") as f:
        if b"vermagic=" + release.encode() + b" " not in f.read():
            sys.exit(f"chef-stamp: {WLAN} was not built for {release}")
    return (
        "chef-cyclo-root 1\n"
        f"kernel-release {release}\n"
        f"kernel-image-sha256 {sha256_file(kernel)}\n"
        f"module-symvers-sha256 {sha256_file(symvers)}\n"
        f"wlan-ko-sha256 {sha256_file(wlan)}\n"
        f"tree-manifest-sha256 {sha256_file(manifest_file)}\n"
    )


def fields(text):
    lines = text.splitlines()
    if not lines or lines[0] != "chef-cyclo-root 1":
        raise ValueError("not a chef-cyclo-root 1 stamp")
    out = {}
    for line in lines[1:]:
        key, _, value = line.partition(" ")
        if not value or key in out:
            raise ValueError(f"bad stamp line {line!r}")
        out[key] = value
    want = {"kernel-release", "kernel-image-sha256", "module-symvers-sha256",
            "wlan-ko-sha256", "tree-manifest-sha256"}
    if set(out) != want:
        raise ValueError("stamp fields differ from version 1")
    return out


def check(stamp_file, root, kernel, symvers, release):
    with open(stamp_file) as f:
        got = fields(f.read())
    want = {
        "kernel-release": release,
        "kernel-image-sha256": sha256_file(kernel),
        "module-symvers-sha256": sha256_file(symvers),
        "wlan-ko-sha256": sha256_file(os.path.join(root, WLAN)),
    }
    bad = [k for k, v in want.items() if got[k] != v]
    if bad:
        sys.exit(f"chef-stamp: {stamp_file} does not match: {', '.join(bad)}")


def ids(stamp_file):
    with open(stamp_file) as f:
        digest = bytes.fromhex(fields(f.read())["tree-manifest-sha256"])
    return f"{uuid.UUID(bytes=digest[:16])} {uuid.UUID(bytes=digest[16:])}"


def main(argv):
    cmd, args = (argv[1], argv[2:]) if len(argv) > 1 else ("", [])
    try:
        if cmd == "manifest" and len(args) == 1:
            sys.stdout.write(manifest(args[0]))
        elif cmd == "stamp" and len(args) == 5:
            sys.stdout.write(stamp(*args))
        elif cmd == "check" and len(args) == 5:
            check(*args)
        elif cmd == "ids" and len(args) == 1:
            print(ids(args[0]))
        else:
            sys.exit(__doc__)
    except (OSError, ValueError) as e:
        sys.exit(f"chef-stamp: {e}")


if __name__ == "__main__":
    main(sys.argv)
