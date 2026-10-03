#!/usr/bin/env bash
# USB-only diagnostic ramdisk. No kernel build or boot-image packaging.
set -euo pipefail
umask 022
cd "$(dirname "$0")/.."
source_root="$PWD/out/initramfs-root"
output_root="$PWD/out/drm-minimal"
[[ ! -L "$output_root" ]] || { echo 'Output directory is a symlink' >&2; exit 1; }
mkdir -p "$output_root"
[[ ! -e "$output_root/root" && ! -e "$output_root/initramfs.cpio.gz" ]] || {
    echo 'Refusing existing diagnostic artifacts; preserve/move out/drm-minimal first' >&2; exit 1;
}
sha256sum out/boot.img out/initramfs.cpio.gz out/kernel/.config out/kernel/arch/arm64/boot/Image.gz-dtb \
    out/kernel-drm/arch/arm64/boot/Image.gz-dtb out/kernel-drm-control/arch/arm64/boot/Image.gz-dtb \
    out/kernel-drm-core/arch/arm64/boot/Image.gz-dtb > "$output_root/protected-before.sha256"
trap 'status=$?; trap - EXIT; sha256sum -c "$output_root/protected-before.sha256" > "$output_root/preservation-after.txt" || status=1; exit "$status"' EXIT
for binary in busybox busybox-extras; do
    [[ -x "$source_root/bin/$binary" ]] || { echo "Missing packaged $binary" >&2; exit 1; }
done
python3 - "$source_root" > "$output_root/dependency-check.txt" <<'PY'
import pathlib, subprocess, re, sys
root=pathlib.Path(sys.argv[1])
for name in ['busybox','busybox-extras']:
 p=root/'bin'/name
 hdr=subprocess.check_output(['readelf','-h',str(p)],text=True)
 assert 'AArch64' in hdr
 program=subprocess.check_output(['readelf','-l',str(p)],text=True)
 assert '[Requesting program interpreter: /lib/ld-musl-aarch64.so.1]' in program
 dynamic=subprocess.check_output(['readelf','-d',str(p)],text=True)
 needed=re.findall(r'Shared library: \[(.*?)\]',dynamic)
 assert needed==['libc.musl-aarch64.so.1'],needed
 print(name,'AArch64 interpreter=/lib/ld-musl-aarch64.so.1 NEEDED=',needed)
p=root/'usr/bin/btprobe'
assert p.is_file() and p.stat().st_mode & 0o111
assert 'AArch64' in subprocess.check_output(['readelf','-h',str(p)],text=True)
assert 'INTERP' not in subprocess.check_output(['readelf','-l',str(p)],text=True)
assert 'Shared library:' not in subprocess.check_output(['readelf','-d',str(p)],text=True)
print('btprobe AArch64 static: no interpreter or NEEDED dependencies; manual recovery only')
assert (root/'lib/libc.musl-aarch64.so.1').is_symlink()
assert (root/'lib/libc.musl-aarch64.so.1').readlink()==pathlib.Path('ld-musl-aarch64.so.1')
assert (root/'lib/ld-musl-aarch64.so.1').is_file()
print('Musl loader plus relative libc symlink closure verified')
PY
mkdir -p "$output_root/root"/{bin,sbin,usr/bin,usr/sbin,lib,etc,proc,sys,dev,tmp,run,root}
ramdisk_root="$output_root/root"
cp -p "$source_root/bin/busybox" "$source_root/bin/busybox-extras" "$ramdisk_root/bin/"
cp -p "$source_root/lib/ld-musl-aarch64.so.1" "$ramdisk_root/lib/"
ln -s ld-musl-aarch64.so.1 "$ramdisk_root/lib/libc.musl-aarch64.so.1"
ln -s busybox "$ramdisk_root/bin/sh"
cp -p "$source_root/usr/bin/btprobe" "$ramdisk_root/usr/bin/btprobe"
cat > "$ramdisk_root/init" <<'INIT'
#!/bin/busybox sh
# PID1 USB-only diagnostic: all state is initramfs/tmpfs/configfs.
export PATH=/sbin:/usr/sbin:/bin:/usr/bin HOME=/root
/bin/busybox --install -s
/bin/busybox-extras --install -s
stage() {
    printf '%s\n' "$1" > /run/drm-diagnostic-stage
    : > "/run/drm-diagnostic-$1"
    printf 'cyclo-drm-minimal: %s\n' "$1" > /dev/kmsg 2>/dev/null || :
}
fail() {
    stage "failed-$1"
    # Keep PID1 alive without a reset policy or disk activity.
    while :; do /bin/busybox sleep 60; done
}
/bin/busybox mount -t proc proc /proc || fail proc
/bin/busybox mount -t sysfs sysfs /sys || fail sysfs
/bin/busybox mount -t devtmpfs devtmpfs /dev || fail devtmpfs
/bin/busybox mkdir -p /dev/pts
/bin/busybox mount -t devpts devpts /dev/pts || fail devpts
/bin/busybox mount -t tmpfs tmpfs /tmp || fail tmp
/bin/busybox mount -t tmpfs tmpfs /run || fail run
stage init-mounted
/bin/busybox mount -t configfs configfs /sys/kernel/config || fail configfs
stage configfs-mounted
SERIAL=chef
for arg in $(/bin/busybox cat /proc/cmdline); do
    case "$arg" in androidboot.serialno=*) SERIAL=${arg#*=} ;; esac
done
G=/sys/kernel/config/usb_gadget/cyclo
/bin/busybox mkdir -p "$G" || fail gadget
printf '%s\n' 0x1d6b > "$G/idVendor" || fail vendor
printf '%s\n' 0x0104 > "$G/idProduct" || fail product
printf '%s\n' 0x0200 > "$G/bcdUSB" || fail usb-version
printf '%s\n' 0x0100 > "$G/bcdDevice" || fail device-version
/bin/busybox mkdir -p "$G/strings/0x409" "$G/configs/c.1/strings/0x409" "$G/functions/ncm.usb0" || fail gadget-dirs
printf '%s\n' "$SERIAL" > "$G/strings/0x409/serialnumber" || fail serial
printf '%s\n' chef-cyclo > "$G/strings/0x409/manufacturer" || fail manufacturer
printf '%s\n' 'Moto One Power (cyclo)' > "$G/strings/0x409/product" || fail product-string
printf '%s\n' ncm > "$G/configs/c.1/strings/0x409/configuration" || fail configuration
printf '%s\n' 500 > "$G/configs/c.1/MaxPower" || fail maxpower
printf '%s\n' 02:43:59:43:4c:01 > "$G/functions/ncm.usb0/dev_addr" || fail device-mac
printf '%s\n' 02:43:59:43:4c:02 > "$G/functions/ncm.usb0/host_addr" || fail host-mac
/bin/busybox ln -s "$G/functions/ncm.usb0" "$G/configs/c.1/" || fail function-link
stage gadget-configured
i=0
while [ -z "$(/bin/busybox ls /sys/class/udc 2>/dev/null)" ] && [ "$i" -lt 100 ]; do
    /bin/busybox sleep 0.1; i=$((i+1))
done
UDC=$(/bin/busybox ls /sys/class/udc 2>/dev/null | /bin/busybox head -n1)
[ -n "$UDC" ] || fail no-udc
printf '%s\n' "$UDC" > "$G/UDC" || fail udc-bind
printf '%s\n' "$UDC" > /run/usb-udc
stage udc-bound
IFACE=$(/bin/busybox cat "$G/functions/ncm.usb0/ifname") || fail ifname
[ -n "$IFACE" ] || fail ifname-empty
/bin/busybox ip link set "$IFACE" up || fail link
/bin/busybox ip addr add 172.16.42.1/24 dev "$IFACE" || fail address
/bin/busybox sed -i "s/^interface .*/interface $IFACE/" /etc/udhcpd.conf || fail dhcp-interface
: > /tmp/udhcpd.leases
printf '%s\n' "$IFACE" > /run/usb-interface
stage ip-configured
stage exec-init
exec /bin/busybox init
INIT
cat > "$ramdisk_root/bin/diagnostic-shell" <<'SHELL'
#!/bin/busybox sh
export PATH=/sbin:/usr/sbin:/bin:/usr/bin HOME=/root
cd /root || exit 1
exec /bin/busybox sh -i
SHELL
cat > "$ramdisk_root/bin/diagnostic-service" <<'SERVICE'
#!/bin/busybox sh
case "$1" in
    telnetd|udhcpd) service=$1; shift ;;
    *) exit 2 ;;
esac
printf 'cyclo-drm-minimal: launching %s\n' "$service" > /dev/kmsg 2>/dev/null || :
: > "/run/drm-diagnostic-$service-starting"
exec /bin/busybox-extras "$service" "$@"
SERVICE
cat > "$ramdisk_root/etc/inittab" <<'INITTAB'
::respawn:/bin/diagnostic-service telnetd -F -b 172.16.42.1 -l /bin/diagnostic-shell
::respawn:/bin/diagnostic-service udhcpd -f /etc/udhcpd.conf
INITTAB
cp initramfs/etc/udhcpd.conf "$ramdisk_root/etc/udhcpd.conf"
printf 'root:x:0:0:root:/root:/bin/sh\n' > "$ramdisk_root/etc/passwd"
printf 'root:x:0:\n' > "$ramdisk_root/etc/group"
printf 'USB-only diagnostic ramdisk; no firmware, modules, display or disk setup\n' > "$ramdisk_root/etc/drm-diagnostic-build"
chmod 755 "$ramdisk_root/init" "$ramdisk_root/bin/diagnostic-shell" "$ramdisk_root/bin/diagnostic-service"
for script in init bin/diagnostic-shell bin/diagnostic-service; do
    sh -n "$ramdisk_root/$script"
    /usr/bin/busybox sh -n "$ramdisk_root/$script"
done
sha256sum scripts/build-drm-diagnostic-initramfs.sh initramfs/init initramfs/etc/udhcpd.conf \
    "$source_root/bin/busybox" "$source_root/bin/busybox-extras" "$source_root/lib/ld-musl-aarch64.so.1" "$source_root/usr/bin/btprobe" > "$output_root/input-hashes.txt"
cp scripts/build-drm-diagnostic-initramfs.sh "$output_root/builder.snapshot.sh"
find "$ramdisk_root" -exec touch -h -d @0 {} +
(cd "$ramdisk_root"; find . -print0 | LC_ALL=C sort -z | cpio --null -o --format=newc --owner=0:0 --reproducible) > "$output_root/initramfs.cpio" 2> "$output_root/cpio-build.log"
gzip -n -9 -c "$output_root/initramfs.cpio" > "$output_root/initramfs.cpio.gz"
gzip -t "$output_root/initramfs.cpio.gz"
cpio -it < "$output_root/initramfs.cpio" > "$output_root/archive-files.txt" 2> "$output_root/cpio-list.log"
sha256sum "$output_root/initramfs.cpio" "$output_root/initramfs.cpio.gz" > "$output_root/artifact-hashes.txt"
wc -c "$output_root/initramfs.cpio" "$output_root/initramfs.cpio.gz" > "$output_root/artifact-sizes.txt"
cat "$output_root/artifact-hashes.txt" "$output_root/artifact-sizes.txt"
