# EH577 Enrolled Vs Identify PGM Debug Workflow

## Goal

Create a repeatable workflow for diagnosing why EH577 enrollment succeeds but
identify currently produces long hold times, retryable capture failures, and
`NO MATCH` / `score 0` results. The outcome of this plan is not just a one-off
inspection of `enrolled.pgm` and `identify.pgm`, but a stable procedure the
user can run in later sessions to generate comparable image artifacts, logs,
and summary data that make the next driver decision obvious.

## Context

- Active driver integration lives in `refs/libfprint/libfprint/drivers/egis0577.c`
  and `.h`
- Mirror/staging copy must stay in sync under `wip-libfprint/drivers/`
- Current capture model is `FP_SCAN_TYPE_PRESS`, not swipe
- Current helpers and wrapper:
  - `refs/libfprint/build/examples/eh577-enroll-helper`
  - `refs/libfprint/build/examples/eh577-identify-helper`
  - `tools/enroll_identify.sh`
- Current image artifacts are written to repo root:
  - `enrolled.pgm`
  - `identify.pgm`
- Current logs show:
  - enroll can complete
  - identify does detect finger presence
  - identify often returns `no-match`
  - some identify attempts return retryable scan errors such as
    `Please try again` or `Minutiae detection failed, please retry`
- The likely remaining failure modes are:
  - enroll and identify images are not orientation-consistent
  - the snapshot image is mirrored, rotated, cropped, or scaled incorrectly
  - identify images contain less usable ridge detail than enroll images
  - the image is accepted upstream but yields too few or poor minutiae for NBIS

## Steps

1. Standardize artifact capture so each run preserves all evidence instead of
   overwriting a single `enrolled.pgm` and `identify.pgm`.
2. Define a user-facing capture script that runs one controlled enrollment plus
   one or more controlled identify attempts and stores:
   - the session log
   - enrolled and identify PGM files
   - metadata describing finger used, attempt number, and whether the attempt
     was expected to match or mismatch
3. Define a lightweight analysis script that reads the saved PGM files and
   produces objective image stats:
   - width / height
   - min / max grayscale
   - mean grayscale
   - number of non-zero pixels
   - histogram buckets
   - bounding box of non-zero region
   - optional ASCII or downscaled textual preview
4. Add a comparison script that takes one enrolled PGM and one identify PGM and
   checks image-level consistency before we reason about Bozorth scores:
   - same dimensions
   - same polarity expectation
   - same ridge orientation family
   - same occupied area ratio
   - same general finger placement region
5. Capture three evidence sets:
   - same-finger enroll + identify
   - different-finger identify against the enrolled gallery
   - repeated identify of the same enrolled finger across multiple attempts
6. Use the image evidence to classify the problem into one of:
   - capture-state bug
   - image-format bug
   - ridge-quality bug
   - matcher-threshold bug
7. Only after the image evidence is consistent, resume driver tuning for
   matching quality or timing.

## Validation

- A single command produces a timestamped evidence directory containing:
  - raw session log
  - one enrolled PGM per enrollment stage or final accepted enrollment image
  - one identify PGM per identify attempt
  - a machine-readable metadata file
  - a textual stats summary for each image
- Two separate sessions can generate artifacts with the same structure
- Another coding session can inspect the saved directory and answer:
  - did identify capture a real fingerprint image
  - does the identify image resemble the enrolled image at all
  - is the failure likely before or after minutiae extraction

## Todo

- [x] Create a dedicated artifact root such as `artifacts/pgm-debug/` with one timestamped subdirectory per run
- [x] Update capture workflow so `enrolled.pgm` and `identify.pgm` are copied immediately after each helper event instead of being overwritten by later attempts
- [x] Define a naming scheme: `enroll-left-thumb-stage-01.pgm`, `identify-left-thumb-attempt-01-match.pgm`, `identify-right-thumb-attempt-01-mismatch.pgm`
- [x] Store per-run metadata in a simple text or JSON file: timestamp, helper used, finger label, expected outcome, log path, driver commit, parent repo commit
- [x] Add a user-run capture script spec under `tools/` for a command like `tools/collect_pgm_debug_run.sh`
- [x] The capture script should support both same-finger and different-finger identify cases without editing the script each time
- [x] The capture script should print the final evidence directory path clearly for handoff to the next session
- [x] Add a PGM stats script spec under `tools/` for a command like `tools/pgm_stats.py <file.pgm>`
- [x] `pgm_stats.py` should parse binary PGM (`P5`) safely and print width, height, min, max, mean, non-zero count, occupied bounding box, and a coarse histogram
- [x] Add a comparison script spec under `tools/` for a command like `tools/compare_pgm_pair.py <enrolled.pgm> <identify.pgm>`
- [x] `compare_pgm_pair.py` should report dimension match, occupancy ratio delta, center-of-mass delta, and whether one image appears inverted relative to the other
- [x] Add optional image rendering support so the analysis script can emit enlarged preview PNGs or textual previews for chat-friendly review
- [x] Preserve identify helper outputs that mention `identify-result`, `identify-retry`, `image-saved`, and `finger-status`
- [x] Preserve enroll helper outputs that mention `image-saved`, stage progress, retry reasons, and `finger-status`
- [ ] Capture at least one same-finger evidence set where identify returns `NO MATCH`
- [ ] Capture at least one retry/failure evidence set where identify says `Minutiae detection failed`
- [ ] Compare enrolled vs identify images manually for orientation, mirroring, crop, ridge spacing, and placement
- [ ] Decide whether the next driver experiment should focus on orientation/polarity, scaling, thresholding, or capture timing

### User Workflow To Generate Evidence

- [ ] Build command to document for future sessions:
  `meson compile -C refs/libfprint/build`
- [ ] Primary capture command to document:
  `./tools/enroll_identify.sh`
- [ ] After the script completes, copy the generated `enrolled.pgm` and `identify.pgm` into the timestamped evidence directory before running the next attempt
- [ ] Always record whether the identify finger was expected to match or mismatch
- [ ] Prefer one run with only a single enrolled finger in the gallery first, then repeat with two enrolled fingers
- [ ] For same-finger tests, use the exact enrolled finger for all identify attempts in the run
- [ ] For mismatch tests, deliberately use a different finger and record which one
- [ ] Share the evidence directory path, not just the top-level log path, in the next coding session

### Interpretation Checklist

- [ ] If enrolled and identify PGM images are visually unrelated, treat the issue as capture/image formatting, not matching
- [ ] If images are visually similar but identify still returns `score 0`, inspect polarity, scaling, and minutiae extraction quality next
- [ ] If identify images are weak, sparse, or clipped while enroll images are strong, focus next on identify capture timing and usable-frame thresholds
- [ ] If both images are strong and consistent but mismatch still scores high or same-finger scores zero, inspect Bozorth thresholding and extracted minutiae counts
