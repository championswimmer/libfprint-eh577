# libfprint-eh577

Reverse-engineering workspace for an open-source Linux fingerprint driver for the EgisTec EH577 USB sensor (`1c7a:0577`).

## Current status

- Repository initialized as a research/work log and code workspace.
- Target hardware detected on this machine:
  - USB VID:PID: `1c7a:0577`
  - Manufacturer: `EgisTec`
  - Product: `EgisTec EH577`
  - USB topology path: `/sys/bus/usb/devices/3-3`
- Stock Ubuntu `fprintd` currently reports **no supported device available**.
- System `libfprint` contains Egis-related drivers for **0570** and Egis MOC devices, but nothing obvious for **0577**.
- Live usbfs probing already confirmed that EH577 accepts early **EH575-style `EGIS` bulk commands** and returns valid `SIGE` responses.
- A first **WIP EH577 libfprint driver skeleton** now exists under `wip-libfprint/`.

## Immediate plan

1. Preserve all local observations in-repo.
2. Study the prior EH575 reverse-engineering effort.
3. Compare EH577 USB descriptors/protocol shape against upstream/libfprint Egis drivers.
4. Capture live USB traffic from a working vendor stack if available.
5. Build a minimal open driver skeleton and iterate toward probe/init/interrupt handling.

## Notes

- Chronological findings: `docs/research-log.md`
- Side-by-side protocol family notes: `docs/protocol-comparison.md`
- Next actions: `docs/todo.md`
- Direct probe tool: `tools/eh577_usbfs_probe.c`
- WIP libfprint port base: `wip-libfprint/`
