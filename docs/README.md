# EH577 docs index

Use this directory for the human-readable project state.

## Start here

- `current-status.md`
  - the current bringup snapshot: what works, what is broken, and where the active code lives
- `todo.md`
  - the live short list of remaining work
- `findings-summary.md`
  - condensed conclusions from the investigation so far

## Detailed references

- `research-log.md`
  - chronological log of experiments, fixes, and conclusions
- `protocol-comparison.md`
  - EH577 compared with EH575, `egis0570`, and `egismoc`
- `plan-enroll-verify.md`
  - how the patched `libfprint` tree is exercised end-to-end

## Code / artifact map

- active driver integration: `refs/libfprint/libfprint/drivers/egis0577.c` and `.h`
- staging mirror: `wip-libfprint/drivers/egis0577.c` and `.h`
- standalone probe: `tools/eh577_usbfs_probe.c`
- probe / runtime logs: `logs/`
- raw frame dumps and rendered images: `dumps/`
- detailed work plans: `.agents/plans/`
