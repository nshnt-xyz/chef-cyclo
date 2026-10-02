# Standard Wi-Fi tools and UI integration

The baseline uses NetworkManager as the sole WLAN manager, with internal DHCP
and standard system D-Bus/libnm interfaces. `nmcli` and `nmtui` are the primary
interfaces. NetworkManager activates one wpa_supplicant through D-Bus using
`/run/wpa_supplicant`; no custom Wi-Fi API or extra DHCP client is started.

Clean baseline `0d17d4c4` passed automatic modem/NM/supplicant/chrony startup,
native5GHz activation, automatic1970-to-current clock synchronization with no
manual refresh, verified HTTPS200 and signed apk update/install of ethtool7.0.
Exact/24 and broad/16 static reapply tests now automatically disconnect, retaining
USB routing, the shared owner, hci0 and zero modem crashes; radio off/on passed.
Final5GHz reconnect, settled ping5/5, HTTPS200 and native wpa_cli PONG passed.
GPS LOC noop succeeded with the same owner, hci0 present and zero modem crashes.
Only USB TCP23/UDP67 listened; WLAN23/80/2947 and UDP123/323 were closed. Earlier55bb evidence
also covers2.4GHz, native wpa_cli/D-Bus, strict USB unmanaged state and disconnect
cleanup. See the [filtered baseline evidence](../../logs/wifi-standard-nm-live-2026-10-02.txt).
Final acceptance excludes the ride variant at the user's request; its newly
built artifact is untested and retained pending the user's planned removal.

Boot coldplugs eudev network metadata, restores the standard D-Bus activation
helper to root:messagebus mode4750 before starting the bus, reserves NM ownership,
and starts one resident `gps-up` with init's `once` action. `wifi-nm` waits for its validated
PID/start-time, readiness marker and QMUX socket before one firmware/module
preparation attempt. NM is supervised independently; it never restarts the
modem after failure. The modem stays resident even when Wi-Fi is idle; power consumption remains to be measured.
`/run/gps-up.log` holds shared support logs. Do not start another gps-up.

Only wlan0 and Bluetooth devices (NM type `bt`, for PAN) are managed. USB,
loopback and WWAN devices are explicitly unmanaged; USB's existing udhcpd is a server for the attached host. Before WLAN
preparation, explicit legacy iptables drops WLAN ingress to 172.16.42.0/24 and a
dedicated policy route preserves the USB subnet. Failure to install protection
blocks NM startup. An NM dispatcher disconnects overlapping IPv4 leases after
activation/renewal; it is not a pre-address veto. The ingress rule protects the
transition. NM1.52.2 does emit reapply, but a live event-time IPv4 query was
empty while a later overlapping address was still being applied. The hook now
observes reapply for a bounded10s window, including an initially old address;
other activation/DHCP events wait only when addresses are empty. An overlap
causes asynchronous disconnect. At the end, empty is allowed only if the
selected profile explicitly disables/ignores IPv4; missing/query-failed or
expected-but-empty state disconnects. This implements activation/DHCP/reapply
checks, not an atomic pre-address veto or an unlimited observer. Continuous
USB ingress/policy protection remains independent of transition timing.
Exact and broad static reapply passed on final0d17; fresh static activation
passed on earlier55bb. Controlled overlapping DHCP-server leases have not been
live-tested; DHCP overlap logic also has host regression coverage. No general
Internet firewall policy is added. Old manual `wifi-up`
refuses NM mode even while NM is starting or unavailable.

The Alpine v3.24 package set includes NetworkManager 1.52.2, libnm, nmcli,
nmtui, eudev, gdbus, wpa_supplicant, iw, full iproute2 (ip/ss/tc), rfkill,
curl/CA certificates, chronyd/chronyc, jq, tcpdump, iperf3 and legacy iptables with its shared
extensions. Runtime `apk`, public keys and main/community repositories are
retained. Compatible tools can be installed with ordinary `apk add`; packages,
profiles and runtime files remain RAM-only. There are no seeded credentials or
connections. Root is the current authorization scope; a future nonroot UI
needs deliberate D-Bus/polkit authorization.

For an interactive WPA2 personal connection, create a volatile profile without
a saved secret, then answer nmcli's secret prompt:

```sh
nmcli device status
nmcli device wifi list ifname wlan0 --rescan yes
nmcli connection add save no type wifi ifname wlan0 con-name ride-wifi \
    ssid 'Your SSID' connection.autoconnect no ipv4.route-metric 100 \
    wifi-sec.key-mgmt wpa-psk wifi-sec.psk-flags 2
nmcli --ask connection up id ride-wifi
nmcli device show wlan0
nmcli connection down id ride-wifi
```

Never pass a PSK in argv, shell history or logs. `nmcli --ask` uses a terminal
prompt; automated clients should use libnm/D-Bus with in-memory secrets or a
SecretAgent instead of piping a password into nmcli. Keyfiles are restricted to
root under `/run/NetworkManager/system-connections`; even saved profiles vanish
on reboot. `nmtui` provides the standard terminal UI. Persistent credential
storage has not been introduced.

For a GUI, use libnm or NetworkManager's system D-Bus API and property signals.
AddAndActivateConnection2 supports memory/volatile profiles. Keep association,
IP/DHCP readiness and Internet reachability separate; a completed association
does not prove usable addresses or Internet access. SSID properties represent
bytes, not guaranteed UTF-8. Native wpa_ctrl clients should keep event ATTACH
and request/reply sockets separate, preserve escaped SSIDs in SCAN_RESULTS,
and use iterative BSS queries for complete lists. Under NM ownership, direct
wpa_cli connection configuration would compete with NM; use it for diagnostics.
Invoke tools with explicit argv arrays rather than shell interpolation.

```sh
wpa_cli -i wlan0 status
wpa_cli -i wlan0 signal_poll
ip -j address show dev wlan0
ip -j route show
ss -lntup
rfkill --json
```

Do not block/unblock all rfkill devices: WLAN and BT power controls are
independent. tcpdump captures and iperf3 servers are explicit diagnostics;
neither is started at boot. Lead verified native ip/rfkill JSON on kernel 4.4,
loaded tcpdump/iperf3 and curl TLS after correcting the clock. Verified HTTPS
requires a real wall clock and the packaged CA bundle; keep certificate checks
enabled. A foreground chronyd (dedicated chrony UID/GID102, privilege drop via libcap) starts independently at boot as an NTP client
(pool.ntp.org, initial large-clock step permitted for the first three updates).
Its private drift/PID and root-accessible chronyc Unix socket are under /run/chrony. `port 0`
and `cmdport 0` disable NTP server and UDP command listeners; no allow rule or
RTC write is configured. A bounded, best-effort NetworkManager dispatcher refreshes chrony pool DNS
on WLAN up and DHCP updates, because chrony initially starts without DNS.
A refresh failure leaves WLAN connected and time synchronization pending.
Network availability and a reachable pool are required;
check `chronyc -h /run/chrony/chronyd.sock tracking` and `sources` before
HTTPS package installation.
Use nmea-broker `-n` when chrony owns the clock. GPS SHM is not enabled on this
kernel; an explicit offline GPS fallback can use the broker's existing one-time
1970 correction without claiming continuous synchronization. See [speedtest evidence](../../logs/wifi-cloudflare-speedtest-2026-10-02.txt).

Native interface references supplied by lead:
[NetworkManager configuration](https://networkmanager.dev/docs/api/latest/NetworkManager.conf.html),
[wpa_supplicant control interface](https://w1.fi/wpa_supplicant/devel/ctrl_iface_page.html),
[iw documentation](https://wireless.docs.kernel.org/en/latest/en/users/documentation/iw.html).
