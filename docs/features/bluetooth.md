# Bluetooth

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior and source

BlueZ scanning, controller restart, and in-band sleep (IBS) were live-verified on 2026-09-15. Classic tooling was live-verified on 2026-10-02 against a desktop BlueZ peer: dual-mode discovery, pairing with bonding, `l2ping`, incoming and outgoing ACL connections, `rfcomm bind` to `/dev/rfcomm0` with an echo through a peer RFCOMM server, obexd D-Bus activation from root's session bus with an OPP session to the peer, `bt-up` restart, and IBS/UART sleep after scanning and after a classic disconnect ([evidence](../../logs/bt-tooling-live-2026-10-02.txt)). Classic HID input, PAN links and HIDP mode were not exercised. Pairing and reading notifications from a real HR/cadence/power sensor remain open.

The adapter runs in BlueZ's default dual mode (BR/EDR + LE), and the kernel carries the classic protocol layers a desktop distribution has: RFCOMM with TTYs, BNEP (PAN) with its filters, HIDP, the virtual HCI driver (`/dev/vhci`) and the AF_ALG hash/skcipher sockets BlueZ's crypto and `bluetooth-meshd` use. Root also has a session bus at `/run/user/0/bus`; telnet shells get `XDG_RUNTIME_DIR=/run/user/0`, `DBUS_SESSION_BUS_ADDRESS` and `HOME=/root` as a desktop login would. D-Bus-activated session services such as obexd (`bluez-obexd`) start on demand, and PipeWire/WirePlumber started by hand from the shell find the bus and runtime directory (not tested).
An iptables rule drops anything arriving on `bnep*` for the USB subnet, so a PAN link cannot reach the root telnet; bluetoothd is not started without it (`wifi-usb-guard` does the same for `wlan0`). Ordinary BlueZ clients installed with `apk add` are expected to work as they would on a desktop distribution, within the [kernel ceiling](#limits) below. Packages and pairings are RAM-only until [writable storage](../next-steps/storage-and-boot.md#persistent-storage) exists.

The WCN3990 uses `/dev/ttyHS0` and `/dev/btpower`. `initramfs/usr/bin/bt-up` powers it, sends the UART boot pulses, attaches the QCA line discipline, and sets the public address from `androidboot.btmacaddr` using `tools/btprobe.c`. D-Bus (system and root session), bluetoothd (after the BNEP guard), and `bt-up` are started by `initramfs/etc/inittab`; bluetoothd enables the adapter (`initramfs/etc/bluetooth/main.conf`).

The kernel's `drivers/bluetooth/hci_qca.c`, `btqca.c`, and `hci_ldisc.c` contain the WCN3990 backport, UART fixes and the firmware debug-log routing. `net/bluetooth/lib.c` has the log-format fix. The protocol options are in `kernel-config/chef-cyclo.config`. Stock `bluetooth_a` supplies `crbtfw21.tlv` and `crnv21.bin`; the controller runs at 3.2 Mbaud after setup.

## Use and inspect

The standard BlueZ tools are the interface. In the phone shell:

```sh
rfkill                    # hci0 soft/hard block state (util-linux)
btmgmt info               # controller settings over mgmt: le, br/edr, powered, ...
bluetoothctl              # interactive: show, scan on/off, pair, trust, connect
btmon                     # live HCI trace
```

A typical classic pairing inside `bluetoothctl`:

```text
agent on
default-agent
scan on
pair AA:BB:CC:DD:EE:FF
trust AA:BB:CC:DD:EE:FF
connect AA:BB:CC:DD:EE:FF
```

Classic serial devices bind with `rfcomm bind 0 AA:BB:CC:DD:EE:FF <channel>` (creates `/dev/rfcomm0`; `sdptool browse` lists channels). Classic keyboards and mice must be paired (input.conf's `ClassicBondedOnly` default) and then appear as ordinary evdev nodes under `/dev/input`; bluetoothd drives them through `/dev/uhid` by default, and through kernel HIDP only with `UserspaceHID=false` in `/etc/bluetooth/input.conf`. Extra clients install normally, for example `apk add bluez-obexd` for obexd, which D-Bus activates on root's session bus.

`bluez-deprecated` (`hciconfig`, `hcitool`, `rfcomm`, `sdptool`, `hcidump`) stays installed for third-party scripts, but prefer `btmgmt`/`bluetoothctl`. Inspect `dmesg` for setup/restart and IBS state, and the UART's sysfs `power/runtime_status` for suspension.

## Modify and verify

Change startup policy in `bt-up` and `initramfs/etc/bluetooth/main.conf`; transport fixes may require the kernel paths above. Rebuild the kernel for driver or config changes and the initramfs for userspace changes, then repack the boot image.

Live regression checks: adapter powers on with the expected address, scanning returns advertisements, power off/on recovers, and terminating `btattach` allows `bt-up` to restart it without a kernel panic. Stop scanning and verify IBS/UART sleep. These are hardware checks; do not infer them from a successful build.

## Limits

Idle current with the panel off and nothing bonded measured the same in dual and LE-only mode (97.4/96.9 vs 97.5 mA, USB unplugged). LE-only was produced by toggling BR/EDR with `btmgmt` under the running bluetoothd, not by restarting it with `ControllerMode = le`; the controller state is the same. Three of the six 45 s blocks (two LE, one dual) had isolated spikes (sd 21 to 27 mA), so the comparison rests on the three quiet blocks. Bluetoothd leaves the adapter non-connectable; page scan runs only while a BR/EDR device is bonded (the kernel's accept list, so it can reconnect), and `btmgmt connectable off` does not stop it; removing the bond does. That bonded case was not measured, and page-scan intervals cannot be tuned (`[BREDR]` settings need a newer mgmt, below). NetworkManager-managed PAN would need `networkmanager-bluetooth` and a change to NM's `unmanaged-devices` policy, which keeps it on `wlan0` only.

The 4.4 kernel speaks mgmt 1.10, while BlueZ 5.86 expects newer: advertisement monitors, PHY selection and extended advertising, LE Audio (ISO sockets), device wake flags and the `[LE]`/`[BREDR]` tuning in `main.conf` are unavailable. HFP/HSP voice (SCO) needs the board PCM path, which is not wired up; A2DP media is ACL data over the UART and does not.

## Transport pitfalls

- **WCN3990 on a 4.4 line discipline:** no serdev, so the SoC's power-on (btpower ioctl + `c0`@2400 / `fc`@115200 pulses) and the UART reopen the pulses require happen in userspace (`bt-up`) before `btattach -P qca`; the kernel's `hci_qca` does the rest. The SoC type is taken from the DT node `compatible = "qca,wcn3990"` (the btpower node), and IBS clock votes go to the UART through the generic `TIOCPMGET`/`TIOCPMPUT` tty ioctls, which msm_serial_hs implements as its runtime-PM vote.

- **A running WCN3990 ignores the power pulses.** Its rails are shared with Wi-Fi and dropping them for <3 s does not reset it; it must be told (IBS wake + vendor pre-shutdown `01 08 fc 00` at 3.2 Mbaud) before `c0`/`fc` work again. `bt-up` does this when it restarts.

- **msm_serial_hs resets RX on every termios change** and asserts RFR on every one too, so any bytes the controller sends between two consecutive `tty_set_termios()` calls are lost. `hci_uart_set_baudrate_flow_control()` changes speed and re-enables CRTSCTS in one call, RTS last.

- **WCN3990 firmware logs arrive as ACL data on handle `0x2EDC`.** Without routing them, every discovery or connection printed bursts of `ACL packet for unknown connection handle 3804` (0x2EDC masked to 12 bits). `hci_qca` hands them to `hci_recv_diag` as mainline does, so they show in `btmon` as Vendor Diagnostic frames and nowhere else.

- **Turning BR/EDR off and on clears SSP.** After `btmgmt bredr off` / `bredr on` the settings lack `ssp`, and the next classic pairing falls back to a legacy PIN and fails. Run `btmgmt ssp on` after such a toggle; bluetoothd never toggles BR/EDR itself.

- **`btmgmt`/`bluetoothctl` quit early with stdin on `/dev/null`** (bt_shell reads EOF before the reply), which is how init runs things. `btprobe bdaddr` speaks mgmt directly for the one command boot needs.

- **The vendor tree's `BT_INFO`/`BT_ERR` printed pointers**: a blanket `%p`→`%pK` sed had turned `%pV` into `%pKV` in `net/bluetooth/lib.c`.

See the [2026-09-15 build-log entries](../build-log.md) for firmware details, measured timings, and the earlier line-discipline double-free fix. Remaining sensor integration and idle-power work is in [connectivity and sensors](../next-steps/connectivity-and-sensors.md).
