# Wi-Fi bring-up research (2026-10-01)

Research and live validation of the reviewed baseline and ride Wi-Fi images.
The final acceptance results are recorded at the end of this document.

## Matching host driver

The project kernel is Motorola 4.4 with ICNSS and CLD_LL_CORE enabled, but no
qcacld source in its tree. Stock vendor contains qca_cld3_wlan.ko; this binary
must not be assumed compatible with the custom kernel (MODVERSIONS is enabled).
Build an external module against the actual kernel output and Module.symvers.

Motorola publishes matching MMI-QPTS30.61-18-10 sources:

- [qcacld-3.0](https://github.com/MotorolaMobilityLLC/vendor-qcom-opensource-wlan-qcacld-3.0/tree/a685f67b37cc311487526dda738301b63b903c9b), commit a685f67b37cc311487526dda738301b63b903c9b.
- [qca-wifi-host-cmn](https://github.com/MotorolaMobilityLLC/vendor-qcom-opensource-wlan-qca-wifi-host-cmn/tree/a6db9b5df15dd4b86d6e49b8e8788154547ab8eb), commit a6db9b5df15dd4b86d6e49b8e8788154547ab8eb.
- [fw-api](https://github.com/MotorolaMobilityLLC/vendor-qcom-opensource-wlan-fw-api/tree/aea65288eb08d96d977b99d9e4b60a05dfecd5f1), commit aea65288eb08d96d977b99d9e4b60a05dfecd5f1.

The common and fw-api repositories have the release tag but no identically named
qpts30 branch. Their annotated tags resolve to the commits above. Kbuild uses
MODNAME=wlan, CONFIG_QCA_CLD_WLAN=m and CONFIG_ICNSS to select SNOC. The three
source directories must be siblings. Kbuild defines MOTO_UTAGS_MAC; the Motorola
MAC parser reads two comma-separated MACs from androidboot.wifimacaddr. The stock
boot property confirms this format; do not record the device addresses publicly.

## Firmware and modem lifecycle

The verified modem_a.img contains /image/wlanmdsp.mbn, /image/bdwlan_chef.bin,
board-specific /image/bdwlan.* and a generic /image/bdwlan.bin. Chef and generic
BDF have different SHA-256 hashes, so they are not interchangeable evidence.
The vendor image provides /etc/wifi/WCNSS_qcom_cfg.ini; driver request paths are
wlan/qca_cld/WCNSS_qcom_cfg.ini and wlan/qca_cld/wlan_mac.bin. The persist backup
has an empty /wifi directory; do not assume a persistent MAC file exists.

The [2026-09-17 TFTP probe](../build-log.md) already observed the modem reading
wlanmdsp.mbn through TFTP and bringing wlan_pd UP. Existing gps-up owns modem
PIL, RMTFS RAM shadows, SERVREG-LOCATOR and TFTP. Initial Wi-Fi bring-up can
require that stack to be resident; it must not duplicate support servers,
restart the modem or stop GPS as part of WLAN teardown.

## Board-data handshake

The kernel ICNSS client registers FW_READY/MSA_READY, sets up MSA and obtains
capabilities, but does not implement BDF_DOWNLOAD. It gates WLAN driver probe
on FW_READY. A userspace WLFW client is therefore a bring-up prerequisite;
confirm the sequence on-device before claiming success.

Use this kernel's drivers/soc/qcom/wlan_firmware_service_v01.{c,h} as the wire
schema. Transport is AF_MSM_IPC (27), implemented by tools/msmipc.{c,h}, rather
than AF_QIPCRTR (42). Discover service 0x45 version 1 instance 0 (encoded
instance 0x1); msmipc_lookup returns all instances, so filter explicitly.
Use stock daemon client ID 0x444d4f4e (DMON), TLV 0x15 u32; do not register
as kernel client ID 0x4b4e454c. The exact stock daemon extracted from vendor_a.img
contains compressed debug symbols: wlfw_send_ind_register_req at 0xd5f4 writes
DMON with its validity flag. An anonymous client failed on its first BDF chunk;
DMON advanced three chunks in the first live probe, with final-chunk rejection
still under investigation.

Expected sequence: IND_REGISTER 0x20 (FW_READY TLV 0x10 and MSA_READY TLV 0x13), wait MSA_READY
0x2b unless already ready, CAP 0x24, BDF_DOWNLOAD 0x25, CAL_REPORT 0x26,
wait FW_READY 0x21. Registration response TLV 0x10 is a u64 status with
FW_READY bit 1 and MSA_READY bit 2. CAP board-info TLV 0x11 is a u32 board ID.
A successful CAP response may omit board-info: the kernel treats absence as
0xff, which the first live probe also required. Known board IDs require their
exact stock BDF; unknown 0xff requires explicit
chef-specific fallback, not arbitrary generic board data.

BDF request TLVs: 0x01 valid u8; 0x10 file_id enum32; 0x11 total_size u32;
0x12 segment u32; 0x13 u16 data count followed by at most 6144 bytes;
0x14 end u8; 0x15 BDF type u8. Empty CAL_REPORT metadata is TLV 0x01 containing
a single zero count byte. All replies need transaction/message/peer matching,
validated result TLV and bounded timeouts. This minimal initial handshake does
not establish calibration persistence or subsystem-recovery behavior.

## Exposure and live acceptance

Bind both telnet shells to 172.16.42.1 and ride HTTP to 172.16.42.1:80 before
association. Keep gpsd without -G. Chrony is currently absent; any later addition
must preserve loopback control/server exposure. Preserve bt-up vendor
pre-shutdown because Bluetooth and Wi-Fi share rails.

Live acceptance remains: module load, FW_READY and wlan0, scan, WPA2 association,
DHCP routes/DNS and network traffic, listener checks, USB preservation, teardown
and BT restart while associated. Target SSID is supplied by the user; credentials
must stay private and volatile. Development uses temporary fastboot boot images,
not flashing stock partitions.

## Initial live probe

A temporary boot of the original baseline image on October 1 exposed
WLFW service 0x45, encoded instance 0x1 after gps-up. ICNSS stats showed successful
registration, MSA setup and CAP, with state 0xd83 and WLAN FW EXISTS, but no
FW_READY yet. The first helper registration succeeded; CAP omitted board-info,
requiring the same 0xff fallback already present in icnss.c. This was sent back
for correction. Before loading WLAN, the live baseline telnet listener was
changed in RAM and verified as 172.16.42.1:23. These observations do not yet
establish successful board-data download, driver load or association.

## Follow-up live probe (October 2)

The DMON production image temporarily booted successfully and kept telnet bound
to USB. A clean modem startup reproduced final BDF rejection. Smaller 1024-byte
chunks, exact stock indication registration flags, and supplying the device MAC
through WLFW did not resolve it. The transferred chef BDF hash matched the local
backup and its 16-bit XOR checksum was 0xffff.

A diagnostic-only client then sent empty CAL_REPORT after preserving/logging the
final BDF failure. CAL_REPORT succeeded and FW_READY arrived. The matching module
loaded and ICNSS reached 0xd8f (FW READY and DRIVER PROBED); wlan0 and p2p0 appeared.
An explicit scan found the user-selected target SSID. Modem crash_count stayed
zero and telnet remained USB-bound. This diagnostic continuation is not packaged
as a production workaround: final BDF rejection must be explained before normal
startup should ignore it. Subsequent diagnostic association and DHCP checks are recorded below; normal
firmware startup remains unresolved.

Evidence: logs/wifi-first-probe-2026-10-01.txt and
logs/wifi-second-probe-2026-10-02.txt. Research binaries and extracted proprietary
stock daemon/firmware stay under ignored out/wifi-research/, not documentation.

## Diagnostic connection and cleanup (October 2)

Disabling Bluetooth startup and omitting the optional BDF-type TLV separately
still reproduced final-chunk error 1. Using the explicit diagnostic CAL_REPORT
continuation, a foreground USB terminal session associated with TP-Link_E975
using WPA2-PSK and acquired 192.168.0.116/24 from 192.168.0.1. WLAN subnet and
default routes were correct, with the independent USB route preserved. Two
internet pings succeeded and example.com resolved through the DHCP DNS server.

Starting bt-up while associated created hci0 and preserved association and
internet traffic. Modem crash_count remained zero. This establishes coexistence
through Bluetooth startup, not sustained simultaneous Bluetooth traffic. The
only observed TCP listener was 172.16.42.1:23. wifi-down removed private session
files, restored the prior empty resolver, removed WLAN routes and lowered wlan0,
while preserving USB, hci0 and the resident GPS support socket.

Two live userland defects were sent to implementation for direct review: the
baseline has no /etc/resolv.conf, and the packaged wpa_passphrase interactive
stdin path requires a terminal and fails when piped. The diagnostic test used a
RAM-only empty resolver and a retained terminal with echo disabled. Credentials
were never written to host files or evidence logs; the device session was torn
down after testing. These results do not resolve the strict production BDF
startup failure or establish calibration correctness after the diagnostic path.

The reviewed userland fixes were then copied into device RAM and retested with
private piped stdin and no pre-existing resolver file. Association, DHCP and
internet traffic succeeded; wifi-down restored resolver absence, deleted the
session directory and preserved the USB route.

To independently check serialization, the BDF request descriptor at 0x693d was
extracted from the exact stock daemon and encoded with Qualcomm's public
[qmi-framework encoder](https://github.com/qualcomm/qmi-framework). Its final
720-byte-segment request was 758 bytes and matched the helper's expected TLVs
byte-for-byte. This checks that descriptor and sample payload, not every request
or firmware state. Stock also attempts optional regdb.bin with BDF type 4 before
type 0; that file was absent from the inspected stock modem and vendor firmware
paths. The on-device mounted modem BDF and wlanmdsp hashes matched the packaged
copies, ruling out a differing active-slot copy in this probe.

## Firmware-side logging and retries

A subsequent clean baseline boot reproduced final BDF error 1 without modem
crashes. A bounded research helper enabled DIAG memory logging for the WLAN PD
only, captured the startup in volatile device memory and closed the logger.
Decoded QMI history showed all four requests/responses; firmware diagnostic
arguments recorded segment numbers 0..3 and cumulative lengths
6144/12288/18432/19152 before the error response. Hashed QSR messages were not
fully decoded: the modem's qdsp6m.qdb did not contain qmi_platform entries.
The capture does not prove that firmware validated or applied the BDF.

The exact stock daemon retries BDF failures up to three times on the same QMI
client. A research-only helper reproduced that bounded behavior without sending
CAL_REPORT after an error; all three attempts were rejected. No retry/bypass
change was added to production. DIAG tooling and raw WLAN-only evidence remain
under ignored out/wifi-research/.

A temporary boot of the backed-up stock Android image reached Android and
connected Wi-Fi. Its reported host driver 5.2.03.13O and firmware
1.0.1.0.717.11 matched the Linux probe. Device fallback properties were chef/APAC
and carrier retin. The unrooted stock shell restricted daemon/firmware reads;
no decoded stock BDF handshake was obtained from available startup logs. This
confirms the stock environment works, not which individual prerequisite differs.

For kernel isolation, a research image combines the stock boot_a kernel and
DTBs with our current userspace ramdisk. One NUL-terminated boot-option parser
string was changed from skip_initramfs to keep_initramfs so Motorola's appended
skip_initramfs cannot bypass /init; kernel instructions were not patched. Stock
config confirms NCM gadget and MSM shared-memory UIO support; /init already has
an mdev fallback for absent devtmpfs. This is an ignored temporary research
image, not a production kernel change. Its bring-up reproduced the same final BDF error with the strict helper,
ruling out the custom kernel changes as the sole cause.

## Exact vendor lifecycle resolves the strict-startup mismatch

On a fresh boot of the stock-kernel/RAM-userspace research image, the exact
backed-up stock cnss-daemon ran with its extracted Android runtime libraries.
A minimal preload supplied the observed chef/APAC/retin properties, redirected
Android logging to a volatile file, and returned failure for Android peripheral
manager registration (the resident gps-up stack already owned modem power).
WLFW transport, IDL encoding and daemon control flow were unchanged. No earlier
userspace WLFW client was run in that boot.

The daemon registered DMON with fw_status 0x4, selected bdwlan_chef.bin, and
logged BDF type 0/result 1/error 1. It then sent empty CAL_REPORT successfully
and received FW_READY 0x21. This establishes that the explicit diagnostic
continuation already tested matches the stock daemon's behavior in this
environment; treating that final response as unconditionally fatal is stricter
than the vendor lifecycle. It does not independently prove BDF application or
calibration accuracy.

The disassembly confirms the behavior: after transport-successful full transfer,
e208 sets the function return register w21 to zero; e2ec specifically checks
error 0x30, rather than rejecting every nonzero business response. Earlier
assumptions that stock rejects/retries result 1 were incorrect. Same-client
retry was a useful negative experiment, not the stock action for this result.
A prior stock-daemon run after our client had registered returned fw_status 0x5
and took its already-registered branch; a fresh first registration was essential
for observing its complete startup sequence.

The implementation now has a narrowly gated compatibility path for this verified
chef BDF and exact CAP firmware/chip tuple, final complete transfer/error 1 only,
with successful CAL_REPORT and FW_READY still required. Other errors remain
fatal. The reviewed production baseline passed a clean first-client boot test.

Evidence: [filtered stock daemon comparison](../../logs/wifi-stock-daemon-comparison-2026-10-02.txt).

After restoring the custom kernel, a GPS LOC no-op client completed without
error. A manual gpsd listener test using the documented UDP loopback input bound
2947 to 127.0.0.1 and ::1 only; telnet remained 172.16.42.1:23. No chrony process
was present. This checks service access/listener scope, not a new GPS fix.

## Reviewed production image live validation

On 2026-10-02, baseline boot.img SHA256
`dd0ea631a1b046821a4086336b3f01d5ca97b97c425c0933563df94c7a53e708`
was temporarily booted with the custom kernel. Normal `wifi-up` was the first
userspace WLFW client after resident `gps-up`. It printed the explicit stock
chef warning, received FW_READY and associated using WPA2 with TP-Link_E975.
DHCP assigned 192.168.0.116/24; connected/default WLAN routes and the USB route
coexisted. The initial three-packet ping lost one packet; subsequent five- and
three-packet checks had no loss. DNS resolved example.com after reconnect settled.

WPA disconnect/reconnect and a complete down/up session passed. Restarting the
init-owned Bluetooth wrapper during association restored hci0 while Internet
pings continued. GPS LOC no-op clients succeeded before and after Wi-Fi down;
modem crash count remained zero. Actual listeners were USB-only Telnet and
loopback-only gpsd (IPv4 and IPv6). WLAN TCP ports 23, 80 and 2947 were closed
while the host could connect to USB Telnet.

Down cleanup completed asynchronously: /run/wifi disappeared, the originally
absent resolver stayed absent, WLAN routes were removed, and USB, hci0 and GPS
remained available. Re-entering the passphrase via echo-disabled stdin restored
association; plaintext credentials were never saved to host files or device
configuration. Session credentials remain volatile in device RAM.

The reviewed ride image SHA256
`06c59544d6b65fc603b4bd28732e47e8a966affcfa0b77528896f82af74cddf2`
also passed a clean normal first-client startup: warning, FW_READY, WPA2,
DHCP 192.168.0.116 and DNS. Settled Internet ping passed 5/5 and an HTTP
fetch of example.com returned success. USB HTTP returned 200 while Wi-Fi was associated;
actual HTTP/Telnet listeners remained USB-only and WLAN ports 23/80/2947 were
closed. Bluetooth hci0 remained present, GPS LOC succeeded, and modem crash
count was zero. The device is left temporarily booted into this ride image
with a volatile Wi-Fi session; no stock partition was flashed.

Evidence: [production acceptance](../../logs/wifi-production-live-2026-10-02.txt).

## Utilities and UI integration prerequisites

The upstream [supplicant control interface](https://w1.fi/wpa_supplicant/devel/ctrl_iface_page.html)
is intended for external UIs. STATUS returns key/value fields; SCAN_RESULTS
uses tab-separated columns with escaped SSID data, and iterative BSS requests
avoid truncation when a single scan-results response is too large. Native
clients can use wpa_ctrl.c with separate request and ATTACH event connections
so events do not interleave with responses. Association events establish link
state; DHCP readiness and Internet reachability need separate checks.

The [iw documentation](https://wireless.docs.kernel.org/en/latest/en/users/documentation/iw.html)
covers nl80211 scanning, link/station measurements and regulatory information.
The installed tools should share the existing session owner rather than start
a competing NetworkManager, iwd or second supplicant. Pre-connection scanning
needs the modem support stack, initialized firmware/module and an interface
that is up; no credentials or DHCP should be required for that state.

Before repacking utilities, Alpine 3.24 aarch64 packages were staged under a
private temporary directory on the running device. Full iproute2 7.0 produced
valid JSON addresses/routes on the 4.4 kernel; jq 1.8.2 parsed them. ss reported
the existing USB-only HTTP/Telnet listeners. rfkill 2.42.3 produced JSON and
identified the WLAN phy0 entry as unblocked. The separate bt_power entry was
soft blocked despite the live hci0 entry being unblocked, so blanket rfkill
changes are inappropriate for WLAN control. iperf3 3.20 and tcpdump 4.99.6
loaded successfully without starting servers or captures.

Cloudflare HTTPS curl tests forced the phone's wlan0 route and validated CA
certificates after correcting its volatile clock from 1970. Comparable 25 MB
transfers measured 60–71 Mbps download and 51–71 Mbps upload. Results and the
working curl commands are in the [speed-test evidence](../../logs/wifi-cloudflare-speedtest-2026-10-02.txt).
These are single-request transfer measurements, not the full browser test.

## Standard NetworkManager runtime research

The user's [wireless guide](https://wiki.archlinux.org/title/Network_configuration/Wireless)
and [network configuration guide](https://wiki.archlinux.org/title/Network_configuration)
describe the usual separation between wireless authentication, network
management and low-level tools. Direct page access was blocked by Anubis;
indexed content and upstream documentation were available. NetworkManager
with wpa_supplicant is the chosen default; one manager/DHCP client owns WLAN.
USB's udhcpd is a server, with its interface explicitly unmanaged by NM.

Alpine 3.24 provides NetworkManager/libnm/nmcli/nmtui 1.52.2 and a Wi-Fi plugin.
A RAM probe using the existing system bus initially reported reason 71: link
not initialized by udev. Starting eudev 3.2.14 and coldplugging network devices
resolved that prerequisite for subsequent NM startup. A separate staging-only
issue was that tar preserved host UID 1000 on plugin files; NM correctly refused
those plugins until their ownership was changed to root. Production newc
packing already forces root ownership.

With strict unmanaged-all-except-wlan0 configuration, no-auto-default, internal
DHCP, file resolver management and initial scan MAC randomization disabled,
standard nmcli listed networks and activated a memory-only WPA2 profile.
The passphrase was supplied to nmcli's echo-disabled --ask secret prompt.
DHCP assigned 192.168.0.116 and the WLAN/default and USB routes coexisted; DNS
and Internet ping worked. GPS LOC succeeded, hci0 remained present and modem
crash count was zero. Running global wpa_supplicant with -u and
-O /run/wpa_supplicant made uncustomized wpa_cli -i wlan0 work. No manual
udhcpc or competing supplicant was running during this NM probe.

Upstream references:
[NetworkManager.conf](https://networkmanager.dev/docs/api/latest/NetworkManager.conf.html),
[nmcli](https://networkmanager.dev/docs/api/latest/nmcli.html),
[NetworkManager D-Bus API](https://networkmanager.dev/docs/api/latest/gdbus-org.freedesktop.NetworkManager.html).
The D-Bus AddAndActivateConnection2 API supports memory/volatile profiles;
nmcli connection add save no supports memory-only settings. Native UIs should
use libnm/D-Bus and a secret agent, rather than passwords in shell argv.

Shared modem lifetime needs one boot owner, with ride logging borrowing the
stack and releasing only its LOC session. Otherwise stopping the logger shuts
down the support services on which WLAN depends. Kernel config has built-in
legacy iptables/filter support and no nf_tables. Address-bound USB listeners
alone do not enforce interface affinity; any additional USB isolation rule
needs the legacy backend and must precede WLAN activation. NM dispatcher
pre-up is synchronous before NM reports activation, but is not documented as
a pre-address DHCP lease rejection mechanism.

A standard NM memory profile also connected to TP-Link_E975 5G at 5745 MHz
using the same passphrase and DHCP address. Single-stream 10-second local
iperf3 tests against the host's Ethernet interface measured receiver throughput
of 28.2 Mbps down / 23.1 Mbps up on 2.4 GHz and 69.5 / 59.4 Mbps on 5 GHz.
Both used the observed enabled power-save setting and had zero reported TCP
retransmissions. These local tests avoid the Internet link as a bottleneck, but
are short measurements of the current radio conditions.
Evidence: [standard NM probe](../../logs/wifi-networkmanager-probe-2026-10-02.txt).

The 1970 boot clock also prevents normal HTTPS curl/apk use before a GPS fix.
A client-only chrony 4.8 probe with pool.ntp.org iburst and initial makestep
synchronized successfully. It then ran under the dedicated chrony account
with its Unix command socket/drift state in /run/chrony, owned by that account.
The pre-existing ntp account occupied UID/GID 123; free 124 was used for the
probe. chronyc requires a named chrony account even when chronyd runs as root,
and its privilege drop requires access to the private socket directory.
Port 0 and cmdport 0 left no UDP NTP/control listener; chronyc Unix tracking
reported normal synchronization. This is a prerequisite for verified TLS,
not a reason to disable certificate checks. nmea-broker already sets time
only if the system is still pre-2020, so its existing GPS bootstrap can remain.
See upstream [chrony configuration](https://chrony-project.org/doc/4.8/chrony.conf.html).


### Clean standard-stack validation findings

The first integrated clean baseline exposed two packaging/startup requirements
that staged research had masked: all-device udev coldplug exceeded its bounded
settle timeout, and apk's user-mode extraction plus root-owned cpio left the
D-Bus launch helper root:root0750. Net-only coldplug and restoring the standard
root:messagebus4750 launch helper before the bus corrected these. Baseline
55bb3a66 then automatically started one shared gps-up, NM, D-Bus-activated
supplicant and chrony without RAM startup repairs; native nmcli connected5GHz.

Chrony initially starts without DNS. It retained unresolved pool sources after
WLAN activation until a manual refresh; a bounded best-effort dispatcher
refresh is needed for automatic time/TLS readiness. With synchronized time,
verified HTTPS, signed apk indexes and installation/execution of ethtool7.0
worked. No certificate bypass was used.

Fresh activation of exact/broad USB-overlapping static IPv4 profiles caused
the dispatcher to disconnect. Reapply revealed a timing race: the reapply
event was delivered while the address list was empty, before the new static
address appeared. This is an observed configuration transition, not absence
of a reapply event; upstream explicitly documents that event in the
[dispatcher reference](https://www.networkmanager.dev/docs/api/latest/NetworkManager-dispatcher.html).
Ingress DROP and dedicated USB policy routing protected USB throughout.
Final revised-image acceptance remains pending; see the
[filtered live record](../../logs/wifi-standard-nm-live-2026-10-02.txt).


Final baseline0d17d4c4 clean-boot acceptance closed these findings: automatic
D-Bus/supplicant startup, chrony pool refresh and1970-to-current synchronization,
verified HTTPS and signed apk installation, exact/broad overlapping-address
reapply disconnection, radio cycling/reconnect and USB isolation all passed.
GPS LOC access and the same shared modem owner survived; crash_count stayed0.
The display remained off and the phone was left connected5GHz with credentials
only in RAM. The reapply check observes a bounded ten-second transition; this
is not a claim of arbitrary future configuration policing. Long-duration
reliability/current remain unmeasured. The user excluded ride-image testing
and deferred a separate GPS/NTP time-service redesign until GPS modernization.
The filtered standard-stack live record above contains final evidence.
