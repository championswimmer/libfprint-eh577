# Dumb digital scaling is not helping EH577 matching

We ran an offline benchmark on the already captured `capture12` PGM images using the new `tools/eh577_scale_benchmark.py` helper. The benchmark compared native images against simple nearest-neighbor 2x and 3x digital upscales, then ran the existing EH577 minutiae and Bozorth helpers on each scale.

## What we tested

Input set:
- `artifacts/capture12/20260613-001309/capture-01.pgm` through `capture-12.pgm`

Scales tested:
- `1x` native
- `2x` nearest-neighbor upscale
- `3x` nearest-neighbor upscale

## Results

The benchmark summary was:

| Scale | Minutiae min/max/mean | Images with >=10 minutiae | Pair scores |
|---|---|---:|---:|
| 1x | `3/8/5.33` | `0` | all `0` |
| 2x | `0/10/2.00` | `2` | all `0` |
| 3x | `0/1/0.08` | `0` | all `0` |

Notable observations:
- Native images already have only **3–8 minutiae**, so they are below the Bozorth 10-minutiae floor.
- 2x scaling did push **two** images up to 10 minutiae, but **no pairwise score became non-zero**.
- 3x scaling made things worse, collapsing most images to zero minutiae.
- Even self- or same-session comparisons remained at score `0` under this dumb scaling approach.

## Conclusion

Simple digital zooming is **not** a useful fix for EH577 matching.

It does not add real fingerprint information, and in this dataset it did not improve Bozorth outcomes. At best it slightly changes minutiae detection; at worst it destroys the extractor output.

## Practical takeaway

- **Do not rely on blind 2x/3x scaling as a matching strategy.**
- **Smaller native images are preferable for speed** when they are already too weak for reliable matching, because enlarging them just adds processing cost without improving the result.
- If we want to improve EH577 matching, the next step should be **better capture quality / preprocessing / gating**, not dumb geometric scaling.
