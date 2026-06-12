# Stage-1 best-frame capture12 analysis: noise still inflates minutiae, clean frames are still minutiae-sparse

Date: 2026-06-12

## Context

After the Stage-1 "bounded turn + pick the cleanest frame" change landed in
[egis0577.c](../../../refs/libfprint/libfprint/drivers/egis0577.c), a new 12-touch
capture run was collected with:

- [tools/capture12.sh](../../../tools/capture12.sh)
- artifacts directory:
  [artifacts/capture12/20260612-154220](../../../artifacts/capture12/20260612-154220)

The user visually labeled these captures:

- `03` — quite a bit of noise
- `06` — strong image, no noise
- `07` — clean image too
- `08` — horrible noise
- `11` — fairly high noise too
- `12` — slight noise but has ridges

Question: does the new best-frame selection improve the clean/noisy split, and do
clean frames now carry enough minutiae for matching?

## What was measured

For each processed `capture-NN.pgm` image:

1. minutiae count using
   [eh577-pgm-minutiae](../../../refs/libfprint/examples/eh577-pgm-minutiae.c)
2. basic image stats using [tools/pgm_stats.py](../../../tools/pgm_stats.py)
3. the previously validated offline **grain %** metric:
   fraction of interior pixels whose value differs from their `3x3` median by `>25`
4. simple ridge-area proxies (`pixels < 180`, `pixels < 160`)

## Results

| Capture | Visual label | Grain % | Mean | Minutiae | Pixels `<180` | Notes |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 01 | unlabelled | 0.029 | 237.3 | 4 | 1278 | clean enough, barely reaches 4 minutiae |
| 02 | unlabelled | 0.205 | 239.3 | 2 | 861 | clean but too few minutiae |
| 03 | noisy | 5.164 | 223.8 | 28 | 3130 | heavy noise, minutiae massively inflated |
| 04 | unlabelled | 0.433 | 238.7 | 2 | 735 | borderline noisy, low minutiae |
| 05 | unlabelled | 0.152 | 243.2 | 3 | 81 | very low ridge area |
| 06 | clean | 0.209 | 231.4 | 4 | 2460 | best-looking clean frame; enough ridge area |
| 07 | clean | 0.000 | 238.2 | 3 | 837 | very clean, but still too little ridge area / minutiae |
| 08 | noisy | 5.958 | 219.5 | 26 | 3655 | worst noise; minutiae again inflated |
| 09 | unlabelled | 0.533 | 241.6 | 2 | 572 | borderline noisy, low minutiae |
| 10 | unlabelled | 0.428 | 237.3 | 2 | 1331 | borderline noisy, low minutiae |
| 11 | noisy | 4.493 | 221.4 | 18 | 3820 | noisy; high false minutiae |
| 12 | slight noise | 0.933 | 231.7 | 4 | 2147 | ridge-rich, but still above the old clean/noisy split |

## Main findings

### 1. The core diagnosis still holds: noisy frames inflate minutiae

The user-identified noisy frames (`03`, `08`, `11`) again produced the largest
minutiae counts:

- `03` → `28`
- `08` → `26`
- `11` → `18`

while the visually clean frames stayed around the real usable range:

- `06` → `4`
- `07` → `3`

So raw minutiae count is still a **bad quality signal** and would still select the
wrong frames.

### 2. The validated grain metric still separates the obviously bad captures

The clearly noisy images landed around `4.5% .. 6.0%` grain, while the clean ones
were much lower:

- `06` → `0.209%`
- `07` → `0.000%`

This is directionally consistent with the earlier offline calibration: the bad frames
are still easy to spot, and the clean frames remain low-grain.

### 3. Capture `12` is the interesting borderline case

`12` visually looked only slightly noisy and had visible ridges. Offline it measured:

- grain `0.933%`
- minutiae `4`
- ridge-area proxy `<180` = `2147`

So `12` is **not junk** — it has real ridge content — but it is not truly clean by
this metric either. It looks like a mixed case: enough ridge area to reach `4`
minutiae, but still carrying noticeable high-frequency contamination.

### 4. Cleanest does not automatically mean matchable

`07` is the clearest counterexample:

- visually clean
- grain `0.000%`
- only `3` minutiae
- much smaller ridge-area proxy than `06` (`837` vs `2460` pixels `<180`)

So the next-stage gate cannot be **noise-only**. A frame can be very clean and still
be too coverage-poor / ridge-poor to be useful.

## Implication for Stage 2

A Stage-2 accept rule should use the **validated grain metric**, not the temporary
`dark<60` shortcut, and it also needs a ridge-area / coverage floor in addition to the
`>=4` minutiae floor.

On this capture set:

- with `grain < 0.3%` and `minutiae >= 4`, only `01` and `06` pass
- `07` fails for too few minutiae despite being perfectly clean
- `12` reaches `4` minutiae but still looks too grainy for a strict clean-frame gate

That means the next question is not just "is the frame clean?" but also
"does it contain enough real ridge area to be worth storing?"

## Recommended next move

1. Implement Stage-2 gating on the selected frame using:
   - calibrated grain %
   - minutiae count
   - a simple ridge-area / coverage floor
2. Keep the current "pick the cleanest frame during the 1s turn" logic, but when
   several candidates are similarly clean, prefer the one with more ridge coverage.
3. Re-run enroll / verify after that gate so the gallery is built from frames more
   like `06` and less like `03` / `08` / `11`.

## Bottom line

Stage 1 fixed the capture-turn behavior, but it did **not** remove the underlying
trade-off:

- noisy frames still manufacture fake minutiae,
- very clean frames can still be too sparse,
- and the promising target remains "clean **and** ridge-rich" frames like `06`.

See also:
- [04-capture-noise-timing-and-denoise-findings.md](04-capture-noise-timing-and-denoise-findings.md)
- [../../todo.md](../../todo.md)
- [../../../.agents/plans/10-eh577-richer-enroll-best-frame.md](../../../.agents/plans/10-eh577-richer-enroll-best-frame.md)
