# EH577 protocol-family comparison

## Target: EH577 (`1c7a:0577`)

Observed USB descriptor shape:

- bulk OUT `0x01`
- bulk IN `0x82`
- interrupt IN `0x83`
- interrupt IN `0x84`
- vendor-specific interface (`ff/ff/00`)
- high-speed USB 2.0
- direct bulk probing confirmed EH575-compatible `EGIS` / `SIGE` command exchange

## Known nearby Egis families

### Upstream `egis0570`

- Devices: `1c7a:0570`, `1c7a:0571`
- Endpoints:
  - bulk OUT `0x04`
  - bulk IN `0x83`
- Transport traits:
  - 7-byte `EGIS` commands
  - 7-byte `SIGE` responses
  - repeated large `32512`-byte image reads
- Model:
  - image/swipe-style
  - no interrupt endpoint logic in driver

### EH575 reference patch (`egis0575`)

- Device: `1c7a:0575`
- Endpoints used by the patch/prototype:
  - bulk OUT `0x01`
  - bulk IN `0x82`
- Descriptor detail from the patch's test fixture:
  - the actual USB device also exposes interrupt IN `0x83` and `0x84`
- Transport traits:
  - `EGIS`/`SIGE` framing
  - mixed packet sizes: 7 / 9 / 13 / 18 bytes
  - large image-like response size: `5356`
  - commands prominently use `0x60`..`0x64`, `0x73`
- Model:
  - image driver
  - no interrupt endpoint use in the public patch/prototype, even though the device descriptor includes them
  - decompiled Windows driver artifacts mention reads against interrupt endpoint `0x83`, so the OEM stack may use interrupts even if the Linux prototype did not

### Upstream `egismoc`

- Devices: `0582`, `0583`, `0584`, `0586`, `0587`, `0588`, `05a1`
- Endpoints:
  - bulk OUT `0x02`
  - bulk IN `0x81`
  - interrupt IN `0x83`
- Transport traits:
  - 8-byte `EGIS`/`SIGE` wrapper
  - dynamic check bytes
  - semantic commands for list/enroll/identify/delete
- Model:
  - match-on-chip
  - interrupt used for finger-on-sensor notifications

## Current best guess for EH577

EH577 now looks **very close to EH575**, not just descriptively but on the wire:

- the descriptor shape is effectively the same family shape
- bulk endpoint numbering matches exactly
- live probing showed EH577 accepts the full EH575 post-init replay and returns valid `SIGE` responses with the expected lengths
- `64 14 ec` returned a full `5356`-byte payload on EH577
- observed state bytes vary across runs:
  - earlier one-off result: `60 01 fc` -> `SIGE 01 01 01`
  - repeated reset-based runs: `60 01 fc` -> `SIGE 01 05 01`
  - `60 00 fc` also varied between `...00 aa 01` and `...00 ab 01`

So the most plausible starting point is now:

1. EH575-derived bulk command transport as the primary implementation base
2. prefer an EH575-style state machine, but do not assume the packet-1 branch is fixed until EH577 state variation is mapped
3. analyze the `64 14 ec` payload contents and determine when it stops being all-zero
4. monitor `0x83` and `0x84` for optional notifications or power-management wake events

## Implications for implementation

- A first libfprint prototype should probably be cloned from the EH575 driver approach, not from `egis0570` or `egismoc`.
- The first milestone is now narrower and clearer: **port EH575 transport logic to EH577 and find the first protocol divergence**.
- The most valuable next artifacts are:
  - repeated first-packet state samples after controlled resets
  - a saved raw `64 14 ec` payload for entropy/image analysis
  - idle / finger-event interrupt captures from `0x83` and `0x84`
  - confirmation of whether the current `wip-libfprint/` skeleton needs state-machine changes beyond device ID/name updates
