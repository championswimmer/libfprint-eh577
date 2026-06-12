# 2026-06-03 — first live protocol probe on EH577

A small standalone usbfs probe was added:

- source: `tools/eh577_usbfs_probe.c`
- binary: `build/eh577_usbfs_probe`

This probe talks directly to `/dev/bus/usb/...` using `USBDEVFS_BULK`, so it does not depend on libusb headers or pyusb.

### Probe setup

- Device power control was forced to `on` via sysfs.
- Runtime status became `active`.
- The probe claimed interface 0 and sent the first EH575 post-init bulk packets over:
  - OUT `0x01`
  - IN `0x82`

### Critical result: EH577 accepts EH575 packets

Observed successful exchanges:

1. `EGIS 60 00 fc` -> `SIGE 00 aa 01`
2. `EGIS 60 01 fc` -> `SIGE 01 01 01`
3. `EGIS 60 40 fc` -> `SIGE 40 80 01`
4. `EGIS 63 09 0b 83 24 00 44 0f 08 20 20 01 05 12`
   -> `SIGE 09 0b 01 83 24 00 44 0f 08 20 20 01 05 12`
5. `EGIS 63 26 06 06 60 06 05 2f 06`
   -> `SIGE 26 06 01 06 60 06 05 2f 06`

#### Important nuance discovered after comparing with the EH575 driver logic

The EH575 patch does **not** simply continue after packet 1.

- In `egis0575.c`, when the response to `EGIS 60 01 fc` has byte 5 equal to `0x01`, the driver switches into the longer `PRE_INIT` sequence.
- Our first raw probe kept sending later post-init packets anyway, and EH577 still accepted them.

This means one of two things is true:

1. EH577 is permissive and accepts both paths, or
2. our first probe proved wire compatibility but not yet the *canonical* init path.

So future probing should prefer an **auto-init** mode that follows the EH575 branch rule.

This is the strongest finding so far.

#### What this proves

- EH577 definitely understands **EH575-style `EGIS` bulk commands**.
- EH577 definitely returns **EH575-style `SIGE` responses**.
- The `60 01 fc -> 01 01 01` response exactly matches a state cue already documented in the EH575 patch comments.

So EH577 is no longer just *suspected* to be similar to EH575; it has now been shown to be **wire-compatible with at least the early EH575 init sequence**.

### Notes on access friction

- Running the probe as the unprivileged user fails with `open: Permission denied` on `/dev/bus/usb/003/004`.
- Some `sudo` invocations worked during probing, but later attempts became inconsistent and started requiring interactive authentication again.
- Because of that, a full replay of all 18 EH575 post-init packets and interrupt polling is still pending.

### Additional artifacts created after the first live probe

#### Improved standalone probe

`tools/eh577_usbfs_probe.c` was expanded to include:

- `reset` mode via `USBDEVFS_RESET`
- proper interrupt URB polling for endpoints `0x83` / `0x84`
- explicit `eh575-preinit`, `eh575-postinit`, and `eh575-repeat` modes
- `eh575-auto` mode that follows the EH575 branch rule and switches into pre-init when packet `60 01 fc` returns the `01 01 01` state

This should reduce ambiguity in future captures and make it easier to identify the first real divergence.

#### WIP libfprint port base

A local driver work area now exists under `wip-libfprint/`:

- `wip-libfprint/drivers/egis0575.c`
- `wip-libfprint/drivers/egis0575.h`
  - extracted from the EH575 patch for direct local reference
- `wip-libfprint/drivers/egis0577.c`
- `wip-libfprint/drivers/egis0577.h`
  - first EH577 draft derived from the EH575 driver skeleton

Current status of the EH577 draft:

- device ID changed to `1c7a:0577`
- component / full-name strings renamed to EH577
- sequence tables still intentionally mirror EH575 until live captures show otherwise
- interrupt endpoints are still not integrated into the libfprint draft yet
