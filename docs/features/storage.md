# Persistent data

[Feature guides](README.md) · [Install handoff](../next-steps/install-layout-handoff.md)

Phase 2 adds writable storage on `userdata`, discovered by its unique
`PARTNAME` in sysfs. It requires slot `_a`, exactly 55,289,298,432 bytes
(107,986,911 sectors), an unmounted block device with matching kernel device
identity, no swap use and no block holders. All other partitions are outside
this command's scope.

Provisioning is explicit and destructive:

```sh
chef-storage format --yes-erase-userdata
```

This command refuses an already provisioned `chefdata` filesystem, an
unreadable existing ext4 label, missing confirmation, and every failed device
preflight. It never runs at boot. Before provisioning, save the old raw
superblock and `dumpe2fs -h` outside the repository and complete the handoff's
review and phone loop proof. The command creates ext4 with label `chefdata`,
`/chef-layout` containing `chef-cyclo-data-v1`, and private versioned service
directories. The positive feature allowlist and dedicated
`/etc/chef/mke2fs.conf` avoid modern distribution feature defaults unsupported
by the phone's kernel; format and mount both disable discard.

Before services start, `/init` runs `chef-storage boot`. It checks the label
and marker with bounded read-only probes, runs `e2fsck -p` with a 60-second
TERM deadline and five-second KILL escalation, and accepts only exit 0 or 1.
It mounts `/data` with `noatime,nodiscard,commit=5,errors=remount-ro` and binds:

| Persistent directory | Service path |
|---|---|
| `/data/v1/bluetooth` | `/var/lib/bluetooth` |
| `/data/v1/networkmanager/system-connections` | `/run/NetworkManager/system-connections` |

Directories are root-owned and mode 0700; connection profile files are mode
0600. NetworkManager continues using its existing keyfile path. Do not put
pairing keys or Wi-Fi credentials in logs, Git, or shared evidence.
Missing, foreign or corrupt storage falls back to RAM and records its reason
in `/run/storage.log` and the kernel log, visible on the log screen.
`chef-storage status` shows the decision and mount status.

The five-second journal commit interval bounds ordinary metadata transaction
age without a constant eMMC write load. It does not guarantee that a recent
application write survives sudden power loss: applications must sync files
and their parent directories when durability matters. Ext4 journal replay
and the bounded boot check handle interrupted transactions; live power-loss
acceptance is recorded in the build log.

BusyBox init runs `chef-storage shutdown` before its global termination of
processes. The hook finds userdata mounts even if `/data` was detached, stops
filesystem users and syncs. A bounded filesystem-wide read-only remount must
succeed before any binds are detached; it protects surviving aliases too.
The hook then removes service binds and `/data`, and checks mountinfo before
claiming full unmount. Failed detaches leave read-only aliases and a log entry.
Consumer setup rollback uses the same read-only transition; if it fails, all
aliases must be detached or an explicit rollback failure is logged. Normal `reboot`,
`poweroff`, buttond and powerd take this path. Forced syscall/sysrq reboots
and the PMIC hard reset bypass it by design.

`scripts/phone-boot.sh` uses a detached shutdown/restart chain. Its manual
`shutdown --pause-init` mode sends init SIGTSTP to suppress respawning writers;
an exit/signal trap resumes init with SIGCONT on every exit, including errors, and a detached 120-second watchdog recovers a killed helper. The
bounded shutdown work stays below the watchdog budget, and the chain refuses
to restart after shutdown failure. `btprobe restart bootloader`
then performs the raw reboot. Use the default shutdown hook for orderly init
poweroff/reboot; sending SIGTSTP from that hook would stall init.

Reserved follow-up locations, still volatile until their consumers migrate:

| State | Planned directory |
|---|---|
| Rides | `/data/v1/rides` |
| Map tiles | `/data/v1/maps` |
| Sensor calibration | `/data/v1/sensors` |
| Power logs | `/data/v1/power` |
| Wall-RTC offset | `/data/v1/time` |
| Chrony drift | `/data/v1/chrony` |
| GNSS RAM shadow | `/data/v1/gnss` |

The root filesystem remains in the boot image's RAM disk in phase 2.
`system_a` is a separate phase 3 task. Recovery uses fastboot in the unchanged
bootloader, reached by holding VolDown through a Power-held reset. Flash a
known-good `boot_a` image with an explicit `_a` target; never use
`fastboot set_active`. See the [recovery guide](../device.md#stock-backups-and-recovery).
