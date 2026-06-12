# EH577 findings summary

This file is the condensed answer to: **what have we already proved, and what did the past investigations teach us?**

## 1. EH577 belongs to the EH575 family

The strongest conclusion from the early probe work is that EH577 is not a new protocol family for Linux bringup purposes.

What was shown:
- EH577 accepts EH575-style `EGIS` bulk requests
- EH577 returns EH575-style `SIGE` replies
- it accepts the EH575 **pre-init**, **post-init**, and **repeat** packet families
- `64 14 ec` returns the expected **5356-byte** payload length

Practical consequence:
- the driver should be built from the EH575 model, not from `egis0570` or `egismoc`

See also:
- `docs/protocol-comparison.md`
- `logs/2026-06-03-eh577-postinit18-analysis.txt`

## 2. Pre-init is the arming step that matters

A major mid-bringup discovery was that the all-zero runtime problem was caused by missing **pre-init**, not by the later image path.

What was shown:
- skipping pre-init leaves the sensor in an unarmed state
- in that state, repeated `64 14 ec` reads are byte-for-byte identical to the known idle zero frame
- once pre-init runs, later post-init loops can produce non-zero frames

Practical consequence:
- the driver must always arm the sensor before entering the normal capture loop

Best reference:
- [01-pre-init-required-to-arm-sensor.md](research-log/2026-06-09/01-pre-init-required-to-arm-sensor.md)

## 3. EH577 really returns image-like data

The non-zero frame captures were reproducible.
They were not one-off noise.

What was shown:
- idle `64 14 ec` payloads are all zero
- finger-interaction captures after proper arming contain large non-zero regions
- multiple successful captures differ from each other, which is what real scan data should do
- the runtime now assembles strips into a visible fingerprint image

Practical consequence:
- the transport and basic capture path are good enough to support a real libfprint driver

Key artifacts:
- `dumps/2026-06-03-postinit-fingerhold-01/`
- `dumps/2026-06-03-postinit-fingerhold-03/`
- `dumps/libfprint-guided-20260610-000413/finger-assembled-136x178.pgm`

## 4. The current blocker moved from capture to matching

Once pre-init was fixed, the patched driver reached image assembly and matcher calls.
The next problem turned out to be **false positives**, not missing frames.

What is now known:
- enroll works
- verify works with the enrolled finger
- but different fingers can still be accepted as `MATCH`

Practical consequence:
- the main work now is image / matcher quality tuning
- protocol discovery is no longer the bottleneck

See:
- `logs/session-20260610-002907.txt`
- `logs/mismatch-20260610-220618.txt`
- `.agents/plans/07-eh577-false-match-debug.md`

## 5. Interrupts are still unresolved, but not required for current bringup

EH577 exposes interrupt IN endpoints `0x83` and `0x84`, but the Linux capture path so far has not depended on them.

What was shown:
- short idle and touch/remove polling windows did not produce useful interrupt payloads
- the working standalone and libfprint capture paths are bulk-based

Practical consequence:
- interrupt support can stay out of the first correctness-focused iteration unless new evidence demands it

## 6. State bytes vary and should not be over-hardcoded

Several small status replies changed across runs and states.
This mattered early because it made naive EH575 assumptions unsafe.

Examples seen in logs:
- `60 00 fc` with `aa` and `ab`
- `60 01 fc` with `01`, `05`, and `00` in the state byte depending on context
- `62 67 03` changing with finger state

Practical consequence:
- use stable transport/length invariants where possible
- treat some status bytes as cues, not fixed truths

## 7. Current mental model

Treat EH577 as:
- a **press / snapshot sensor** (`FP_SCAN_TYPE_PRESS`) that captures one frame
  per touch — **not** a swipe device (swipe/strip-assembly was an early mistake,
  removed in commit `a5a4e7f`; do not reintroduce it),
- borrowing the **EH575 bulk command family** at the transport level only,
- driven primarily over bulk endpoints,
- requiring **pre-init** to arm capture,
- using a software finger-present heuristic for the current Linux path,
- and currently limited more by **matcher quality** than by raw USB transport.

## Read next

- current snapshot: [current-status.md](current-status.md)
- chronological detail: [research-log.md](research-log.md)
- open tasks: [todo.md](todo.md)
