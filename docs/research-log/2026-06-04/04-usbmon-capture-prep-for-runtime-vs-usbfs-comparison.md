# 2026-06-04 — usbmon capture prep for runtime-vs-usbfs comparison

Plan 06 (`.agents/plans/06-eh577-usbmon-runtime-comparison.md`) was started to prepare side-by-side `usbmon` captures for:

1. the patched `libfprint` runtime path, and
2. the known-good standalone usbfs post-init path.

### Prep completed

- verified the intended artifact names and created fresh dump directories for the first runtime and standalone capture pair
- earlier in the same session, confirmed that EH577 was healthy enough for the libusb smoketest and that `usbmon3` exists locally
- added `tools/eh577_usbmon_capture.sh` so one command can be wrapped with start/stop of `tcpdump`
- updated `tools/eh577_guided_capture.sh` so agent-harness runs no longer hard-fail immediately when upfront sudo validation is unavailable non-interactively

### Blocker observed

Generic `sudo tcpdump ...` still may require interactive authentication from this harness even when targeted EH577 probe commands sometimes work with `sudo -n`.

That means the actual runtime and standalone `usbmon` traces should be launched from an interactive terminal session rather than relying on a fully unattended agent run.

### Practical next step

Use the new wrapper and the filenames recorded in plan 06 / `logs/2026-06-04-usbmon-capture-prep-summary.txt` to collect:

- `logs/2026-06-04-usbmon-libfprint-runtime-01.pcap`
- `logs/2026-06-04-usbmon-usbfs-postinit-01.pcap`

with the matching runtime/probe text logs and dump directories, then compare the first divergence before the runtime remains stuck on the idle-identical zero `64 14 ec` payload.
