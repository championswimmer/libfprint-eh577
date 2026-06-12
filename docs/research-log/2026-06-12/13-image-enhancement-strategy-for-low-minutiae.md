# Image-enhancement strategy for low-minutiae EH577 captures

Date: 2026-06-12

## Question

Can we improve the visibility of the processed EH577 PGM images with brightness /
contrast / sharpening so that NBIS sees more real minutiae?

## External research summary

Relevant fingerprint-processing literature and NBIS documentation agree on the same
broad pipeline:

- normalize contrast / brightness before minutiae extraction
- enhance ridge-valley structure locally rather than globally when images are uneven
- avoid treating a higher minutiae count as automatically better, because enhancement
  can also create false minutiae from noise
- Gabor / FFT / orientation-aware ridge enhancement is the standard heavier-weight
  approach for low-quality fingerprint images
- NBIS `mindtct` itself has a command-line low-contrast enhancement option (`-b`) in
  the full NBIS tool, but libfprint's embedded NBIS path currently calls
  `get_minutiae()` directly and does not expose that CLI option as-is

Sources checked:

- NIST NBIS documentation: MINDTCT detects minutiae and includes quality maps such as
  low-contrast / low-flow regions.
- NBIS `mindtct` manual references a low-contrast image enhancement option.
- Fingerprint enhancement literature commonly uses normalization, local histogram /
  contrast methods, and Gabor/FFT ridge enhancement before binarization and thinning.

## Local offline experiment

Added helper:

- [tools/eh577_enhance_pgm_experiment.py](../../../tools/eh577_enhance_pgm_experiment.py)

It reads `capture-*.pgm`, writes enhanced variants, and reports:

- NBIS minutiae count via
  [eh577-pgm-minutiae](../../../refs/libfprint/examples/eh577-pgm-minutiae.c)
- grain % using the existing `3x3 median diff >25` metric
- ridge pixel count (`pixel < 180`)

First test set:

- [artifacts/capture12/20260612-163325](../../../artifacts/capture12/20260612-163325)
- generated variants under
  [artifacts/capture12/20260612-163325/enhance-experiment](../../../artifacts/capture12/20260612-163325/enhance-experiment)

Transforms tested:

- `gamma12`: mild darkening / gamma contrast
- `stretch1`: 1..99 percentile contrast stretch
- `stretch5`: 5..99 percentile contrast stretch
- `med_stretch`: 3x3 median then percentile stretch
- `local36`: local z-score normalization (`radius=8`, `gain=36`)
- `local36_us`: `local36` plus mild unsharp mask

## Results on 20260612-163325

Original minutiae counts:

| capture | orig minutiae |
| --- | ---: |
| 01 | 3 |
| 02 | 3 |
| 03 | 2 |
| 04 | 5 |
| 05 | 3 |
| 06 | 2 |
| 07 | 4 |

Average result by transform:

| transform | avg minutiae | avg grain % | notes |
| --- | ---: | ---: | --- |
| `orig` | 3.14 | 0.366 | current baseline |
| `gamma12` | 3.29 | 0.454 | very mild; low risk but little benefit |
| `stretch1` | 3.29 | 1.517 | more visible ridges but more grain |
| `stretch5` | 3.29 | 2.096 | visually best after review; monitor grain because it sits near the 2% gate |
| `med_stretch` | 3.43 | 0.459 | modest gain, low grain; worth visual inspection |
| `local36` | 5.29 | 0.543 | strongest minutiae gain, but may over-create ridge area |
| `local36_us` | 5.43 | 0.827 | strongest gain; higher risk of synthetic minutiae |

Important per-frame observations:

- `capture-03`: `2 -> 6` minutiae with `local36`
- `capture-05`: `3 -> 6` minutiae with `local36`
- `capture-06`: `2 -> 4` minutiae with `local36_us`
- `capture-07`: `4 -> 7` minutiae with `local36` / `local36_us`

So local contrast normalization can materially increase NBIS minutiae count on the
current small EH577 images.

## Caution

The local normalization variants also greatly increase the number of pixels counted as
ridge-area (`pixel < 180`). That is expected because the transform remaps local
neighborhoods around a mid-gray output, but it means some of the extra minutiae may be
synthetic or less reproducible.

Therefore, **do not accept higher minutiae count alone as success**. A transform is only
useful if it improves cross-press Bozorth scores for the same finger without increasing
different-finger false matches.

## Recommended strategy

### Stage A — offline visual/minutiae calibration

Use the new helper on every future capture set:

```bash
./tools/eh577_enhance_pgm_experiment.py artifacts/capture12/<session>
```

Then render promising variants to PNG and eyeball whether ridges are genuinely clearer
or whether the transform is hallucinating texture.

Initial variants to focus on:

1. `med_stretch` — safest first candidate; small minutiae gain, low grain
2. `local36` — strongest candidate for solving low minutiae, but needs match testing
3. `local36_us` — use only if `local36` improves but still leaves broken ridges

Avoid for now:

- global equalization / aggressive contrast stretch as a driver default
- strong unsharp alone
- selecting transforms solely by minutiae count

### Stage B — matching validation

For each transform, measure:

- per-image minutiae count
- self-match sanity score
- same-finger cross-press Bozorth score
- different-finger Bozorth score

The winning transform is the one that raises **same-finger cross-press score**, not the
one that maximizes minutiae count.

### Stage C — driver integration if validated

If one transform proves useful, apply it in the driver after:

1. raw frame capture
2. warm-background subtraction
3. resize to `208x104`
4. polarity normalization

and before libfprint/NBIS minutiae extraction.

Most practical first driver candidate:

- optional env-gated `local36`-style local contrast normalization
- integer implementation, no floating-point dependency required after calibration
- keep disabled or env-gated until cross-press matching proves it helps

## Follow-up decision: use `stretch5` in the capture flow

After visual review, `stretch5` was judged to do the best job improving actual PGM
visibility. The driver was updated to incorporate this transform into the capture flow:

1. raw frame capture
2. warm-background subtraction
3. resize to `208x104`
4. polarity normalization
5. **stretch5 enhancement**: map the 5th..99th percentile intensity range to `20..245`
6. Stage-2 grain / ridge / minutiae gate
7. submit the enhanced image to libfprint

This means the saved capture PGMs and the image passed to NBIS are now the enhanced
image, not just an offline visualization.

## Current conclusion

Use `stretch5` as the current driver-side enhancement because it gives the best visual
ridge visibility so far. The next validation question is whether this visibility gain
also improves same-finger cross-press Bozorth scores and does not reintroduce
mismatched-finger false matches.

Keep `local36` / `local36_us` as future fallback ideas only if `stretch5` improves
visibility but does not improve matching enough.
