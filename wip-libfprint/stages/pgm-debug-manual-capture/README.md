# Current EH577 driver stage: manual PGM debug capture

This folder is a fresh snapshot of the current EH577 bringup stage after adding
manual PGM-debug capture support.

It mirrors the active integration files from `refs/libfprint`:

- `libfprint/drivers/egis0577.c`
- `libfprint/drivers/egis0577.h`
- `examples/eh577-pgm-debug.c`

Key additions in this stage:

- `EGIS0577_PGM_DEBUG_*` driver mode for repeated manual snapshot dumping.
- Interactive `eh577-pgm-debug` example binary with single-key controls:
  - `f` starts saving frames.
  - `s` stops saving frames.
  - `x` exits.
- Enhanced PGM output uses the same current press-capture processing path:
  background subtraction, active-width crop/dead-strip removal, resize,
  polarity normalization, and stretch5 contrast enhancement.
- Per-frame CSV metrics correspond directly to saved PGM filenames and include
  presence, coverage, intensity, grain, ridge pixels, minutiae, stretch p5/p99,
  and basic pixel stats.
- The parent workspace wrapper is `tools/eh577_pgm_debug.sh`.

The active integration source remains under `refs/libfprint`; this folder is a
review/recovery snapshot only.
