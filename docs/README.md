# EH577 docs index

Use this directory for the human-readable project state.

## Start here

- [current-status.md](current-status.md)
  - the current bringup snapshot: what works, what is broken, and where the active code lives
- [todo.md](todo.md)
  - the live short list of remaining work
- [findings-summary.md](findings-summary.md)
  - condensed conclusions from the investigation so far

## Detailed references

- [research-log.md](research-log.md)
  - chronological log of experiments, fixes, and conclusions (index to daily splits)
- [protocol-comparison.md](protocol-comparison.md)
  - EH577 compared with EH575, `egis0570`, and `egismoc`
- [plan-enroll-verify.md](plan-enroll-verify.md)
  - how the patched `libfprint` tree is exercised end-to-end

## Code / artifact map

- active driver integration: [egis0577.c](../refs/libfprint/libfprint/drivers/egis0577.c) and [egis0577.h](../refs/libfprint/libfprint/drivers/egis0577.h)
- staging mirror: [egis0577.c](../wip-libfprint/drivers/egis0577.c) and [egis0577.h](../wip-libfprint/drivers/egis0577.h)
- standalone probe: [eh577_usbfs_probe.c](../tools/eh577_usbfs_probe.c)
- probe / runtime logs: [logs/](../logs/)
- raw frame dumps and rendered images: [dumps/](../dumps/)
- detailed work plans: [.agents/plans/](../.agents/plans/)
