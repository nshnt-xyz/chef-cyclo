#!/bin/sh
# link-applets.sh ROOT: make the busybox and busybox-extras applet links in
# ROOT from the packages' own lists (ROOT/etc/busybox-paths.d, which is
# `busybox --list-full`). Alpine makes them in a post-install trigger that
# apk --no-scripts skips; the root is read-only at runtime, so they are made
# at build time: absolute links to /bin/busybox and /bin/busybox-extras,
# busybox first, never replacing an existing path, as `busybox --install -s`.
set -eu
ROOT=$1
for pkg in busybox busybox-extras; do
    list="$ROOT/etc/busybox-paths.d/$pkg"
    [ -s "$list" ] && [ -x "$ROOT/bin/$pkg" ] || { echo "missing $pkg or its applet list" >&2; exit 1; }
    while IFS= read -r applet; do
        case "$applet" in
        ''|/*|*..*|*' '*) echo "bad applet path '$applet' in $list" >&2; exit 1 ;;
        esac
        [ -e "$ROOT/$applet" ] || [ -L "$ROOT/$applet" ] || ln -s "/bin/$pkg" "$ROOT/$applet"
    done < "$list"
done
