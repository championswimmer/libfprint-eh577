# EH577 Touch And Temperature Guards

## Goal

Turn the current ad-hoc EH577 finger detection and implicit libfprint temperature behavior into an explicit, evidence-backed guard strategy that reduces false captures, avoids accidental over-throttling, and leaves a clear path toward hardware-backed touch detection if the Windows stack clues can be mapped onto EH577 protocol commands.

## Context

- Relevant local files:
  - `windows-driver/EgisTouchFP0577.inf`
  - `windows-driver/EgisTouchFP0577.dll`
  - `windows-driver/EgisTouchFPEngine0577.dll`
  - `refs/libfprint/libfprint/drivers/egis0577.c`
  - `refs/libfprint/libfprint/drivers/egis0577.h`
  - `refs/libfprint/libfprint/drivers/egis0570.c`
  - `refs/libfprint/libfprint/drivers/upektc.c`
  - `refs/libfprint/libfprint/drivers/egismoc/egismoc.c`
  - `refs/libfprint/libfprint/fp-device.c`
  - `refs/libfprint/libfprint/fpi-device.c`
  - `logs/2026-06-03-eh577-guided-postinit-fingerhold-01.txt`
  - `logs/identify-session-20260610-234839.txt`
  - `docs/todo.md`
- Windows INF clues already present for EH577:
  - `SensorMode = 1`
  - `RemoteWakeupEnable = 1`
  - `FetchImageMode = 0x80000003`
  - `FingerOnMode = 0`
  - `FingerOnThresholdLoose = 2`
  - `FingerOnThreshold = 6`
  - `ResumingDetectModeParameterTuningEnabled = 0`
- Windows DLL strings strongly suggest a dedicated detect/sensor split and tunable detect-mode parameters:
  - `SetDetectModeParameters`
  - `FPS_CTL(Detect)` / `PGA_CONTL(Detect)` / `DETC(Detect)`
  - `FPS_CTL(Sensor)` / `PGA_CONTL(Sensor)`
  - `CALIBRATE_TYPE(Detect)` / `CALIBRATE_TYPE(Sensor)`
  - `DeviceIdleEnabled` / `DefaultIdleTimeout` / `ResumeFromSuspend`
- Current EH577 libfprint state:
  - finger presence is software-gated by `EGIS0577_MIN_ACTIVE_PIXELS = 1000`
  - the driver already calls `fpi_image_device_report_finger_status()`
  - `FpDeviceClass.temp_hot_seconds` is currently left at the default zero-initialized value, which makes libfprint opt into its generic temperature model rather than explicitly disabling or tuning it
  - runtime logs already show EH577 sessions flipping to `FP_TEMPERATURE_WARM`, so the temperature policy is currently implicit rather than deliberate
- Comparable libfprint patterns found locally:
  - `egis0570` reports finger presence from processed frame content
  - `upektc` reports finger presence from a histogram/pixel threshold before capture
  - many MOC drivers set `temp_hot_seconds = -1` to disable thermal throttling
  - `egismoc` explicitly sets `temp_hot_seconds = 0` to use libfprint's generic temperature model

## Steps

1. Document the Windows guard-related knobs and separate solid evidence from guesses.
2. Audit libfprint guard patterns and classify them into:
   - software image heuristics
   - hardware detect/wakeup modes
   - generic temperature-model policy
3. Decide the EH577 near-term policy for each guard:
   - finger-on guard for capture start
   - finger-off guard between stages
   - temperature guard policy (`-1`, generic default, or tuned values)
4. Design an EH577-specific finger-touch strategy with thresholds and hysteresis based on measured idle vs finger-present frame statistics.
5. Identify what extra reverse-engineering is needed to move from software-only guards to hardware detect-mode guards:
   - Windows usbmon / USBPcap capture targets
   - packet/response candidates around detect mode, idle mode, and resume paths
   - interrupt / remote-wakeup relevance
6. Make the first implementation pass conservative:
   - explicit temperature choice in driver class init
   - improved software finger guards if justified by existing logs
   - leave hardware detect-mode work behind feature flags or follow-up tasks until commands are proven
7. Validate against real EH577 behavior and record the remaining unknowns.

## Validation

- A written note exists summarizing the Windows `FingerOn*` / detect-mode evidence and how trustworthy each clue is.
- A driver comparison note exists showing which libfprint drivers use software finger heuristics and which ones opt into or out of temperature throttling.
- EH577 has an explicit temperature policy in the plan and, when implemented, in the driver source.
- Guided idle/touch/remove runs can be used to measure false-positive and false-negative finger detection before and after the guard changes.
- The follow-up implementation path clearly distinguishes:
  - what can be done now from Linux-only evidence
  - what requires proprietary-stack traffic capture

## Todo

- [ ] Copy the Windows INF and DLL guard clues into a dedicated note or research-log section with exact key/value pairs and strings.
- [ ] Audit `egis0570`, `upektc`, and at least two more libfprint drivers for finger-on/finger-off guard patterns worth borrowing.
- [ ] Decide whether EH577 should explicitly disable libfprint temperature throttling for now (`temp_hot_seconds = -1`) or intentionally keep the generic model, and record why.
- [ ] Derive a stricter EH577 finger-present/finger-absent guard design from existing nonzero-pixel counts, including hysteresis or loose/strict thresholds if needed.
- [ ] Identify which Windows/proprietary-stack capture would most likely reveal `FingerOnMode`, detect-mode tuning, or remote-wakeup commands.
- [ ] Add the chosen guard policy to `refs/libfprint/libfprint/drivers/egis0577.c` and mirror it into `wip-libfprint/drivers/egis0577.c` when implementation starts.
- [ ] Validate the guard behavior with guided idle, touch, hold, and remove runs; record false positives, false negatives, and any temperature-property changes.
- [ ] Update `docs/todo.md`, `docs/research-log.md`, and the bringup plan with the resulting policy and any remaining blockers.
