---
name: libfprint-patch-build
description: Use when adapting the EH575 work into an EH577 libfprint driver, editing the local WIP driver files, comparing against upstream libfprint Egis drivers, or building libfprint to validate the EH577 port.
---

# Patch And Build Libfprint

Use this when the task is in the driver porting path rather than the live probe path.

## Read first

Read only what is needed for the current edit:

- `AGENTS.md`
- `wip-libfprint/drivers/egis0577.c`
- `wip-libfprint/drivers/egis0577.h`
- `refs/EgisTec-EH575/libfprint.patch`
- `refs/libfprint/libfprint/drivers/egis0570.c`
- `refs/libfprint/libfprint/drivers/egismoc/egismoc.c`

## Porting stance

Default assumptions:

- EH577 should be treated as EH575-family hardware
- start from the EH575 patch structure, not from scratch
- keep response handling flexible where state bytes have already varied
- do not model EH577 as `egismoc` unless new endpoint or packet evidence forces that

## Preferred edit order

1. Update device IDs and driver registration.
2. Keep sequence tables aligned with the known-good EH575 replay path.
3. Relax reply parsing where state bytes are known to vary.
4. Preserve room for later interrupt integration instead of assuming interrupts are unused.
5. Validate the edited driver against captured logs and dumps before broad refactors.

## Build workflow

The upstream reference tree is under `refs/libfprint/` and uses Meson.

Typical local build flow:

```bash
meson setup refs/libfprint/build refs/libfprint
meson compile -C refs/libfprint/build
meson test -C refs/libfprint/build
```

If the build directory already exists and options changed:

```bash
meson setup --reconfigure refs/libfprint/build refs/libfprint
```

Use the local WIP files as the staging area for EH577-specific edits unless the task explicitly asks for an upstream-style patch.

## Validation checklist

Before calling the patch good, verify:

- the code still matches the observed endpoint layout
- packet and response handling still fit the successful EH575 replay logs
- no logic assumes fixed values for bytes already seen to vary
- build output is clean enough to distinguish new errors from existing upstream noise

## When blocked

If a driver change depends on behavior not yet captured, switch back to the probe workflow and gather the missing evidence instead of guessing protocol details.
