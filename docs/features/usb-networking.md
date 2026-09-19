# USB networking and shell

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior

`initramfs/init` creates a USB NCM Ethernet gadget, assigns `usb0` 172.16.42.1/24, and configures loopback. `udhcpd` offers the host addresses .2–.9 with no router option. Linux hosts use `cdc_ncm`. The phone buzzes once when `/init` starts and twice when the network is ready.

## Use

After [booting the image](../building.md), let the host obtain a DHCP lease, then run on the host:

```sh
ping 172.16.42.1
telnet 172.16.42.1
```

The shell is root with no password. In the phone shell, `reboot` returns to the flashed OS. Download any RAM-only evidence first. The ride image additionally exposes [HTTP log extraction](ride-logging.md).

## Modify and verify

Relevant files are `initramfs/init`, `initramfs/etc/udhcpd.conf`, and both overlays' `etc/inittab`. Change gadget/address setup in `/init` and daemon arguments in the appropriate inittab, then rebuild that image. Keep the loopback configuration: gpsd and its UDP broker depend on it.

Live checks: confirm host enumeration and DHCP, ping and telnet, then unplug/replug and confirm recovery. The 2026-09-18 ride tests verified NCM re-enumeration, DHCP, and HTTP after reconnecting. The host-side MAC was observed to vary; do not rely on a fixed interface name.

## Limitations

The passwordless telnet listener currently binds all interfaces. Before Wi-Fi is enabled, bind it to 172.16.42.1 or replace it with key-authenticated dropbear. See [connectivity plans](../next-steps/connectivity-and-sensors.md). A failed fastboot bulk transfer is a separate host-controller issue covered in [build troubleshooting](../building.md#build-and-boot-troubleshooting).

See the [build log](../build-log.md) entries for first boot (2026-09-13) and ride testing (2026-09-18) for evidence.
