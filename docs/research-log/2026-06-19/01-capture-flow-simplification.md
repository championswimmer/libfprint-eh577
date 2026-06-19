# Capture flow simplification — 2026-06-19

## Goal

Rewrite the `egis0577` driver capture loop to match an explicit, simple flow:

1. **Settle gap (0–400 ms)**: after first finger detection, ignore all frames.
2. **Evaluation window (400–1400 ms)**: check each frame against the quality gate.
   Accept the first frame that passes.
3. **End of turn**:
   - **Accept**: apply stretch5 contrast enhancement, submit to enroll/verify.
   - **Timeout (no valid frame)**: report failure, block new turns until finger lifts.
     A new turn is only armed once a zero/below-threshold frame is seen.

## What changed in egis0577.c

### Struct
- Removed `best_frame`, `best_sat`, `best_coverage` — the "collect best frame across
  the whole window" strategy was replaced by "accept first passing frame".
- Removed `rearm_not_before_time` — time-based re-arm block replaced by
  `waiting_for_lift` (a boolean cleared only on an actual no-finger frame).
- Removed `capture_nonzero` — was set by the removed `finalize_turn` and only logged.
- Added `waiting_for_lift` — set when a turn times out or after each accepted enroll
  frame; cleared when a zero / below-threshold frame is seen.

### State machine
- Removed `SM_PROCESS_IMG` state — no longer needed since `save_img` now evaluates,
  enhances (stretch5 via `stage2_snapshot_quality_ok`), and submits the image in one
  shot, calling `on_frame_accepted` directly.

### Functions removed
- `finalize_turn()` — merged into `save_img`.
- `process_imgs()` — logic folded into `save_img`; stage2 now runs exactly once.
- `capture_usable()` — pre-gate (coverage/intensity) removed; quality gate is
  sufficient and avoids skipping marginal frames that could still pass stage2.
- `clear_best_frame()` — no longer needed.
- `count_saturated_pixels()` — was used only for best-frame ranking.

### `save_img` rewrite
Single function implementing the full turn flow described above.  The stage2 quality
check (`stage2_snapshot_quality_ok`) normalises polarity and applies stretch5 in-place
on a temporary resized image; if accepted, that already-enhanced image is passed
directly to `on_frame_accepted` — no second copy or second stage2 call.

### `on_frame_accepted_enroll`
`rearm_not_before_time` settle timer replaced by `self->waiting_for_lift = TRUE`,
requiring actual lift detection rather than a time delay before the next enroll turn.

## What changed in egis0577.h

### Quality gate (stage2)
Gate reduced from 5 conditions to 4:
- Kept: `grain < 6%`, `minutiae > 2`, `ridge_pixels > 600`
- Added back: `minutiae < 10` (upper cap, re-introduced at value 10 vs old 16, to
  reject noisy frames that manufacture false minutiae via stretch5 artefacts)
- Removed: `stretch_p5 >= MIN_STRETCH_P5` (no longer a gate; stretch5 still applied)
- Removed: `EGIS0577_STAGE2_MIN_STRETCH_P5` constant
- `EGIS0577_STAGE2_MAX_MINUTIAE` kept but lowered from 16 → 10

Gate conditions now use strict `>` (not `>=`) for minutiae and ridge_pixels, matching
the spec literally: `minutiae > 2` and `ridge_pixels > 600`.

### Constant removed
- `EGIS0577_ENROLL_REARM_SETTLE_MS` — no longer needed (lift-based, not time-based)

## Rationale

The old flow ran stage2 twice (once as a "fast exit" in `save_img`, once in
`process_imgs`) and tracked the "best" frame by saturated-pixel count as a fallback.
The new flow is: evaluate every settled frame, accept immediately on first pass, time
out if nothing passes within the window, then require a real lift.  This is simpler,
eliminates the duplicate NBIS minutiae extraction, and makes the touch-removal
requirement explicit and lift-driven rather than timer-driven.
