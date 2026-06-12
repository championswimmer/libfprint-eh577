# WIP libfprint driver artifacts

This directory contains local working copies derived from the EH575 reverse-engineering patch, used as the starting point for EH577 support.

## Files

- `drivers/egis0575.c`
- `drivers/egis0575.h`
  - extracted from `refs/EgisTec-EH575/libfprint.patch` for easy local inspection
- `drivers/egis0577.c`
- `drivers/egis0577.h`
  - initial EH577 draft created by mechanically adapting the EH575 driver skeleton

## Important status note

`egis0577.c` / `egis0577.h` are **not yet proven correct**.

What is known so far:

- EH577 (`1c7a:0577`) accepts the early EH575 bulk commands on `0x01` / `0x82`
- EH577 returns valid `SIGE` replies
- EH577 returns `01 01 01` on packet `EGIS 60 01 fc`
  - the EH575 patch interprets that as a cue to switch into the longer pre-init sequence

So this WIP draft is intended as a **porting base**, not a finished driver.

## Next implementation steps

1. Replay the full `eh575-auto` sequence against EH577.
2. Record the first packet where EH577 diverges from EH575, if any.
3. Confirm whether packet `64 14 ec` returns a `5356`-byte payload.
4. Decide whether interrupt endpoints `0x83` / `0x84` are required for production use.
5. Once sequence behavior is confirmed, turn this draft into an actual libfprint patch.

## Older Implementation & Debugging Details

The `egis0577.c` and `egis0577.h` files preserved in this WIP directory reflect an earlier stage of reverse engineering before major simplifications were applied to the main integration tree. They contain several debugging artifacts that were highly useful during the initial bringup phase:

1. **Dynamic Environment Tuning**: Rather than hardcoding timing delays and image quality thresholds, this iteration parses configuration values at runtime via environment variables (e.g., `EGIS0577_PRE_FRAME_DELAY_MS`, `EGIS0577_STAGE2_GRAIN_PCT_X1000`). This allowed rapid iteration to find stable operating parameters without recompiling.
2. **EH575 `REPEAT_PACKETS` Legacy Path**: This version retains the logic for the `EGIS0577_REPEAT_PACKETS` sequence inherited from the EH575 driver. It was eventually discovered that entering this path consumes the limited frame-read budget of the EH577 device, which would cause transport wedging, but the code is kept here for historical reference.
3. **Raw Frame Dumping**: The codebase includes `dump_frame_if_requested`, enabling raw 5356-byte image frames to be dumped to disk (via `EGIS0577_FRAME_DUMP_DIR`) for offline analysis with tools like `capture12` to build the Stage-2 minutiae gate.
