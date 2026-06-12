# 2026-06-04 — libfprint guided touch 06 conservative pacing result

A conservative pacing experiment was run with:

- `EGIS0577_PRE_FRAME_DELAY_MS=20`
- `EGIS0577_POLL_LOOP_DELAY_MS=10`

Artifacts created:

- `logs/2026-06-04-img-capture-guided-touch-06.txt`
- `logs/2026-06-04-img-capture-guided-touch-06-summary.txt`
- `dumps/2026-06-04-libfprint-guided-touch-06/`

### What changed

The runtime log confirmed that the pacing hooks were active:

- frame requests before `64 14 ec` were delayed by 20 ms
- poll-loop restarts were delayed by 10 ms

### What did not change

- `136` dumped runtime frames were captured
- every dumped frame remained all-zero
- all dumped frames had the same SHA-256 hash as the standalone idle frame:
  - `bd2386946403b82cd740767b15507502a27a7d5b4c86440a5af0966c71ad8a38`

### Conclusion

A small pacing change is not sufficient to move the libfprint runtime off the idle-identical zero-frame path.
This weakens the idea that the divergence is caused only by very small loop-timing differences.

### Next branch

Either:

1. try a stronger pacing experiment,
2. or move up to `usbmon` comparison if stronger pacing still yields the same idle-identical payload.
