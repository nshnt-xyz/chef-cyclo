# Intermittent shutdown hang (handoff)

[Next steps index](README.md) · [Boot compression handoff](boot-compression-handoff.md) · [Install layout handoff](install-layout-handoff.md) · [Live testing](../live-testing.md)

**Status:** started 2026-10-05 at the user's request. Handed to Herdr agents
`rootfs_impl` (implementation, live runs) and `rootfs_review` (review),
coordinated by `rootfs_research`. **Resolved the same day** (see
[result](#result-rootfs_impl-2026-10-05)): a use-after-free in the Bluetooth
UART driver (`hci_qca`), fixed in the kernel (#22) and installed as the pair
`system_a` `e72a3039` + `boot_a` `81a5c5ef`.

## Symptom (from the boot compression run, 2026-10-05)

3 of 15 ordinary `reboot`s hung, on both the gzip `5c509eb2` and the
uncompressed `df474390` boot image, so it is not the packaging. Raw data:
`~/chef-cyclo-evidence/boot-compression-20261005/hang-summary.txt` and
`step3s-reboot3*`, the summary in
[`logs/boot-compression-2026-10-05.txt`](../../logs/boot-compression-2026-10-05.txt).

- Next boot reports `androidboot.bootreason=kernel_panic`,
  `powerup_reason 0x00020000`, PON "Hard Reset and 'warm' boot",
  power-off reason PS_HOLD. Normal reboots show a cold PON.
- Host ping (every 0.2 s) stops about 3.0 s after the request instead
  of 5.1 s, so the **whole kernel** stops within a few hundred ms after
  the last `::shutdown` action (`umount -a -r`, the `/firmware` p25
  remount line). The phone is back after 32 to 49 s instead of about
  21 s: roughly 11 to 15 s extra, which fits a watchdog bark/bite
  (hard lockup or bus stall), not a panic (`PANIC_TIMEOUT=5` would add
  only about 5 s).
- `/data` was fully detached and read-only every time before the hang.
- Hangs happened at 35 to 58 s uptime; 5 reboots at 36 to 37 s and 2 at
  180 s passed on the gzip image. About 143 earlier reboots in the
  evidence (mostly longer uptimes, before the phase 3 read-only root)
  never showed it.
- State at the request was the same on hung and normal short reboots:
  modem `ONLINE` with `restart_level` `RELATED`, crash counts 0;
  `adsp`, `venus`, `a512_zap` `OFFLINING` (never loaded); `gps-manager`
  idle; `gps-up` still answering modem TFTP requests (`mcfg.tmp`
  timeouts) at short uptimes.

## Facts checked (rootfs_research, 2026-10-05)

- **The kmsg stream cannot see the kill phase.** busybox init, after the
  `::shutdown` actions, sends SIGTERM to every process, syncs, waits 1 s,
  sends SIGKILL, syncs and calls `reboot(2)`; its own messages go to the
  console, which is `console=null`. The streamed `cat /dev/kmsg` runs
  under `telnetd`, so it dies in that SIGTERM. "Same last line on every
  reboot" says only that the hang is at or after the kill phase; ping is
  the only signal that outlives it.
- **No netconsole:** `NETCONSOLE` and `NETPOLL` are off, and enabling
  them changes the core kernel (and the `wlan.ko`/stamp pairing). Not in
  this task unless everything else fails, and then only after asking.
- **pstore:** ramoops is configured (`mem_address` 0xaf000000, 768 KiB,
  `console_size` 256 KiB, `record_size` 128 KiB, `dump_oops=1`) but
  `rootfs_impl` found nothing retained across even a normal warm reboot.
  Check once whether the region is wiped by the bootloader (for example a
  `pmsg` write, then a warm `reboot`); if it cannot hold data across a
  reset, drop it.
- busybox has `nc`, `ping`, `timeout`, `pkill`. The msm watchdog is
  initialised (`17817000.qcom,wdt`).

## Plan

### A. Locate it without changing any image

The inittab lives on read-only `system_a`, but a test can still run the
real shutdown sequence with markers from `/run`:

1. Stop init from respawning the services the tracer stops, without
   killing anything. **Do not delete inittab lines:** our busybox (Alpine
   1.37.0) has `FEATURE_KILL_REMOVED=y`, so a `kill -HUP 1` reload
   SIGTERMs every running entry missing from the new inittab (telnetd,
   udhcpd and buttond included, leaving only a hard reset), and an
   unreadable override makes init fall back to its built-in inittab,
   which drops `chef-storage shutdown`. Either bind-mount a copy over
   `/etc/inittab` in which only the action of the tracer's targets
   changes from `respawn` to `once` with byte-identical command text
   (busybox matches entries by command, so the running pid is kept; the
   `telnetd`/`udhcpd` entries stay `respawn` and the `::shutdown` lines
   stay identical), or pause init with `kill -TSTP 1` and a bounded
   `kill -CONT 1` watchdog. Confirm with `ps` that nothing was killed or
   restarted. (Found by `rootfs_review`.)
2. A tracer script in `/run` reproduces init's order exactly:
   `chef-storage shutdown`, `umount -a -r`, then the kill phase, but as
   steps: one service (or a small group) at a time, SIGTERM, bounded
   wait, SIGKILL if needed; then `sync` and `reboot -f`. Before and after
   every step send a marker to the host over UDP (`nc -u` to the USB
   host address; also `/dev/kmsg`), and keep `telnetd`, `udhcpd` and the
   marker path until last. The host records markers with
   `CLOCK_MONOTONIC` and pings every 50 ms. The last marker before ping
   stops names the step.
3. Run it at the risky uptime (about 35 to 40 s after boot) and at a
   few minutes, enough runs to see several hangs (about 1 in 5 so far).
   Also try the order that mirrors init's (everything at once) to prove
   the tracer itself still reproduces the hang; if the hang vanishes
   under the stepwise order, that is a finding too (a race between
   daemons dying together).
4. Candidates to watch, all with hardware release paths: `rmtfs`,
   `qmuxd-lite`, `tftp`/`gps-up`, `servreg-locator` while the modem is
   `ONLINE` (Qualcomm Android keeps `rmt_storage` alive through shutdown
   as `shutdown critical`), `wpa_supplicant`/NetworkManager taking
   `wlan0` down, `btattach` closing the BT UART, `fblog` releasing
   `/dev/fb0`, `buttond`/`powerd`, `sensord`/audio if started, then
   `reboot(2)` itself (kernel device shutdown, PIL modem stop).

### B. Fix and prove

Fix at the source once the step is known (for example an orderly stop of
the modem stack or Wi-Fi/BT before the global kill, as a `::shutdown`
step with bounded waits and kmsg lines), keeping `chef-storage shutdown`
first and its data guarantees unchanged. Host tests in the existing
style. Because the stamp covers the root tree, a change on `system_a`
means a new `system_a` image plus its matching stage-1 `boot_a` image:
use the phase 3 guarded procedure (`current-slot` a, hashes, `fastboot
boot` first, `rootfs_review` clearance of each exact hash, read-back).
Acceptance: at least 30 ordinary reboots at the risky uptime plus some
at longer uptimes with no `kernel_panic` bootreason (at the observed
rate, 30 clean runs by chance is about 0.1 %), the poweroff path
(`buttond`/`powerd` `poweroff`) checked a few times, and the usual
regression.

## Rules

- Hangs recover by themselves through the watchdog in under a minute,
  so no user action is expected; if the phone is gone from USB and
  fastboot for more than 2 minutes, stop and report to `rootfs_research`.
- The user is available; ask through `rootfs_research` before anything
  needing them (hard reset, unplug, cold boot).
- Kernel unchanged. `/data` handling unchanged unless the evidence points
  there.
- No writes to `persist`, EFS or anything outside `system_a`, `boot_a`,
  `userdata`. Test images under other names; `out/boot.img` promotion is
  the coordinator's call.
- Raw evidence in `~/chef-cyclo-evidence/shutdown-hang-20261005/`
  (private), a credential-free summary in `logs/`, build log entry, docs
  (`live-testing.md` gotcha, the boot compression handoff's open item).
  Stage the diff, do not commit; report to `rootfs_research` with the
  staged diff SHA-256.

## Decision (user, 2026-10-05): kernel fix

Root cause located by `rootfs_impl` on run t33 and confirmed by
`rootfs_review` from the pstore oops: a use-after-free in
`drivers/bluetooth/hci_qca.c` `qca_close()` (non-sync `del_timer` before
`destroy_workqueue`, whose `qca_wq_awake_device` re-arms
`wake_retrans_timer`; the timer then fires on freed `qca`), followed by a
wedged panic restart and a watchdog bite. The user approved **fixing the
kernel** (option A); this lifts the "kernel unchanged" rule for this fix
only.

- Fix `qca_close()` so no qca timer or work survives it on 4.4 (no
  `timer_shutdown_sync`): `del_timer_sync` both timers,
  `destroy_workqueue`, `del_timer_sync` both again, and make sure the
  `tx_idle` callback cannot queue onto the destroyed workqueue. Check the
  driver's other teardown paths. Minimal patch in the kernel tree's
  existing local-patch style, with the reasoning in the commit comment.
- Rebuild with `scripts/mkinstall.sh` (kernel, `wlan.ko`, `system_a`,
  stage-1, full RAM image, matching stamp). Check `wlan.ko` vermagic and
  `Module.symvers` against the new kernel.
- Proof is a deterministic reproducer, not reboot counting: BT idle more
  than 2 s (IBS TX asleep), queue HCI traffic, kill `btattach` within a
  few ms. Show it hits on the current kernel (each hit is a watchdog
  reset, recovered in under a minute), then never with the fix over
  enough tries; `rootfs_review` clears the exact script first. Keep 30+
  ordinary reboots plus a few `poweroff`s as the regression.
- Install with the phase 3 guarded procedure (`fastboot boot` first,
  then `system_a` + `boot_a` as a matching pair, read-back). If the new
  pair fails, the fallback is the current pair (`9c47c6b8` + `5c509eb2`)
  or the full RAM image `out/boot.img` `df856fc3`.
- Answer in the write-up why earlier reboots never showed it (for
  example HCI traffic `bluetoothd` sends at SIGTERM since phase 2 made
  its state persistent).
- Additions from `rootfs_review`: (1) **stamp interlock**: once a new
  `system_a` is flashed, the installed stage-1 `5c509eb2` refuses it and
  any reboot lands in rescue. So prove the fix first with **no flash**,
  by `fastboot boot` of the full RAM image built from the fixed kernel
  (never reads `system_a`), against the same reproducer on the current
  kernel. Then flash `system_a`, `fastboot boot` the new stage-1, and
  flash `boot_a`, with no ordinary reboot between the two flashes; if the
  new stage-1 fails, flash `system_a` `9c47c6b8` back (pairs with
  `5c509eb2`) or `boot_a` `df856fc3`. (2) Every reproducer try requires
  `/data` remounted read-only (checked in mountinfo) and refuses to start
  before `abslot` has marked the boot, so a hit cannot damage `/data` or
  spend slot retries.

## Result (rootfs_impl, 2026-10-05)

Raw evidence: `~/chef-cyclo-evidence/shutdown-hang-20261005/` (private);
summary in [logs](../../logs/shutdown-hang-2026-10-05.txt).

**Plan A, tracing.** busybox init is paused with `kill -TSTP 1` (not an
inittab reload, see A1); a tracer in `/run` replays init's shutdown with UDP
markers acked by the host, a `/dev/kmsg` forwarder and a 50 ms heartbeat, and
the host pings every 50 ms. At SIGTERM two teardowns always race: `gps-up`
closes `/dev/subsys_modem`, so the modem stops and takes the WLAN firmware (a
PD in the modem) down while `wpa_supplicant` disconnects; and `fblog`'s
framebuffer release sends a backlight DCS after the panel is off (DSI command
DMA timeout on every shutdown, a `clk_branch_wait` WARN in 2 of 15). Both are
harmless: neither was involved in the hang. The tracer and plain init
reboots then ran clean 32 times in a row (0 of 32 against 3 of 15 in the
morning, Fisher p about 0.03); the USB port type (CDP then, SDP now) was a
false lead.

**Found.** Run t33, an ordinary reboot at 200.7 s uptime, hung, and pstore
held the oops on the first boot after it (it keeps nothing over a normal,
cold-PON reboot, which is why earlier checks found it empty): a NULL pointer
dereference at 0x40 in softirq, PC `_raw_spin_lock_irqsave`, LR
`hci_ibs_wake_retrans_timeout`, about 100 ms after init's SIGTERM, then
"Kernel panic - not syncing: Fatal exception in interrupt", a panic restart
that wedged, and "Causing a watchdog bite!" 10 s later. That is the 32 to 49 s
return and the warm PON with `bootreason=kernel_panic`.

**Cause.** `drivers/bluetooth/hci_qca.c` `qca_close()` stopped the IBS timers
with a plain `del_timer()` and then destroyed its workqueue. Draining the
workqueue runs a pending `qca_wq_awake_device()`, which sends WAKE and
re-arms `wake_retrans_timer` for 100 ms; `qca_close()` then frees
`qca_data` and `hci_uart_tty_close()` frees `hu`, and the timer fires on the
freed data. btattach is closed by init's SIGTERM, so every shutdown ran the
race. The ordering is upstream's (mainline fixed it much later with
`timer_shutdown_sync()`); our IBS clock-vote patch only made the work longer.
A second gap next to it: the drained work calls `hci_uart_tx_wakeup()`, which
schedules `hu->write_work` after `hci_uart_tty_close()` cancelled it.

**Fix.** Kernel commit `8ef120353` ("Bluetooth: hci_qca: stop timers and
drain work before freeing qca", submodule branch
`chef-cyclo-qpts30.61-18-10`): `del_timer_sync()` on both timers,
`flush_workqueue()`, `del_timer_sync()` on both again, `destroy_workqueue()`,
then purge the queues; and `cancel_work_sync(&hu->write_work)` once more in
`hci_uart_tty_close()` after `proto->close()`, outside `proto_lock`. The user
lifted the kernel-unchanged rule for this fix. `Module.symvers` is unchanged,
so `wlan.ko` keeps its vermagic and CRCs.

**Not reproducible on demand.** A reproducer that kills btattach (and
bluetoothd) with IBS TX asleep, in five variants (together, with HCI traffic
0 to 10 ms before, btattach alone, chef-storage's order, and with `hci0`
left up), ran 60 tries on the old kernel without a hit. With bluetoothd
alive it powers `hci0` down within 2 s of SIGTERM, and with `hci0` up the
close completed cleanly. So the proof is the t33 oops plus the source, and
acceptance is a reboot count. On the fixed kernel (full RAM image
`42f362b0`, `fastboot boot`) the same reproducer ran 40 tries as a
functional check of the new teardown: 40 of 40 survived, BT re-attached each
time, and no WARN, BUG, sleep-in-atomic or IBS timeout lines.

**Why the earlier reboots never showed it.** Not established. The close only
crashes when a wake is queued while btattach's line discipline closes with
IBS TX asleep, which depends on what BlueZ is doing at that instant. Since
phase 2 `chef-storage shutdown` stops bluetoothd (it holds `/data` binds) about
2 s before init's SIGTERM reaches btattach, which is about the IBS idle
timeout (2 s), so the transmit side tends to go to sleep just as btattach
closes; before phase 2 both died in the same SIGTERM. Most of the earlier 143
reboots were also at long uptimes. The hangs since then came at 35 to 58 s
and at 200 s uptime.

**Install.** Built with `SKIP_KERNEL=1 scripts/mkinstall.sh` after the kernel
build: kernel `0cf2981c`, `system_a` `e72a3039` (stamp `13f336e5`),
`stage1.cpio.gz` `00086082`, `boot-stage1.img` `81a5c5ef`, `boot-ram.img`
`42f362b0`. Proven first by `fastboot boot` of the full RAM image (no
flash), then the phase 3 order: `fastboot flash system_a`, `fastboot boot`
of the new stage-1 (stamp, read-back), `fastboot flash boot_a`, reboot; both
partitions read back exactly and `abslot` marked the slot. Fallbacks: the old
pair `9c47c6b8` + `5c509eb2` (kept in `out/pre-btfix/`) or `out/boot.img`
`df856fc3`.

**Acceptance.** 105 shutdowns on the installed pair (2026-10-05 18:10 to 20:26Z) without a hang: 100 ordinary reboots (79 at 36.2 to 40.5 s uptime, 1 at 121.6 s, 20 at 180.1 to 180.6 s), all `bootreason=reboot` with a cold PON, and 5 `poweroff`s at 36 to 40 s that came back in charger mode (USB attached) with a cold PON. Ping gap 15.75 to 19.05 s, no pstore records; the shutdown kmsg streams carry no oops (only the known `fblog` `clk_branch_wait` WARN, 5 of 105). At the pooled pre-fix rate (about 4 in 65), 0 in 100 by chance is about 0.2 %. Final getvar: slot `a` current, successful, retry 6; `b` unbootable.

**Residuals, not fixed.** (a) `hci_uart_write_work()` updates `hdev->stat`
inside its send loop and `hci_free_dev()` runs before `proto->close()`, so a
frame dequeued in that gap (or a partial `hu->tx_skb`) can still touch the
freed `hdev`; mainline closed that with `HCI_UART_PROTO_READY` gating. (b) A
partial `hu->tx_skb` left at that point is not freed. (c) The panic restart
itself wedges until the watchdog bites (about 10 s); a later panic will
again take 30 to 50 s to come back.

