# Current EH577 driver stage: Fast Exit + Adjusted Thresholds

This folder is a snapshot of the current `egis0577.c` / `egis0577.h` implementation copied from `refs/libfprint`.

These files are kept as the current bringup reference stage. They include the latest improvements over the previous stretch5 + stage2 recovery snapshot:

- Increased finger settling time from 200ms to 300ms.
- "Fast exit" logic: frames in the 300ms..1200ms capture window are immediately submitted if they pass the Stage-2 quality gate (minutiae, grain, stretch, etc.), bypassing the full timeout.
- Tweaked Stage-2 thresholds to reduce false rejections / false acceptances:
  - Minimum minutiae increased from 2 to 4.
  - Maximum minutiae decreased from 18 to 16.
  - Minimum ridge pixels increased from 400 to 600.
  - Grain noise threshold increased from 4.000% to 6.000%.

The active integration source is still:
- `refs/libfprint/libfprint/drivers/egis0577.c`
- `refs/libfprint/libfprint/drivers/egis0577.h`

This folder is for comparison/review only.
