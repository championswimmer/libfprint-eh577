# Identify enable + enroll/identify tools revamp — 2026-06-19

## What changed

### 1. Driver: identify enabled (`egis0577.c`)

Added `on_frame_accepted_identify`: mirrors `on_frame_accepted_verify` but loops
over all enrolled prints supplied by `fpi_device_get_identify_data`.  For each
print it unpacks the NCC gallery and runs `peak_ncc` against all gallery frames,
then reports the enrolled `FpPrint` with the highest score that clears
`NCC_THRESHOLD` with at least `NCC_MIN_MATCHES` hits, or NULL for no-match.

Changes to the class init:
- Added `dev_class->identify = dev_identify` (same as verify: calls `start_capture_action`)
- Removed `dev_class->features &= ~FP_DEVICE_FEATURE_IDENTIFY` — the auto-init
  now sees the identify vfunc and enables the flag automatically

The dispatch in `on_frame_accepted` now routes `FPI_DEVICE_ACTION_IDENTIFY` to
`on_frame_accepted_identify` instead of falling through to the default unref.

### 2. Enroll helper: stdout now unbuffered (`eh577-enroll-helper.c`)

Added `setbuf(stdout, NULL)` at the start of `main`.  The identify helper already
had this; without it, enroll progress events (`EH577_HELPER enroll-stage ...`)
stall in glibc's full-buffer mode when the helper runs through a process
substitution pipe, making the shell-side read loop receive them all at once at
EOF rather than in real time.

### 3. New binary: `eh577-reset-helper` (`eh577-reset-helper.c`)

Opens the fingerprint device and immediately closes it.  Emits:
- `EH577_RESET device-opened` — after `fp_device_open` completes
- `EH577_RESET device-closed` — after `fp_device_close` completes

The `enroll_identify.sh` script calls this binary at the start of each session
to give the driver a clean initialisation cycle (USB interface claim + pre-init +
release) before longer-running enroll or identify operations.  The reset binary
does **not** recover a hardware-wedged device (only a USB replug does that); it
only ensures the driver starts from a known-open state rather than whatever
leftover state a crashed previous session may have left.

### 4. `tools/enroll_identify.sh` — four targeted improvements

**a. Sudo upfront**
`sudo -v` is called once at the top if the credential cache is not already warm.
A background keepalive loop (`sudo -n -v` every 55 s) ensures the cache does not
expire during a long session.  All subsequent `sudo` calls are `sudo -n`
(non-interactive), so no interactive prompts can block the script mid-session.

**b. All debug to log file only**
`exec 3>>"$LOG"` opens fd 3 for log appends (unchanged from before).
Helper invocations use `2>>'$LOG'` inside the `sudo -n sh -c` block so that
`G_MESSAGES_DEBUG` output from the driver goes to the log and never to the
terminal.  The enroll helper's `setbuf(stdout, NULL)` (see above) ensures
structured events arrive at the shell read loop in real time.

**c. Terminal guidance**
- Added `STAGE_HINTS[]` for per-stage angle guidance during enroll
- Combined `retry_wait_lift` and `finger_down` no-lift paths into a single
  `need_lift|finger_down` case that shows "Not captured — lift finger and try
  again", covering both true too-soon lifts and driver-reported turn-timeout
  absent events from the new `waiting_for_lift` logic
- Added `no-gallery` result case with a clear "run enroll first" message
- `identify-complete result=no-gallery` now surfaces a readable error rather
  than a generic "no result" fallback

**d. fprint-level device reset**
After stopping fprintd and waiting for the USB device to be free, the script
runs `eh577-reset-helper` (see above) and checks for `EH577_RESET device-closed`
to confirm it completed.  A warning is printed if the reset does not complete
cleanly.

## Files modified

| File | Change |
|------|--------|
| `refs/libfprint/libfprint/drivers/egis0577.c` | add identify |
| `wip-libfprint/egis0577.c` | mirror |
| `refs/libfprint/examples/eh577-enroll-helper.c` | `setbuf(stdout, NULL)` |
| `refs/libfprint/examples/eh577-reset-helper.c` | new |
| `refs/libfprint/examples/meson.build` | add `eh577-reset-helper` |
| `tools/enroll_identify.sh` | full revamp |
