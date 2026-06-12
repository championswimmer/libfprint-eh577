# EH577 todo

This is the short, current work list.
For the detailed running plans, see `.agents/plans/`.

## Highest priority

### 1. Fix false matches

Goal: same finger reliably verifies as `MATCH`, different finger reliably verifies as `NO MATCH`.

Plan:
- `.agents/plans/07-eh577-false-match-debug.md`

Open tasks:
- [ ] Capture and compare `enrolled.pgm` vs `verify.pgm` for mismatched-finger runs
- [ ] Record minutiae counts and bozorth3 scores from debug logs
- [ ] Tune `EGIS0577_BZ3_THRESHOLD`
- [ ] Test whether resize settings are helping or hurting
- [ ] Improve press-snapshot ridge area / contrast (NOT swipe assembly — press only)
- [ ] Confirm 3× same-finger `MATCH` and 3× different-finger `NO MATCH`

### 2. Finalize touch / finger-present / temperature guards

Plan:
- `.agents/plans/08-eh577-touch-temperature-guards.md`

Open tasks:
- [x] Decide the long-term `finger_present` threshold / hysteresis policy
- [x] Decide whether EH577 should explicitly opt into or out of libfprint temperature throttling
- [x] Validate idle / touch / hold / remove behavior after guard tuning
- [x] Record the chosen guard policy in both code and docs

## Medium priority

### 3. Decide what still needs protocol follow-up

- [ ] Decide whether interrupt endpoints `0x83` / `0x84` can stay unsupported in the first serious patch
- [ ] If capture behavior still looks suspicious, run the usbfs-vs-runtime `usbmon` comparison from plan 06
- [ ] Extract more useful clues from the Windows EH577 package only if they help the remaining guard / power-management questions

### 4. Keep the code paths aligned

- [ ] Mirror the final EH577 behavior cleanly between `refs/libfprint/` and `wip-libfprint/`
- [ ] Keep code comments aligned with the current evidence, especially around state bytes and pre-init requirements

## Upstreaming / polish

- [ ] Collect a clean evidence set for the final state: working enroll, working same-finger verify, working different-finger reject
- [ ] Decide the minimum acceptable scope for a first upstreamable patch
- [ ] Prepare the patch / patch series from `refs/libfprint/`

## Recently completed

- [x] Proved EH575-family protocol compatibility on live EH577 hardware
- [x] Identified that **pre-init** is required to arm the sensor
- [x] Captured reproducible non-zero `64 14 ec` frames
- [x] Integrated `egis0577` into the local `refs/libfprint/` tree and built it successfully
- [x] Reached press-snapshot fingerprint image output through libfprint
- [x] Reached end-to-end enroll / verify execution with the patched driver
- [x] Tightened the finger-present gate to reduce phantom captures
