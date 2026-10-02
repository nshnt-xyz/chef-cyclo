# Bluetooth

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior and source

BlueZ scanning, controller restart, and in-band sleep (IBS) were live-verified on 2026-09-15. Classic tooling was live-verified on 2026-10-02 against a desktop BlueZ peer: dual-mode discovery, pairing with bonding, `l2ping`, incoming and outgoing ACL connections, `rfcomm bind` to `/dev/rfcomm0` with an echo through a peer RFCOMM server, obexd D-Bus activation from root's session bus with an OPP session to the peer, `bt-up` restart, and IBS/UART sleep after scanning and after a classic disconnect ([evidence](../../logs/bt-tooling-live-2026-10-02.txt)). A second run the same day ([evidence](../../logs/bt-tooling2-live-2026-10-02.txt)) verified classic HID input from a keyboard emulated on the peer (`tools/bt-hid-emu.py`), through bluetoothd's default uhid path and through kernel HIDP; PAN as PANU to the peer's NAP, both with a plain `Network1.Connect` and as a NetworkManager connection, with the BNEP guard dropping the peer's attempts to reach the USB telnet; and A2DP from PipeWire/WirePlumber to the peer (aptX HD and SBC). A third run on kernels #19 and #20 verified `bluetooth-meshd` with a local network created by `mesh-cfgclient`, and measured idle current with a bonded peer. Pairing and reading notifications from a real HR/cadence/power sensor remain open.

The adapter runs in BlueZ's default dual mode (BR/EDR + LE), and the kernel carries the classic protocol layers a desktop distribution has: RFCOMM with TTYs, BNEP (PAN) with its filters, HIDP, the virtual HCI driver (`/dev/vhci`) and the AF_ALG hash, skcipher and AEAD sockets BlueZ's crypto and `bluetooth-meshd` use. The AEAD socket carries a backport of the 4.9 buffer layout (`crypto/algif_aead.c`), which ell (meshd's crypto library) expects; it is verified live only through meshd's AES-CCM self-test and mesh traffic. Root also has a session bus at `/run/user/0/bus`; telnet shells get `XDG_RUNTIME_DIR=/run/user/0`, `DBUS_SESSION_BUS_ADDRESS` and `HOME=/root` as a desktop login would. D-Bus-activated session services such as obexd (`bluez-obexd`) start on demand, and PipeWire/WirePlumber started by hand from the shell find the bus and runtime directory.
An iptables rule drops anything arriving on `bnep*` for the USB subnet, so a PAN link in the PANU role (the phone as a client, `bnep0` as its IP interface) cannot reach the root telnet; bluetoothd is not started without the rule (`wifi-usb-guard` does the same for `wlan0`). It does not cover the phone as a NAP: there `bnep*` is a bridge port and traffic arrives on the bridge. NetworkManager manages Bluetooth devices (NM type `bt`) besides `wlan0`, so PAN profiles work with `nmcli`; USB, loopback and WWAN stay unmanaged, and `networkmanager-bluetooth` is already in the image. If PipeWire is installed, `/etc/wireplumber/wireplumber.conf.d/90-chef-cyclo.conf` keeps it to Bluetooth: no ALSA monitor (the board card and its [playback limits](audio.md#playback-limits-and-diagnostics) belong to `audio-up`/`tinyplay`), no camera monitors, and only the A2DP source role. Ordinary BlueZ clients installed with `apk add` are expected to work as they would on a desktop distribution, within the [kernel ceiling](#limits) below. Packages and pairings are RAM-only until [writable storage](../next-steps/storage-and-boot.md#persistent-storage) exists.

The WCN3990 uses `/dev/ttyHS0` and `/dev/btpower`. `initramfs/usr/bin/bt-up` powers it, sends the UART boot pulses, attaches the QCA line discipline, and sets the public address from `androidboot.btmacaddr` using `tools/btprobe.c`. D-Bus (system and root session), bluetoothd (after the BNEP guard), and `bt-up` are started by `initramfs/etc/inittab`; bluetoothd enables the adapter (`initramfs/etc/bluetooth/main.conf`).

The kernel's `drivers/bluetooth/hci_qca.c`, `btqca.c`, and `hci_ldisc.c` contain the WCN3990 backport, UART fixes and the firmware debug-log routing. `net/bluetooth/lib.c` has the log-format fix. `crypto/algif_aead.c` has the AF_ALG AEAD layout backport meshd needs. The protocol options are in `kernel-config/chef-cyclo.config`. Stock `bluetooth_a` supplies `crbtfw21.tlv` and `crnv21.bin`; the controller runs at 3.2 Mbaud after setup.

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

Classic serial devices bind with `rfcomm bind 0 AA:BB:CC:DD:EE:FF <channel>` (creates `/dev/rfcomm0`; `sdptool browse` lists channels). Classic keyboards and mice must be paired (input.conf's `ClassicBondedOnly` default) and then appear as ordinary evdev nodes under `/dev/input`; bluetoothd drives them through `/dev/uhid` by default, and through kernel HIDP only with `UserspaceHID=false` in `/etc/bluetooth/input.conf`. Extra clients install normally, for example `apk add bluez-obexd` for obexd, which D-Bus activates on root's session bus. Read evdev nodes with `cat` (or `apk add evtest`), not busybox `hexdump`, which reads less than one event and gets `EINVAL`.

PAN as a client of a NAP (a phone hotspot or a desktop with BlueZ's `NetworkServer1`), after pairing:

```sh
nmcli connection add type bluetooth con-name pan bluetooth.type panu bluetooth.bdaddr AA:BB:CC:DD:EE:FF \
      ipv4.method manual ipv4.addresses 10.77.0.2/24 ipv6.method disabled
nmcli connection up pan          # bnep0 with the manual address
```

The verified runs used a manual address. NM's default is DHCP from the NAP, which is untested here, and a lease overlapping the USB subnet can break USB telnet (see [limits](#limits)).

NM also generates an `<alias> Network` profile (autoconnect off) for a paired NAP. Without NM, `gdbus call --system -d org.bluez -o /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF -m org.bluez.Network1.Connect nap` returns `bnep0` and the link stays up after `gdbus` exits; address it yourself (`ip addr add 10.77.0.2/24 dev bnep0`).

A2DP to headphones or a speaker (RAM-only, about 350 MiB with dependencies):

```sh
apk add pipewire wireplumber pipewire-spa-bluez pipewire-tools
cd /root; setsid pipewire </dev/null >/run/pipewire.log 2>&1 &
setsid wireplumber </dev/null >/run/wireplumber.log 2>&1 &
bluetoothctl connect AA:BB:CC:DD:EE:FF   # after pair + trust
wpctl status                              # the device's sink, e.g. bluez_output.AA_BB_CC_DD_EE_FF.1
pw-play --target bluez_output.AA_BB_CC_DD_EE_FF.1 file.wav
```

Codecs follow the sink (aptX HD and SBC were verified); `wpctl set-profile <device> <a2dp-sink-sbc index>` forces SBC.

`bluez-deprecated` (`hciconfig`, `hcitool`, `rfcomm`, `sdptool`, `hcidump`) stays installed for third-party scripts, but prefer `btmgmt`/`bluetoothctl`. Inspect `dmesg` for setup/restart and IBS state, and the UART's sysfs `power/runtime_status` for suspension.

## Modify and verify

Change startup policy in `bt-up` and `initramfs/etc/bluetooth/main.conf`; transport fixes may require the kernel paths above. Rebuild the kernel for driver or config changes and the initramfs for userspace changes, then repack the boot image.

Live regression checks: adapter powers on with the expected address, scanning returns advertisements, power off/on recovers, and terminating `btattach` allows `bt-up` to restart it without a kernel panic. Stop scanning and verify IBS/UART sleep. These are hardware checks; do not infer them from a successful build.

Without a classic keyboard, `tools/bt-hid-emu.py PHONE_MAC` turns a desktop's BlueZ adapter into one: it publishes an HID keyboard record and, for each line on stdin, connects to the phone's PSM 17/19 and types `a b c a`. A desktop's input plugin already listens on PSM 17/19 and owns UUID 0x1124, so the record goes through a record-only `Profile1` under a private UUID and the desktop connects out, as a reconnecting keyboard does. Start it before pairing (the phone must see the record), trust the desktop on the phone, and expect `KEY_A`, `KEY_B`, `KEY_C`, `KEY_A` on the new `Bus=0005` evdev node. Do not connect HID from the phone: the desktop's input plugin answers unknown devices with a virtual-cable unplug.

## Limits

Idle current with the panel off and nothing bonded measured the same in dual and LE-only mode (97.4/96.9 vs 97.5 mA, USB unplugged). LE-only was produced by toggling BR/EDR with `btmgmt` under the running bluetoothd, not by restarting it with `ControllerMode = le`; the controller state is the same. Three of the six 45 s blocks (two LE, one dual) had isolated spikes (sd 21 to 27 mA), so the comparison rests on the three quiet blocks. Bluetoothd leaves the adapter non-connectable; page scan runs only while a BR/EDR device is bonded (the kernel's accept list, so it can reconnect), and `btmgmt connectable off` does not stop it; removing the bond does. With a bonded peer (no connection, panel off, USB unplugged), page scan on vs off (`hciconfig hci0 pscan`/`noscan`) showed no resolvable page-scan cost: against the mean of its two neighbouring blocks each block differs by -0.2 to +0.2 mA. The raw pairs (99.7/98.8, 98.0/97.6, 97.4/96.9 mA) differ by about 0.6 mA only because page scan always came first while the current drifted down 0.56 mA per block. Three isolated 246.6 mA samples were left out, as in the unbonded run, and the level matches the unbonded figures. Page-scan intervals cannot be tuned (`[BREDR]` settings need a newer mgmt, below).

`bluetooth-meshd` (in the image) needs the adapter to itself: on this 4.4 kernel there is no mgmt mesh support, so it takes an unpowered controller over the HCI user channel and bluetoothd loses `hci0` until meshd exits (it gets it back and powers it on). Power the adapter off first (`btmgmt power off`), then start `/usr/lib/bluetooth/bluetooth-meshd`; `mesh-cfgclient` needs `mkdir -p ~/.config` (it creates only `~/.config/meshcfg`). Mesh state lives in `/var/lib/bluetooth/mesh` and `~/.config/meshcfg`, RAM-only like pairings. Provisioning a real mesh device is untried.

PipeWire pulls in libcamera, Mesa and LLVM through `pipewire-libs`; with WirePlumber's default monitors it probed `/dev/video*` and the vendor V4L2 driver logged kernel `WARNING`s, which the drop-in above prevents. The overlap check that NM's dispatcher applies to `wlan0` leases does not cover `bnep*`. The ingress rule still blocks PAN traffic to the USB subnet, but a PAN lease containing the USB host's address (udhcpd hands out 172.16.42.2 to .9) puts it in the local routing table, which is consulted before table 142, so the phone answers itself and USB telnet breaks. The phone as a NAP (BlueZ `NetworkServer1` or an NM `bluetooth.type nap` profile, both bridged) is untested and not covered by the `bnep+` rule.

The 4.4 kernel speaks mgmt 1.10, while BlueZ 5.86 expects newer: advertisement monitors, PHY selection and extended advertising, LE Audio (ISO sockets), device wake flags and the `[LE]`/`[BREDR]` tuning in `main.conf` are unavailable. HFP/HSP voice (SCO) needs the board PCM path, which is not wired up; A2DP media is ACL data over the UART and does not.

## Transport pitfalls

- **WCN3990 on a 4.4 line discipline:** no serdev, so the SoC's power-on (btpower ioctl + `c0`@2400 / `fc`@115200 pulses) and the UART reopen the pulses require happen in userspace (`bt-up`) before `btattach -P qca`; the kernel's `hci_qca` does the rest. The SoC type is taken from the DT node `compatible = "qca,wcn3990"` (the btpower node), and IBS clock votes go to the UART through the generic `TIOCPMGET`/`TIOCPMPUT` tty ioctls, which msm_serial_hs implements as its runtime-PM vote.

- **A running WCN3990 ignores the power pulses.** Its rails are shared with Wi-Fi and dropping them for <3 s does not reset it; it must be told (IBS wake + vendor pre-shutdown `01 08 fc 00` at 3.2 Mbaud) before `c0`/`fc` work again. `bt-up` does this when it restarts.

- **msm_serial_hs resets RX on every termios change** and asserts RFR on every one too, so any bytes the controller sends between two consecutive `tty_set_termios()` calls are lost. `hci_uart_set_baudrate_flow_control()` changes speed and re-enables CRTSCTS in one call, RTS last.

- **WCN3990 firmware logs arrive as ACL data on handle `0x2EDC`.** Without routing them, every discovery or connection printed bursts of `ACL packet for unknown connection handle 3804` (0x2EDC masked to 12 bits). `hci_qca` hands them to `hci_recv_diag` as mainline does, so they show in `btmon` as Vendor Diagnostic frames and nowhere else.

- **Turning BR/EDR off and on clears SSP.** After `btmgmt bredr off` / `bredr on` the settings lack `ssp`, and the next classic pairing falls back to a legacy PIN and fails. Run `btmgmt ssp on` after such a toggle; bluetoothd never toggles BR/EDR itself.

- **AF_ALG AEAD before 4.9 wants tag space in the input.** The 4.4 `algif_aead` treated the last tag-length bytes of an encryption input as room for the tag, so ell's AES-CCM self-test encrypted the wrong length and meshd exited with `Mesh Crypto functions unavailable` even with `CONFIG_CRYPTO_USER_API_AEAD=y`. The kernel carries upstream's 4.9 layout ("crypto: algif_aead - fix AEAD tag memory handling"); the port also frees the last output scatterlist, whose pinned pages the old early `break` leaked on every read (from code reading), and refuses requests that would leave the input or output list empty.

- **NM calls the Bluetooth device type `bt`.** `except:type:bluetooth` in `unmanaged-devices` matches nothing; `nmcli -f GENERAL.TYPE device show` is the reference.

- **`btmgmt`/`bluetoothctl` quit early with stdin on `/dev/null`** (bt_shell reads EOF before the reply), which is how init runs things. `btprobe bdaddr` speaks mgmt directly for the one command boot needs.

- **The vendor tree's `BT_INFO`/`BT_ERR` printed pointers**: a blanket `%p`→`%pK` sed had turned `%pV` into `%pKV` in `net/bluetooth/lib.c`.

See the [2026-09-15 build-log entries](../build-log.md) for firmware details, measured timings, and the earlier line-discipline double-free fix. Remaining sensor integration and idle-power work is in [connectivity and sensors](../next-steps/connectivity-and-sensors.md).
