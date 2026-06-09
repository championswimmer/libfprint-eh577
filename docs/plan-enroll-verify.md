# Plan: enroll and verify with the patched EH577 libfprint driver

## Status at the start of this plan

The EH577 libfprint driver (`egis0577`) has been validated end-to-end:

- Driver runs pre-init (29 packets, arms sensor), then post-init polling loop
- 8 consecutive nonzero frames (~1067 bytes each) captured during a single finger hold
- 8 strips assembled into a 136×178 pixel fingerprint image (`finger.pgm`)
- Minutiae scan completed in 3 ms
- All work lives in `refs/libfprint/` — the patched upstream source tree

**Not yet done**: enrollment and verification via the full libfprint enrollment/verify API.

---

## Key paths

| Thing | Path |
|-------|------|
| Patched libfprint source | `refs/libfprint/` |
| Built `.so` (with egis0577 driver) | `refs/libfprint/build/libfprint/libfprint-2.so.2.0.0` |
| Built example binaries | `refs/libfprint/build/examples/` |
| `enroll` example | `refs/libfprint/build/examples/enroll` |
| `verify` example | `refs/libfprint/build/examples/verify` |
| `img-capture` example | `refs/libfprint/build/examples/img-capture` |
| Driver source | `refs/libfprint/libfprint/drivers/egis0577.c` |
| Driver header | `refs/libfprint/libfprint/drivers/egis0577.h` |
| Guided capture helper | `tools/eh577_guided_img_capture.sh` |

The built example binaries already link against the patched `.so` at build-relative
paths — no install needed. Use `LD_LIBRARY_PATH` to ensure they pick up the patched
library rather than any system-installed libfprint.

---

## Approach: use the built-in `enroll` and `verify` example programs

The libfprint source tree ships two example programs in `examples/`:

- `enroll` — opens the device, runs the enrollment flow (asks for multiple finger
  presentations), and saves the enrolled print to a file `enrolled.print` in the
  current directory.
- `verify` — opens the device, loads `enrolled.print` from the current directory,
  runs one finger presentation, and reports MATCH or NO_MATCH.

These use the standard libfprint `FpDevice` API (enroll/verify callbacks), so they
exercise exactly the same code path that `fprintd` would use. They do NOT require
`fprintd` or D-Bus at all.

### Why not use fprintd directly

- The installed system `fprintd` (1.94.5) links against the system `libfprint-2.so.2`
  (`/lib/x86_64-linux-gnu/libfprint-2.so.2`), not the patched one.
- Overriding a system daemon's shared library requires either `LD_PRELOAD` hacks or
  installing the patched library system-wide — both are risky.
- The `enroll`/`verify` example programs are the cleanest path and are sufficient to
  validate the driver works for real biometric matching.

---

## Pre-flight checks

Before running enroll/verify, verify these things:

### 1. Device is present and active

```bash
lsusb | grep '1c7a:0577'
cat /sys/bus/usb/devices/3-3/power/control
```

Expected: device listed; power/control = `on` (or set it):

```bash
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control'
```

### 2. fprintd is not running (it would steal the device)

```bash
systemctl status fprintd
```

If active, stop it:

```bash
sudo systemctl stop fprintd
```

### 3. Patched library is built

```bash
ls -la refs/libfprint/build/libfprint/libfprint-2.so.2.0.0
```

If missing or stale after a source edit, rebuild:

```bash
ninja -C refs/libfprint/build
```

---

## Step 1: enroll

Enrollment requires **multiple** finger presentations (libfprint calls this "stages").
The `enroll` example runs through however many stages the device requires.

The EH577 driver is a **swipe sensor** (`FP_SCAN_TYPE_SWIPE`). Each stage is one
18-packet post-init cycle. The driver collects `EGIS0577_CONSECUTIVE_CAPTURES` = 8
strips per stage before declaring a stage complete. Expect 2–5 stages (varies by
libfprint version and device config).

**Timing note**: the driver starts with pre-init (29 packets, ~0.5 s) before the first
post-init frame. Budget ~1–2 s from `enroll` start before the first TOUCH prompt is
meaningful.

### Command

Run from the repo root:

```bash
LIBFP="refs/libfprint/build/libfprint"
sudo sh -c "
  LD_LIBRARY_PATH='$LIBFP' \
  G_MESSAGES_DEBUG=all \
  refs/libfprint/build/examples/enroll
" 2>&1 | tee logs/enroll-$(date +%Y%m%d-%H%M%S).txt
```

### What to expect

```
Opened device. Opening device...
Enrolling...
Lift finger...
Place finger on reader...
[finger detected, strips collected]
...
Enrollment complete!
```

For a swipe sensor, each stage is one swipe. Lift and re-place the finger between
stages when prompted. Keep the finger in contact for ~2 s per stage.

If the device returns "no finger" for a stage (zero frame), libfprint will retry
automatically — just try again.

On success, `enrolled.print` is written in the current directory.

---

## Step 2: verify

After `enrolled.print` exists, run one verification:

```bash
LIBFP="refs/libfprint/build/libfprint"
sudo sh -c "
  LD_LIBRARY_PATH='$LIBFP' \
  G_MESSAGES_DEBUG=all \
  refs/libfprint/build/examples/verify
" 2>&1 | tee logs/verify-$(date +%Y%m%d-%H%M%S).txt
```

Place the same finger on the sensor when prompted. Expected output:

```
MATCH    ← success
```

or:

```
NO MATCH ← finger not recognized (retry or re-enroll)
```

---

## Step 3: cross-check with a different finger

To confirm the matcher rejects non-matching fingers, enroll one finger, then run
`verify` with a different finger. Expected: `NO MATCH`.

---

## Troubleshooting guide

### "Error capturing data: Unexpected short error"

Pre-init packet has wrong `response_length`. Check `egis0577.h`:
- `73 14 ec` in `EGIS0577_PRE_INIT_PACKETS` must have `response_length = 7`
  (not 5356 — device returns 7 bytes for this command in pre-init context).

Rebuild after any header change:

```bash
ninja -C refs/libfprint/build
```

### "No device found" / device not enumerated

```bash
lsusb | grep '1c7a:0577'
```

If missing, the device may be in autosuspend:

```bash
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control'
```

Or the USB path changed (unplug/replug changes the bus/device number). Check:

```bash
lsusb -v -d 1c7a:0577 2>/dev/null | grep -E 'Bus|Device|idProduct'
```

### "Permission denied" on /dev/bus/usb

Run under `sudo` as shown above.

### Enrollment always returns zero frames

The sensor was not armed. This means pre-init didn't run or didn't work.
Check the log for:

```
Initial packet array: pre-init
```

If it says `post-init` instead, the driver was not rebuilt after the SM_INIT fix.
Rebuild and check `refs/libfprint/libfprint/drivers/egis0577.c` line ~525:

```c
self->pkt_array = EGIS0577_PRE_INIT_PACKETS;
```

### All stages succeed but `verify` always returns NO MATCH

This may be an image quality issue. The assembled fingerprint at 136×178 without
pixman resize may not have enough ridge detail for the bozorth3 minutiae matcher.

Options:
1. **Enable pixman in the build** — rebuild with pixman support (see below).
2. **Increase EGIS0577_CONSECUTIVE_CAPTURES** — collect more strips per stage
   for a taller assembled image (edit `egis0577.h`, rebuild).
3. **Lower BZ3 threshold** — `EGIS0577_BZ3_THRESHOLD` is 15; try 10 (edit header,
   rebuild). Lower = more permissive match.

### "Libfprint compiled without pixman support, impossible to resize" (CRITICAL warning)

Non-fatal for image capture, but the resize step is skipped. To enable pixman:

```bash
cd refs/libfprint
meson setup build --reconfigure -Dpixman=enabled
ninja -C build
```

Verify pixman is found first:

```bash
pkg-config --modversion pixman-1   # should print e.g. 0.44.0
```

If not found, install the dev package:

```bash
sudo apt install libpixman-1-dev
```

---

## State after this plan is complete

If enroll and verify both succeed, the driver is functionally complete for a
first upstream submission. The remaining polish items are:

- Pixman build (resize support)
- Generating a clean `git format-patch` from `refs/libfprint` against upstream
- Opening a PR / issue on the upstream `libfprint` GitHub
- Adding a `60 2d 02` / `62 67 03` status-check early-exit to skip `64 14 ec`
  when no finger is detected (reduces unnecessary bulk reads)
- Deciding whether interrupt endpoints (`0x83` / `0x84`) need handling for
  production quality (they were silent in all tests so far — safe to omit for now)

---

## Register state quick reference (for debugging)

| Register read | Armed (good) | Unarmed (bad) |
|---------------|-------------|---------------|
| `60 01 fc` byte 5 | `0x00` | `0x05` |
| `60 40 fc` byte 5 | `0x00` | `0x80` |
| `62 67 03` bytes 8-9 | `22 08` / `2b 0c` / `54 13` (nonzero) | `ff 00` |

If the post-init loop shows the "bad" column values, the sensor was not armed —
pre-init did not run or failed silently. Check the log for `pre-init[N/29]` lines.
