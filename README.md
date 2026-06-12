# libfprint-eh577

Reverse-engineering workspace for an open-source Linux fingerprint driver for the EgisTec EH577 USB sensor (`1c7a:0577`).

> **Sensor model: PRESS, not swipe.** The EH577 is a press / snapshot sensor
> (`FP_SCAN_TYPE_PRESS`) — one frame per touch. It borrows the EH575 *bulk
> command family* on the wire, but it is **not** a swipe sensor. Early swipe /
> strip-assembly modeling was a mistake and has been removed; do not reintroduce
> it.

## Current status

- Repository initialized as a research/work log and code workspace.
- Target hardware detected on this machine:
  - USB VID:PID: `1c7a:0577`
  - Manufacturer: `EgisTec`
  - Product: `EgisTec EH577`
  - USB topology path: `/sys/bus/usb/devices/3-3`
- Stock Ubuntu `fprintd` currently reports **no supported device available**.
- System `libfprint` contains Egis-related drivers for **0570** and Egis MOC devices, but nothing obvious for **0577**.
- Live usbfs probing confirmed that EH577 accepts the full **EH575-style bulk command family** and returns valid `SIGE` responses.
- Idle large-payload captures are zero-filled, but a guided **post-init finger-hold** run produced the first non-zero `5356`-byte payload.
- A first **WIP EH577 libfprint driver skeleton** now exists under `wip-libfprint/`.

## Immediate plan

1. Preserve all local observations in-repo.
2. Confirm reproducibility of the new non-zero post-init finger-hold payload.
3. Compare EH577 USB descriptors/protocol shape against upstream/libfprint Egis drivers.
4. Capture live USB traffic from a working vendor stack if available.
5. Turn the WIP EH577 skeleton into a buildable libfprint patch and iterate from there.

## Notes

- Chronological findings: `docs/research-log.md`
- Side-by-side protocol family notes: `docs/protocol-comparison.md`
- Next actions: `docs/todo.md`
- Direct probe tool: `tools/eh577_usbfs_probe.c`
- Guided finger-timing helper: `tools/eh577_guided_capture.sh`
- Payload-to-image helper: `tools/eh577_dump_to_pgm.py`
- WIP libfprint port base: `wip-libfprint/`
