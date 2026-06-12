# Stretch5 follow-up: visibility improved, but noise/fingerprint separation needs a pre-stretch p5 gate

Date: 2026-06-12

## Context

After `stretch5` was integrated into the live driver capture flow, a new capture run was
collected:

- [artifacts/capture12/20260612-170905](../../../artifacts/capture12/20260612-170905)

Visual labels from review:

- `capture-01` — very very noisy
- `capture-02` — very very noisy
- `capture-06` — fairly ok
- `capture-07` — great
- `capture-08` — great

Important observation: `stretch5` makes ridges more visible, but it also makes noise
and fingerprint texture harder to separate if the quality gate only looks at the final
enhanced image.

## Metrics from saved enhanced PGMs

| Capture | Visual label | Enhanced grain % | Ridge `<180` | Minutiae |
| --- | --- | ---: | ---: | ---: |
| 01 | very very noisy | 8.871 | 5967 | 36 |
| 02 | very very noisy | 6.411 | 4861 | 17 |
| 03 | unlabelled | 2.932 | 6044 | 8 |
| 04 | unlabelled | 8.838 | 5207 | 35 |
| 05 | unlabelled | 4.607 | 6154 | 9 |
| 06 | fairly ok | 3.227 | 5710 | 5 |
| 07 | great | 2.032 | 6582 | 4 |
| 08 | great | 1.061 | 6543 | 6 |

As expected, noisy enhanced frames can still manufacture many minutiae. `01`, `02`,
and `04` are the obvious examples.

## Key new separation signal

The driver debug logs show the percentile stretch inputs (`p5`, `p99`) before stretch5.
Those `p5` values separate the visibly noisy captures from the good captures much better
than post-stretch minutiae:

| Capture | Visual label | pre-stretch p5 |
| --- | --- | ---: |
| 01 | very very noisy | 94 |
| 02 | very very noisy | 132 |
| 04 | unlabelled/noisy-looking by metrics | 99 |
| 06 | fairly ok | 199 |
| 07 | great | 185 |
| 08 | great | 180 |

Interpretation: heavy dark noise pulls the 5th percentile down before stretch. Once
`stretch5` remaps that low percentile to `20`, the final enhanced image hides this
separation and makes the texture/noise distinction harder.

## Driver change made

The Stage-2 gate now records `stretch_p5` / `stretch_p99` while applying stretch5 and
adds a pre-stretch low-percentile floor:

- `EGIS0577_STAGE2_MIN_STRETCH_P5=100`

The gate now requires:

- pre-stretch `p5 >= 100`
- enhanced grain `< 8.000%`
- `3 <= minutiae <= 12`
- ridge pixels `>= 600`

The `capture12.sh` wrapper now forwards the new env var:

- `EGIS0577_STAGE2_MIN_STRETCH_P5`

## Expected effect on 20260612-170905

Using the new defaults:

- reject `01` and `02` because `stretch_p5` is too low
- reject `04` for the same reason
- accept only frames with at least 4 minutiae among otherwise-clean captures (`06`, `07`, and `08` are candidates from the visual review, but the minutiae floor now decides which of them are actually submitted)
- keep `stretch5` for visibility / NBIS, but avoid using stretch5 output as the only
  noise-separation signal

## Current conclusion

`stretch5` should stay in the capture flow for visibility, but Stage-2 quality needs
pre-stretch and fake-minutiae guards. The best pre-stretch signal found in this run is
the stretch input `p5`. The driver also caps minutiae at `12` because too many minutiae
on this tiny sensor usually means noise, not a better fingerprint. These checks reject
obvious noisy frames before stretch5 can make them look ridge-like.
