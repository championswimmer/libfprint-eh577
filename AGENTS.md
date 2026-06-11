# AGENTS.md

## Purpose

This repository is the bringup workspace for an open-source `libfprint` driver for the **EgisTec EH577** USB fingerprint sensor (`1c7a:0577`).

## Current state

- EH577 is now treated as an **EH575-family bulk image/swipe sensor**.
- The active driver port lives in `refs/libfprint/libfprint/drivers/egis0577.c` and `.h`.
- The driver already:
  - runs the required **pre-init** sequence,
  - enters the **post-init** capture loop,
  - captures **non-zero** frames,
  - assembles a fingerprint image,
  - and reaches **enroll/verify** through libfprint example programs.
- The main remaining correctness bug is **false matches**: different fingers can still verify as `MATCH`.
- Secondary open questions:
  - better touch / finger-present guards,
  - explicit temperature policy,
  - whether interrupt endpoints matter for a production-quality driver,
  - and how much extra protocol evidence is still needed before upstreaming.

## Repo map

### `docs/`
Primary human-readable documentation.

- `docs/README.md`
  - doc index; start here for the current document map
- `docs/current-status.md`
  - where the bringup stands right now
- `docs/findings-summary.md`
  - key conclusions from past investigations
- `docs/research-log.md`
  - chronological investigation log
- `docs/todo.md`
  - remaining work / priorities
- `docs/plan-enroll-verify.md`
  - notes for the libfprint enroll/verify validation flow
- `docs/protocol-comparison.md`
  - EH577 vs EH575 / `egis0570` / `egismoc`

### `logs/`
Text evidence from probe runs, driver runs, enroll/verify sessions, and analysis summaries.

### `dumps/`
Raw binary frame dumps and rendered `.pgm` artifacts.

### `tools/`
Local utilities used during reverse engineering and bringup.

Important ones:
- `tools/eh577_usbfs_probe.c` — standalone usbfs probe
- `tools/eh577_guided_capture.sh` — timed touch/remove helper
- `tools/eh577_dump_to_pgm.py` — render raw frame dumps to PGM
- `tools/enroll_identify.sh` — helper around patched libfprint enroll/identify testing

### `refs/`
Reference source trees.

- `refs/EgisTec-EH575/`
  - archived EH575 reverse-engineering effort; primary protocol template
- `refs/libfprint/`
  - upstream libfprint clone plus the **active EH577 driver work**

### `wip-libfprint/`
Smaller staging/mirror area for EH577 driver edits. Useful for isolated review, but `refs/libfprint/` is the authoritative integration tree.

### `.agents/plans/`
Numbered work plans and investigation checklists.

## Working notes

- Direct USB access usually needs `sudo`.
- Artifacts created under `sudo` may be owned by `root`.
- Keep `AGENTS.md` lean; put detailed status/history/todo material under `docs/`.

## If starting fresh

Read these first:

1. `docs/README.md`
2. `docs/current-status.md`
3. `docs/todo.md`
4. `docs/findings-summary.md`
