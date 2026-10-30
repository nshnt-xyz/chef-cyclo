# Research and investigation records

[Project overview](../../README.md) · [Current features](../features/README.md) · [Next steps](../next-steps/README.md) · [Build log](../build-log.md)

These records preserve completed investigations, original designs, experiment plans and detailed findings. Dated hypotheses may be superseded by later results in the same record. Current operating contracts belong in feature guides; unfinished work belongs in next steps.

- [Offline maps](offline-maps.md): October 17 survey of OSM map and elevation sources, measured regional sizes, raster versus on-device vector rendering, and refreshing tiles over Wi-Fi.
- [LVGL fbdev prototype](lvgl-fbdev-prototype.md): October 3 prototype behind the LVGL-on-fbdev UI decision: pixel path, page flips, cost, screen power and touch-driver findings.
- [DRM bring-up](drm-bringup.md): hardware revision/resource mapping, the October 3 compile and boot experiments, and the October 24 and 30 in-tree SDE bring-up (scanout, page flips, panel re-init after power-off, touch and backlight on blank work; the MDSS GDSC is held on).
- [Display modernization](display-modernization.md): initial DRM/KMS kernel support investigation and standard touch discovery work on October 2 (historical).
- [Battery and charging](battery-and-charging.md): stock policy research and the September 26 powerd implementation.
- [Wi-Fi](wifi.md): matching Motorola driver sources, modem dependency and board-data handshake for October 1 bring-up.
- [Sensors](sensors.md): protocol recovery, bring-up, magnetometer calibration and compass experiments through October 1.
- [Speaker protection](speaker-protection.md): playback bring-up evidence and the investigation abandoned September 26, including diagnostic hazards.

Completed implementation handoffs and acceptance checklists live in the [archive](../archive/README.md).

Keep unique findings and evidence references here. Update feature guides and roadmaps when behavior or decisions change rather than treating an original experiment plan as an active task.
