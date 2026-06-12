# AGENTS.md

## Purpose

This repository is the bringup workspace for an open-source `libfprint` driver for the **EgisTec EH577** USB fingerprint sensor (`1c7a:0577`).

## Current state

- **The EH577 is a PRESS (snapshot) sensor — `FP_SCAN_TYPE_PRESS`.** Each touch
  yields a single captured frame. It is **not** a swipe sensor. Modeling it as an
  EH575-style swipe device with multi-strip `fpi_assemble_frames` assembly was an
  early mistake and has been removed (commit `a5a4e7f`). **Do not reintroduce
  swipe mode, strip assembly, or `RFMGHEIGHT`/`RFMDIS` stripe cropping.** All
  capture and matching work happens within the press model.
- It still borrows the **EH575 bulk command family** at the transport/protocol
  level (that part is wire-compatible); only the *capture model* is press.
- The active driver port lives in [egis0577.c](refs/libfprint/libfprint/drivers/egis0577.c) and [egis0577.h](refs/libfprint/libfprint/drivers/egis0577.h).
- The driver already:
  - runs the required **pre-init** sequence,
  - enters the **post-init** capture loop,
  - captures **non-zero** frames,
  - assembles a single **press snapshot** image,
  - and reaches **enroll/verify** through libfprint example programs.
- The main remaining correctness bug is **false matches**: different fingers can still verify as `MATCH`.
- Secondary open questions:
  - better touch / finger-present guards,
  - explicit temperature policy,
  - whether interrupt endpoints matter for a production-quality driver,
  - and how much extra protocol evidence is still needed before upstreaming.

## Repo map

### [docs/](docs/)
Primary human-readable documentation.

- [README.md](docs/README.md)
  - doc index; start here for the current document map
- [current-status.md](docs/current-status.md)
  - where the bringup stands right now
- [findings-summary.md](docs/findings-summary.md)
  - key conclusions from past investigations
- [research-log.md](docs/research-log.md)
  - chronological investigation log (index to daily splits)
- [todo.md](docs/todo.md)
  - remaining work / priorities
- [plan-enroll-verify.md](docs/plan-enroll-verify.md)
  - notes for the libfprint enroll/verify validation flow
- [protocol-comparison.md](docs/protocol-comparison.md)
  - EH577 vs EH575 / `egis0570` / `egismoc`

### [logs/](logs/)
Text evidence from probe runs, driver runs, enroll/verify sessions, and analysis summaries.

### [dumps/](dumps/)
Raw binary frame dumps and rendered `.pgm` artifacts.

### [tools/](tools/)
Local utilities used during reverse engineering and bringup.

Important ones:
- [eh577_usbfs_probe.c](tools/eh577_usbfs_probe.c) — standalone usbfs probe
- [eh577_guided_capture.sh](tools/eh577_guided_capture.sh) — timed touch/remove helper
- [eh577_dump_to_pgm.py](tools/eh577_dump_to_pgm.py) — render raw frame dumps to PGM
- [enroll_identify.sh](tools/enroll_identify.sh) — helper around patched libfprint enroll/identify testing

### [refs/](refs/)
Reference source trees.

- [refs/EgisTec-EH575/](refs/EgisTec-EH575/)
  - archived EH575 reverse-engineering effort; primary protocol template
- [refs/libfprint/](refs/libfprint/)
  - upstream libfprint clone plus the **active EH577 driver work**

### [wip-libfprint/](wip-libfprint/)
Smaller staging/mirror area for EH577 driver edits. Useful for isolated review, but `refs/libfprint/` is the authoritative integration tree.

### [.agents/plans/](.agents/plans/)
Numbered work plans and investigation checklists.

## Working notes

- Direct USB access usually needs `sudo`.
- Artifacts created under `sudo` may be owned by `root`.
- Keep `AGENTS.md` lean; put detailed status/history/todo material under `docs/`.

## Documenting research

When logging research, investigations, or experimental results:
1. **Directory Structure**: Create a directory for the day under [docs/research-log/](docs/research-log/) named `YYYY-MM-DD/`.
2. **Day Files**: Split each topic/experiment into its own file inside the day's directory using the pattern `NN-<slug>.md` (e.g., `01-initial-local-triage.md`).
3. **Top-Level Index**: Update the main [research-log.md](docs/research-log.md) by adding a section for the day with a link to the day's directory, followed by a bulleted list of links to the daily files along with a 1-liner summary of what happened.
4. **Link Format**: All links to files, folders, and code symbols must be formatted as relative markdown links.

## If starting fresh

Read these first:

1. [README.md](docs/README.md)
2. [current-status.md](docs/current-status.md)
3. [todo.md](docs/todo.md)
4. [findings-summary.md](docs/findings-summary.md)
