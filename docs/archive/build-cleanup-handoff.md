# Build and tree cleanup (standard interfaces step 1)

User request 2026-10-18 ("Start step 1"). The scope is section 0 of
[standard interfaces](../next-steps/standard-interfaces.md#0-build-and-tree-cleanup);
read it first, this file adds the decisions and the working rules.
No behavior change on the phone.

Roles: `cleanup_impl` (Claude Opus) implements, `cleanup_review` (Claude
Opus) reviews, `cleanup_coord` coordinates, decides and talks to the
user. Report with `herdr agent prompt cleanup_coord "[From: cleanup_impl] ..."`
(or `cleanup_review`). Send a message when a milestone is ready for
review, when review is done, when blocked, and at the end. Do not poll or
wait on each other's panes; the other side reports back.

No em dashes anywhere (code, comments, docs, commit messages, messages).

Evidence dir: `~/chef-cyclo-evidence/build-cleanup-20261018/` (create it).
Review diffs go to `review/<milestone>.diff` there; the reviewer
snapshots what it reviewed (sha256 in its note) and checks the commit
against that snapshot.

## Working rules

- Work on `main` in this checkout. Milestones in the order below, each
  reviewed before the next. One commit per milestone after the
  reviewer's explicit approval. Do not push. Do not touch the
  `experiment/*` or `ui` branches.
- No phone access. No `fastboot`, `phone.py`, `phone-boot.sh`, telnet or
  ssh to the phone: the coordinator runs the live check with the user.
- Match the surrounding code's style and comment density. Keep the
  explanatory comments that `scripts/mkinitramfs.sh` carries for each
  binary: move them next to the matching Makefile rule, do not drop them.
- Ask `cleanup_coord` before deciding anything this file and section 0
  do not settle. Do not guess at doc targets for dangling references.

## Baseline (before M1)

Build the image from the current `main` (`6209e0f`) with the normal
build (`docs/building.md`) and save in the evidence dir:
`baseline-files.txt` (every path in `out/initramfs-root` with type,
mode and size, sorted), `baseline-sha256.txt` (sha256 of every regular
file), and the `make test` result of every suite with its wall time.
Every later milestone is compared against this.

## M1. Remove the experiments

Both are preserved on local branches created at `08e1ec9`
(`experiment/speaker-protection`, `experiment/drm`); remove from main:

- Speaker protection: `tools/afe-topology-cal.c`, `tools/tert-tx-hold.c`,
  `tools/acdb-afe-topology.py`, `initramfs/usr/bin/afe-debug`,
  `initramfs/usr/bin/spk-protect-probe`, their tests
  (`test_afe_topology_cal.c`, `test_tert_tx_hold.c`, `test_afe-debug.sh`,
  `test_spk-protect-probe.sh`, `test_acdb-afe-topology.sh`), their
  `tools/Makefile` rules, `.gitignore` lines, `mkinitramfs.sh` build and
  chmod lines, and any header or shim used only by them (check with grep
  before deleting a shared one).
- **Keep** `tas2560-send-cal`, `wavtone`, `audio-up`, `speaker-test-tone`:
  production (speaker test tone).
- DRM: `scripts/build-drm-experiment.sh`,
  `scripts/build-drm-diagnostic-initramfs.sh`,
  `kernel-config/drm-helper-namespace.patch`. The kernel branch keeps its
  DRM helper namespacing commit (`64fa801`); do not touch `kernel/`.
- Docs that describe these as present (`docs/features/audio.md`,
  `docs/building.md`, `docs/repository-layout.md`, `docs/build-log.md`
  where it is a current description, not history): say they live on the
  experiment branch. `docs/research/`, `docs/archive/` and `logs/` stay
  as they are.

Acceptance: staged image file list equals the baseline minus exactly the
removed files; all suites pass.

## M2. Dangling references and repository layout

28 references point to missing docs (grep outside `docs/archive`,
`docs/research`, `logs`). Targets, decided:

- `sensors-plan.md` (with or without `docs/next-steps/`): it was renamed to
  `docs/research/sensors.md` in `c06a8a1`, so research and section
  pointers (sections 7 and 8) go there with the numbers kept; otherwise use
  `docs/next-steps/sensors.md` when the text is about plans, and
  `docs/features/sensors.md` when it describes how the shipped code works.
- `battery-and-charging-plan.md`: `docs/features/battery-and-charging.md`.
- `gps-userspace-handoff.md`: `docs/features/gps.md` (and
  `docs/building.md` for the toolchain provisioning ones, like the
  "3.1" pointer in `tools/Makefile`). Where a pointer cites a section
  number that has no successor, rewrite the sentence to stand alone.
- `tmp/tftp-review-round1.md`: drop the pointer, keep the fact it
  supports if one is stated.

If a reference fits none of these, list it in the review note and ask.
Added 2026-10-18: pointers to old README entries (now in
`docs/build-log.md`) are fixed in M2 too: dated entries to the build-log
entry with the same title, "README GPS section" to
`docs/features/gps.md`, "README next-steps item N" loses the number and
keeps the fact. `REPORT.md` pointers name local evidence, not repo docs,
and stay.
Then bring `docs/repository-layout.md` up to date with the tree (blesensd,
gps-manager, the settings app, abslot, rtc-edge, wifi-psk, wlan-fw,
`stage1/`, chef-state, chef-storage, chef-reboot, and anything else
missing). Acceptance: a grep shows zero dangling references.

## M3. One build, one `make test`

Decisions:

- **Each component Makefile owns its phone build**: `tools/`,
  `tools/rmtfs`, `tools/servreg-locator`, `tools/tftp`, `tools/sensord`,
  `tools/blesensd` get a `device` target like `tools/chefui` has
  (`DEVICE_CC ?=` the musl gcc under `toolchain/aarch64-musl`). Every
  binary `mkinitramfs.sh` compiles today moves into one of them,
  including `wifi-psk`, `wlan-fw`, `irsc`, `qmuxd-lite`, `btprobe`
  (keeps its Android GCC freestanding flags, as a `BTCC ?=` variable in
  `tools/Makefile`) and the chefui pair (already there).
- **Device flags**: one set for every musl binary,
  `-std=gnu11 -Wall -Wextra -Werror -O2 -static`, plus what each binary
  needs (`$KHDR`, `-I` paths, `-DCHEF_BASU`, libraries). No `-Wno-cpp`.
  Stripping stays as it is today (only chefui strips); no new
  optimization or linker flags. If a binary does not build clean under
  these flags, stop and report the warning; do not silence it.
- **Host flags**: `-Werror` in the host build too, unless a test
  harness genuinely cannot meet it (report which and why).
- **Output out of the source tree**: host objects, host binaries and
  test binaries go to `<component>/build/host/`, device binaries to
  `<component>/build/device/` (the chefui layout). `.gitignore` becomes
  one `build/` pattern per component instead of the per-binary list.
  Update every test script and doc that runs a binary by its old path.
- **Install target**: each component gets
  `install-device DESTDIR=...` that installs its device binaries to
  `$(DESTDIR)/usr/bin` with mode 755. A new top-level `Makefile` at the
  repo root has `device`, `install-device`, `test` and `clean`, each
  delegating to every component. `mkinitramfs.sh` reduces to
  `make -j"$(nproc)" device` and `make install-device DESTDIR="$ROOT"`.
- **Stays in `mkinitramfs.sh`**: everything that is not compiling our
  code: the overlay copy and modes, SSH keys, firmware and seed
  extraction from `stock/`, the rootfs checks (gpsd, tinymix/tinyplay
  present), the sensord registry map, image info, the key scan and the
  cpio. The "missing required source" loops go (make reports a missing
  prerequisite); the LVGL and basu checks become Makefile checks like
  chefui's `check-lvgl`.
- **`make test` at the root** runs all 8 suites in parallel (`tools`,
  `tools/rmtfs`, `tools/servreg-locator`, `tools/tftp`, `tools/sensord`,
  `tools/blesensd`, `tools/chefui`, `scripts/tests`), each suite's
  output to `build/test-logs/<suite>.log` at the repo root, then a
  one-line pass or fail per suite with its wall time and a non-zero exit
  if any failed. Reducing the shell tests' fixed `sleep`s is out of
  scope; note the slowest suites in the log.

Added after the baseline (coordinator, 2026-10-18):

- **Reproducible `wlan.ko`**: `scripts/build-wifi.sh` exported the pinned
  trees into a `mktemp` directory whose path landed in 78 `__FILE__`
  strings, so the module differed on every build. It exports into the
  fixed `$CHEF_ROOT/out/wifi-build` instead (removed and recreated each
  run, still removed on exit, still `git archive` only). The Android
  GCC 4.9 has no `-fmacro-prefix-map`. qcacld's Kbuild also stamps
  `date -u` into the driver version string, so `build-wifi.sh` passes
  `TIMESTAMP` as the pinned qcacld commit's committer date (UTC), the
  `SOURCE_DATE_EPOCH` idea. Two consecutive builds must give the same
  sha256.
- **Flakes under parallel load** (`tools/tests/test_chef_reboot.py`
  `test_term_and_int_resume_init`, chefui `test_sys.c:907`): find the
  cause. A test timing assumption is fixed in the test (wait on the
  condition, not a fixed sleep or tight deadline), a test-only change
  with the cause in the review note. A product bug goes to the
  coordinator first. Never silenced, and never retried blindly.
  Approved exception (coordinator, 2026-10-18): the chefui storage
  tests build their fixture on the host's shared `/tmp` tmpfs, where
  another process can change the free space mid-scan; the product then
  correctly reports an estimate. Those steps repeat, bounded at 100,
  only for that case: `test_sys.c` when the statvfs pair around the
  direct scan moved, `test_settings.c` (648 and `test_storage`) when the
  result is an estimate that is otherwise fully consistent, or the 30 s
  cache was dropped by a capacity change. Anything else fails at once.
  A test must still cover the capacity-change estimate path
  deterministically.

- **Kernel headers**: `-Wno-cpp` was hiding `kernel/include/uapi/linux/types.h`'s
  "kernel headers from user space" `#warning` in the 7 binaries that
  include `tools/msmipc.h`. A make rule copies the one exported UAPI
  header they need, `kernel/include/uapi/linux/msm_ipc.h`, into
  `build/<config>/include/linux/`, and that `-I` replaces
  `-I kernel/include/uapi -I kernel/include` in host and device builds;
  the toolchain's sanitized `linux/types.h` and `linux/ioctl.h` serve
  the rest. Tried by cleanup_impl: all device binaries build clean under
  the full flag set and are byte-identical to the baseline.
- **`tools/device.mk`**: one shared include with `DEVICE_CC`, the device
  flag set and the `msm_ipc.h` rule, included by every component
  Makefile (chefui keeps its `DEVICE_CC ?=`, takes the flags).

Acceptance: `make test` passes from a clean tree and in 5 consecutive runs;
`git status` is clean after a build and test run (nothing written into
the source tree); the staged image's file list equals the M1 list;
every binary's size is within a few percent of the baseline (list each
change and its cause, for example `-std=gnu11` on a binary that was
built with the compiler default); `mkinitramfs.sh` has no `$MUSLCC`
lines left.

## M4. Rename `initramfs/` to `rootfs-overlay/`

Its own commit: `git mv initramfs rootfs-overlay` and the path updates
only (about 140 references in about 45 files). Keep the names
`scripts/mkinitramfs.sh`, `out/initramfs-root` and
`out/initramfs.cpio.*`: they produce the RAM image's initramfs.
`docs/archive/`, `docs/research/`, `logs/` and the dated entries of
`docs/build-log.md` keep the old path (decided 2026-10-18; so do this
file and section 0 of the plan, which describe the rename);
`docs/repository-layout.md` says what the directory used to be called.
Acceptance: `grep -rn 'initramfs/'` outside those keep-lists hits only
`out/initramfs-*` and the script name; the staged image is identical to
M3's (same file list and sha256 of every file except
`/etc/chef/image-info`).

## End

`cleanup_impl` builds the final RAM image (`docs/building.md`) and
reports its path and sha256 plus the comparison tables to
`cleanup_coord`. The coordinator runs the live check: boots it from RAM
to the status bar and checks buttons, touch, Wi-Fi, a GPS lease and
sensors with the user. Then the coordinator moves this file to
`docs/archive/` and records the result.

## Result (coordinator, 2026-10-18)

Done. Commits `6947ecb` (M1), `53e6ce7` (M2), `7584546` (M3), `6472f3a`
(M4), `a11079d` and `4bd4085` (follow-ups), each approved by
cleanup_review and checked against the reviewed diff. The final RAM
image `ca43cff1` matched the baseline apart from the listed exceptions
and passed the live check with the user (status bar, power button,
touch, Wi-Fi, GPS lease, sensors). Record:
[build cleanup log](../../logs/build-cleanup-2026-10-18.txt).
Not installed: the installed pair stays `2faf5f0e` + `606bd269` until
a later step installs a new one.
