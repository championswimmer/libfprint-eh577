# EH577 Runtime Capture Timeout

## Goal

Debug and fix the current runtime failure where the patched `egis0577` driver enumerates, opens, and activates successfully in `libfprint`, but `img-capture` and `fprintd-enroll` stall at `AWAIT_FINGER_ON` and eventually time out instead of reaching the meaningful post-init-led capture path already proven by the standalone probe.

The concrete outcome is:

- the failure stage is narrowed precisely,
- the EH577 driver state machine is adjusted based on evidence,
- `img-capture` progresses past the current timeout,
- and patched `fprintd-enroll` is re-tested without modifying the system installation.

## Context

- Relevant files:
  - `AGENTS.md`
  - `docs/research-log.md`
  - `.agents/plans/02-eh577-driver-bringup.md`
  - `logs/2026-06-03-libfprint-build-validation.txt`
  - `logs/2026-06-03-eh577-finger-analysis.txt`
  - `logs/2026-06-03-eh577-postinit-fingerhold-reproducibility.txt`
  - `logs/2026-06-03-img-capture-smoketest.txt`
  - `logs/2026-06-04-patched-fprintd-daemon-enroll.txt`
  - `logs/2026-06-04-patched-fprintd-enroll.txt`
  - `wip-libfprint/drivers/egis0577.c`
  - `wip-libfprint/drivers/egis0577.h`
  - `refs/libfprint/libfprint/drivers/egis0577.c`
  - `refs/libfprint/libfprint/drivers/egis0577.h`
- Already validated:
  - EH577 is EH575-family on the bulk protocol.
  - Full EH575 post-init / pre-init / repeat paths are accepted on the live device.
  - Meaningful non-zero `5356`-byte frames were captured twice with the standalone probe during **post-init finger hold**.
  - `egis0577` is integrated into the latest upstream `libfprint` tree and builds successfully.
  - The built stack enumerates EH577 and claims it with the `egis0577` driver.
  - `img-capture` and patched `fprintd-enroll` both currently fail at the same stage: **capture timeout after activation / AWAIT_FINGER_ON**.
- Current working hypothesis:
  - the driver’s runtime state machine is not yet reproducing the same capture timing/loop behavior that the standalone probe used successfully,
  - and/or finger detection/reporting is not transitioning the image pipeline forward correctly.
- Constraints:
  - USB access still requires root or suitable permissions.
  - Do not replace the system `libfprint`/`fprintd`; use `LD_LIBRARY_PATH` and manual daemon runs.
  - `sudo -n` may expire; for longer scripted work prefer helpers that acquire sudo up front.

## Steps

1. Record the current runtime failure boundary clearly.
2. Instrument the EH577 driver around activation, packet sequencing, zero-frame handling, finger-state reporting, and image submission.
3. Compare the runtime driver flow against the standalone probe’s known-good post-init finger-hold behavior.
4. Adjust the runtime state machine conservatively so it can continue through idle zero frames and reach meaningful post-init capture.
5. Rebuild and re-run `img-capture` after each targeted driver change.
6. Once `img-capture` progresses further, re-test patched `fprintd-enroll` on the real system bus.
7. If the runtime behavior still diverges from the standalone probe, capture `usbmon` traces for side-by-side comparison.

## Validation

This plan is successful when all of the following are true:

- the exact current timeout stage is documented in a dedicated log,
- at least one EH577 runtime driver change is justified by logs/captures rather than guesswork,
- `img-capture` progresses beyond the current `AWAIT_FINGER_ON` timeout or the exact remaining blocker is documented precisely,
- patched `fprintd-enroll` is re-tested against the updated driver,
- and the resulting behavior is written back to `docs/research-log.md` and `AGENTS.md`.

## Todo

### Phase 0 — lock the current failure baseline

- [x] Save a concise log describing the current `img-capture` timeout boundary
- [x] Save a concise log describing the current patched `fprintd-enroll` timeout boundary
- [x] Mark in notes that verify is blocked downstream of enroll because no prints are enrolled yet

### Phase 1 — instrument the driver

- [x] Add `fp_dbg()` logging around EH577 activation and state transitions
- [x] Log packet-array selection changes (`POST_INIT`, `PRE_INIT`, `REPEAT`)
- [x] Log when zero frames are seen and what the driver does next
- [x] Log when finger status is reported true/false
- [x] Log when strips/frames are appended and when an image is emitted

### Phase 2 — compare runtime flow vs standalone probe evidence

- [x] Compare driver packet ordering with `tools/eh577_usbfs_probe.c`
- [x] Compare runtime assumptions against the successful post-init finger-hold captures
- [x] Decide whether the current runtime path should force post-init-led polling before any pre-init branch
- [x] Decide that the finger-detection heuristic is not the current blocker because libfprint runtime dumps are byte-identical to the standalone idle zero frame

### Phase 3 — targeted runtime fixes

- [x] Patch the smallest likely blocker in `egis0577.c`
- [x] Rebuild `refs/libfprint` sequentially (`meson compile` then `meson test`)
- [x] Re-run `img-capture` with the patched tree
- [x] Record whether the timeout moved, disappeared, or changed shape
- [x] If still blocked, iterate once more with the next smallest evidence-backed fix

### Phase 4 — patched fprintd runtime re-test

- [ ] Re-run patched `fprintd-enroll` on the real system bus with the updated driver
- [ ] Record whether enrollment still times out, begins swipes, or progresses through stages
- [ ] If enroll progresses, attempt a patched `fprintd-verify` smoke test afterward

### Phase 5 — trace escalation if needed

- [x] Decide that runtime still diverges after timing/pacing experiments
- [ ] Capture `usbmon` for a patched `img-capture` or patched `fprintd-enroll` run
- [ ] Compare the runtime USB sequence against the standalone successful probe sequence
- [ ] Record the first meaningful divergence point

### Phase 6 — documentation and handoff

- [ ] Update `.agents/plans/02-eh577-driver-bringup.md` with what this focused runtime plan completed
- [ ] Update `docs/research-log.md` with the runtime findings
- [ ] Update `AGENTS.md` with the new current blocker or milestone
- [ ] Leave the repo in a state where the next agent can continue from the exact runtime failure point
