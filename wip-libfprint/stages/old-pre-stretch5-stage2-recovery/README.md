# Old EH577 driver stage: pre-stretch5 / pre-Stage2-recovery

This folder is a snapshot of the older `egis0577.c` / `egis0577.h` implementation from `refs/libfprint` immediately before commit `81c9285`.

These files are kept only as a bringup reference point. They represent an earlier stage of the EH577 driver before the latest capture improvements such as stretch5 image enhancement, the current Stage-2 quality gate, minutiae upper-cap noise guard, and action-aware noisy-burst recovery.

Do not treat this folder as the active driver source. The active integration source is still:

- `refs/libfprint/libfprint/drivers/egis0577.c`
- `refs/libfprint/libfprint/drivers/egis0577.h`
