# AGENTS.md

## Purpose

This repository is a reverse-engineering workspace for an open-source Linux fingerprint driver for the **EgisTec EH577** USB fingerprint sensor.

Target device on this machine:

- USB VID:PID: `1c7a:0577`
- USB product string: `EgisTec EH577`
- Sysfs path: `/sys/bus/usb/devices/3-3`
- USB device node typically used in probes: `/dev/bus/usb/003/004`

The immediate goal is to turn the current reverse-engineering results into a real `libfprint` driver.

---

## Big picture / current conclusion

The most important finding so far is:

- **EH577 is wire-compatible with the EH575 protocol family**.
- It accepts the full **EH575 post-init bulk sequence** on:
  - bulk OUT `0x01`
  - bulk IN `0x82`
- It returns valid `SIGE` responses to `EGIS` requests.
- It returns the expected **5356-byte** response for packet `64 14 ec`.

Important nuance:

- some response bytes vary across runs / state
- the 5356-byte payload captured so far is **all zeros** when idle
- interrupt endpoints `0x83` / `0x84` have been **silent** in short idle/post-init polls

So the current working hypothesis is:

1. start from the EH575 driver structure
2. keep EH577 state handling flexible
3. figure out when the payload becomes meaningful and whether interrupts matter during finger interaction

---

## Repository layout

### Top level

- `README.md`
  - short project overview
- `AGENTS.md`
  - this file; handoff / workspace briefing for future runs
- `docs/`
  - human-readable research notes and TODOs
- `logs/`
  - text logs from actual probing sessions
- `dumps/`
  - raw binary response dumps from the probe
- `tools/`
  - standalone local utilities used for reverse engineering
- `build/`
  - compiled probe binary
- `refs/`
  - reference source trees cloned for study
- `wip-libfprint/`
  - extracted/adapted driver files for an eventual EH577 libfprint patch

### docs/

- `docs/research-log.md`
  - chronological log of what has been investigated and learned
- `docs/protocol-comparison.md`
  - side-by-side comparison of EH577 vs EH575 vs `egis0570` vs `egismoc`
- `docs/todo.md`
  - checklist of open tasks

### logs/

Most important logs right now:

- `logs/2026-06-03-eh577-eh575-bulk-probe.txt`
  - first successful proof that EH577 accepts early EH575 packets
- `logs/2026-06-03-eh577-auto-init-run.txt`
  - full post-init replay log showing successful 18-packet sequence
- `logs/2026-06-03-eh577-postinit18-dump.txt`
  - full post-init replay with raw per-packet dumps saved to disk
- `logs/2026-06-03-eh577-postinit18-analysis.txt`
  - summarized analysis of the raw dump set
- `logs/2026-06-03-eh577-idle-interrupt-poll.txt`
  - idle interrupt polling
- `logs/2026-06-03-eh577-postinit-interrupt-poll.txt`
  - interrupt polling after post-init

### dumps/

Currently most important dump set:

- `dumps/2026-06-03-postinit18/`
  - raw `.bin` responses for each packet of a full EH575 post-init replay on EH577
  - packet 17 file is the saved 5356-byte payload

Note:

- these files were created under `sudo`, so ownership may be `root:root`

### tools/

- `tools/eh577_usbfs_probe.c`
  - standalone usbfs-based probe tool
  - no libusb dev headers required
  - directly uses `USBDEVFS_*` ioctls

Supported modes in the tool:

- `reset`
- `poll-int [loops]`
- `eh575-preinit [count]`
- `eh575-postinit [count]`
- `eh575-repeat [count]`
- `eh575-auto [count]`

Useful feature:

- if `EH577_DUMP_DIR` is set, the tool writes raw response bytes per packet into that directory

### build/

- `build/eh577_usbfs_probe`
  - compiled version of the probe

### refs/

Reference repos cloned locally:

- `refs/EgisTec-EH575`
  - previous reverse-engineering effort for the related EH575 sensor
- `refs/libfprint`
  - upstream libfprint source

Key reference files:

- `refs/EgisTec-EH575/libfprint.patch`
  - contains the reverse-engineered EH575 driver patch
- `refs/EgisTec-EH575/archive/src/io.rs`
  - Rust prototype with packet sequences
- `refs/libfprint/libfprint/drivers/egis0570.c`
- `refs/libfprint/libfprint/drivers/egis0570.h`
- `refs/libfprint/libfprint/drivers/egismoc/egismoc.c`
- `refs/libfprint/libfprint/drivers/egismoc/egismoc.h`

### wip-libfprint/

This is the local driver staging area.

- `wip-libfprint/drivers/egis0575.c`
- `wip-libfprint/drivers/egis0575.h`
  - extracted directly from the EH575 patch for reference
- `wip-libfprint/drivers/egis0577.c`
- `wip-libfprint/drivers/egis0577.h`
  - first draft created by adapting the EH575 driver skeleton to PID `0577`
- `wip-libfprint/README.md`
  - notes about the WIP port base

Important:

- `egis0577.c/.h` are **not** a finished driver
- they are a starting point only

---

## Hardware / descriptor facts

EH577 descriptor facts observed locally:

- one interface
- vendor-specific class/subclass/protocol
- endpoints:
  - `0x01` bulk OUT
  - `0x82` bulk IN
  - `0x83` interrupt IN
  - `0x84` interrupt IN
- high-speed USB 2.0

This matches the EH575 family shape much more closely than `egismoc`.

---

## What was tried so far

### 1. Baseline system support check

Checked:

- `lsusb`
- installed `fprintd` / `libfprint` packages
- udev/hwdb entries
- existing libfprint strings and drivers

Result:

- stock Ubuntu `fprintd` reports **no devices available**
- installed libfprint has no EH577 driver
- upstream source has `autosuspend.hwdb` knowledge of `1c7a:0577`, but no actual driver

### 2. Compare against EH575 / upstream Egis drivers

Compared EH577 against:

- EH575 reverse-engineered patch
- upstream `egis0570`
- upstream `egismoc`

Result:

- EH577 bulk endpoints match EH575 exactly (`0x01` / `0x82`)
- EH577 descriptor shape aligns with EH575-family hardware
- EH577 does **not** match `egismoc` endpoint numbering

### 3. Build direct usbfs probe

A standalone C probe was written to talk straight to `/dev/bus/usb/...`.

Reason:

- no PyUSB installed
- libusb runtime exists but headers were missing
- usbfs ioctls were enough for direct probing

Result:

- probe compiled successfully
- probe could reset device, claim interface, send EH575 packets, read responses, poll interrupts, and dump raw payloads

### 4. Early EH575 packet replay on EH577

Sent early EH575 post-init packets.

Result:

- EH577 immediately responded correctly with `SIGE`
- this was the first proof of protocol compatibility

### 5. Full EH575 post-init replay on EH577

Sent all 18 EH575 post-init packets.

Result:

- all packets succeeded
- `64 14 ec` returned `5356` bytes
- some smaller reply bytes varied between runs

### 6. Interrupt polling

Polled interrupt endpoints `0x83` and `0x84`:

- idle after reset
- after post-init replay

Result:

- no payloads observed in short polling windows

### 7. Raw payload dumping

Saved per-packet response dumps with `EH577_DUMP_DIR=...`.

Result:

- packet 17 (`64 14 ec`) dump is exactly `5356` bytes
- all bytes are zero in the captured idle run

---

## What worked

These are the strongest validated results so far:

1. **EH577 accepts EH575 bulk protocol framing**
   - `EGIS` requests
   - `SIGE` responses

2. **EH577 accepts the full EH575 post-init sequence**
   - not just the first few packets

3. **EH577 returns the expected large response size**
   - `64 14 ec` -> `5356` bytes

4. **An EH577 driver skeleton now exists**
   - `wip-libfprint/drivers/egis0577.c/.h`

5. **Raw dumps are now being saved for analysis**
   - useful for future tooling and regression checks

---

## Important response details already observed

Examples from successful captures:

- `60 00 fc` -> `53 49 47 45 00 aa 01`
- `60 01 fc` -> `53 49 47 45 01 05 01`
- `60 40 fc` -> `53 49 47 45 40 80 01`
- `62 67 03` -> `53 49 47 45 67 03 01 00 ff 00` in one capture
- `60 00 02` -> `53 49 47 45 00 ab 01`
- `64 14 ec` -> `5356` bytes, all zero in the saved idle capture

State variation seen across runs:

- `60 00 fc` / related commands have returned both `aa` and `ab`
- `60 01 fc` was once seen as `01 01 01`, later as `01 05 01`
- `60 2d 02` and `62 67 03` also changed between captures

Interpretation:

- EH577 is stateful
- do **not** hardcode all reply bytes as fixed constants without more evidence

---

## Current approach

The current development strategy is:

1. **Use EH575 as the primary protocol template**
   - not `egis0570`
   - not `egismoc`

2. **Keep a lightweight standalone probe available**
   - faster to iterate than editing libfprint immediately
   - can dump payloads and test sequences safely

3. **Capture state variation before hardcoding driver logic**
   - especially around `60 00/01 fc`
   - and around later status-like responses

4. **Use the local WIP driver only as a port base for now**
   - sequence tables may still need updates
   - branching behavior may need changes

5. **Focus next on meaningful data acquisition**
   - finger interaction
   - payload entropy/image analysis
   - interrupts during touch/remove

---

## Recommended next steps

### Highest priority

1. **Poll with a finger physically touching / leaving the sensor**
   - run `poll-int` while interacting with the reader
   - see whether `0x83` / `0x84` ever produce data

2. **Capture post-init payloads during finger interaction**
   - especially packet `64 14 ec`
   - compare idle vs touch vs swipe/hold

3. **Characterize state variation systematically**
   - repeated reset + first few packets
   - record exact changing bytes

4. **Try EH575 pre-init and repeat paths explicitly**
   - not just post-init
   - especially if finger interaction changes behavior

5. **Convert the WIP skeleton into a real libfprint patch**
   - once sequence/state behavior is better understood

### Good commands to rerun

Reset:

```bash
sudo ./build/eh577_usbfs_probe /dev/bus/usb/003/004 reset
```

Post-init replay:

```bash
sudo ./build/eh577_usbfs_probe /dev/bus/usb/003/004 eh575-postinit 18
```

Auto-init replay:

```bash
sudo ./build/eh577_usbfs_probe /dev/bus/usb/003/004 eh575-auto
```

Interrupt polling:

```bash
sudo ./build/eh577_usbfs_probe /dev/bus/usb/003/004 poll-int 40
```

Replay with raw dumps:

```bash
mkdir -p dumps/run-name
sudo env EH577_DUMP_DIR="$PWD/dumps/run-name" \
  ./build/eh577_usbfs_probe /dev/bus/usb/003/004 eh575-postinit 18
```

---

## Cautions / gotchas

### Sudo behavior

- `sudo -n` has sometimes worked, sometimes expired during longer automation
- if commands start failing with interactive-auth errors, refresh sudo first

### Device ownership

- direct access to `/dev/bus/usb/003/004` requires root here
- unprivileged runs fail with permission denied

### Dump file ownership

- dump/log artifacts created under sudo may be owned by root
- be careful when editing/cleaning them later

### Interrupt silence does not mean interrupts are useless

- so far they were silent only in short idle/post-init tests
- they may still matter during finger events or OEM-style arming

### All-zero 5356-byte payload does not mean capture is fake

- it may be an idle frame
- it may require pre-init/repeat/finger state
- it may need timing differences

---

## Suggested mental model for future work

Treat EH577 as:

- an **EH575-family image/swipe device**,
- with a mostly compatible bulk protocol,
- plus some state-dependent reply bytes,
- and possibly optional/conditional interrupt behavior.

Do not start from scratch unless new evidence disproves EH575 compatibility.

---

## If you only read three files first

Read these first:

1. `docs/research-log.md`
2. `logs/2026-06-03-eh577-postinit18-analysis.txt`
3. `tools/eh577_usbfs_probe.c`

Then inspect:

4. `wip-libfprint/drivers/egis0577.c`
5. `refs/EgisTec-EH575/libfprint.patch`

---

## Current status in one sentence

**EH577 already speaks the EH575 bulk protocol well enough that the next job is not discovery of the family, but refining state handling and turning the existing WIP skeleton into a working libfprint driver.**
