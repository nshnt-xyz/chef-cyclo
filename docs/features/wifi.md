# Wi-Fi

Baseline `0d17d4c4` uses [standard NetworkManager/nmcli/nmtui](wifi-ui.md),
with one boot-resident modem owner and volatile profiles. Clean automatic
startup, native5GHz, automatic chrony clock correction, verified HTTPS and signed
apk installation passed, as did exact/broad static reapply rejection and radio
off/on. Final5GHz reconnect, settled ping5/5, HTTPS200, native wpa_cli PONG,
GPS LOC noop and USB-only listeners passed with hci0 present and crash_count0. Ride is
excluded from current acceptance at the user's request; no new ride test is
claimed. The older manual workflow below is historical and refused in NM builds.

## Earlier manual workflow and device evidence

Manual WPA2/DHCP/Internet/DNS, reconnect, BT coexistence and cleanup were
verified on baseline/ride images on 2026-10-02.

Start `gps-up` first and keep it running: Wi-Fi currently depends on its
modem, IRSC and support services. `wifi-up` uses `wlan-fw` to initialize board
data over AF_MSM_IPC, then loads the matching source-built WLAN module. It
requires the per-device bootloader Wi-Fi MACs and refuses a shared fallback.
It never starts or stops gps-up itself. Start a WPA2 personal session in a
USB shell. The command owns the session and stays in the foreground. Pass the
passphrase on stdin, never as a command-line argument or in shell history:

```sh
stty -echo
wifi-up -- 'Your SSID'
stty echo
```

Type the passphrase followed by Enter after starting wifi-up. The terminal stays
without echo until the session ends; use another USB shell for diagnostics. If
you interrupt the surrounding shell before `stty echo`, restore echo manually.
For automation, feed a protected RAM-only file through stdin instead.
`wifi-psk` derives WPA2 PSKs without the packaged wpa_passphrase interactive
TTY requirement; it never writes the plaintext passphrase into the config.

```sh
wifi-status
wifi-down
```

Association has a 60-second timeout. DHCP then supplies a WLAN address, default
route and DNS. Logs and the derived WPA key are kept in the root-only directory
`/run/wifi`; nothing is written to stock partitions or persisted across boots.
Ctrl-C, TERM or wifi-down stops the owned DHCP/supplicant processes, clears WLAN
addresses, brings wlan0 down, restores the prior resolver configuration and
removes session files. `wifi-down` signals the foreground owner asynchronously;
wait for `/run/wifi` to disappear before starting another session. A hard kill can leave stale state; avoid SIGKILL. Only one
session is supported, and changing DNS concurrently with it is unsupported.
There is no boot-time association, open-network mode or enterprise setup.
Wi-Fi down leaves the module and firmware resident and disables wlan0; it does
not establish that the WLAN hardware is powered off.

Both image variants bind telnet to 172.16.42.1; ride HTTP binds
172.16.42.1:80. Keep gpsd without `-G` so its client port stays on loopback. Earlier manual images had no chrony. New standard builds add client-only chrony
with `port 0`/`cmdport 0` and root-only Unix control, described in the
[standard guide](wifi-ui.md). Wi-Fi teardown never controls the
shared BT rails, and bt-up retains its vendor pre-shutdown restart sequence.

Build with `scripts/build-wifi.sh` against the current kernel and its
Module.symvers. This fetches three pinned Motorola MMI-QPTS30.61-18-10 WLAN
source trees into `out/wifi-research`, then builds `out/wifi/wlan.ko` using the
normal cross compiler and kernel flags. Builds use fresh exports of the pinned
Git trees, so cached untracked or ignored files cannot alter build inputs. `mkinitramfs.sh` also invokes it and
extracts stock BDF/INI/firmware from local partition backups; it never packages
the stock Android precompiled module. Run `make -C tools test-wifi` for host
protocol/DHCP checks.

The clean final baseline image passed normal first-client initialization,
private piped stdin, startup with no resolver file, WPA2/DHCP/Internet/DNS and
WPA disconnect/reconnect. Restarting init-owned bt-up recovered hci0 while
Internet traffic remained working, with modem crash_count zero. GPS LOC noop
succeeded before and after Wi-Fi down. Teardown removed the private session,
restored resolver absence and left only the USB route.

Actual listeners were verified: telnet on USB only and gpsd on 127.0.0.1/::1;
WLAN TCP ports 23, 80 and 2947 were closed. The final ride image also passed normal first-client initialization,
WPA2/DHCP/DNS and USB HTTP 200 while associated. Its HTTP/telnet listeners were
USB-only and WLAN ports 23/80/2947 were closed; hci0 remained present, GPS LOC
returned success and modem crash_count stayed zero. Its initial Internet ping
lost one packet; settled ping passed 5/5 with zero loss and an HTTP request to
example.com returned success. Current measurements do not establish WLAN
power-off current or long-ride reliability. Record future image hashes and
device evidence in the [build log](../build-log.md).

The stock chef cnss-daemon was observed receiving final BDF result `1`, error
`1`, then sending CAL_REPORT successfully and receiving FW_READY. `wlan-fw`
mirrors that lifecycle only after every earlier chunk succeeded and the final
chunk completes the full transfer, with exact `chef` device-tree model identity, board `0xff`, CAP chip `0x140`, family
`0x4002`, SoC `0x40050000`, firmware `0x101402cd`, and the exact chef BDF SHA-256
`b72b699075a087fe5c299a83768f2c2c1a8dbbea576099c80183970f32b8c43c`.
It checks the same open file with sha256sum, preserves a warning, and still
requires successful CAL_REPORT plus FW_READY. Any other error, firmware
identity, board data or incomplete transfer remains fatal. This is a verified
vendor lifecycle compatibility rule, not permission to ignore arbitrary BDF
failures. See [research evidence](../research/wifi.md).
