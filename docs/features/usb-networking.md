# USB networking and shell

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior

`initramfs/init` creates a USB NCM Ethernet gadget through `initramfs/usr/lib/chef/usb-gadget.sh`, assigns `usb0` 172.16.42.1/24, and configures loopback. `udhcpd` offers the host addresses .2–.9 with no router option, from `/run/udhcpd.conf` (the `/etc/udhcpd.conf` template with the actual interface; the root is read-only). The stage-1 rescue shell uses the same helper, so a refused `system_a` still answers on 172.16.42.1 ([live testing](../live-testing.md#gotchas)). Linux hosts use `cdc_ncm`. The phone buzzes once when `/init` starts and twice when the network is ready.

## Use

After [booting the image](../building.md), let the host obtain a DHCP lease, then run on the host:

```sh
ping 172.16.42.1
telnet 172.16.42.1
```

The shell is root with no password. In the phone shell, `reboot` returns to the flashed OS. Download any RAM-only evidence first.

## Modify and verify

Relevant files are `initramfs/init`, `initramfs/usr/lib/chef/usb-gadget.sh` (shared with `stage1/init`), `initramfs/etc/udhcpd.conf`, and `initramfs/etc/inittab`. Change gadget/address setup in the helper and daemon arguments in the inittab, then rebuild the images (`scripts/mkinstall.sh`). Keep the loopback configuration: gpsd and its UDP broker depend on it.

Live checks: confirm host enumeration and DHCP, ping and telnet, then unplug/replug and confirm recovery. The 2026-09-18 tests (on the since-dropped ride image) verified NCM re-enumeration, DHCP, and HTTP after reconnecting. The host-side MAC was observed to vary; do not rely on a fixed interface name.

## Limitations

The image binds the passwordless telnet listener to 172.16.42.1. Key-only [SSH](ssh.md) listens on every interface, USB included. See [connectivity plans](../next-steps/connectivity-and-sensors.md). A failed fastboot bulk transfer is a separate host-controller issue covered in [build troubleshooting](../building.md#build-and-boot-troubleshooting).

See the [build log](../build-log.md) entries for first boot (2026-09-13) and USB replug testing (2026-09-18) for evidence.
