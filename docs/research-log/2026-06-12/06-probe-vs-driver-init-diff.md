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

## Decisive next test (empirical, not more code)

Speculating from old dumps is unreliable (different finger/pressure/day). Capture
fresh frames **now, same finger**, with the probe in several modes and compare
saturated-pixel counts against the driver's frames:

1. `eh575-postinit` (post-init only) — does the probe still produce clean frames today?
2. pre-init then post-init (mimic the driver's order) — does adding pre-init saturate?
3. `eh575-postinit-reset` — does a USB reset clean it up?

If the probe is clean now and the driver saturates with the same finger → it's the
driver's pre-init or read-cadence. If the probe also saturates now → the 2026-06-03
clean frame was a lighter press and saturation is purely pressure/contact (then the
fix is frame-selection/timing, not init). See the
[probe test skill](../../../tools) workflow.
