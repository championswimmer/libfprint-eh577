# 02 — Restoring `tools/capture12.sh`: capture vfunc + unarmed reinit/rearm

## Context

After the driver migrated from `FpImageDevice` to a plain `FpDevice` (to do NCC
matching instead of minutiae/bozorth), [`tools/capture12.sh`](../../../tools/capture12.sh)
stopped working. Two distinct failures, fixed in sequence.

## Failure 1 — "Device does not support capture"

- `FpImageDevice` auto-provided `fp_device_capture()`; a plain `FpDevice` only
  exposes capture if the driver wires a `capture` vfunc.
- The migrated class wired `open/close/enroll/verify/identify` but **no
  `capture`**, so `fpi_device_class_auto_initialize_features` never set
  `FP_DEVICE_FEATURE_CAPTURE`. The helper bailed at its
  `fp_device_has_feature(..., CAPTURE)` check.
- Confirmed in `artifacts/capture12/20260613-164125/../debug.log`:
  *"Device … does not support capture"*.

**Fix:** added `dev_capture()` → `start_capture_action()` (same SSM as the other
actions), wired `dev_class->capture` before `auto_initialize_features`, and added
an `FPI_DEVICE_ACTION_CAPTURE` branch to `on_frame_accepted` that hands the
stage-2-qualifying `FpImage` to `fpi_device_capture_complete()`. Because capture
flows through the same `process_imgs` → stage-2 gate path as enroll/verify, every
returned image is stage-2-qualifying by construction and is the exact resized
snapshot the NCC matcher sees.

## Failure 2 — stuck in an unarmed poll loop

With capture wired, the helper got past the feature gate but hung. Log
(`20260613-164125/debug.log`) pattern:

1. First touch → Stage-2 reject ("likely noise": grain 14%, minutiae 18>16).
2. Reject path runs `clear_background()` + `has_pre_init_run=FALSE` +
   `restart_capture_cycle` (claim recycle + PRE_INIT) — i.e. one full reinit.
3. Then *"Persistent finger-like startup frames for >750ms during enroll/capture;
   refusing to arm…"* repeats forever (~every 18 ms).

**Root cause:** `calculate_finger_heuristics` subtracts `self->background`; with
background NULL (just cleared on the reject), coverage is computed on **raw**
pixels. The noisy idle sensor reads coverage 17–25% ≥ the 18% `finger_detected`
threshold, so every frame is routed to the unarmed branch and never reaches the
baseline-rebuild path (the only path that re-arms). The unarmed enroll/capture
branch had no escalation — it looped `restart_for_next_poll` indefinitely.

Rendered raw dumps (`*-post-init-nonzero-{17..25}.bin`) show structureless
speckle with large dead zones, **not** a clean ridge press — sensor-noise state,
not a hard finger.

**Fix (bounded escalation, no USB reset):** in the unarmed enroll/capture branch:
- Up to `EGIS0577_UNARMED_REINIT_MAX` (=2) times, force a fresh init+rearm:
  claim recycle + PRE_INIT re-run (`restart_capture_cycle`, the
  standalone-probe-style fresh transport session). This is **not**
  `restart_capture_cycle_with_usb_reset` — no `g_usb_device_reset`, which is
  known to wedge this hardware until a cold reboot.
- After those are exhausted, `report_unarmed_stuck()` surfaces a retryable
  `FP_DEVICE_RETRY_REMOVE_FINGER`: for capture → `fpi_device_capture_complete
  (dev, NULL, retry)` + `SM_DONE`; for enroll → `fpi_device_enroll_progress`
  with a retry (NULL print) and another reinit cycle. The capture helper already
  handles `FP_DEVICE_RETRY` ("lift your finger before retry", auto-retry after
  1.5 s), so the loop now terminates and prompts the user.

New counter `unarmed_reinit_attempts` persists across `restart_capture_cycle`,
resets only at action start and on successful arm.

## Status

- **Guaranteed (verified by build + control-flow):** the script no longer hangs;
  after the bounded reinit attempts it prompts lift + auto-retries.
- **Hardware-gated (unverified):** whether a re-press then yields a
  stage-2-qualifying image. Log evidence shows one PRE_INIT recycle did **not**
  settle the sensor in its window, so the deterministic win is the retry/lift
  prompt, not the reinit. Watch on the next run: after "lift your finger", does a
  fresh press capture+save, or immediately re-enter the stuck loop?
- **Staged fallback if reinit proves insufficient:** stop `clear_background()` on
  the Stage-2 reject so the clean idle baseline built during initial arming is
  retained for subtraction (caveat: won't help if the noise is high-valued
  additive). Needs the device to decide.

## Follow-up — gate fresh baseline on noise only + lower min-minutiae

A clean capture run (`20260613-170230`, 7 saved / 31 retries) showed the
fresh-baseline recovery was firing on *non-noisy* rejects. Breakdown of the 31
retries: 26 genuinely noisy (grain/stretch/minutiae-high), but **5 were
`minutiae_low` only** — clean frames (grain ~2–4%, good stretch) from a light or
partial press. Those were wrongly triggering `clear_background()` + PRE_INIT
reinit.

Two changes:

1. **Recovery is now gated on `stage2_reject_is_noise_like`.** Only noisy rejects
   wipe the warm baseline and force a fresh transport session. A clean-but-
   insufficient reject (e.g. low minutiae) keeps the good baseline and takes a
   light retry (`no_finger_retry_delay_ms`, no reinit) — the baseline stays
   valid for the next press and we avoid feeding the noisy-rearm loop.
2. **`EGIS0577_STAGE2_MIN_MINUTIAE` default 4 → 2** (`egis0577.h`). Of the 5
   low-minutiae rejects, 4 had minutiae=2 and would now pass the gate; the one
   with minutiae=1 still fails but now takes the non-noise keep-baseline retry.

Driver: [`egis0577.c`](../../../refs/libfprint/libfprint/drivers/egis0577.c),
[`egis0577.h`](../../../refs/libfprint/libfprint/drivers/egis0577.h).
