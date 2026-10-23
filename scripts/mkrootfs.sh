#!/bin/sh
# Populate out/rootfs with Alpine aarch64 packages: musl, busybox (+extras for
# telnetd/udhcpd), dbus, BlueZ, qmicli and gpsd. apk only unpacks archives, so the host's
# static x86_64 apk does the job with --no-scripts; whatever the skipped
# post-install scripts would have done (users, machine-id) is provided by the
# rootfs-overlay/ tree instead. The package cache lives under toolchain/ so a
# rebuild can reuse the local cache.
set -eu
cd "$(dirname "$0")/.."
APK=toolchain/apk/apk.static
[ -x "$APK" ] || { echo "run scripts/setup-toolchain.sh first" >&2; exit 1; }

ALPINE=${ALPINE:-https://dl-cdn.alpinelinux.org/alpine/v3.24}
ROOT=out/rootfs
CACHE=$PWD/toolchain/apk/cache
# gpsd: 3.27.3-r1, /usr/sbin/gpsd + libgps.so.32,
# pulls libcap2 + libstdc++ (+libgcc); 551 KB apk, 1.2 MB installed. Fed over
# loopback UDP by tools/nmea-broker.c. No gpsd-clients (3.7 MB, ncurses):
# verify with busybox nc on 2947 instead.
#
# tinyalsa (audio, docs/features/audio.md): community, 2.0.0-r4, 270 KB,
# libc-only -- tinymix/tinyplay/tinycap/tinypcminfo, the userspace side of
# rootfs-overlay/usr/bin/audio-up and speaker-test-tone. Not alsa-utils: that
# pulls ncurses/fftw/dialog, ~6 MB, for tools we don't need (amixer/aplay
# have no functional advantage here over tinymix/tinyplay). tinymix's
# control names match stock's mixer_paths.xml verbatim.
# SSH (docs/features/ssh.md): dropbear 2026.91 (530 KB installed; its
# utmps-libs and skalibs-libs were already in the root) and
# openssh-sftp-server (199 KB) at
# /usr/lib/ssh/sftp-server, the path compiled into Alpine's dropbear. Modern
# scp speaks SFTP, so that gives both scp and sftp; dropbear-scp (67 KB plus
# the 331 KB dbclient it depends on) would only add the legacy `scp -O`.
# tzdata (2026e-r0, 191 KB apk, 452 KB installed): the zone files under
# /usr/share/zoneinfo. The system zone is Asia/Kolkata the Alpine way,
# /etc/localtime linking to it and /etc/timezone naming it (both from
# rootfs-overlay/etc/), so date, logs and chefui all show IST; chefui falls
# back to the settings' POSIX TZ string only when the zone file is missing.
# UI/network tools: real iproute2 (ip/tc/ss), util-linux-misc provides rfkill.
# The prebuilt CA bundle works with --no-scripts; no trust-store regeneration
# is required. jq supports native ip/rfkill JSON workflows. tcpdump/iperf3
# are manual diagnostics only; no capture, server or extra manager is started.
# OpenRC (docs/features/services.md): openrc 0.63.2 (4.4 MB installed; it
# pulls ifupdown-ng, bridge, openrc-user and libcap2, none of them run),
# run from inittab as on Alpine, starts and supervises the services. Of the
# -openrc subpackages only those whose scripts we run: busybox-openrc
# (syslog), busybox-extras-openrc (udhcpd), dbus-openrc, bluez-openrc
# (bluetooth), chrony-openrc (chronyd), networkmanager-openrc; each is a few
# KB of scripts and conf.d defaults. The `!` entries keep out the
# subpackages apk would add on its own (install_if: a package's -openrc
# once openrc is in, ifupdown-ng's executors): scripts we never run, and
# each init.d script costs time in OpenRC's dependency scan.
# polkit (docs/features/users-and-permissions.md): 127-r2 (133 KB
# installed) with polkit-common (371 KB), polkit-noelogind-libs (200 KB),
# duktape-libs (329 KB, its JavaScript rules engine) and linux-pam (3.3 MB,
# 2.7 MB of it PAM modules nothing here loads), so NetworkManager can let
# the netdev group (chef-settings, as chefui) drive it. polkitd is D-Bus
# activated (the package's service file) and drops to polkitd itself;
# polkit-openrc stays out. mkinitramfs.sh removes pkexec,
# polkit-agent-helper-1 and unix_chkpwd.
# UPower (docs/features/battery-and-charging.md): upower 1.91.0-r2 (128 KB
# apk, 468 KB installed; libgudev and libpolkit-gobject are already in the
# root), with upower-udev, which apk adds on its own (install_if udev). Our
# OpenRC service upower runs upowerd as user upower; mkinitramfs.sh removes
# the package's D-Bus activation file, which would start a root upowerd
# whenever the service is down.
PKGS="alpine-baselayout musl busybox busybox-extras
      dbus bluez bluez-btmgmt bluez-btmon bluez-deprecated
      qmi-utils gpsd tinyalsa wpa_supplicant iw
      iproute2 util-linux-misc curl ca-certificates-bundle jq tcpdump iperf3
      networkmanager networkmanager-wifi networkmanager-cli networkmanager-tui
      eudev glib apk-tools iptables iptables-legacy chrony e2fsprogs e2fsprogs-extra
      dropbear openssh-sftp-server tzdata polkit upower
      openrc busybox-openrc busybox-extras-openrc dbus-openrc bluez-openrc
      chrony-openrc networkmanager-openrc
      !bluez-deprecated-openrc !dropbear-openrc !eudev-openrc !gpsd-openrc
      !iperf3-openrc !iptables-openrc !polkit-openrc !udev-init-scripts-openrc
      !wpa_supplicant-openrc !ifupdown-ng-iproute2 !ifupdown-ng-wifi"

rm -rf "$ROOT"
mkdir -p "$ROOT" "$CACHE"
"$APK" --root "$ROOT" --arch aarch64 --initdb \
       --keys-dir "$PWD/toolchain/apk/keys" \
       --repository "$ALPINE/main" --repository "$ALPINE/community" \
       --cache-dir "$CACHE" --no-scripts --no-interactive --usermode \
       add $PKGS
# Keep runtime apk, repositories and public signing keys so standard Wi-Fi
# tools can be installed normally. Downloads/caches remain volatile in RAM.
rm -rf "$ROOT/var/cache/apk"
mkdir -p "$ROOT/etc/apk/keys"
cp toolchain/apk/keys/* "$ROOT/etc/apk/keys/"
printf '%s/main\n%s/community\n' "$ALPINE" "$ALPINE" > "$ROOT/etc/apk/repositories"

du -sh "$ROOT"
ls "$ROOT/usr/lib/bluetooth/bluetoothd" "$ROOT/usr/bin/btattach" "$ROOT/usr/bin/dbus-daemon" \
   "$ROOT/usr/bin/qmicli" "$ROOT/usr/sbin/gpsd" "$ROOT/usr/lib/libgps.so.32" \
   "$ROOT/bin/busybox" "$ROOT/bin/busybox-extras" \
   "$ROOT/usr/bin/tinymix" "$ROOT/usr/bin/tinyplay" \
   "$ROOT/usr/sbin/dropbear" "$ROOT/usr/bin/dropbearkey" "$ROOT/usr/lib/ssh/sftp-server" \
   "$ROOT/usr/share/zoneinfo/Asia/Kolkata" \
   "$ROOT/sbin/openrc" "$ROOT/sbin/supervise-daemon" "$ROOT/etc/init.d/syslog" \
   "$ROOT/etc/init.d/udhcpd" "$ROOT/etc/init.d/dbus" "$ROOT/etc/init.d/bluetooth" \
   "$ROOT/etc/init.d/chronyd" "$ROOT/etc/init.d/networkmanager" \
   "$ROOT/usr/lib/polkit-1/polkitd" "$ROOT/usr/libexec/upowerd" \
   "$ROOT/usr/share/dbus-1/system-services/org.freedesktop.PolicyKit1.service"
