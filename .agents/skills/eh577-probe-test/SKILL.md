---
name: eh577-probe-test
description: Use when probing, replaying packets against, polling interrupts from, or collecting dumps/logs for the EgisTec EH577 fingerprint sensor. Covers the local usbfs probe workflow, expected EH575-compatible behavior, finger-interaction testing, and how to record results safely.
---

# EH577 Probe And Test

Start here when the task involves live device interaction or interpreting probe results.

## Read first

Read only the files you need:

- `AGENTS.md` for the current device facts and workflow
- `docs/research-log.md` for chronology
- `logs/2026-06-03-eh577-postinit18-analysis.txt` for the strongest current result
- `tools/eh577_usbfs_probe.c` if you need probe behavior or packet mode details

## Working model

Assume EH577 is an EH575-family device unless new evidence contradicts that.

Validated facts so far:

- bulk OUT is `0x01`
- bulk IN is `0x82`
- interrupt IN endpoints are `0x83` and `0x84`
- EH575 post-init replay succeeds
- `64 14 ec` returns a 5356-byte payload
- idle captures have produced an all-zero 5356-byte payload

Do not hardcode state bytes as fixed constants without repeated captures. Some `SIGE` replies vary across runs.

## Preferred workflow

1. Reset the device before a fresh capture series.
2. Use `eh575-auto` or `eh575-postinit 18` to establish the baseline.
3. If collecting raw payloads, set `EH577_DUMP_DIR` to a new dated directory under `dumps/`.
4. If checking finger events, run `poll-int` while touching and removing a finger.
5. Compare idle vs touch vs repeated-run behavior before changing driver logic.

## Commands

Device access here normally requires root.

Reset:

```bash
sudo ./build/eh577_usbfs_probe /dev/bus/usb/003/004 reset
```

Auto-init:

```bash
sudo ./build/eh577_usbfs_probe /dev/bus/usb/003/004 eh575-auto
```

Post-init replay:

```bash
sudo ./build/eh577_usbfs_probe /dev/bus/usb/003/004 eh575-postinit 18
```

Interrupt polling during finger interaction:

```bash
sudo ./build/eh577_usbfs_probe /dev/bus/usb/003/004 poll-int 40
```

Replay with raw dumps:

```bash
mkdir -p dumps/2026-06-03-sample-run
sudo env EH577_DUMP_DIR="$PWD/dumps/2026-06-03-sample-run" \
  ./build/eh577_usbfs_probe /dev/bus/usb/003/004 eh575-postinit 18
```

## What to record

For every meaningful run, capture:

- exact command used
- whether the device was freshly reset
- whether a finger was present, absent, touching, or swiping
- which packet responses changed
- whether `64 14 ec` stayed zero-filled or gained entropy
- whether either interrupt endpoint returned data

Write a summary log under `logs/` and keep raw dumps under `dumps/`.

## Cautions

- `sudo` may expire during longer runs
- dump files created as root may be owned by `root:root`
- interrupt silence in short tests does not prove interrupts are irrelevant
- an all-zero 5356-byte payload can still be a valid idle frame

## Done criteria

The probe/test task is not complete until the result includes:

- the commands that were run
- what changed relative to prior captures
- whether the result supports or weakens the EH575-compatibility model
