# Offline minutiae / Bozorth / NCC analysis on two `capture12` folders

## Summary

Ran an offline comparison on two `capture12` folders:

- Folder A: [`artifacts/capture12/20260613-014802`](../../../artifacts/capture12/20260613-014802)
- Folder B: [`artifacts/capture12/20260613-024113`](../../../artifacts/capture12/20260613-024113)

Goals:

1. Verify whether offline minutiae extraction reproduces the driver's stage-2 minutiae counts.
2. Verify whether stock NBIS/Bozorth3 is usable on these saved PGM snapshots.
3. Verify whether normalized cross-correlation (NCC) separates same-folder pairs from cross-folder pairs well enough to justify the planned driver refactor in [`docs/plan-fpdevice-ncc-refactor.md`](../../plan-fpdevice-ncc-refactor.md).

## Inputs and tools

### Capture folders

- Folder A contains 7 saved PGM captures plus [`debug.log`](../../../artifacts/capture12/20260613-014802/debug.log).
- Folder B contains 12 saved PGM captures plus [`debug.log`](../../../artifacts/capture12/20260613-024113/debug.log).

### Offline tools used

- [`refs/libfprint/build/examples/eh577-pgm-minutiae`](../../../refs/libfprint/build/examples/eh577-pgm-minutiae)
  - runs libfprint/NBIS minutiae extraction on a list of PGM files
- [`refs/libfprint/build/examples/eh577-pgm-match`](../../../refs/libfprint/build/examples/eh577-pgm-match)
  - runs stock NBIS Bozorth3 all-pairs matching on those minutiae
- [`refs/libfprint/build/examples/eh577-pgm-match-relaxed`](../../../refs/libfprint/build/examples/eh577-pgm-match-relaxed)
  - investigation-only helper with `MIN_COMPUTABLE_BOZORTH_MINUTIAE` lowered from `10` to `1`
- [`tools/eh577_offline_pgm_workflow.py`](../../../tools/eh577_offline_pgm_workflow.py)
  - wrapper that compares offline minutiae counts against `debug.log` and emits a JSON report
- [`tools/eh577_pgm_correlate`](../../../tools/eh577_pgm_correlate)
  - standalone normalized cross-correlation matcher
- Source for the NCC tool: [`tools/eh577_pgm_correlate.c`](../../../tools/eh577_pgm_correlate.c)

Generated JSON reports:

- [`artifacts/capture12/20260613-014802/offline-report.json`](../../../artifacts/capture12/20260613-014802/offline-report.json)
- [`artifacts/capture12/20260613-024113/offline-report.json`](../../../artifacts/capture12/20260613-024113/offline-report.json)

## 1. Offline minutiae extraction reproduces the driver exactly

### Folder A (`20260613-014802`)

Accepted minutiae counts extracted from `debug.log`:

- `6, 7, 4, 4, 9, 8, 4`

Offline libfprint/NBIS counts from `eh577-pgm-minutiae`:

- `6, 7, 4, 4, 9, 8, 4`

Result: **exact match for all 7 captures**.

### Folder B (`20260613-024113`)

Accepted minutiae counts extracted from `debug.log`:

- `3, 4, 5, 4, 3, 2, 2, 3, 2, 5, 3, 2`

Offline libfprint/NBIS counts from `eh577-pgm-minutiae`:

- `3, 4, 5, 4, 3, 2, 2, 3, 2, 5, 3, 2`

Result: **exact match for all 12 captures**.

### Interpretation

The offline path is faithfully reproducing the same minutiae-extraction behavior as the EH577 driver's stage-2 quality gate. So any mismatch problems seen later are not because the saved PGMs differ from what the driver counted.

## 2. Stock Bozorth3 is not usable on these saved EH577 captures

### Why

Stock NBIS Bozorth3 refuses to compute a non-zero score when either side has fewer than `10` minutiae. Both folders are far below that:

- Folder A: `4..9` minutiae
- Folder B: `2..5` minutiae

### Stock matrix outcome

For both folders, the stock all-pairs Bozorth3 matrix was **all zeros**.

### Relaxed investigation-only Bozorth3

Using the custom helper with the internal floor lowered from `10` to `1`:

- Folder A diagonal scores became non-zero: `8, 8, 4, 4, 15, 22, 5`
- Folder B diagonal scores became mostly zero/very low: `0, 4, 0, 4, 0, 0, 0, 0, 0, 3, 0, 0`
- In **both folders**, all off-diagonal scores remained `0`

### Interpretation

Even when forced to score tiny templates, Bozorth3 provides no useful same-finger cross-match structure on these snapshots. This is consistent with the current hypothesis that EH577's tiny active area and sparse minutiae make minutiae-based matching a poor fit.

## 3. NCC analysis on Folder A vs Folder B

### Assumption under test

This comparison is only meaningful if Folder A and Folder B are **different fingers**, while all captures inside each individual folder are **the same finger**. That is the intended meaning of the folder naming/use during this investigation.

### Command used

```bash
tools/eh577_pgm_correlate --search 30 --threshold 0.50 \
  artifacts/capture12/20260613-014802/capture-*.pgm \
  artifacts/capture12/20260613-024113/capture-*.pgm
```

### A -> A matrix

```text
A->A
     A    1 A    2 A    3 A    4 A    5 A    6 A    7
A 1 |  1.00  0.58  0.53  0.35  0.31  0.28  0.18
A 2 |  0.58  1.00  0.56  0.40  0.11  0.33  0.31
A 3 |  0.53  0.56  1.00  0.49  0.17  0.46  0.32
A 4 |  0.35  0.40  0.49  1.00  0.10  0.49  0.29
A 5 |  0.31  0.11  0.17  0.10  1.00  0.07  0.12
A 6 |  0.28  0.33  0.46  0.49  0.07  1.00  0.71
A 7 |  0.18  0.31  0.32  0.29  0.12  0.71  1.00
```

### A -> B matrix

```text
A->B
     B    1 B    2 B    3 B    4 B    5 B    6 B    7 B    8 B    9 B   10 B   11 B   12
A 1 |  0.11  0.11  0.11  0.09  0.10  0.10  0.09  0.14  0.13  0.09  0.11  0.14
A 2 |  0.13  0.12  0.12  0.08  0.09  0.09  0.09  0.13  0.09  0.11  0.10  0.09
A 3 |  0.14  0.12  0.10  0.09  0.14  0.12  0.08  0.10  0.11  0.12  0.11  0.09
A 4 |  0.11  0.12  0.11  0.09  0.12  0.10  0.09  0.10  0.08  0.09  0.06  0.09
A 5 |  0.07  0.10  0.04  0.05  0.10  0.12  0.08  0.15  0.13  0.08  0.12  0.10
A 6 |  0.12  0.14  0.12  0.11  0.14  0.10  0.10  0.08  0.11  0.09  0.09  0.13
A 7 |  0.20  0.16  0.17  0.09  0.14  0.12  0.10  0.09  0.09  0.12  0.11  0.13
```

### B -> B matrix

```text
B->B
     B    1 B    2 B    3 B    4 B    5 B    6 B    7 B    8 B    9 B   10 B   11 B   12
B 1 |  1.00  0.76  0.46  0.34  0.28  0.41  0.17  0.23  0.37  0.37  0.20  0.42
B 2 |  0.76  1.00  0.41  0.22  0.28  0.30  0.17  0.23  0.33  0.24  0.23  0.36
B 3 |  0.46  0.41  1.00  0.28  0.25  0.30  0.23  0.18  0.35  0.17  0.17  0.42
B 4 |  0.34  0.22  0.28  1.00  0.33  0.30  0.27  0.18  0.51  0.51  0.55  0.45
B 5 |  0.28  0.28  0.25  0.33  1.00  0.43  0.22  0.38  0.40  0.43  0.46  0.40
B 6 |  0.41  0.30  0.30  0.30  0.43  1.00  0.29  0.76  0.69  0.46  0.55  0.65
B 7 |  0.17  0.17  0.23  0.27  0.22  0.29  1.00  0.28  0.24  0.31  0.34  0.23
B 8 |  0.23  0.23  0.18  0.18  0.38  0.76  0.28  1.00  0.44  0.39  0.87  0.33
B 9 |  0.37  0.33  0.35  0.51  0.40  0.69  0.24  0.44  1.00  0.77  0.40  0.90
B10 |  0.37  0.24  0.17  0.51  0.43  0.46  0.31  0.39  0.77  1.00  0.67  0.45
B11 |  0.20  0.23  0.17  0.55  0.46  0.55  0.34  0.87  0.40  0.67  1.00  0.37
B12 |  0.42  0.36  0.42  0.45  0.40  0.65  0.23  0.33  0.90  0.45  0.37  1.00
```

## 4. NCC summary statistics

Computed from the full `19 x 19` combined matrix, excluding self-matches:

- **A-A**
  - count: `42`
  - mean: `0.3410`
  - median: `0.3200`
  - min: `0.0700`
  - max: `0.7100`
  - pairs with score `>= 0.50`: `8`

- **A-B**
  - count: `168`
  - mean: `0.1075`
  - median: `0.1050`
  - min: `0.0400`
  - max: `0.2000`
  - pairs with score `>= 0.50`: `0`

- **B-B**
  - count: `132`
  - mean: `0.3839`
  - median: `0.3550`
  - min: `0.1700`
  - max: `0.9000`
  - pairs with score `>= 0.50`: `24`

## 5. Interpretation

### Main result

NCC shows exactly the separation pattern that the planned refactor needs:

- same-folder pairs (**A-A**, **B-B**) score much higher than cross-folder pairs (**A-B**)
- the cross-folder distribution is not merely lower on average; it is **cleanly separated** from the high-scoring same-folder pairs in this sample
- in this dataset, **A-B tops out at only `0.20`**, while many same-folder pairs sit in the `0.50..0.90` range

### Caveats

1. Folder A is weaker than Folder B.
   - A-A mean: `0.3410`
   - B-B mean: `0.3839`
   - Folder A also has a visibly weak outlier capture (`A5`) that does not correlate well with the rest.

2. Not every same-finger pair is strong.
   - Some A-A and B-B pairs are only in the `0.17..0.49` range.
   - So a single-frame, single-threshold rule is likely less robust than a multi-frame gallery with hit counting.

3. Several strong same-finger pairs still land near the search-window boundary.
   - The current offline NCC run used `--search 30`, which is better than the older `--search 20`, but it still produced some large best offsets.
   - Re-validating with a wider search window (`30..40`) on more folders is still worth doing before hardening constants.

### What this says about the NCC refactor plan

These two folders strongly support proceeding with the direction described in [`docs/plan-fpdevice-ncc-refactor.md`](../../plan-fpdevice-ncc-refactor.md):

- Bozorth/minutiae matching is not producing useful cross-press evidence on EH577 snapshots.
- NCC on raw pixels does produce strong same-finger vs different-finger separation on the saved captures.
- A multi-frame enrollment gallery plus a policy like:
  - `best_ncc >= 0.50`, and
  - `>= 2` gallery hits
  still appears reasonable as the next implementation direction.

## 6. Practical decision from this log

Based on these offline results, the NCC refactor is justified enough to prototype in-driver.

This does **not** prove the final production threshold, but it does answer the immediate gating question:

> Is NCC promising enough offline on real EH577 captured data to justify implementing the `FpDevice` + embedded NCC plan?

For these two folders, **yes**.
