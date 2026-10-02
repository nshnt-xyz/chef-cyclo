# DRM bring-up experiments

[Display research](display-modernization.md) · [Roadmap](../next-steps/ui-and-ride-app.md)

Started 2026-10-02 after committing initial standard touch work as `173ab54`. Coordinator owns research and device tests; fresh Herdr implementation/review sessions own the isolated compile experiment and independent review. This record distinguishes source support, compilation, module initialization and physical scanout.

## Actual display hardware

[Live cached capability evidence](../../logs/drm-bringup-2026-10-02-hardware.txt) identifies `hw_rev=805437440`, hexadecimal `0x30020000`: major 3, minor 2. Although the vendor ABI prints `mdp_version=5`, that label does not select the old upstream MDP5 implementation. The working pipeline has two VIG pipes, three DMA pipes, one cursor pipe, seven blending stages and no shared-memory-pool blocks. GPU sysfs identifies `Adreno509v1` with available frequencies 430/370/266/160 MHz.

The upstream [SDM660 DPU 3.2 catalog](https://github.com/torvalds/linux/blob/ce1e0223d8ad4211275c82a17ed6d43ab81e13d9/drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_3_2_sdm660.h) identifies the same major/minor and provides matching core clock-control registers and functional blocks. This supports investigating that native hardware backend, not adopting another phone's panel or complete DT.

Register offsets need an explicit base convention. The vendor MDSS resource begins at 0x0c900000; its MDP block starts at +0x1000. Several upstream catalog offsets are relative to that MDP base:

| Block | Vendor offset from MDSS | Upstream catalog offset from MDP |
| --- | --- | --- |
| CTL0 | 0x2000 | 0x1000 |
| VIG0 | 0x5000 | 0x4000 |
| DMA0 | 0x25000 | 0x24000 |
| LM0 | 0x45000 | 0x44000 |
| DSPP0 | 0x55000 | 0x54000 |
| Pingpong0 | 0x71000 | 0x70000 |
| DSI0 interface | 0x6b800 | 0x6a800 |

These entries are a source cross-check, not a register-write recipe. Retain physical resource lengths, interrupt routing, clocks, IOMMU and bandwidth votes when mapping bindings.

## Downstream driver blockers established by source

- `drivers/gpu/drm/msm/msm_drv.c` dispatches only MDP4 and SDE. It never invokes `mdp5_kms_init`, despite MDP5 objects appearing in its Makefile. The existing `qcom,mdss_mdp` node matches neither DRM platform compatible.
- SDE's catalog obtains geometry from `qcom,sde-*` DT properties. Its hardware-format initialization switch handles 1.7, 3.0 and 4.0 revisions, with format initialization for 3.0/4.0; it has no 3.2 case. Merely changing the compatible would not supply valid pipe formats and resources.
- The legacy DRM DSI PHY path matches 28 nm and 20 nm only. The newer `dsi-staging/dsi_catalog.c` has actual operations only for PHY4.0; cases1.0/2.0/3.0 return unsupported. The board's 14 nm PHY therefore needs real implementation work in either path.
- With KGSL built in, the DRM Makefile excludes Adreno driver objects. This preserves GPU ownership in the initial compile experiment, but provides no native GPU acceleration.
- The current NT36xxx touch power lifecycle depends on fb blank notifications and shared panel rails. A display-owner transfer must replace that ordering before physical touch can be accepted.

A local SDE port would require 3.2 capabilities, DT resource translation and a 14 nm PHY implementation, as well as panel/power integration. A newer native kernel already has relevant display/PHY code but needs chef board enablement and validation of USB/power and existing subsystem compatibility. Neither route is a one-line config change.

## Isolated downstream compile experiment

The initial module configuration failed. The reviewed built-in configuration subsequently completed full compile/link closure after DRM-private helper namespace separation. See [the compile record](downstream-drm-compile.md) for all six attempts and reproduction. `out/kernel`, baseline images, board bindings, fbdev ownership and KGSL were preserved.

This experiment can establish build viability. It does not demonstrate `/dev/dri`, a connected panel, modesetting, touch wake, rendering performance or Mesa compatibility. No forced binding or unbinding on the running phone is authorized by this compile step.

### Ownership check before a built-in probe

The DRM top-level compatible does not tell the whole ownership story. Live [binding evidence](../../logs/drm-bringup-2026-10-02-display-bindings.txt) confirms that `c994000.qcom,mdss_dsi_ctrl0` uses `qcom,mdss-dsi-ctrl` and is bound to `mdss_dsi_ctrl`. The legacy DRM DSI driver matches that **same compatible**. Enabling it built in could compete with the verified vendor driver before userspace starts. Therefore the no-binding probe must disable `CONFIG_DRM_MSM_DSI` and its dependent PLL/28nm/20nm paths, rather than assume unchanged DT prevents conflict. The staging DSI path uses different controller/display compatibles and has no chef board integration.

The full board source resolves the panel supplies to PM660 L11 (`wqhd-vddio`), PM660L L6 (`vdda-3p3`), and LCDB LDO/NCP providers (`lab`/`ibb`). Carry these exact provider mappings into the panel-port investigation; the panel's vendor supply descriptors alone do not identify regulator hardware.

### Panel-driver translation checks

Parsed the selected Tianma NT vendor command descriptors as seven-byte headers plus declared payload length. The on sequence has 29 packets and 130 ms total explicit post-command delay; off has two packets and 155 ms (35 ms after display-off, 120 ms after sleep-in). Preserve low-power on-command and high-speed off-command state separately.

A subtle packet issue: the vendor on sequence encodes sleep-out/display-on as type0x05 short writes with two-byte `11 00` / `29 00` payload storage. The extra zero is padding for this packet type, not evidence of a DCS parameter. Passing those stored payloads unchanged to a helper that chooses type from length could turn them into type0x15 parameter writes. A future panel driver must interpret packet type and padding, not just copy payloads into length-selected DCS calls.

The nominal timing totals are 1182 by 2266 at60 Hz, yielding 160,704,720 Hz pixel clock before rounding. This calculation is a starting check; reconcile it with PHY/link clock behavior and measured mode before accepting the DRM panel timing.


## Temporary built-in boot probe: failed to reach USB shell

The coordinator audited the enabled driver OF aliases against the live board compatibles, disabled the conflicting legacy DRM DSI path, and confirmed the chef DTB was byte-identical to baseline. This is a source-level ownership check, not proof of runtime safety. Evidence: [alias audit](../../logs/drm-bringup-2026-10-02-alias-audit.txt).

Changing the kernel configuration invalidated 208 of the baseline WLAN module's 425 imported CRCs, including `module_layout`. Rebuilt the pinned WLAN sources against `out/kernel-drm`; the replacement matched all 425 imports with no missing symbols or CRC mismatches. This establishes ABI preparation only; the probe never reached a shell to test loading. Evidence: [WLAN ABI comparison](../../logs/drm-bringup-2026-10-02-wlan-abi.txt).

Packaged a separate rootfs copy with the matching WLAN module and `/etc/drm-probe-build`, then generated `out/drm-probe/boot.img` using the normal boot header and command line. [Artifact hashes](../../logs/drm-bringup-2026-10-02-image-hashes.txt) identify the exact image. Two temporary `fastboot boot` attempts reported successful transfer/boot commands, but neither reached the USB shell; the phone was subsequently detected in fastboot. The first shell polling window was 45 seconds. Bootloader reason was `Reboot mode set to fastboot`, which does not identify a kernel fault. No panic or DRM runtime diagnosis is established.

Restored the verified baseline after each attempt, without flashing. The final [recovery transcript](../../logs/drm-bringup-2026-10-02-recovery.txt) shows baseline kernel #20, both readiness markers, WLED brightness 1542, and NVT libinput discovery. Pstore contained only a zero-byte annotation, with no console-ramoops to explain the probe failure. NetworkManager was not running at the captured check; do not treat marker presence as proof of service availability.

Next prerequisite: obtain early boot console or persistent crash evidence, and compare an isolated baseline-config rebuild before changing hardware bindings. Then narrow the configuration/initcall difference. Do not infer a specific panic, watchdog, binding conflict, or successful DRM registration from the current evidence. SDE3.2, 14nm PHY, panel integration, native scanout and acceleration remain outstanding.


### Follow-up baseline platform inventory

Implementation and review independently found a conditional pre-userspace wait in `msm_drv.c`: the late initcall waits indefinitely if `msm_drm_probed` or `find_device` is true. The probe sets that flag before a DMA-mask failure can return without completing; `find_device` also considers already-bound matching devices. This is an unproven hazard, not a diagnosis of the failed boots. Registration-only initcalls did not reveal unconditional display MMIO or power changes without a matched probe.

The [baseline platform inventory](../../logs/drm-bringup-2026-10-02-platform-inventory.txt) supplements the OF audit with device names, modaliases, bound drivers and driver overrides. No device name matches `mdp`, `dsi_phy`, `drm_dsi_ctrl`, `msm-dsi-display`, `sde_hdmi`, `msmdrm_smmu`, `hdmi_msm` or `msm_edp` (including numeric platform suffixes). This baseline inventory cannot prove the experiment's actual probe state before it failed. The later check also confirms NetworkManager running with `wlan0` available. The earlier recovery check found it absent; the later result does not establish the cause of that earlier state.
