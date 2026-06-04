# EH577 Guided Touch Continuation

## Goal

Continue the live EH577 runtime work with a safer, more disciplined guided-touch loop that only starts when the device is healthy, captures the exact runtime behavior under the latest driver changes, and distinguishes clearly between:

- USB-open crashes,
- idle zero-frame polling,
- successful non-zero frame capture,
- and actual finger/image pipeline progress.

The concrete outcome is:

- guided-touch retries are performed in a reproducible order,
- preflight failures are separated from runtime failures,
- the next successful or failed run produces a useful log artifact,
- and the work can continue without repeating avoidable bad runs.

## Context

- Relevant files:
  - `.agents/plans/03-eh577-runtime-capture-timeout.md`
  - `AGENTS.md`
  - `docs/research-log.md`
  - `tools/eh577_guided_capture.sh`
  - `tools/eh577_guided_img_capture.sh`
  - `build/eh577_libusb_smoketest`
  - `refs/libfprint/libfprint/drivers/egis0577.c`
  - `wip-libfprint/drivers/egis0577.c`
  - `logs/2026-06-04-img-capture-guided-touch-02-summary.txt`
  - `logs/2026-06-04-img-capture-guided-touch-03-failure-summary.txt`
- Current validated runtime state:
  - EH577 driver no longer dies in the PRE_INIT misbranch.
  - EH577 can complete post-init and poll within libfprint when the device stays healthy.
  - Guided touch 02 showed only zero frames while the runtime was using the wrong post-init/repeat behavior.
  - The driver has now been patched again to stay on post-init polling.
  - Guided touch 03 did not test that change because the device crashed at USB-open time first.
- Current safety improvements:
  - `tools/eh577_guided_img_capture.sh` now does:
    - USB presence preflight
    - live libusb preflight
    - sudo-upfront execution
    - automatic log tail on failure
- Constraints:
  - A bad run can wedge the device until a cold reboot.
  - Guided runs should only be started after the wrapper preflight passes.
  - Each run should produce a distinct named log file.

## Steps

1. Confirm the device is healthy after recovery before every guided run.
2. Use the new wrapper as the only supported path for guided `img-capture` retries.
3. Run one focused touch-hold experiment at a time and inspect the log immediately.
4. Classify each run as preflight failure, open crash, zero-frame runtime, or meaningful runtime progress.
5. Only after a clean runtime log, decide whether the next change should be driver logic, timing, or a deeper USB trace.

## Validation

This plan is successful when all of the following are true:

- the next guided run is clearly classified before any further code change,
- the wrapper prevents at least one avoidable bad run or clearly reports why it refused to start,
- a subsequent successful runtime log shows whether post-init polling changed frame content or finger-state behavior,
- and the next action is chosen from evidence rather than repeated guesswork.

## Todo

### Phase 0 — enforce safe run discipline

- [x] Only start the next guided run after a fresh recovery and successful wrapper preflight
- [x] Use a fresh log filename for every attempt
- [x] Record whether the wrapper failed in preflight, failed at open, or reached packet exchange

### Phase 1 — test the new post-init-polling behavior

- [x] Run a guided touch attempt with the current post-init-polling driver
- [x] Check whether the run reaches packet exchange after open
- [ ] Check whether any `64 14 ec` frame becomes non-zero
- [ ] Check whether finger status ever transitions to `on`
- [ ] Check whether an image is assembled or submitted

### Phase 2 — classify the outcome

- [x] If preflight fails, record that separately as a USB-health blocker
- [x] If open fails with `errno=5`, record that separately as a USB-open crash blocker
- [x] If runtime reaches polling but frames stay zero, record that as a capture-mode blocker
- [ ] If non-zero frames appear but finger detection still fails, record that as a heuristic/pipeline blocker

### Phase 3 — choose the next evidence-backed branch

- [x] If post-init polling still gives all-zero frames, decide whether to add targeted frame dumps inside libfprint
- [ ] Add targeted frame dumps inside libfprint and collect a guided-touch runtime sample
- [ ] If post-init polling gives non-zero frames without progress, inspect the finger heuristic threshold next
- [ ] If runtime diverges in an unclear way, promote `usbmon` capture to the next local step
- [ ] If runtime progresses meaningfully, fold the result back into the fprintd-enroll path

### Phase 4 — documentation and handoff

- [x] Update `logs/` with a summary for the next guided run result
- [ ] Update `.agents/plans/03-eh577-runtime-capture-timeout.md` with what was learned
- [ ] Update `docs/research-log.md` once the next guided result is classified
- [x] Leave clear instructions for the exact next live step
- [x] Record when USB health is the active blocker instead of runtime logic