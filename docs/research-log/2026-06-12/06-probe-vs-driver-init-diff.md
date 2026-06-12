# Probe vs driver init diff — sequences are identical, cadence differs

Date: 2026-06-12

## Question

The 2026-06-03 standalone-probe finger frame was clean (max value 105, **no**
saturated hot-pixels), while the current libfprint driver produces frames saturated
at 255 with 76–400 hot-pixels each. Hypothesis: the driver's init/gain register
sequence differs from the probe's and leaves the sensor at a saturating gain.

## Finding: the register sequences are byte-for-byte identical

Diffed [tools/eh577_usbfs_probe.c](../../../tools/eh577_usbfs_probe.c)
`eh575_pre_init[]` / `eh575_post_init[]` against the driver's
[egis0577.h](../../../refs/libfprint/libfprint/drivers/egis0577.h)
`EGIS0577_PRE_INIT_PACKETS` / `EGIS0577_POST_INIT_PACKETS` (whitespace-normalized):

- **PRE: 29 packets — IDENTICAL.**
- **POST: 18 packets — IDENTICAL.**

So there is **no init-register / gain-config difference** to exploit. The saturation
is not caused by the sensor being configured differently.

## The real difference is read cadence / session structure

- **Probe** (`eh575-postinit` mode, which produced the clean dumps): sends the
  18-packet post-init sequence **once** and reads exactly **one** `64 14 ec` frame,
  then releases and exits (`run_sequence`, one pass). No pre-init in that mode.
- **Driver**: runs PRE_INIT (29) then POST_INIT (18), reads a frame, then **loops** —
  re-running POST_INIT before every frame and reading **hundreds** of frames per
  session, recycling the interface claim (full PRE_INIT+POST_INIT) every 6 reads.

Other notes:
- The probe's clean dump was `eh575-postinit` (post-init only, **no** pre-init); the
  driver always runs pre-init first. Worth testing whether pre-init contributes.
- The probe's clean frame had nonzero≈1594 (a lighter press) vs the driver's finger
  frames at nonzero≈2000–2600 — consistent with the
  [pressure-driven saturation finding](04-capture-noise-timing-and-denoise-findings.md).
- There is also a `eh575-postinit-reset` probe mode that USB-resets after claim
  (EH575 Rust-prototype style); the driver never resets. Untested for saturation.

## RESULT (2026-06-12 probe captures, same finger, via tools/probe-tests/)

Captured one `64 14 ec` finger frame per mode and measured saturated pixels:

| frame | nonzero | saturated(>=250) | finger px | max |
|---|---|---|---|---|
| probe postinit-only | 665 | 153 | 287 | 255 |
| probe postinit-**reset** | 665 | 154 | 283 | 255 |
| probe preinit+postinit | 377 | 160 | 123 | 255 |
| driver (today) | ~2700 | ~480 | ~1600 | 255 |

**The probe saturates in every mode today** — with/without pre-init, with/without
USB reset, single-shot. So pre-init order, read cadence, and the missing USB reset
are all **ruled out** as the cause. The init-diff direction yields no copyable fix:
probe and driver init are byte-identical AND both saturate.

## IMPORTANT caveat (per user, 2026-06-12)

Ignore all pre-2026-06-06 results — they were captured/reasoned under the wrong
**swipe**-sensor assumption (EH577 is PRESS). That includes the 2026-06-03
"clean, unsaturated (max=105, sat=0)" probe frame this entry originally leaned on.
**There is therefore no validated evidence that the sensor ever produced
unsaturated frames.** The "saturation is a regression from a previously clean
state" framing is dropped; saturation may simply be how this sensor/init behaves.

## Where this leaves it

- No "better init" hidden in the probe to copy — both paths saturate identically.
- Open levers (all still within PRESS): (a) experiment with the gain/exposure
  registers in the init sequence (`63 09 ...`, `63 26 ...`) to reduce saturation at
  the source; (b) non-destructive **fresh no-finger baseline subtraction** to remove
  the fixed hot-pixels; (c) moderate-contact frame selection.
