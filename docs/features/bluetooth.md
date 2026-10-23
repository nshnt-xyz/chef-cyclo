# Bluetooth

[Feature index](README.md) · [Build instructions](../building.md)

## Current behavior and source

BlueZ scanning, controller restart, and in-band sleep (IBS) were live-verified on 2026-09-15. Classic tooling was live-verified on 2026-10-02 against a desktop BlueZ peer: dual-mode discovery, pairing with bonding, `l2ping`, incoming and outgoing ACL connections, `rfcomm bind` to `/dev/rfcomm0` with an echo through a peer RFCOMM server, obexd D-Bus activation from root's session bus with an OPP session to the peer, `bt-up` restart, and IBS/UART sleep after scanning and after a classic disconnect ([evidence](../../logs/bt-tooling-live-2026-10-02.txt)). A second run the same day ([evidence](../../logs/bt-tooling2-live-2026-10-02.txt)) verified classic HID input from a keyboard emulated on the peer (`tools/bt-hid-emu.py`), through bluetoothd's default uhid path and through kernel HIDP; PAN as PANU to the peer's NAP, both with a plain `Network1.Connect` and as a NetworkManager connection, with the BNEP guard dropping the peer's attempts to reach the USB telnet; and A2DP from PipeWire/WirePlumber to the peer (aptX HD and SBC). A third run on kernels #19 and #20 verified `bluetooth-meshd` with a local network created by `mesh-cfgclient`, and measured idle current with a bonded peer. Pairing and reading notifications from a real HR/cadence/power sensor remain open.

The adapter runs in BlueZ's default dual mode (BR/EDR + LE), and the kernel carries the classic protocol layers a desktop distribution has: RFCOMM with TTYs, BNEP (PAN) with its filters, HIDP, the virtual HCI driver (`/dev/vhci`) and the AF_ALG hash, skcipher and AEAD sockets BlueZ's crypto and `bluetooth-meshd` use. The AEAD socket carries a backport of the 4.9 buffer layout (`crypto/algif_aead.c`), which ell (meshd's crypto library) expects; it is verified live only through meshd's AES-CCM self-test and mesh traffic. Root also has a session bus at `/run/user/0/bus`; telnet shells get `XDG_RUNTIME_DIR=/run/user/0`, `DBUS_SESSION_BUS_ADDRESS` and `HOME=/root` as a desktop login would. D-Bus-activated session services such as obexd (`bluez-obexd`) start on demand, and PipeWire/WirePlumber started by hand from the shell find the bus and runtime directory.
An iptables rule drops anything arriving on `bnep*` for the USB subnet, so a PAN link in the PANU role (the phone as a client, `bnep0` as its IP interface) cannot reach the root telnet; bluetoothd is not started without the rule (`wifi-usb-guard` does the same for `wlan0`). It does not cover the phone as a NAP: there `bnep*` is a bridge port and traffic arrives on the bridge. NetworkManager manages Bluetooth devices (NM type `bt`) besides `wlan0`, so PAN profiles work with `nmcli`; USB, loopback and WWAN stay unmanaged, and `networkmanager-bluetooth` is already in the image. If PipeWire is installed, `/etc/wireplumber/wireplumber.conf.d/90-chef-cyclo.conf` keeps it to Bluetooth: no ALSA monitor (the board card and its [playback limits](audio.md#playback-limits-and-diagnostics) belong to `audio-up`/`tinyplay`), no camera monitors, and only the A2DP source role. Ordinary BlueZ clients installed with `apk add` are expected to work as they would on a desktop distribution, within the [kernel ceiling](#limits) below. Packages remain RAM-only. Phase 2 images with healthy [writable storage](storage.md) persist pairings under `/data/v1/bluetooth`, bound at `/var/lib/bluetooth`; RAM-only fallback loses new pairings on reboot.

The WCN3990 uses `/dev/ttyHS0` and `/dev/btpower`. `rootfs-overlay/usr/bin/bt-up` powers it, sends the UART boot pulses, attaches the QCA line discipline, and sets the public address from `androidboot.btmacaddr` using `tools/btprobe.c`. D-Bus (system and root session), bluetoothd (after the BNEP guard) and `bt-up` are OpenRC services (`dbus`, `root-session-bus`, `bluetooth`, `bt-up`; [services](services.md)); bluetoothd enables the adapter (`rootfs-overlay/etc/bluetooth/main.conf`).

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

`bluez-deprecated` (`hciconfig`, `hcitool`, `rfcomm`, `sdptool`, `hcidump`) stays installed for third-party scripts, but prefer `btmgmt`/`bluetoothctl`. Inspect `dmesg` for setup/restart and IBS state (`logread | grep bt-up` for the attach helper's own lines), and the UART's sysfs `power/runtime_status` for suspension.

## Modify and verify

Change startup policy in `bt-up` and `rootfs-overlay/etc/bluetooth/main.conf`; transport fixes may require the kernel paths above. Rebuild the kernel for driver or config changes and the initramfs for userspace changes, then repack the boot image.

Live regression checks: adapter powers on with the expected address, scanning returns advertisements, power off/on recovers, and terminating `btattach` allows `bt-up` to restart it without a kernel panic. Stop scanning and verify IBS/UART sleep. These are hardware checks; do not infer them from a successful build.

Without a classic keyboard, `tools/bt-hid-emu.py PHONE_MAC` turns a desktop's BlueZ adapter into one: it publishes an HID keyboard record and, for each line on stdin, connects to the phone's PSM 17/19 and types `a b c a`. A desktop's input plugin already listens on PSM 17/19 and owns UUID 0x1124, so the record goes through a record-only `Profile1` under a private UUID and the desktop connects out, as a reconnecting keyboard does. Start it before pairing (the phone must see the record), trust the desktop on the phone, and expect `KEY_A`, `KEY_B`, `KEY_C`, `KEY_A` on the new `Bus=0005` evdev node. Do not connect HID from the phone: the desktop's input plugin answers unknown devices with a virtual-cable unplug.

## Limits

Idle current with the panel off and nothing bonded measured the same in dual and LE-only mode (97.4/96.9 vs 97.5 mA, USB unplugged). LE-only was produced by toggling BR/EDR with `btmgmt` under the running bluetoothd, not by restarting it with `ControllerMode = le`; the controller state is the same. Three of the six 45 s blocks (two LE, one dual) had isolated spikes (sd 21 to 27 mA), so the comparison rests on the three quiet blocks. Bluetoothd leaves the adapter non-connectable; page scan runs only while a BR/EDR device is bonded (the kernel's accept list, so it can reconnect), and `btmgmt connectable off` does not stop it; removing the bond does. With a bonded peer (no connection, panel off, USB unplugged), page scan on vs off (`hciconfig hci0 pscan`/`noscan`) showed no resolvable page-scan cost: against the mean of its two neighbouring blocks each block differs by -0.2 to +0.2 mA. The raw pairs (99.7/98.8, 98.0/97.6, 97.4/96.9 mA) differ by about 0.6 mA only because page scan always came first while the current drifted down 0.56 mA per block. Three isolated 246.6 mA samples were left out, as in the unbonded run, and the level matches the unbonded figures. Page-scan intervals cannot be tuned (`[BREDR]` settings need a newer mgmt, below).

`bluetooth-meshd` (in the image) needs the adapter to itself: on this 4.4 kernel there is no mgmt mesh support, so it takes an unpowered controller over the HCI user channel and bluetoothd loses `hci0` until meshd exits (it gets it back and powers it on). Power the adapter off first (`btmgmt power off`), then start `/usr/lib/bluetooth/bluetooth-meshd`; `mesh-cfgclient` needs `mkdir -p ~/.config` (it creates only `~/.config/meshcfg`). Mesh state lives in `/var/lib/bluetooth/mesh` and `~/.config/meshcfg`, RAM-only like pairings. Provisioning a real mesh device is untried.

PipeWire pulls in libcamera, Mesa and LLVM through `pipewire-libs`; with WirePlumber's default monitors it probed `/dev/video*` and the vendor V4L2 driver logged kernel `WARNING`s, which the drop-in above prevents. The overlap check that NM's dispatcher applies to `wlan0` leases does not cover `bnep*`. The ingress rule still blocks PAN traffic to the USB subnet, but a PAN lease containing the USB host's address (udhcpd hands out 172.16.42.2 to .9) puts it in the local routing table, which is consulted before table 142, so the phone answers itself and USB telnet breaks. The phone as a NAP (BlueZ `NetworkServer1` or an NM `bluetooth.type nap` profile, both bridged) is untested and not covered by the `bnep+` rule.

The 4.4 kernel speaks mgmt 1.10, while BlueZ 5.86 expects newer: advertisement monitors, PHY selection and extended advertising, LE Audio (ISO sockets), device wake flags and the `[LE]`/`[BREDR]` tuning in `main.conf` are unavailable. HFP/HSP voice (SCO) needs the board PCM path, which is not wired up; A2DP media is ACL data over the UART and does not.

## Transport pitfalls

- **WCN3990 on a 4.4 line discipline:** no serdev, so the SoC's power-on (btpower ioctl + `c0`@2400 / `fc`@115200 pulses) and the UART reopen the pulses require happen in userspace (`bt-up`) before `btattach -P qca`; the kernel's `hci_qca` does the rest. The SoC type is taken from the DT node `compatible = "qca,wcn3990"` (the btpower node), and IBS clock votes go to the UART through the generic `TIOCPMGET`/`TIOCPMPUT` tty ioctls, which msm_serial_hs implements as its runtime-PM vote.

- **Closing the line discipline used to race the IBS timers.** Before kernel #22, `qca_close()` stopped the IBS timers without waiting and then drained its workqueue, whose pending wake work re-armed the 100 ms wake-retransmit timer just before `qca_data` was freed. Killing `btattach` (every shutdown does, now through bt-up's stop) then panicked occasionally (about 6 % of shutdowns on 2026-10-05). Fixed by kernel commit `8ef120353`; see the [shutdown hang](../archive/shutdown-hang-handoff.md#result-rootfs_impl-2026-10-05).

- **A running WCN3990 ignores the power pulses.** Its rails are shared with Wi-Fi and dropping them for <3 s does not reset it; it must be told (IBS wake + vendor pre-shutdown `01 08 fc 00` at 3.2 Mbaud) before `c0`/`fc` work again. `bt-up` does this when it restarts.

- **msm_serial_hs resets RX on every termios change** and asserts RFR on every one too, so any bytes the controller sends between two consecutive `tty_set_termios()` calls are lost. `hci_uart_set_baudrate_flow_control()` changes speed and re-enables CRTSCTS in one call, RTS last.

- **WCN3990 firmware logs arrive as ACL data on handle `0x2EDC`.** Without routing them, every discovery or connection printed bursts of `ACL packet for unknown connection handle 3804` (0x2EDC masked to 12 bits). `hci_qca` hands them to `hci_recv_diag` as mainline does, so they show in `btmon` as Vendor Diagnostic frames and nowhere else.

- **Turning BR/EDR off and on clears SSP.** After `btmgmt bredr off` / `bredr on` the settings lack `ssp`, and the next classic pairing falls back to a legacy PIN and fails. Run `btmgmt ssp on` after such a toggle; bluetoothd never toggles BR/EDR itself.

- **AF_ALG AEAD before 4.9 wants tag space in the input.** The 4.4 `algif_aead` treated the last tag-length bytes of an encryption input as room for the tag, so ell's AES-CCM self-test encrypted the wrong length and meshd exited with `Mesh Crypto functions unavailable` even with `CONFIG_CRYPTO_USER_API_AEAD=y`. The kernel carries upstream's 4.9 layout ("crypto: algif_aead - fix AEAD tag memory handling"); the port also frees the last output scatterlist, whose pinned pages the old early `break` leaked on every read (from code reading), and refuses requests that would leave the input or output list empty.

- **NM calls the Bluetooth device type `bt`.** `except:type:bluetooth` in `unmanaged-devices` matches nothing; `nmcli -f GENERAL.TYPE device show` is the reference.

- **`btmgmt`/`bluetoothctl` quit early with stdin on `/dev/null`** (bt_shell reads EOF before the reply), which is how init runs things. `btprobe bdaddr` speaks mgmt directly for the one command boot needs.

- **The vendor tree's `BT_INFO`/`BT_ERR` printed pointers**: a blanket `%p`→`%pK` sed had turned `%pV` into `%pKV` in `net/bluetooth/lib.c`.

See the [2026-09-15 build-log entries](../build-log.md) for firmware details, measured timings, and the earlier line-discipline double-free fix. Remaining sensor integration and idle-power work is in [connectivity and sensors](../next-steps/connectivity-and-sensors.md).


## BLE sensors

`blesensd` is the resident owner of Heart Rate (0x180D/0x2A37), Cycling
Power (0x1818/0x2A63) and Cycling Speed and Cadence (0x1816/0x2A5B).
Its supervisor (the OpenRC service `blesensd`) respawns it; it runs as user `blesensd`, with no groups or capabilities beyond its own ([users and permissions](users-and-permissions.md)). Settings and future ride clients share its Unix socket,
`/run/chef/blesensd/blesensd.sock`; they never own separate notification sessions.
One device has one saved record and connection, even with several services;
the [Bluetooth pages](system-ui.md#bluetooth-pages) show one row per kind.

Discovery uses `Transport=le` and the three sensor service UUIDs, only while
at least one client asks for `scan on`. Pairing is bounded at 20 seconds:
connect, resolve, subscribe, bond if authentication is required (also when
Connect fails for authentication before services resolve), trust and
save. The daemon registers its own NoInputNoOutput agent. It requests the
default-agent role only during an active pairing attempt, since BlueZ may
request security during service resolution before StartNotify. On completion
or cancellation it unregisters, restoring the previous default agent, then
registers normally for the next request. Unregistration is confirmed and
retried before normal registration or a subsequent pairing can continue.
Its agent keeps rejecting unrelated devices even during a cleanup retry. It answers only bluetoothd's unique bus name and
only for the current pairing device; service authorization also accepts
saved sensors. Delayed replies carry the pairing attempt or saved sensor
identity plus its connection epoch, so a cancelled attempt, disconnected
link or reused record slot cannot consume another attempt's reply.

**Reconnect policy.** BlueZ does not automatically reconnect these GATT
sensor profiles. `blesensd` calls Connect with backoff from 5 seconds to
30 seconds, only while any client holds `hold on`, or during a default
300-second window after daemon start, adapter power-on, bluetoothd appearing,
pairing or a link drop. At expiry the daemon cancels any outstanding Connect through Disconnect,
so the pending BlueZ procedure cannot keep scanning beyond the window.
Outside that window, absent sensors stay `waiting`
and no Connect attempt runs. A hold resumes attempts at once; leaving a
page or closing its connection releases the hold. Established connections
and notifications remain alive when a hold ends. `-r SECONDS` changes the
window, `-w MM` changes the wheel circumference (default 2105 mm).

StartNotify is requested once per connection epoch regardless of BlueZ's
global Notifying property, which can belong to another client. Only Value
changes on our established sessions count; cached properties and ReadValue
measurements never look live. A link or object loss clears every kind and
its revolution baseline. Five seconds without a measurement produces null;
a new watcher receives only fresh values. Optional cadence/speed is null as
soon as its flag is absent; its baseline resets then and after a stale gap,
so two fresh samples are required again. Repeated valid stopped counters
remain fresh and produce zero after the stop interval. HR accepts 8/16-bit measurements;
power is signed, optional fields must be complete, and CSC handles event
and revolution rollover. Battery comes from BlueZ Battery1, or one 0x2A19
read per connection plus an owned notification session when notify is
offered. Invalid battery packets are ignored; disconnect/object loss resets
the fallback value and session, which is acquired again on reconnect. RSSI is the last advertisement, marked as last seen
when not live.

```sh
blesensd status
blesensd list
blesensd watch                      # snapshot then JSON event lines
blesensd scan                       # scan and watch until interrupted
blesensd pair AA:BB:CC:DD:EE:FF       # waits for done or failed
blesensd cancel AA:BB:CC:DD:EE:FF
blesensd forget AA:BB:CC:DD:EE:FF     # waits for confirmed removal
```

### Socket contract

`/run/chef/blesensd/blesensd.sock` ([runtime files](runtime-files.md)) speaks the shared [local socket protocol](local-sockets.md):
requests are words on one line, every line from the daemon is a JSON object
with a `type`. This is proto 1 (2026-10-20; before it the lines had the
`type` envelope but no hello, and every refusal was `unknown request`). The
code is `tools/blesensd/server.c`. `ADDR` is `AA:BB:CC:DD:EE:FF`, upper-case
hex.

| Request | Reply |
|---|---|
| (connect) | `{"type":"hello","daemon":"blesensd","proto":1}` |
| `status` | `{"type":"status","bluez":true,"adapter":true,"powered":true,"scanning":false,"loaded":true,"sensors":1,"connected":1,"pairing":false,"agent":true,"wheel_mm":2105,"held":true,"reconnecting":true}` |
| `list` | a `sensor` line per saved sensor, then `{"type":"ok","cmd":"list"}` |
| `watch` | `{"type":"ok","cmd":"watch"}`, then a snapshot (the `adapter` line, every `sensor` with its fresh `value` lines, every `found` device, the running `pair` step), then every event below |
| `scan on`, `scan off` | `{"type":"ok","cmd":"scan"}`: LE discovery for the sensor services runs while any connection has `scan on` |
| `hold on`, `hold off` | `{"type":"ok","cmd":"hold"}`: saved sensors are reconnected while any connection has `hold on` |
| `pair ADDR` | `{"type":"ok","cmd":"pair"}` before the first `pair` event, or an error with the refusal |
| `cancel ADDR` | `{"type":"ok","cmd":"cancel"}`; the pairing then fails with `cancelled` |
| `forget ADDR` | `{"type":"ok","cmd":"forget"}` once accepted; `forgotten` follows once BlueZ confirmed the removal and both saves are done |

`status`: `bluez` whether bluetoothd answered, `adapter` and `powered` the
adapter, `scanning` our discovery session, `loaded` whether the records were
read, `sensors` and `connected` the saved sensors (not being forgotten) and
the connected ones, `pairing` a pairing in progress, `agent` our agent
registered, `wheel_mm` the `-w` circumference, `held` whether a connection
holds, `reconnecting` whether reconnection runs (a hold or the window).

Events, to watchers only (and in the `watch` snapshot):

```json
{"type":"adapter","present":true,"powered":true,"scanning":false}
{"type":"sensor","addr":"AA:BB:CC:DD:EE:FF","kinds":"hr,power","name":"CHEF HR","state":"connected","battery":80,"rssi":-58,"rssi_live":false,"paired":"2026-10-13"}
{"type":"value","addr":"AA:BB:CC:DD:EE:FF","kind":"hr","value":142}
{"type":"value","addr":"AA:BB:CC:DD:EE:FF","kind":"power","value":245,"cadence":88}
{"type":"value","addr":"AA:BB:CC:DD:EE:FF","kind":"csc","speed":25.3,"cadence":90}
{"type":"found","addr":"AA:BB:CC:DD:EE:FF","kinds":"hr","name":"CHEF HR","rssi":-60}
{"type":"lost","addr":"AA:BB:CC:DD:EE:FF"}
{"type":"pair","addr":"AA:BB:CC:DD:EE:FF","state":"connecting","reason":""}
{"type":"forgotten","addr":"AA:BB:CC:DD:EE:FF"}
```

`sensor` goes out when anything in it changes: `state` is `off` (Bluetooth
off), `waiting`, `connecting`, `connected` or `forgetting`; `battery`
(percent) and `rssi` (the last advertisement, live only while scanning)
are `null` when unknown; `paired` is the date or `""` (paired while the
clock was unset). `value` carries HR bpm, power W with cadence rpm, or CSC
speed km/h with cadence rpm; a member is `null` when there is no value
(five seconds without a measurement, an absent optional field, no link).
`found` offers a device with sensor services seen in our scan that is not
saved, again when its RSSI or name changes; `lost` takes it back. `pair`
steps through `connecting`, `resolving`, `bonding` (only when needed),
`saving`, then `done` or `failed` with a `reason` (`cancelled`,
`timed out`, `trust`, a BlueZ error name, ...).

Errors:

| Cause | Line |
|---|---|
| `pair` refused | `{"type":"error","cmd":"pair","reason":"<why>"}`: `loading` (records not read yet), `bluetooth off`, `busy` (another pairing runs), `already paired`, `not a sensor in range`, `too many sensors` (eight saved) |
| `cancel` of no running pairing of `ADDR` | `{"type":"error","cmd":"cancel","reason":"not pairing"}` |
| `forget` before the records are read, or of no saved sensor | `{"type":"error","cmd":"forget","reason":"loading"}` / `"not paired"` |
| a missing, extra or invalid argument (`scan` without `on`/`off`, a bad `ADDR`, `status foo`) | `{"type":"error","cmd":"scan","reason":"bad arguments"}` (for every request) |
| unknown command | `{"type":"error","cmd":"<as sent>","reason":"unknown command"}` |
| line over 255 bytes | `{"type":"error","reason":"line too long"}`; the rest of the line is discarded |
| 17th connection | `{"type":"error","reason":"too many clients"}`, then closed |

A word sent back (`<as sent>`) is its first 32 bytes, JSON-escaped. Empty
lines are ignored; a CR before the newline is stripped.

Connections and limits: `scan`, `hold` and `watch` live on the connection
and go when it closes, whatever the reason, so a client that dies cannot
leave the radio scanning or reconnecting for it. The socket is mode 0660,
in the `blesensd:blesensd` 0750 directory `/run/chef/blesensd`. Up to 16
connections. Each has a 16384-byte output queue and
every line is must-deliver (a lost `null` value or `sensor` state would
leave a client showing a stale one): a client that stops reading until a
line no longer fits is dropped like one that closed, its scan and hold
with it. Clients ignore unknown `type`s and members; a new member or type
keeps proto 1, a change that breaks a client bumps it (the
[compatibility rules](local-sockets.md#replies-and-events-the-envelope)).

The command-line client (the commands above the table) never prints the
hello and prints every other line raw. `status` ends at the `status` line,
`list` and `cancel` at their `ok`, `pair` at the pairing's `done` or
`failed`, `forget` at `forgotten` (both send `watch` first: the outcome is
an event); `watch` and `scan` (which sends `watch` and `scan on`) run until
the daemon goes or ^C. Exit 0 at the end, 1 on an error line for the
request, a failed pairing, no connection or the daemon closing first; 64
on a usage error.

Records use `chef-state blesens-load` and `blesens-save` under
`/data/v1/blesens/sensors` (directory 0700, file 0600, sealed version
`chef-blesens 1`, at most eight records). Each line is
`sensor ADDR hr,power,csc DATE|- -|forget NAME`. Without owned storage,
`/run/chef/state/blesens/sensors` is the fallback. Forget first durably marks its
record `forget`, then removes the BlueZ device and only after confirmation
saves the record deletion before reporting `forgotten`. Both saves must
succeed; failed saves retry, leaving the pending removal visible to watchers.
Pending Forget survives either daemon restarting;
errors retry. Storage children have bounded lifetimes and separate process
groups, killed as a group on timeout, leader exit or daemon exit.

### PC sensor emulator and verification

```sh
scripts/ble-sensor-emu.py --kinds hr,power,csc --name CHEF-SENSOR
make -C tools/blesensd test
python3 tools/tests/test_chef_state.py -k blesens
```

For actual bond-persistence checks, add `--bond --central PHONE_ADDR`.
The emulator supplies a temporary NoInputNoOutput default agent that accepts
only BlueZ requests about that phone; it restores Pairable and unregisters
at exit, returning the previous default agent. This uses BlueZ
[encrypt-notify](https://github.com/bluez/bluez/blob/master/src/gatt-database.c)
permissions on the measurement CCC; the ordinary mode needs no bond.

The emulator registers a BlueZ GATT application and LE advertisement on the
PC adapter, changing all measurements at 1 Hz and re-advertising after a
central disconnects. It runs as the user with python3 dbus-python and GLib.
The advertisement sets Discoverable on its LE instance; with the PC adapter
not globally discoverable, BlueZ marks that advertisement BR/EDR unsupported.
Keep classic Discoverable off (`bluetoothctl discoverable off`), so a saved
LE bond reconnects over LE after reboot. For this dual-mode PC peer, the
classic bond must be removed on both sides
before LE testing (user approved for the test peer), otherwise BlueZ can
choose BR/EDR. Real sensors are normally LE-only. Do not remove unrelated
bonds. Stop the emulator by its recorded PID, not a broad process pattern.

Host tests use a fake bluetoothd on a private D-Bus, never the system bus:
notification ownership, async identity, all-kind resets, confirmed Forget,
scoped agent, fresh watches, framing, the socket contract (hello, errors,
limits, a slow watcher dropped only on overflow), bounded reconnect and
storage child lifecycle. Live evidence and provenance are recorded in the
[M3 log](../../logs/system-ui-slice2-2026-10-11.txt).
Real sensor validation, wheel circumference UI, classic PC re-pair and
USB-out idle-current measurements remain next steps.
