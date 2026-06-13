# Current EH577 driver stage: stretch5 + Stage-2 gate + noisy-burst recovery

This folder is a snapshot of the current `egis0577.c` / `egis0577.h` implementation copied from `refs/libfprint` after commit `81c9285`.

These files are kept as the current bringup reference stage. They include the latest capture improvements:

- EH577 press/snapshot capture model
- bounded touch turns and best-frame selection
- warm no-finger background subtraction
- stretch5 contrast enhancement
- Stage-2 quality gate: pre-stretch p5, grain, ridge pixels, and minutiae range
- upper minutiae cap because too many minutiae usually means noise on this tiny sensor
- action-aware noisy-burst recovery with fresh baseline/reinit

The active integration source is still:

- `refs/libfprint/libfprint/drivers/egis0577.c`
- `refs/libfprint/libfprint/drivers/egis0577.h`

This folder is for comparison/review only.
