# SSH with key login, then install the next pair

User request 2026-10-10: control the phone without the USB cable (it
gets in the way of physical tests such as the magnetometer holds), and
install the next pair so `chef-reboot` (4a52140) is on the phone. Do both
in one install: build SSH first, then one system_a + boot_a pair carrying
4a52140, a202272 and the SSH commit.

Roles: `ssh_impl` implements, `ssh_review` reviews, `ssh_coord` (the
coordinator) researches and talks to the user. Report with `herdr agent
prompt ssh_coord "..."`. No em dashes anywhere (code, docs, messages,
commits). The previous pair's notes are in the memory files and in
`logs/state-persistence-followup-2026-10-08.txt` and
`logs/reboot-bootloader-2026-10-10.txt`; read the phone live-test drivers
(`scripts/phone.py`, `scripts/phone-boot.sh`, `docs/live-testing.md` or
wherever the build log points) before touching the phone.

## Today

- The only shell is busybox `telnetd -b 172.16.42.1 -l /bin/sh`
  (`initramfs/etc/inittab` line 3): passwordless root, deliberately bound
  to the USB gadget address. Keep it exactly as it is: it is the recovery
  path.
- Wi-Fi works (NetworkManager, DHCP; the phone was 192.168.0.116 on the
  home network on 2026-10-10). Bluetooth PAN also gives IP.
- `/etc/passwd` already has an `sshd` user (uid 22) from the base layout.
- The root is the read-only `system_a` image built from Alpine packages
  (`scripts/mkrootfs.sh` `PKGS`, `--no-scripts`, applet links at build
  time); `/root`, `/run`, `/var` are tmpfs; durable state only through the
  `/data/v1/...` allowlist (`chef-storage`, `chef-state ours`).
- The host has no SSH key yet (`~/.ssh/*.pub` does not exist).

## Design (adjust with reasons)

- **Server: dropbear** from Alpine (`dropbear`, plus `dropbear-scp` and
  an sftp server, for example `openssh-sftp-server`, so `scp` and `sftp`
  both work; check sizes and pick the smallest set that gives both).
  Started from inittab (`::respawn:`), foreground, logging to syslog or
  stderr that reaches kmsg in the way the other daemons do.
- **Key login only.** No password logins at all (`-s`), root allowed only
  with a key. Listens on port 22 on all interfaces (USB, Wi-Fi, PAN). No
  other auth method.
- **Authorized keys at build time**, from a gitignored host file (pattern:
  `tools/wifi-psk`; for example `secrets/ssh/authorized_keys`, checked by
  the build: refuse to build an image with SSH enabled and an empty or
  malformed file, or build with dropbear disabled and say so). Baked into
  the read-only image so SSH works on RAM-only boots and after a wiped
  `/data`. dropbear reads `~/.ssh/authorized_keys` of the user, and
  `/root` is tmpfs seeded from the image: confirm how `/root` is set up in
  `/init` and make the key file land there with the strict permissions
  dropbear demands (it refuses group/world-writable paths).
- **Host keys persist on /data**: `/data/v1/ssh/` (root, 0700), added to
  `chef-storage`'s optional state directories. Generated once with
  `dropbearkey` (ed25519; ecdsa only if a client needs it) if missing, so
  the host key does not change across boots (no host-key warnings). If
  `/data` is not ours (`chef-state ours` fails), generate into `/run/ssh`
  and log that the host key is ephemeral this boot. Never put a private
  host key in the image or the repo.
- **Host side**: a script or documented commands to create a dedicated
  key `~/.ssh/chef-cyclo_ed25519` if missing (no passphrase by default so
  the agents can use it non-interactively; say so in the docs), copy its
  public half into the gitignored authorized_keys file, and an
  `~/.ssh/config`-style snippet or a `scripts/phone.py` transport option
  (for example `--ssh HOST`) so the existing `run/push/pull` drivers work
  over SSH as well as telnet. Do not edit the user's `~/.ssh/config`
  yourself; print the snippet.
- **Finding the phone on Wi-Fi**: the DHCP address can change. Show the
  current Wi-Fi IPv4 address on the panel (one kmsg line from the
  existing NetworkManager dispatcher on connect/up, which fblog shows),
  and document it. No mDNS daemon unless it is trivial.
- **Shutdown**: dropbear must not hold `/data` (host key read at start
  only; confirm with `chef-storage shutdown`'s fuser path) and must not
  delay the `::shutdown` hooks. An SSH session that runs `reboot` or
  `chef-reboot bootloader` must still complete them.
- Power: confirm idle dropbear costs nothing measurable (no polling).

## Acceptance

- Host: build checks (missing or bad authorized_keys, permissions), a
  test of the host key bootstrap logic (ours / RAM-only / existing key
  kept), `scripts/phone.py` transport selection if added. `make -C tools
  test` passes.
- Live on a RAM image (`scripts/phone-boot.sh`, which now uses
  `chef-reboot`): `ssh` over Wi-Fi with the key works and gives a root
  shell; password login is refused; a wrong key is refused; telnet over
  USB still works unchanged; `scp` and `sftp` both copy a file; the host
  key is identical after a reboot; with USB unplugged, a run of
  `scripts/phone.py` over SSH works; the panel shows the Wi-Fi IP; an
  ordinary reboot from an SSH session completes all shutdown hooks with
  rc 0 (power rows on /data, as in the previous acceptance).

## Install

The user has asked for the install. After the SSH commit is approved:

- Build the pair from the exact tree (4a52140 + a202272 + the SSH
  commit), kernel unchanged. `ssh_review` gives exact-diff clearance
  against the full diff since the installed pair's commit 8201c79 (record
  that diff's hash), then `ssh_coord` gives the final go.
- Same install order as on 2026-10-08 (see
  `logs/state-persistence-followup-2026-10-08.txt` section 5): slot a
  check + artifact rehash, flash system_a_a, re-check slot, stage-1
  fastboot boot of the new stage-1 (must pass), slot check + rehash,
  flash boot_a_a, reboot. Leave the current installed pair via
  `chef-reboot` pushed to `/run/push` (the installed image lacks it), not
  the raw route. Keep the current pair (system_a 84a0baa4 + boot_a
  6062f115) and the one before (799577b6 + e5b57eff) in `out/` as
  fallbacks.
- Post-install regression as before (binaries, owned binds, Wi-Fi/HTTPS,
  chrony Normal, BT powered, powerd batching, mag-group-2980 unchanged)
  plus SSH over Wi-Fi with USB unplugged, then one ordinary reboot and one
  `chef-reboot bootloader` + normal boot back, both with all hooks rc 0.
- Record in a new log `logs/ssh-and-install-2026-10-10.txt` and the
  build log; commit after review.

## Process

- RAM images and reboots are fine. Ask the coordinator before any step
  that needs the user to touch the phone (unplugging USB for the
  cable-free check is one: announce it through the coordinator).
- Never delete `/data/v1/sensors/mag-group-2980` (a real learned bias)
  or anything else under `/data/v1` that you did not create.
- Commit only after the reviewer approves. Do not push.
