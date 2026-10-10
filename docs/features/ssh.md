# SSH

[Feature index](README.md) · [Live testing](../live-testing.md) · [USB networking](usb-networking.md)

## Current behavior

`dropbear` listens on port 22, IPv4 and IPv6, on every interface (USB 172.16.42.1, Wi-Fi, Bluetooth PAN) and accepts public-key logins only. It runs from inittab under `initramfs/usr/bin/chef-sshd` (`::respawn:`). The passwordless busybox `telnetd` on 172.16.42.1 (inittab line 3) is unchanged: it stays the recovery shell over USB.

- **Keys that may log in** (to any account with a login shell, which today is only root: every other account's shell is outside `/etc/shells`, and a host test keeps it so) are baked into the image as `/etc/chef/ssh/authorized_keys` from the gitignored host file `secrets/ssh/authorized_keys`. dropbear reads them there (`-D /etc/chef/ssh`); it checks that the file and every directory up to `/` are owned by root and not writable by group or others. `/root` is an empty tmpfs that `/init` mounts, so nothing is copied at boot, and SSH works the same on RAM-only boots and after a wiped `/data`.
- **No password logins at all** (`-s`). root's password is empty in `/etc/shadow`; with `-s` dropbear offers only `publickey`, and without `-B` it never accepts a blank password. Three auth tries per connection (`-T 3`).
- **Host key**: one ed25519 key in `/data/v1/ssh/dropbear_ed25519_host_key` (root, 0700 directory, 0600 key), generated on the phone with `dropbearkey` the first time (temp name, sync, rename, sync) and kept after that, so the host key is the same on every boot, RAM images included, as long as `/data` is ours. A key that `dropbearkey -y` cannot read (corrupt, empty) is moved to `.bad` and replaced, with a kmsg line. When `/data` is not ours (`chef-state ours` fails) or the `/data` step fails, the key is generated in `/run/ssh` for this boot only and kmsg says `host key in /run/ssh is ephemeral`; `/run/ssh/host-key-source` says `persistent` or `ephemeral`. If `/data/v1/ssh` cannot be set up (for example `/data` remounted read-only after an error) but already holds a valid key in a root-only directory, that key is used as it is. Private host keys are never in the image or the repository: `scripts/ssh-keys.py scan` refuses a staged tree that contains private key material, in both `mkinitramfs.sh` and `mksystem.sh`.
- **dropbear never touches `/data`.** It re-executes itself for every connection and reads its host key each time, so `chef-sshd` gives it a copy in `/run/ssh`. `chef-storage shutdown`'s `fuser -m /data` therefore never finds it, and it does not delay the `::shutdown` lines. An SSH session that runs `reboot` or `chef-reboot bootloader` ends when init's final SIGTERM reaches it, after the hooks.
- **Logs** go to kmsg (and so to the panel through fblog) as `dropbear[PID]: ...` lines, with at most 20 lines per 60 s, except successful logins (`Pubkey auth succeeded`), which always pass; the rest are counted (`chef-sshd: N dropbear log lines suppressed`), so a Wi-Fi brute force cannot flood the screen. At start: `chef-sshd: host key persistent SHA256:...; listening on port 22, key login only`.
- **Forwarding**: local forwarding (`ssh -L`) works, so the host can reach loopback services such as gpsd on 2947; remote forwarding (`-R`, a listener on the phone) is off (`-k`). A key holder is root anyway, so `-L` grants nothing new. `-K 30` sends keepalives while a session is open, so a session whose host vanished is closed.
- **Idle cost**: none measurable. dropbear blocks in `select()` without a timeout and has no timers while no session is open; `chef-sshd`'s log reader blocks on the pipe.
- **Finding the phone on Wi-Fi**: `/etc/NetworkManager/dispatcher.d/30-chef-wifi-ip` writes `chef Wi-Fi: <profile> IPv4 <address>, ssh root@<address>` to kmsg whenever `wlan0` comes up or its lease changes, and fblog shows it on the panel.
- **Login shells** read `/etc/profile`, and `/etc/profile.d/chef-session.sh` gives them root's session bus (`XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS`) like the telnet shell.
- **Packages**: `dropbear` (530 KB installed; the `utmps-libs` and `skalibs-libs` it needs were already in the root) and `openssh-sftp-server` (199 KB) at `/usr/lib/ssh/sftp-server`, the path compiled into Alpine's dropbear. OpenSSH's `scp` uses the SFTP protocol by default, so `scp` and `sftp` both work; the legacy `scp -O` protocol needs `dropbear-scp` and is not in the image.

## Use

Once per host (creates `~/.ssh/chef-cyclo_ed25519` with no passphrase, so scripts and agents can use it without a prompt, adds its public half to `secrets/ssh/authorized_keys` and prints an `~/.ssh/config` snippet; it never edits `~/.ssh/config`):

```sh
scripts/ssh-setup.sh
```

Then build an image (the build refuses a missing, empty or malformed `secrets/ssh/authorized_keys`; `SSH=0` builds without SSH, loudly, and `chef-sshd` then stays idle) and boot or install it. With the snippet:

```sh
ssh chef                       # Wi-Fi address from the panel
ssh chef-usb                   # over USB
scp file chef:/run/            # SFTP protocol
sftp chef
```

Or without it:

```sh
scripts/phone.py --ssh 192.168.0.116 run 'uname -a'
PHONE_SSH=192.168.0.116 scripts/phone.py push build/tool -d /run/test
```

`phone.py --ssh` uses the dedicated key only (`IdentitiesOnly`, `BatchMode`), its own `~/.ssh/chef-cyclo_known_hosts` with `HostKeyAlias chef-cyclo` (one entry whatever address the phone has) and `StrictHostKeyChecking accept-new`, and does not read `~/.ssh/config`. Its remote commands get the telnet shell's `PATH`, `HOME` and session bus. A host key mismatch (for example a RAM-only boot with an ephemeral key) stops it with the `ssh-keygen -R chef-cyclo -f ~/.ssh/chef-cyclo_known_hosts` line to use if the change is expected.

## Modify and verify

Files: `initramfs/usr/bin/chef-sshd`, `initramfs/etc/inittab`, `initramfs/usr/bin/chef-storage` (creates `/data/v1/ssh`), `initramfs/etc/NetworkManager/dispatcher.d/30-chef-wifi-ip`, `initramfs/etc/profile.d/chef-session.sh`, `scripts/ssh-keys.py`, `scripts/ssh-setup.sh`, `scripts/phone.py`, `scripts/mkrootfs.sh`, `scripts/mkinitramfs.sh`, `scripts/mksystem.sh`. Host tests: `tools/tests/test_ssh.py` (in `make -C tools test`; `chef-sshd` under bash and BusyBox ash).

Live checks: key login over Wi-Fi gives a root shell; password, empty-password and wrong-key attempts are refused; telnet over USB unchanged; `scp` and `sftp` copy a file; the host key fingerprint is the same after a reboot; `phone.py --ssh` works with the USB cable out; the panel shows the Wi-Fi address; `fuser -m /data` does not list dropbear; a `reboot` from an SSH session completes every shutdown hook.

## Limitations

The DHCP address can change; read it from the panel or the router. No mDNS. Over IPv6 the phone takes the router's global addresses, so the home router's inbound IPv6 policy decides whether port 22 is reachable from the internet (key login only either way). dropbear's built-in caps on unauthenticated connections (5 per address, 30 in all, 300 s auth timeout) mean a LAN peer can hold off new logins for up to the auth timeout; telnet over USB stays the fallback. Only ed25519 host keys (any current OpenSSH client accepts them). The key in `secrets/ssh/authorized_keys` is part of the image: removing a key needs a rebuild and reinstall.
