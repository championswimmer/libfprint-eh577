# 2026-06-04 — libfprint guided touch 07 strong pacing result

A stronger pacing experiment was run with:

- `EGIS0577_PRE_FRAME_DELAY_MS=100`
- `EGIS0577_POLL_LOOP_DELAY_MS=50`

Artifacts created:

- `logs/2026-06-04-img-capture-guided-touch-07.txt`
- `logs/2026-06-04-img-capture-guided-touch-07-summary.txt`
- `dumps/2026-06-04-libfprint-guided-touch-07/`

### What changed

The runtime log confirmed that the stronger pacing hooks were active:

- frame requests before `64 14 ec` were delayed by 100 ms
- poll-loop restarts were delayed by 50 ms

### What did not change

- `34` dumped runtime frames were captured
- every dumped frame remained all-zero
- all dumped frames had the same SHA-256 hash as the standalone idle frame:
  - `bd2386946403b82cd740767b15507502a27a7d5b4c86440a5af0966c71ad8a38`

### Conclusion

Even a much stronger pacing change did not move the libfprint runtime off the idle-identical zero-frame path.
This makes a simple loop-timing mismatch less likely as the primary remaining explanation.

### Next branch

Promote `usbmon` to the next local step so the patched runtime USB sequence can be compared directly against the successful standalone post-init finger-hold flow.
