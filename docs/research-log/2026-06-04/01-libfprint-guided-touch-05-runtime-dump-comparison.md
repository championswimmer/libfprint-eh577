# 2026-06-04 — libfprint guided touch 05 runtime dump comparison

After a cold reboot, `tools/eh577_guided_img_capture.sh` was used to run a fresh guided `img-capture` session with the new `EGIS0577_FRAME_DUMP_DIR` runtime dump hook enabled.

Artifacts created:

- `logs/2026-06-04-img-capture-guided-touch-05.txt`
- `logs/2026-06-04-img-capture-guided-touch-05-summary.txt`
- `dumps/2026-06-04-libfprint-guided-touch-05/`

### What the runtime actually did

- EH577 opened successfully after reboot.
- The patched EH577 runtime stayed on the **post-init** polling path.
- The dump hook captured `1380` raw runtime `64 14 ec` buffers.
- Every dumped buffer was exactly `5356` bytes.
- Every dumped buffer had `0` non-zero bytes.
- All `1380` dumped runtime frames had the same SHA-256 hash:
  - `bd2386946403b82cd740767b15507502a27a7d5b4c86440a5af0966c71ad8a38`

### Direct comparison with known standalone captures

The runtime dump hash was compared against prior standalone probe artifacts:

- standalone idle post-init frame:
  - `dumps/2026-06-03-postinit18/eh575-postinit-17-64_14_ec.bin`
  - SHA-256 `bd2386946403b82cd740767b15507502a27a7d5b4c86440a5af0966c71ad8a38`
- standalone successful finger-hold frame 01:
  - SHA-256 `51e6dcd08a54f9d1aeed5269e362e4720c2c3835994c586bdac299df569c95c3`
- standalone successful finger-hold frame 03:
  - SHA-256 `826fc28994c7371d70761ad6444b2ddcd9322bab0547a2dd74d5f4eeb09f4e28`

Comparison result:

- **runtime frame == standalone idle frame**
- **runtime frame != successful finger-hold frame 01**
- **runtime frame != successful finger-hold frame 03**

### Conclusion

This is stronger than simply observing that libfprint frames are all-zero.
It shows that the current libfprint runtime is receiving the same byte-for-byte idle payload that the standalone probe saw when no meaningful finger capture occurred.

That means the current blocker is **not primarily the finger heuristic or image assembly path**.
The remaining divergence is earlier:

- timing/pacing of the runtime loop,
- missing runtime arming/setup state,
- or some other state transition difference before meaningful data generation.

### Recommended next branch

1. compare runtime post-init loop pacing against the successful standalone post-init finger-hold flow
2. try a conservative timing/pacing experiment before `64 14 ec` or between post-init loops
3. escalate to `usbmon` only if timing experiments still do not expose the divergence
