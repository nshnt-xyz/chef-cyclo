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
