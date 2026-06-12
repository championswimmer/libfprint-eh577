# Driver-side noisy-burst recovery: fresh baseline/reinit after repeated noise-like rejects

Date: 2026-06-12

## Context

After `stretch5` and the Stage-2 gate became usable, repeated captures showed a
stateful noisy mode: once the sensor/AGC/hot-pixel state became noisy, many subsequent
attempts produced frames with low pre-stretch `p5`, high grain, and/or very high
minutiae counts. Rejecting those frames prevented bad images from being submitted, but
it did not actively recover the sensor state.

This needed to work for all libfprint consumers, not only the local capture helper.
Therefore the recovery policy was implemented inside the driver.

## Noise-like reject definition

A Stage-2 reject is counted as **noise-like** if it fails any of these checks:

- pre-stretch `p5 < EGIS0577_STAGE2_MIN_STRETCH_P5`
- enhanced grain `>= EGIS0577_STAGE2_GRAIN_PCT_X1000`
- minutiae `> EGIS0577_STAGE2_MAX_MINUTIAE`

Low-minutiae rejects are deliberately **not** counted as noisy, because they usually
mean weak/partial ridge capture rather than sensor noise.

## Action-aware recovery policy

The driver now tracks a noise-like reject streak and triggers a fresh-baseline recovery
when the streak crosses an action-aware threshold.

For enrollment/capture:

- trigger after `3` consecutive noise-like rejects
- allow up to `2` recovery attempts per action
- wait `2000ms`
- require `2` clean baseline frames before arming capture again

For verify/identify:

- trigger after `2` consecutive noise-like rejects
- allow up to `1` recovery attempt per action
- wait `500ms`
- require `1` clean baseline frame before arming capture again

This keeps enrollment/capture more patient while keeping login/verify responsive and
bounded.

## Recovery action

When recovery triggers, the driver:

1. clears the current capture frame
2. clears the best-frame candidate
3. clears the warm background
4. resets `capture_armed`, `turn_open`, and startup-transient timing
5. marks recovery active
6. forces `has_pre_init_run = FALSE` so the next cycle starts from full pre-init
7. recycles the USB interface claim and restarts from `SM_INIT`
8. waits for the configured number of clean no-finger baseline frames before arming
   capture again

The reject reason includes:

```text
noise recovery: fresh baseline/reinit requested
```

## Why driver-side matters

Because this is inside [egis0577.c](../../../refs/libfprint/libfprint/drivers/egis0577.c),
it applies to:

- `capture12.sh`
- enroll helper
- identify helper
- fprintd / GNOME enrollment
- PAM/login verify flows

The local helper only affects terminal UX; the recovery itself is universal.

## Current Stage-2 gate around this recovery

At the time of implementation, the Stage-2 gate is:

- `pre_stretch_p5 >= 100`
- `grain < 8.000%`
- `3 <= minutiae <= 12`
- `ridge_pixels >= 600`

The upper minutiae cap exists because too many minutiae on this tiny sensor usually
means noise, not a better fingerprint.

## Current conclusion

The driver now not only rejects noisy frames, but also actively recovers from repeated
noise-like rejects by forcing a fresh baseline/reinit cycle. This should reduce long
runs where every subsequent capture remains noisy after the sensor enters a bad state.
