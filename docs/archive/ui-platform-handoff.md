# UI platform: implementation handoff (2026-10-03)

**Archived 2026-10-11.** Completed and live verified on the date recorded below. Current behavior belongs in the linked feature guide; application follow-ups belong in [next steps](../next-steps/README.md). The original design and process instructions below are historical, not active assignments.

[UI platform spec](ui-platform.md)

Coordinator: herdr agent `ui_coord` (research, prototype, device tests, commits). Implementation: `ui_impl`. Review: `ui_review`. All three run in the same herdr workspace; address each other with `herdr agent prompt <name> "<text>"` (without `--wait`) and start every message with `[From: <agent-name>]`. Never poll or wait on another agent; each one reports when it is done.

## Completion update (2026-10-03)

Implementation, independent reviews, guided live acceptance, final package
verification and a fresh final-image boot are complete. This handoff's pause/resume sections below are
historical. Use the [feature guide](../features/ui-platform.md) for the current
contract and the [acceptance record](ui-platform.md#final-acceptance-record-2026-10-03)
for final results. User requested the combined implementation/docs/evidence
commit after completion. No partitions were flashed.

## Read first

1. [docs/next-steps/ui-platform.md](ui-platform.md): the specification and acceptance list. It is the contract.
2. [docs/research/lvgl-fbdev-prototype.md](../research/lvgl-fbdev-prototype.md): measured findings and hazards behind every decision.
3. [docs/features/display-and-touch.md](../features/display-and-touch.md#framebuffer-and-touch-contract) and `tools/fbdev.h`: the binding framebuffer contract.
4. Prototype source `logs/lvgl-proto-2026-10-03-lvproto.c` and config delta `logs/lvgl-proto-2026-10-03-lv_conf-delta.txt`: working reference for the shadow copy, pan flip, buttond claim and poll loop. Its touch handling uses `lv_evdev`, which the spec replaces.
5. Existing patterns: `tools/sensord/` (component Makefile and tests), `tools/tests/test_fblog.c` and `tools/tests/test_buttond.c` (syscall mocking, fake sockets), `tools/fblog/fblog.c`, `tools/buttond.c`, and `scripts/mkinitramfs.sh` (how shipped binaries are built).

## Implementation (`ui_impl`)

- Add LVGL as a submodule at `third_party/lvgl` pinned to tag `v9.6.0` (`https://github.com/lvgl/lvgl.git`; cloning is slow, allow several minutes). The release tarball SHA256 is in `logs/lvgl-proto-2026-10-03-hashes.txt` if you need a cross-check.
- Build everything in the spec: `tools/chefui/` (library, `chefui-demo`, Makefile with device/host/test targets, tests), the fblog flag-on-resume change and its test, `scripts/mkinitramfs.sh` packaging of `/usr/bin/chefui-demo`, and doc updates for the `/run/fblog.off` meaning.
- Cross toolchain: `toolchain/aarch64-musl/bin/aarch64-buildroot-linux-musl-gcc`, static. LVGL warnings may be silenced for LVGL's own sources only.
- SDL2: the user is installing `libsdl2-dev`. If it is still missing when you reach the host backend, implement it anyway, keep unit tests independent of SDL, and say so in your report.
- Verify: `make -C tools/chefui test`, the device cross build, `make -C tools test` and the other component suites you touch. Report the stripped `chefui-demo` size.
- When complete, prompt `ui_review` with a summary of what changed and which tests ran. Fix its findings and re-request review until it approves. Then report to `ui_coord`: files changed, test results, binary size, deviations from the spec with reasons, and anything that needs live verification.

## Review (`ui_review`)

- Until `ui_impl` asks for a review, study the spec, the prototype findings and the contract. Do not edit files.
- Review against the spec and the contract. Concentrate on:
  - never reading the fb mapping, and never `FBIOPUT_VSCREENINFO`
  - POWERDOWN before close on every exit path
  - lock handling, including handoff inheritance with no unlocked gap
  - the flag ordering and the fblog resume change
  - touch slot persistence, frame-by-frame delivery, `SYN_DROPPED` resync
  - buttond reconnect
  - main-loop wakeups on a static page
  - host/device parity
  - test coverage of each spec bullet
- Build and run all tests yourself. Send concrete findings (file:line, failure scenario) to `ui_impl`. When nothing blocking remains, report your verdict and any residual risks to `ui_coord`.

## Rules for both

- Host only: no phone or USB network access, no fastboot, no flashing.
- No kernel or DT changes, no commits, no pushes.
- Leave `out/` baseline artifacts (`out/kernel`, `out/boot.img`, `out/initramfs.cpio.gz`) unchanged; build the initramfs only into a separate output if you need to check packaging, and say how.
- Do not run `herdr server stop` or close panes you did not create.

## Status at pause (2026-10-03, ~22:00 IST)

Work stopped at the user's request, to resume later. This section merges the coordinator state with the two agent handoffs. The full agent handoffs are in `out/handoff/ui_impl.md` and `out/handoff/ui_review.md`, with the reviewer's round-1 findings, checklist and baseline hashes in `out/handoff/ui_review-files/`. `out/` is gitignored, so a backup copy is in `~/chef-cyclo-evidence/lvgl-proto-20261003T134429Z/handoff/`. Read those before resuming either role.

### Where each role stopped

| Role | State |
| --- | --- |
| `ui_coord` | Research, prototype and spec done. Live acceptance **not started**. Nothing committed; the user decided docs are committed together with the implementation. Kernel `64fa801` is pushed. |
| `ui_impl` | Host implementation complete. Round-1 review findings 1–8 fixed and tested. Last edit: fblog idle wait moved to `ppoll` with signals blocked (finding 7 race). One optional test is still to write (SIGTERM during fblog idle returns promptly). The full `make -C tools test` has not been rerun since that edit. Has **not** re-requested review. |
| `ui_review` | Round 1 done and sent. Waiting for `ui_impl`'s re-request. Has not reviewed the fixes. No verdict sent. |

### Implementation summary (uncommitted, from `ui_impl`)

- **`tools/chefui/`** (new):
  - public API `chefui.h`
  - pure host-tested modules `copy`, `plan`, `touch`, `buttons`, `fbscreen`
  - glue `chefui.c`, `backend_fb.c`, `backend_sdl.c`
  - `lv_conf.h`, with `LV_ASSERT_HANDLER __builtin_abort();`
  - `demo.c` (`chefui-demo`)
  - a `Makefile` with `device`/`host`/`test` targets building into `tools/chefui/build/`
  - tests: copy 58, plan 34, touch 71, buttons 47, fbscreen 134, chefui 124 (end-to-end with headless LVGL)
  - Stripped device demo is about 896 KB.
- **`third_party/lvgl`:** v9.6.0 at `80ca777e…`. The submodule is staged, with a blobless sparse local checkout (GitHub throttled full clones); its sources were verified identical to the release tarball.
- **fblog:** after a borrow, it checks `/run/fblog.off` before resuming. While idle, it waits on inotify of the flag's directory, falling back to a 250 ms recheck, and now uses `ppoll`. New fblog tests pass, 2083/2083.
- **`fbdev.h`:** adds `fb_pan()` and documents `CHEFUI_LOCK_FD` and the screen-off flag.
- **Packaging:**
  - LZMA is the default ramdisk (`out/initramfs.cpio.lzma`); `GZIP=1` gives the old gzip output.
  - The build checks for `CONFIG_RD_LZMA=y`, and `mkboot.sh` follows the choice.
  - `chefui-demo` is installed as `/usr/bin/chefui-demo` and is not in inittab.
  - Last LZMA build: 32,936,035 bytes with a loader margin of 14,512,128 bytes. That build contains the **pre-fix** demo and must be rebuilt.
- **Ride image removed:**
  - deleted: `initramfs-ride/`, `scripts/mkride.sh`, the ride-logger test, and `docs/features/ride-logging.md` (deleted outright, per the user)
  - current-behavior docs reworded
  - intentional survivors (history only) are listed in `out/handoff/ui_impl.md`
- **Deviations from the spec**, numbered as in the `ui_impl` handoff:
  1. Start-dark sizing comes from sysfs `modes`, validated at first screen-on.
  2. Idle fblog wakes on inotify, not by polling.
  3. LZMA default ramdisk.
  4. Animations are paused, best effort, while the screen is off.
  5. chefui re-reads input itself, because LVGL ignores `continue_reading` in event mode.
  6. Gesture data is only fed after the first contact.
  7. Touch release inside LVGL's input handling uses `lv_indev_reset`.
  8. A failed pan marks both pages stale.
  9. Handoff drops buttond claims until the peer claims; a power press in that window uses buttond's default.
  10. Brightness commits by panning to the front page (needs live check).
  11. A missing buttond adds 1 wakeup/s while reconnecting.
  12. The host build logs to stderr only; host key and gesture paths have no automated tests.
  13. `LV_COLOR_FORMAT_DEFAULT XRGB8888` is used instead of the deprecated `LV_COLOR_DEPTH 32`.

  Full reasons are in `out/handoff/ui_impl.md`.

### Protected artifacts

`out/boot.img` (`18687a12…`), `out/initramfs.cpio.gz` (`fa051784…`) and `out/kernel` are the verified baseline and were not modified. The reviewer's hash check is in `out/handoff/ui_review.md`.

**Hazard:** `mkboot.sh` still defaults `OUT=out/boot.img`. When packing the LZMA test image, always pass a different name, e.g. `RAMDISK=out/initramfs.cpio.lzma OUT=out/boot-chefui.img sh scripts/mkboot.sh`.

The old ride outputs (`out/boot-ride.img`, `out/initramfs-ride.cpio.gz`, `out/initramfs-root-ride`) are still on disk, gitignored; deleting them is the user's call. `scripts/__pycache__/` is a stray test artifact and safe to delete.

### Pending user decisions

- **Log screen on chefui:** proposed as `chefui-log`, the first app after this phase. It needs a new chefui "background" mode (draws only while nobody holds the screen; same lock-shared protocol as fblog). Keep the current fblog as an automatic fallback (a wrapper drops back to it if `chefui-log` crashes repeatedly at startup) until it has proven itself, then delete fblog. Not yet answered.

### Queued after this phase

1. ~~Remove `libinput` and `libinput-tools` from the rootfs~~ (user-approved 2026-10-03). **Done 2026-10-04** and live-booted, see the [build log](../build-log.md). See [ui-platform.md](ui-platform.md#out-of-scope-this-phase).
2. `chefui-log`, if the user approves it.

### Resume steps

1. **Agents.** If `ui_impl` and `ui_review` are still alive in herdr (`herdr agent list`), prompt `ui_impl` to continue from its "First steps to resume". Otherwise start fresh `codex` agents with those names in sibling panes, and point each at this file plus its own `out/handoff/<name>.md`.
2. **Implementation finish:** `ui_impl` runs `make -C tools test`, `make -C tools/chefui clean && make -C tools/chefui -j$(nproc) test device host` and `python3 scripts/tests/test_chef_loader_budget.py`. It then rebuilds `out/initramfs.cpio.lzma`, rechecks the loader margin, and re-requests review. `ui_review` re-reviews findings 1–4, checks 5–8, reruns all tests and the baseline hash diff, then reports its verdict to `ui_coord`.
3. **Coordinator.** After approval:
   - pack `out/boot-chefui.img` (never over `out/boot.img`)
   - ask the user once before the reboot into fastboot
   - `fastboot boot` the image
   - verify LZMA boot: readiness markers, WLAN, fblog, boot time
   - run the spec's [live acceptance](ui-platform.md#live-acceptance-coordinator-with-the-user-present) with the user present, using the guided-run format
4. **Commit.** After acceptance passes, commit the docs, evidence, implementation and submodule together (user decision), with evidence logs under `logs/`. Then move the platform's operating contract into a feature guide.

Phone state at pause: baseline kernel #20 running from a temporary boot. `/tmp/lvproto` and its logs are in RAM only and are lost at reboot; their copies are already under `logs/lvgl-proto-2026-10-03-*`. The evidence and driver scripts (`remote.py`, HTTP push from host 172.16.42.3:8765) are in `~/chef-cyclo-evidence/lvgl-proto-20261003T134429Z/`.
