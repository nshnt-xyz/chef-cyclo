# Display and touch modernization investigation

[Research index](README.md) · [Roadmap](../next-steps/ui-and-ride-app.md) · [Existing contract](../features/display-and-touch.md)

**Historical:** on 2026-10-03 the DRM/Wayland route was shelved in favour of LVGL on the existing fbdev; see the [UI roadmap](../next-steps/ui-and-ride-app.md#ui-stack) and [prototype](lvgl-fbdev-prototype.md).

Initial investigation: 2026-10-02. The target is native DRM/KMS, a current Wayland compositor and libinput. No DRM driver has been enabled or verified by this initial work.

## Verified baseline and first touch integration

The connected phone runs `4.4.192-cyclo+` build #20. Cached fb sysfs reports `mdssfb_80000`, `U:1080x2246p-60`, virtual size 1080x4492, 32 bits per pixel and stride 4352. `/dev/kgsl-3d0` exists and `/dev/dri` does not. The configured kernel has `CONFIG_QCOM_KGSL=y`, `CONFIG_FB_MSM_MDSS=y` and DRM disabled. The panel selected by the bootloader is Tianma NT, rather than the FocalTech fallback in the board DT. Existing [device identity](../device.md) records the replacement NT36525 touchscreen; do not substitute an NT36672A driver merely because the original assembly used that chip.

Initial `udevadm info` had no `ID_INPUT` properties. `udevadm test-builtin input_id /sys/class/input/event1` recognized `ID_INPUT=1` and `ID_INPUT_TOUCHSCREEN=1`. Targeted input coldplug then populated both in the udev database. This is a boot metadata initialization gap, not evidence that a custom device classification or calibration rule is needed. The baseline deliberately coldplugs only network devices to avoid earlier broad-trigger delays; add input separately with bounded settling and preserve NetworkManager readiness on input failure. Current libinput [requires input classification](https://wayland.freedesktop.org/libinput/doc/latest/device-configuration-via-udev.html) before accepting a device. The rebuilt image subsequently passed actual libinput initialization (results below).

The existing `fbtouch info` confirmed MT-B, ten slots and ABS ranges 0..720 / 0..1600. It also queries panel status, which can issue a DSI transaction; the new inventory must instead use an explicit cached-attribute allowlist. Never open `/dev/fb0` merely to inventory it: even read-only opens change the display lifecycle. Never read NVT firmware/buildid/ic_ver while the panel is off.

Baseline capture: [device metadata](../../logs/display-modernization-2026-10-02-baseline.txt), taken after the manual input-only coldplug. Physical taps, corner alignment and multitouch gestures require later hands-on validation; software discovery cannot prove them.

## Existing 4.4 DRM code is not a config-only route

The tree contains `drivers/gpu/drm/msm`, including MDP5, SDE and two DSI implementations. Its existence is more nuanced than saying the kernel contains no DRM code: the running kernel exposes no DRM interface, and this tree's code is not an integrated chef driver.

`msm_drv.c` platform matching accepts `qcom,mdp` and `qcom,sde-kms`; the working board uses downstream MDSS nodes and panel properties. The MDP5 configuration table has revisions 0, 2, 3, 6, 7 and 9, and rejects an unknown minor revision. There is no explicit SDM636/660 entry in that table. Enabling DRM requires matching hardware configuration, DT bindings, DSI/PHY support, clocks, IOMMU and panel integration, not just setting `CONFIG_DRM_MSM`. The Makefile excludes its DRM GPU objects when `CONFIG_QCOM_KGSL=y`; acceleration needs a separate ownership and compatibility decision.

Do not bind fbdev and DRM drivers to the same hardware. A downstream port should start with a documented resource/binding map and isolated build configuration before a temporary boot experiment.

## Upstream route: SoC support exists; board support remains work

Sources inspected against upstream commit `ce1e0223d8ad4211275c82a17ed6d43ab81e13d9`; recheck support before selecting a kernel release.

- [SDM636 description](https://github.com/torvalds/linux/blob/ce1e0223d8ad4211275c82a17ed6d43ab81e13d9/arch/arm64/boot/dts/qcom/sdm636.dtsi) inherits SDM660 and explicitly identifies Adreno 509.
- [SDM660 description](https://github.com/torvalds/linux/blob/ce1e0223d8ad4211275c82a17ed6d43ab81e13d9/arch/arm64/boot/dts/qcom/sdm660.dtsi) supplies display graph/DSI descriptions, including a 14 nm DSI PHY. [SDM630 description](https://github.com/torvalds/linux/blob/ce1e0223d8ad4211275c82a17ed6d43ab81e13d9/arch/arm64/boot/dts/qcom/sdm630.dtsi) supplies inherited display resources.
- [Sony SDM636 board](https://github.com/torvalds/linux/blob/ce1e0223d8ad4211275c82a17ed6d43ab81e13d9/arch/arm64/boot/dts/qcom/sdm636-sony-xperia-ganges-mermaid.dts) is a reference for the SoC, not a drop-in chef board description.
- [DRM driver matching](https://github.com/torvalds/linux/blob/ce1e0223d8ad4211275c82a17ed6d43ab81e13d9/drivers/gpu/drm/msm/msm_drv.c) includes SDM630/660 display migration handling between MDP5 and DPU. Check the chosen release's actual driver selection instead of assuming one backend for all releases.
- [A5xx GPU implementation](https://github.com/torvalds/linux/blob/ce1e0223d8ad4211275c82a17ed6d43ab81e13d9/drivers/gpu/drm/msm/adreno/a5xx_gpu.c) explicitly handles A509; this does not establish chef firmware, power or Mesa integration.
- [Upstream Novatek I2C driver](https://github.com/torvalds/linux/blob/ce1e0223d8ad4211275c82a17ed6d43ab81e13d9/drivers/input/touchscreen/novatek-nvt-ts.c) advertises NT11205 and NT36672A, not this replacement NT36525. Validate protocol/reset/firmware requirements before adapting or porting the current driver.

The inspected upstream [DT build list](https://github.com/torvalds/linux/blob/ce1e0223d8ad4211275c82a17ed6d43ab81e13d9/arch/arm64/boot/dts/qcom/Makefile) includes the Sony SDM636 board but no chef target. This establishes a board-description gap in that upstream snapshot, not absence of all community work.

Initial preference is to investigate a newer native DRM kernel as an isolated bring-up branch, while retaining 4.4 as the working baseline. This is provisional: working modem/GPS/audio/USB support and board power sequencing may determine whether a downstream DRM port is smaller. No full kernel migration has been attempted.

## Board details to preserve

Local source: `kernel/arch/arm/boot/dts/qcom/sdm636-chef.dtsi` and `dsi-panel-mot-tianma-nt-618-fhd-vid-common.dtsi`.

| Resource | Current board description |
| --- | --- |
| Panel | 1080x2246, 60 Hz, 24-bit DSI video burst, four lanes |
| Horizontal timing | front porch 40, back porch 40, pulse 22 |
| Vertical timing | front porch 10, back porch 8, pulse 2 |
| Panel pins | reset GPIO53, TE GPIO59 |
| Panel reset | low 10 ms, high 2 ms, low 5 ms, high 10 ms |
| Touch | I2C address 0x62, IRQ GPIO67, reset GPIO66 |
| Backlight | PM660L WLED strings 0/1, CABC and DCS brightness integration |
| Panel supplies | wqhd-vddio, LAB and IBB with load/delay requirements |

Translate the vendor DSI command descriptors into equivalent DRM panel transactions with their packet types, ordering and delays. Preserve power and backlight sequencing; copying bytes without interpreting descriptor headers is insufficient. Verify actual regulator providers in the complete DT before constructing a new board description.

The current NT36xxx driver registers an fb notifier: suspend runs on `FB_EARLY_EVENT_BLANK` POWERDOWN, releasing contacts before shared rails go down; resume runs on `FB_EVENT_BLANK` UNBLANK and resets the controller. DRM does not automatically replace that notification relationship. Explicit shared-rail suspend/reset ordering is a prerequisite for a successful DRM port. The raw test client's missing `SYN_DROPPED` resynchronization is a baseline limitation; the new input stack must be checked for recovery.

## Next bounded experiment

First establish coldplug plus libinput initialization on the current kernel and test metadata-only inventory with the screen both on and off. Rebuild and temporarily boot the image, record hashes and automated results, and leave physical alignment/gesture acceptance pending. Then map the chef resources onto a selected upstream release and prepare a separate minimal USB-plus-display kernel experiment. Do not replace the verified baseline or claim successful DRM/Mesa support from source inspection.

## Initial image results

Temporary `fastboot boot` succeeded with boot image SHA256 `18687a1209077c45a92c67c66d8c057dafe5a139f05e73193d0ce8b9f5fd7ad5`; kernel #20 was unchanged. Rootfs gained libinput 1.31.3 and diagnostics; apk reported 124 MiB installed, including Python dependencies pulled by libinput-tools, and the compressed ramdisk is about 45 MiB. Reconsider diagnostic package footprint before production packaging.

Input coldplug started at kernel uptime 5.172018 s, init handoff occurred at 5.172351 s, and input readiness at 5.193532 s (about 22 ms). Both readiness markers were present. The first successful recorded shell query was at 24.86 s; there is no matched baseline boot-to-shell timing comparison, so do not treat this as a latency benchmark. NetworkManager, buttond and fblog were running.

[Libinput evidence](../../logs/display-modernization-2026-10-02-libinput.txt): native `list-devices` recognizes NVTCapacitiveTouchScreen as touch on seat0 with identity calibration; `debug-events` reports DEVICE_ADDED, touch capability and ten contacts in the [health capture](../../logs/display-modernization-2026-10-02-final-health.txt). No physical touch reports were generated during the unattended test. Existing virtual pointer/fingerprint inputs are also enumerated; the future compositor must deliberately choose devices and preserve buttond ownership.

[Dark inventory evidence](../../logs/display-modernization-2026-10-02-dark-inventory.txt): both inventory modes and libinput discovery succeeded while WLED remained zero, and before/after dmesg snapshots were identical. `lcd-backlight/brightness` retains the requested 96 while dark and is not proof of actual illumination. Removing the off flag alone leaves the idling fblog asleep; its documented restart restored WLED to 1542 ([wake evidence](../../logs/display-modernization-2026-10-02-wake-restored.txt)). An earlier test attempted the restart before the replacement fblog existed; that attempt is retained in the raw evidence rather than counted as a successful wake.

[Screen-cycle evidence](../../logs/display-modernization-2026-10-02-screen-cycles.txt): foreground test committed five frames, performed POWERDOWN/UNBLANK, blanked before close and handed back to fblog. Zero touch reports is expected without a person; it does not validate the controller's physical event delivery. No CTP_I2C or BUS ERROR messages appeared. No visual observer confirmed output, no physical gesture or dropped-event recovery test was performed, and no GPU/compositor/performance result is claimed.

Host inventory/syscall tests and coldplug success/failure/stall tests passed; reviewer independently checked these and existing fbtouch 71/71 and fblog 2003/2003 checks. Both agents agreed ready before live testing. Image hashes are [recorded separately](../../logs/display-modernization-2026-10-02-image-hashes.txt). The phone was left running the new temporary baseline with the log screen restored; no partition was flashed.

## Historical GPU follow-up proposal (2026-09-19)

Not planned for the LVGL platform, which renders on the CPU. Record kept for a possible later mainline route. The Adreno 509 currently uses the downstream `kgsl` driver and is unused by our display clients. As part of the kernel investigation, establish the exact GPU revision, DRM `msm`/Mesa freedreno compatibility, required firmware, and buffer-sharing support with the display driver. Do not assume KMS scanout also enables GPU rendering, or that Mesa can use the existing KGSL interface unchanged.

Once software-rendered DRM/KMS works, bring up Mesa/EGL acceleration and verify actual hardware rendering, compositor buffer import/presentation, and recovery across screen cycles. Compare CPU usage, frame latency and power on data pages and rotating maps. Record software-rendering fallback behavior. Libhybris over stock Android GLES blobs may be investigated if the native route is blocked, but is a separate compatibility approach, not proof of native DRM/Mesa support.
