# Stage-2 threshold tuning so far: best working capture gate is grain 2.0%, minutiae 2, ridge pixels 600

Date: 2026-06-12

## Context

After the Stage-2 processed-image gate was implemented in
[egis0577.c](../../../refs/libfprint/libfprint/drivers/egis0577.c), the initial
strict defaults were:

- grain `< 0.300%`
- minutiae `>= 4`
- ridge pixels `>= 1000`

In real capture testing via [tools/capture12.sh](../../../tools/capture12.sh), that
proved too strict for the current EH577 output. The driver spent too much time
rejecting touches before even getting to save a capture.

## Thresholds finally used in live testing

The best working runtime thresholds **so far** were:

```bash
G_MESSAGES_DEBUG=all \
EGIS0577_STAGE2_GRAIN_PCT_X1000=2000 \
EGIS0577_STAGE2_MIN_MINUTIAE=2 \
EGIS0577_STAGE2_MIN_RIDGE_PIXELS=600 \
./tools/capture12.sh
```

That corresponds to:

- grain `< 2.000%`
- minutiae `>= 2`
- ridge pixels `>= 600`

## Interpretation

These are **not** a claim that the low-minutiae problem is solved. They are the
best practical capture-gate thresholds found so far for keeping the Stage-2 gate
usable on real hardware while still rejecting obviously bad touches.

What this tuning means in practice:

- the original `0.300% / 4 / 1000` gate was over-constrained for current live data
- a looser grain gate is needed to admit usable-but-not-perfect presses
- the minutiae floor also has to stay low for now because current clean captures are
  still often sparse
- ridge-area floor `600` is a workable compromise so far between rejecting tiny
  partials and admitting realistic current presses

## Remaining problem explicitly not solved here

The underlying issue remains:

- **real clean captures still tend to have low minutiae counts**

So these tuned thresholds should be treated as the current **best operating point**,
not as the final image-quality answer. The next major problem is still to improve
ridge continuity / coverage / reproducible minutiae, rather than tightening the gate
again immediately.

## Current conclusion

For ongoing capture / enroll / verify work, the best Stage-2 thresholds **so far**
should be considered:

- `EGIS0577_STAGE2_GRAIN_PCT_X1000=2000`
- `EGIS0577_STAGE2_MIN_MINUTIAE=2`
- `EGIS0577_STAGE2_MIN_RIDGE_PIXELS=600`

until better evidence shows a superior operating point.

## Related

- [11-stage1-capture12-quality-analysis.md](11-stage1-capture12-quality-analysis.md)
- [../../../.agents/plans/10-eh577-richer-enroll-best-frame.md](../../../.agents/plans/10-eh577-richer-enroll-best-frame.md)
- [../../todo.md](../../todo.md)
