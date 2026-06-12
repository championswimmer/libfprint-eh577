# 2026-06-12 — Windows INF and DLL guard clues

Extracted from the Windows driver stack for EH577:

### INF Key/Value Pairs
- `SensorMode = 1`
- `RemoteWakeupEnable = 1`
- `FetchImageMode = 0x80000003`
- `FingerOnMode = 0`
- `FingerOnThresholdLoose = 2`
- `FingerOnThreshold = 6`
- `ResumingDetectModeParameterTuningEnabled = 0`

### DLL Strings
- `SetDetectModeParameters`
- `FPS_CTL(Detect)` / `PGA_CONTL(Detect)` / `DETC(Detect)`
- `FPS_CTL(Sensor)` / `PGA_CONTL(Sensor)`
- `CALIBRATE_TYPE(Detect)` / `CALIBRATE_TYPE(Sensor)`
- `DeviceIdleEnabled` / `DefaultIdleTimeout` / `ResumeFromSuspend`

These clues strongly suggest a dedicated detect/sensor split and tunable detect-mode parameters.
