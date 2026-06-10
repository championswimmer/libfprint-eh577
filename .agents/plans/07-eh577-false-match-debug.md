# EH577 False Match Debug

## Goal

Determine why the driver accepts unrelated fingers as MATCH. Either the bozorth3
threshold is too permissive, the assembled image lacks enough ridge detail for
meaningful minutiae, or the resize is producing an artefact that looks the same
regardless of finger. End state: verify reliably rejects a different finger and
accepts the enrolled one.

## Context

- Enroll + verify both succeed end-to-end (session log: `logs/session-20260610-002907.txt`)
- Pixman resize now active: assembled image is scaled 2× (`EGIS0577_RESIZE = 2`)
- Driver constants in `refs/libfprint/libfprint/drivers/egis0577.h`:
  - `EGIS0577_BZ3_THRESHOLD 15`
  - `EGIS0577_CONSECUTIVE_CAPTURES 8`
  - `EGIS0577_IMGWIDTH 103`, `EGIS0577_RFMGHEIGHT 24`
- Assembled image before resize: 103 × ~178 px; after 2×: ~206 × 356 px
- `enrolled.pgm` and `verify.pgm` are written to repo root each run
- Matcher is bozorth3 via `fpi_image_device_image_captured`

## Steps

1. **Collect image evidence** — run enroll, then verify with a *different* finger.
   Save both `enrolled.pgm` and `verify.pgm`. Visually compare: do they look
   like the same finger or are they clearly different? This tells us whether the
   problem is image quality (both look like noise) or threshold (images differ
   but score is still above threshold).

2. **Inspect minutiae counts** — enable `G_MESSAGES_DEBUG=all` during verify and
   grep for bozorth3 score lines (`bz3 score`, `minutiae`). Check how many
   minutiae are detected and what the match score is.

3. **Test with threshold = 0** — temporarily set `EGIS0577_BZ3_THRESHOLD 0` in
   `egis0577.h`, rebuild, and verify with a different finger. If it now rejects,
   the threshold is the problem. If it still matches, image quality is.

4. **Inspect image dimensions and content** — check that the assembled image is
   not all-zero or near-uniform. A uniform image will produce zero minutiae and
   bozorth3 may score any two zero-minutiae prints as matching.

5. **Try without resize** — set `EGIS0577_RESIZE 1` (no-op resize), rebuild,
   re-enroll and re-verify same finger + different finger. Determine if 2×
   bilinear upscale is hurting ridge sharpness.

6. **Increase strip count** — try `EGIS0577_CONSECUTIVE_CAPTURES 16`, rebuild,
   re-enroll. A taller assembled image gives bozorth3 more ridge crossings.

7. **Raise threshold** — try `EGIS0577_BZ3_THRESHOLD 40`, rebuild, re-enroll
   and re-verify. Confirm different finger is rejected; confirm same finger
   still matches.

8. **Tune to stable values** — iterate until: same finger → MATCH, different
   finger → NO MATCH, consistently across 3 runs each.

## Validation

- `./tools/enroll_and_verify.sh` with the enrolled finger → `MATCH`
- Manual verify run with a deliberately different finger → `NO MATCH`
- Both results stable across at least 3 consecutive runs each

## Todo

- [ ] Collect enrolled.pgm + verify.pgm with mismatched fingers; visually inspect
- [ ] Run verify with `G_MESSAGES_DEBUG=all`; grep for bozorth3 score and minutiae count
- [ ] Set `EGIS0577_BZ3_THRESHOLD 0`, rebuild, verify different finger — does it reject?
- [ ] Check assembled image for near-uniform / all-zero content
- [ ] Try `EGIS0577_RESIZE 1` (no upscale), re-enroll and re-verify both fingers
- [ ] Try `EGIS0577_CONSECUTIVE_CAPTURES 16`, re-enroll and re-verify
- [ ] Tune `EGIS0577_BZ3_THRESHOLD` to value that reliably separates same vs different
- [ ] Confirm 3× same-finger MATCH + 3× different-finger NO MATCH
