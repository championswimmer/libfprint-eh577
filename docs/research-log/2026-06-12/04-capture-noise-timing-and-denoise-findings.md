# Capture saturation noise: source, timing, and why denoising failed

Date: 2026-06-12

## Context

Same-finger identify kept scoring a flat `0/12` against all 15 enrolled templates,
with frequent `Failed to detect minutiae: No minutiae found`. Raw 5356-byte frames
were captured for the first time via `EGIS0577_FRAME_DUMP_DIR`
(see [enroll_identify.sh](../../../tools/enroll_identify.sh)) and analyzed offline.

Evidence sessions:
- [artifacts/pgm-debug/20260612-052326](../../../artifacts/pgm-debug/20260612-052326) — raw frames, pre-denoise driver
- [artifacts/pgm-debug/20260612-053509](../../../artifacts/pgm-debug/20260612-053509) — raw frames, with 3x3 median denoise

## What the raw frames show

The frames contain **real fingerprint ridges** (faint diagonal parallel ridges,
partial coverage, right ~40% blank) **plus heavy salt-and-pepper saturated (255)
hot-pixels**. That impulse noise is what defeats NBIS MINDTCT minutiae extraction →
score 0. Key measurements:

- **Saturation is universal**: every one of 394 finger frames saturates (76–400
  hot-pixels/frame, *zero* clean frames). The old 2026-06-03 standalone-probe frames
  maxed at value 105 with **no** saturation — so this is a capture-config regression,
  not inherent to the sensor.
- **Hot-pixels are fixed-location**: across 40 frames, 102 of ~173 saturated spots
  recur in ≥50% of frames; only 2 are one-off. Fixed-pattern, not random.
- **Hot-pixels are present in the no-finger baseline**: idle frames already carry
  them (session 053509 id-01 idle: sat≈263; id-02 idle: sat≈120). So most of the
  noise is a fixed sensor pattern, removable by subtracting a real no-finger baseline.
- **Saturation rises with finger pressure**: within an identify capture sequence,
  as the finger presses harder the finger-pixel count and the saturation rise
  together — id-01 went baseline sat≈263 → **449 at peak press**, then a lighter
  plateau (fin≈1140) sat≈195. Peak press is the *noisiest*; moderate contact is
  cleaner.
- **Baseline state varies per run**: id-01 idle nz≈1360 vs id-02 idle nz≈525 — the
  sensor AGC/gain state differs run-to-run, so a fixed background frame is not
  reusable across sessions; the baseline must be captured fresh each capture.

## Why 3x3 median denoising was the wrong fix

A 3x3 median removed the hot-pixels offline (91–400 → 1–18 residual) and *looked*
clean on best-case frames, but on real ridges (~2–3 px wide) it **erodes and merges
ridges into mushy blobs**, which is its own minutiae killer. Adding it did change the
images (enroll PGM min 154→7, more contrast) but identify still scored 0, and the
ridges were visibly degraded. Conclusion: do not denoise destructively.

## Direction (for next session)

1. **Cut back denoising** — drop the median filter; it damages ridge structure.
2. **Attack the noise at the source / with non-destructive means**:
   - Subtract a **fresh no-finger baseline** captured at the start of each capture
     (the hot-pixels live in the baseline, so this removes them without touching
     ridges) — but the baseline must contain the hot-pixels, i.e. capture it from a
     real warm no-finger frame, not an all-zero cold frame.
   - **Timing / frame selection**: prefer a *moderate-contact* frame (finger-pixels
     ~1000–1400) over the peak-press frame (finger-pixels >1600, saturation >400).
     The driver currently accepts whatever first passes the usable threshold, which
     can be a peak-saturation frame.
3. Open question worth chasing: the saturation is a **capture-config regression** vs
   the 2026-06-03 standalone probe (which produced clean, unsaturated frames).
   Diffing the probe's init/gain register sequence against the driver's pre/post-init
   could remove the noise at the hardware-config level — the cleanest fix of all.

See also: [[../../current-status.md]], driver
[egis0577.c](../../../refs/libfprint/libfprint/drivers/egis0577.c) `process_imgs` /
`save_img`. Capture model is PRESS, not swipe.
