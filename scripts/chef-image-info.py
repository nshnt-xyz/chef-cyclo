#!/usr/bin/env python3
"""Generate and validate non-security display metadata for a chef image."""
import os
import re
import subprocess
import sys
from pathlib import Path

MIN_EPOCH = 1767225600  # 2026-01-01 UTC
MAX_EPOCH = 4102444800  # 2100-01-01 UTC, exclusive
COMMIT_RE = re.compile(r"[0-9a-f]{40}\Z")
EPOCH_RE = re.compile(r"(?:0|[1-9][0-9]{0,11})\Z")


def parse(text):
    if not isinstance(text, str) or len(text.encode("utf-8")) > 1024:
        raise ValueError("metadata exceeds 1024 bytes")
    if not text.endswith("\n") or any(ord(c) < 32 and c != "\n" for c in text):
        raise ValueError("invalid controls or final newline")
    lines = text[:-1].split("\n")
    if len(lines) != 6 or lines[0] != "chef-image-info 1":
        raise ValueError("unsupported metadata layout")
    values = {}
    expected = ("name", "version", "commit", "dirty", "epoch")
    for line, key in zip(lines[1:], expected):
        field, sep, value = line.partition(" ")
        if not sep or field != key or not value or key in values:
            raise ValueError("invalid metadata field order")
        values[key] = value
    if values["name"] != "chef-cyclo" or values["version"] != "dev":
        raise ValueError("invalid image identity")
    if not COMMIT_RE.fullmatch(values["commit"]):
        raise ValueError("invalid commit")
    if values["dirty"] not in ("0", "1"):
        raise ValueError("invalid dirty flag")
    if not EPOCH_RE.fullmatch(values["epoch"]):
        raise ValueError("invalid epoch")
    epoch = int(values["epoch"])
    if not MIN_EPOCH <= epoch < MAX_EPOCH:
        raise ValueError("epoch out of range")
    return values


def generate(root=None, environ=None):
    root = Path(root or Path(__file__).resolve().parent.parent)
    env = os.environ if environ is None else environ
    commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"],
                                     text=True).strip()
    if not COMMIT_RE.fullmatch(commit):
        raise ValueError("git returned an invalid HEAD")
    dirty = bool(subprocess.check_output(["git", "-C", str(root), "status", "--porcelain"],
                                         text=True))
    if "SOURCE_DATE_EPOCH" in env:
        raw = env["SOURCE_DATE_EPOCH"]
        if not EPOCH_RE.fullmatch(raw) or not MIN_EPOCH <= int(raw) < MAX_EPOCH:
            raise ValueError("SOURCE_DATE_EPOCH must be canonical seconds in 2026..2099")
        epoch = int(raw)
    else:
        epoch = int(subprocess.check_output(["git", "-C", str(root), "show", "-s", "--format=%ct", "HEAD"],
                                            text=True).strip())
    if not MIN_EPOCH <= epoch < MAX_EPOCH:
        raise ValueError("commit epoch out of supported range")
    result = ("chef-image-info 1\nname chef-cyclo\nversion dev\n"
              f"commit {commit}\ndirty {int(dirty)}\nepoch {epoch}\n")
    parse(result)
    return result


def main():
    if len(sys.argv) != 1:
        raise ValueError("no arguments are accepted")
    sys.stdout.write(generate())


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        sys.exit(f"chef-image-info: {exc}")
