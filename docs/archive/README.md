# Completed implementation plans

[Project overview](../../README.md) · [Current features](../features/README.md) · [Next steps](../next-steps/README.md) · [Research](../research/README.md) · [Build log](../build-log.md)

Archived during the 2026-10-11 documentation review. These files preserve original designs, instructions, reviews and acceptance evidence. Their old assignments, approval procedures, image hashes and pending notes describe their original sessions; use feature guides and the build log for current operation. Outstanding work is tracked in next steps. Captured logs may still name the original `docs/next-steps/` paths; the same filenames now live here.

| Record | Outcome |
|---|---|
| [UI platform design](ui-platform.md), [handoff](ui-platform-handoff.md) | LVGL/fbdev + SDL platform accepted 2026-10-03; libinput removal completed 2026-10-04. |
| [Shared evdev helper](evdev-helper-handoff.md) | Implemented and live verified 2026-10-04. |
| [GNSS-only RF](gnss-only-rf-handoff.md) | Measured 2026-10-04; boot default retained, no RF helper needed. |
| [GPS manager](gps-manager-handoff.md) | Demand-driven leases and recovery implemented 2026-10-04. |
| [GPS time](gps-time-handoff.md) | chrony SHM source implemented and live verified 2026-10-04. |
| [Install layout](install-layout-handoff.md) | All three phases completed 2026-10-04/05; user cold-boot checks accepted 2026-10-06. |
| [Boot compression](boot-compression-handoff.md) | Experiment completed 2026-10-05; uncompressed image available as an opt-in, gzip retained. |
| [Shutdown hang](shutdown-hang-handoff.md) | Bluetooth UART use-after-free fixed in kernel #22, 105 shutdowns verified 2026-10-05. |
| [State persistence](state-persistence-handoff.md) | Crash, clock, power and bias persistence installed 2026-10-06; bias seed accepted live 2026-10-10. GNSS assistance remains separate. |
| [Persistence follow-up](state-persistence-followup-handoff.md) | Power log batching and bounded state shutdown installed 2026-10-08. |
| [Return to bootloader](reboot-bootloader-handoff.md) | State-saving helper verified 2026-10-10; installed 2026-10-11. |
| [SSH and install](ssh-and-install-handoff.md) | Key-only SSH and updated pair installed 2026-10-11, including the userdata mount-sweep fix. |
| [Build and tree cleanup](build-cleanup-handoff.md) | Standard interfaces step 1 done 2026-10-18: experiments off main, one build through make, one parallel `make test`, reproducible `wlan.ko`, `initramfs/` renamed `rootfs-overlay/`; RAM image live verified; installed as `8b8cddf0` + `41dca3ff`. |
| [buttond on the shared library](buttond-handoff.md) | Standard interfaces step 2 done 2026-10-18: `tools/buttond/`, the shared `tools/common/` (log, socket server, JSON reader), buttond and chefui on the JSON line protocol with written contracts; RAM image live verified; installed as `188b83e6` + `c5ed4d0e`. |
| [gps-manager and the clients](gps-clients-handoff.md) | Standard interfaces step 3 done 2026-10-19: `tools/gps-manager/` split and on the JSON line protocol with its contract, `tools/common/lineclient`, LVGL-free GPS and BLE clients, the settings reader contract; RAM image live verified; installed as `31f1aa31` + `1c1450bb`. |
| [sensord and blesensd](sensors-blesensd-handoff.md) | Standard interfaces step 4 done 2026-10-20: `sensord.c` and `blesensd.c` split, both daemons on `tools/common/` (sensord proto 2 with the envelope, blesensd proto 1 with a hello) with their written contracts; RAM image live verified; installed as `61048eae` + `087d3c7c`. |
| [/run/chef, screen handover, tzdata](run-chef-handoff.md) | Standard interfaces step 5 done 2026-10-20: our runtime files under `/run/chef/` (one C header, a guard test), the screen-off flag renamed `screen-off` and buttond without the `/proc` scan, tzdata with `Asia/Kolkata` and the POSIX fallback; RAM image live verified; installed as `77dd0a57` + `f5338097`. |
| [syslog](syslog-handoff.md) | Standard interfaces step 6 done 2026-10-21: busybox `syslogd` RAM ring with `logread`, `chef_log` and the shell helper on `/dev/log` with a kmsg fallback, third-party daemons logging there, the ring saved to `/data/v1/log` at every orderly shutdown; RAM image live verified; installed as `9a28e590` + `981d4f03`. |
| [OpenRC](openrc-handoff.md) | Standard interfaces step 7 done 2026-10-22: busybox init plus OpenRC 0.63.2, every service supervised by `supervise-daemon` with `need`/`after` dependencies instead of marker files, `gps-up` renamed `modem-up`, sensord its own service, the shutdown and `chef-reboot` paths through `openrc shutdown`; RAM image live verified; installed as `96e8c87f` + `2915db3c`. |
| [Daemon users](daemon-users-handoff.md) | Standard interfaces step 8 done 2026-10-23: buttond, powerd, sensord, blesensd, gps-manager and chef-settings as their own users with only the groups and capabilities they need, udev groups for their nodes, per-daemon runtime directories, polkit for NetworkManager, chronyd as `root:chrony` for the GPS SHM segment, `/usr/lib/chef/sys.sh`; RAM image live verified; installed as `7fb63760` + `6d7adb8d`. |
| [Standard hardware interfaces](hardware-interfaces-handoff.md) | Standard interfaces step 9 done 2026-10-24: kernel #24 `current_now` in the ABI sign, UPower as user `upower`, buttond's uinput key passthrough and force-feedback vibrator, `sensor-proxy` for `net.hadess.SensorProxy`, the powerd split; RAM image and guided checks live verified; installed as `e4bcd986` + `eac67a74`. |
| [Boot splash](boot-splash-handoff.md) | B01 done 2026-10-25: `chef-settings` shows the splash once per boot, its six steps following OpenRC's `started`/`failed` entries and `modem-up`'s new `/run/chef/modem-ready`, a 30 s cap and syslog step times; RAM image live verified; installed as `0e57a39d` + `89af1e83`. |
