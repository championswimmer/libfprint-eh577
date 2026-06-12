# EH577 Richer Enroll: Pick Best Frame Instead Of First

## Goal

Make enrollment build **denser, higher-quality gallery templates** by selecting the
**best** frame from each enroll press instead of accepting the *first* frame that
passes `capture_usable`. The aim is to raise the count of **real, reproducible**
minutiae in each stored template so that a later identify press can actually
correspond against it.

## Status

**Stage 1 IMPLEMENTED (2026-06-12), built, awaiting hardware test.** The bounded-turn
capture state machine is in `save_img`/`finalize_turn` (egis0577.c): per touch, skip
the first 100ms (settle), then over a 100..1000ms window keep the cleanest *usable*
frame (lowest saturated-pixel count — the cheap proxy for grain), and finalize at the
1s cap (submit best, or retry if none). A turn never blocks >1s, fixing the "stuck on
hold still" hang, and it submits the cleanest frame instead of the first usable one.
New struct fields: turn_open, best_frame, best_sat, best_coverage.

**Stage 2 IMPLEMENTED (2026-06-12), awaiting hardware test:** the driver now applies
a processed-image gate in `process_imgs` before handing a snapshot to libfprint. It
normalizes the final resized `208x104` image into the same polarity used by the
offline capture12 tooling, computes the validated `3x3 median diff > 25` grain %,
counts NBIS minutiae directly via `get_minutiae()`, and measures ridge-area with a
simple normalized-output dark-pixel count (`<180`). The current driver-side capture flow also applies **stretch5** enhancement after
normalization and before Stage-2 gating / submission: map the final `208x104` image's
5th..99th percentile intensity range to `20..245`. This was chosen after visual review
as the best visibility improvement among the offline PGM transforms tried so far.

Follow-up live testing showed that stretch5 improves visibility but also makes noise
and fingerprint texture harder to separate in the final enhanced image. The gate now
therefore also checks the **pre-stretch p5** value: obvious dark-noise frames pull this
5th percentile down before stretch remaps it.

The code-level defaults now match the best **live-tested operating thresholds so far**:

- pre-stretch `p5 >= 100`
- enhanced grain `< 8.000%`
- `3 <= minutiae <= 12`
- ridge pixels `>= 600`

with optional runtime overrides via:

- `EGIS0577_STAGE2_MIN_STRETCH_P5=100`
- `EGIS0577_STAGE2_GRAIN_PCT_X1000=8000`
- `EGIS0577_STAGE2_MIN_MINUTIAE=3`
- `EGIS0577_STAGE2_MAX_MINUTIAE=12`
- `EGIS0577_STAGE2_MIN_RIDGE_PIXELS=600`

and all three are env-tunable via:

- `EGIS0577_STAGE2_GRAIN_PCT_X1000`
- `EGIS0577_STAGE2_MIN_MINUTIAE`
- `EGIS0577_STAGE2_MIN_RIDGE_PIXELS`

A Stage-2 failure now reports a retry instead of submitting the frame upstream.

**Best thresholds so far (2026-06-12 live tuning):** after stretch5, treat the
`p5>=100 / grain<8.0% / 3<=minutiae<=12 / ridge>=600` gate as the current operating
point for real testing. This does **not** mean image quality is solved; it only means
stretch5 needs a pre-stretch noise-separation signal. The driver now also has
action-aware noisy-burst recovery: repeated noise-like rejects clear the warm
background, force full pre-init/reinit, and require clean baseline frames before
arming again. The remaining unsolved problem is still the low count of real
reproducible minutiae on otherwise acceptable clean frames. The new `stretch5`
enhancement is intended to improve visibility/minutiae enough to re-test that open
problem.

**First Stage-1 capture12 result (20260612-154220):** the turn logic behaved, but the
new run confirmed the same quality split still dominates: noisy frames again inflated
minutiae (`03`=`28`, `08`=`26`, `11`=`18`), while clean frames stayed sparse
(`06`=`4`, `07`=`3`). The validated offline grain metric still separated the obvious
bad frames (`03`/`08`/`11` ~`4.5%..6.0%`) from clean ones (`06`=`0.209%`,
`07`=`0.000%`), but `12` landed as a ridge-rich borderline case (`grain=0.933%`,
`minutiae=4`). This is why the implemented Stage 2 gates on both **noise** and
**ridge content**, not on minutiae alone and not on the temporary `dark<60` proxy.

Open dependency below still gates whether richer enroll can ever fix matching.

## Context (what we know as of 2026-06-12)

- The match pipeline itself is **sound**: a probe with 27 minutiae self-matched at
  **238** (threshold 9). So extraction, coordinates, resize, and Bozorth all work.
  This is NOT a format/coordinate bug. (session `20260612-145213`)
- Cross-press score is a flat **0** for the same finger. Diagnosed as
  **capture-area / too-few-reproducible-minutiae**, not a bug. See
  [docs/research-log/2026-06-12/](../../docs/research-log/2026-06-12/) and the
  identify-issues notes.
- The driver currently accepts the **first** frame that passes `capture_usable`
  (`save_img` in [egis0577.c](../../refs/libfprint/libfprint/drivers/egis0577.c)) —
  no comparison across the many frames seen during one press.
- Per-frame minutiae yields measured this session:
  - **clean** images → ~2-3 minutiae (the *real* reproducible count)
  - **noisy** images → 18-27 minutiae, but mostly **spurious** noise speckle that
    does not reproduce between presses (confirmed: noisy attempts 01/04 had 27/18,
    clean attempt 02 had 2; user confirmed visually).

## Critical caveat — "best" must NOT mean "most minutiae"

Selecting the frame with the **highest minutiae count** would pick the **noisiest**
frame (spurious speckle inflates the count), making the gallery *worse* for
cross-matching, not better. The whole value of this plan depends on a quality metric
that rewards **clean, real ridge content**, explicitly NOT raw minutiae count.

Candidate "best frame" quality signals (favor real ridges, penalize noise):
- **VALIDATED noise metric — "grain %":** fraction of pixels whose value differs by
  >25 from their 3x3 median. Measured against a human eyeballing 12 captures
  (artifacts/capture12/20260612-150529) it matched perfectly: every CLEAN image
  scored grain ≤ 0.10%, every NOISY image ≥ 0.60% (6x gap, nothing between).
  **Threshold ~0.3%: below = clean, above = noisy.** Catches mid-gray graininess /
  broken ridges, not just dark speckle. Independent of minutiae count (avoids the
  "most minutiae = noisiest" trap). High-freq mean-|Laplacian| also separated
  (clean ~6, noisy 8-25) but grain% is the cleaner discriminator.
- high finger-coverage in the real ridge value range (`count_finger_pixels_raw`,
  ~16..149) — more genuine contact area (also use to reject faint frames)
- **low** saturated/hot-pixel count (sat≈255) — cleaner frame, better baseline match
- good agreement with the rolling warm baseline (small residual after subtraction)
- NEVER raw minutiae count (rewards noise)

So "best frame" = **lowest grain% among frames with adequate coverage** — pick the
cleanest sufficiently-covered frame, not the one with the most minutiae. The first
Stage-1 capture12 rerun reinforced the "adequate coverage" part: `07` was extremely
clean but still only yielded `3` minutiae because its ridge area was much smaller than
`06`.

Tooling: `tools/capture12.sh` + `eh577-capture-helper` capture N images per touch via
fp_device_capture (driver's processed output) for this kind of offline calibration.

## Open dependency (decide first)

This plan only helps if the **real** (clean-frame) minutiae count can be raised above
~2-3. If the clean-image ceiling is genuinely ~2-3 (small-sensor limit), picking a
better frame still leaves too few reproducible minutiae and matching will still fail.
**Resolve first:** test offline whether sharpening ridge contrast/continuity (not
noise removal) raises the *real* minutiae count on a clean enroll frame. If it does →
this plan is worth building. If it stays ~2-3 → this is a small-sensor wall and the
effort belongs on coverage/placement or a non-minutiae matcher instead.

## Steps (when picked up)

1. During an enroll stage, poll several frames across the press window instead of
   stopping at the first usable one (bounded by a frame count or a short time budget,
   mindful of the ~8-large-read-per-claim ceiling — may need claim recycling).
2. Score each candidate frame with the clean-ridge quality metric above.
3. Keep the single best frame (or accumulate/average several well-aligned clean
   frames to suppress noise while preserving real ridges).
4. Submit only the chosen/accumulated frame as the stage image.
5. Keep stages at varied finger placements so the gallery covers more of the
   fingertip (improves the odds an identify press overlaps some template).

## Validation

- Gallery template minutiae (the `minutiae: ... template=N` instrumentation in
  [fpi-print.c](../../refs/libfprint/libfprint/fpi-print.c)) rise on **clean** frames,
  not via noise.
- At least one same-finger identify produces a **non-zero** cross-press score
  (ideally ≥ threshold), with the probe also being a clean (low-saturation) frame.
- No regression in enroll completion time / reliability.

## Todo

- [ ] Resolve the open dependency: does stretch5 enhancement raise real clean-frame minutiae and same-finger cross-press Bozorth scores?
- [x] Define and implement the clean-ridge "best frame" quality score (grain + ridge-area / coverage floor)
- [ ] Add multi-frame polling within an enroll stage (respect claim/read budget)
- [ ] Implement best-frame selection (and/or aligned-frame accumulation)
- [ ] Re-measure gallery template minutiae on clean frames
- [ ] Re-test same-finger cross-press score for a non-zero result
- [ ] Decide go/no-go vs the coverage/placement and non-minutiae-matcher alternatives
