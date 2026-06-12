# EH577 current status

## Summary

The EH577 bringup is past the protocol-discovery phase.

**Capture model: PRESS (`FP_SCAN_TYPE_PRESS`) — one snapshot frame per touch,
not swipe.** Swipe / multi-strip assembly was an early mistake, removed in commit
`a5a4e7f`; do not reintroduce it.

The project now has a real `libfprint` driver port in `refs/libfprint/` that can:

- open the device,
- run **pre-init** to arm the sensor,
- enter the **post-init** capture loop,
- capture **non-zero** `64 14 ec` frames,
- build a single **press snapshot** image,
- and complete **enroll** / **verify** flows with libfprint example programs.

The main unresolved issue is **biometric correctness**: different fingers can still be accepted as `MATCH`.

## What is proven

### Protocol / transport

- EH577 is wire-compatible with the **EH575** bulk protocol family.
- Bulk endpoints used by the current driver path:
  - OUT `0x01`
  - IN `0x82`
- Interrupt endpoints `0x83` / `0x84` exist on the device, but they have not been required for the working Linux capture path.

### Sensor state model

- **Pre-init is required** to arm the sensor.
- If pre-init is skipped, `64 14 ec` reads stay all-zero and the runtime behaves like the known idle path.
- After pre-init, post-init polling can produce meaningful non-zero captures during finger interaction.

Useful register cues observed so far:

| Read | Armed / useful state | Unarmed / idle-zero state |
|---|---|---|
| `60 01 fc` byte 5 | `0x00` | `0x05` |
| `60 40 fc` byte 5 | `0x00` | `0x80` |
| `62 67 03` bytes 8-9 | non-zero during finger activity | `ff 00` / `00 00` |

### Driver/runtime status

The active EH577 driver work is in:

- `refs/libfprint/libfprint/drivers/egis0577.c`
- `refs/libfprint/libfprint/drivers/egis0577.h`

Current validated behavior:

- pre-init runs once on open
- post-init loop polls for a finger-present frame
- a single accepted press snapshot frame becomes the fingerprint image
- end-to-end enroll/verify runs complete through libfprint example binaries

Important supporting artifacts:

- first assembled image milestone:
  - `logs/libfprint-guided-20260610-000413.txt`
  - `dumps/libfprint-guided-20260610-000413/finger-assembled-136x178.pgm`
- successful enroll/verify session:
  - `logs/session-20260610-002907.txt`
- known false-match session:
  - `logs/mismatch-20260610-220618.txt`

## What changed recently

- The driver was fixed to always run **pre-init first** instead of relying on the earlier EH575-style branch assumption.
- The libfprint path now reaches real image assembly and matcher invocation.
- Finger-presence gating was tightened (`EGIS0577_MIN_ACTIVE_PIXELS` increased to 1000) to reduce phantom / idle-triggered captures.

## Main problems still open

### 1. False matches

The current blocker is not transport anymore; it is match quality.

Observed problem:
- enroll one finger
- verify with a different finger
- verify can still report `MATCH`

The active debug track is in:
- `.agents/plans/07-eh577-false-match-debug.md`

Likely causes under test:
- Bozorth3 threshold too permissive
- snapshot image quality / ridge area still too weak
- resize / contrast / capture-timing tuning not yet right (within press model)

### 2. Touch / finger-present / temperature guards

The driver currently relies on a software finger-present heuristic.
That is good enough to capture, but not yet a final guard policy.

The active follow-up is in:
- `.agents/plans/08-eh577-touch-temperature-guards.md`

Open points:
- settle final finger-present threshold / hysteresis
- decide explicit `temp_hot_seconds` policy
- determine whether Windows detect-mode clues map to real EH577 commands worth implementing later

### 3. Interrupts and deeper protocol evidence

So far, interrupts have been silent in the Linux tests that were run.
They may still matter for:
- remote wake / low-power behavior
- OEM-style touch detection
- a future polished upstream driver

This is a follow-up question, not the current blocker.

## Where to work

- authoritative driver integration tree: `refs/libfprint/`
- smaller staging mirror: `wip-libfprint/`
- standalone probe and helpers: `tools/`
- evidence logs: `logs/`
- raw dumps / PGM output: `dumps/`

## Suggested next reads

1. `docs/todo.md`
2. `docs/findings-summary.md`
3. `.agents/plans/07-eh577-false-match-debug.md`
4. `.agents/plans/08-eh577-touch-temperature-guards.md`
