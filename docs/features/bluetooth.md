# Bluetooth LE

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior and source

BlueZ scanning, controller restart, and in-band sleep (IBS) were live-verified on 2026-09-15. Pairing and reading notifications from a real HR/cadence/power sensor remain open.

The WCN3990 uses `/dev/ttyHS0` and `/dev/btpower`. `initramfs/usr/bin/bt-up` powers it, sends the UART boot pulses, attaches the QCA line discipline, and sets the public address from `androidboot.btmacaddr` using `tools/btprobe.c`. D-Bus, bluetoothd, and `bt-up` are started by inittab; bluetoothd enables the adapter.

The kernel's `drivers/bluetooth/hci_qca.c`, `btqca.c`, and `hci_ldisc.c` contain the WCN3990 backport and UART fixes. `net/bluetooth/lib.c` has the log-format fix. Stock `bluetooth_a` supplies `crbtfw21.tlv` and `crnv21.bin`; the controller runs at 3.2 Mbaud after setup.

## Use and inspect

In the phone shell, run `bluetoothctl` interactively:

```text
show
scan on
scan off
power off
power on
```

`btmgmt`, `btmon`, and `hciconfig` are also included. Inspect `dmesg` for setup/restart and IBS state, and the UART's sysfs `power/runtime_status` for suspension. The live test observed both IBS directions asleep and the UART suspended after scanning stopped.

## Modify and verify

Change startup policy in `bt-up` and `initramfs/etc/bluetooth/main.conf`; transport fixes may require the kernel paths above. Rebuild the kernel for driver changes and the initramfs for userspace changes, then repack the boot image.

Live regression checks: adapter powers on with the expected address, scanning returns advertisements, power off/on recovers, and terminating `btattach` allows `bt-up` to restart it without a kernel panic. Stop scanning and verify IBS/UART sleep. These are hardware checks; do not infer them from a successful build.

## Transport pitfalls

- **WCN3990 on a 4.4 line discipline:** no serdev, so the SoC's power-on (btpower ioctl + `c0`@2400 / `fc`@115200 pulses) and the UART reopen the pulses require happen in userspace (`bt-up`) before `btattach -P qca`; the kernel's `hci_qca` does the rest. The SoC type is taken from the DT node `compatible = "qca,wcn3990"` (the btpower node), and IBS clock votes go to the UART through the generic `TIOCPMGET`/`TIOCPMPUT` tty ioctls, which msm_serial_hs implements as its runtime-PM vote.

- **A running WCN3990 ignores the power pulses.** Its rails are shared with Wi-Fi and dropping them for <3 s does not reset it; it must be told (IBS wake + vendor pre-shutdown `01 08 fc 00` at 3.2 Mbaud) before `c0`/`fc` work again. `bt-up` does this when it restarts.

- **msm_serial_hs resets RX on every termios change** and asserts RFR on every one too, so any bytes the controller sends between two consecutive `tty_set_termios()` calls are lost. `hci_uart_set_baudrate_flow_control()` changes speed and re-enables CRTSCTS in one call, RTS last.

- **`btmgmt`/`bluetoothctl` quit early with stdin on `/dev/null`** (bt_shell reads EOF before the reply) — the way init runs things. `btprobe bdaddr` speaks mgmt directly for the one command boot needs.

- **The vendor tree's `BT_INFO`/`BT_ERR` printed pointers**: a blanket `%p`→`%pK` sed had turned `%pV` into `%pKV` in `net/bluetooth/lib.c`.

See the [2026-09-15 build-log entries](../build-log.md) for firmware details, measured timings, and the earlier line-discipline double-free fix. Remaining sensor integration and idle-power work is in [connectivity and sensors](../next-steps/connectivity-and-sensors.md).
