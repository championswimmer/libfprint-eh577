# EH577 Driver Consolidation + Debug Workflow Rebuild

## Goal

Reach a clean, minimal `FpDevice`-based driver that has shed all the
over-engineered recovery machinery accumulated during bringup tuning, and
provides two reliable standalone debug workflows:

1. **PGM capture on demand** — stage-2-gated (grain < 6%, ≥ 2 minutiae,
   400 ms settle, 1400 ms turn timeout), dumps a `.pgm` per accepted press,
   then resets driver state so the next touch starts fresh.
2. **Offline enroll + identify loop** — uses our own built driver
   (`LD_LIBRARY_PATH`), our own helper binaries, and a shell script;
   no system fprintd involvement.

The NCC matcher and all FpDevice action plumbing stay.  The cleanup is
about removing state that was never stable, never used in production, or
that made the code too hard to reason about.

---

## Context

- Driver lives in `refs/libfprint/libfprint/drivers/egis0577.{c,h}` (authoritative)
  and is mirrored to `wip-libfprint/` after every edit.
- Snapshotted pre-cleanup as `wip-libfprint/stages/ncc-matching-hardened-startup/`.
- Current driver is `FpDevice` (not `FpImageDevice`); NCC matching is done
  in-driver via `peak_ncc()`.  **Do not change the base class.**
- Example helper sources under `refs/libfprint/examples/`:
  `eh577-capture-helper.c`, `eh577-enroll-helper.c`, `eh577-identify-helper.c`.
- Timing already confirmed correct: `EGIS0577_FINGER_SETTLE_MS 400`,
  `EGIS0577_TURN_TIMEOUT_MS 1400`.
- USB reset is known to wedge the hardware; that recovery path must be
  removed entirely, not just disabled behind a flag.
- `IDENTIFY` feature is already disabled in `dev_class->features`; remove
  its implementation too so dead code does not confuse future readers.

---

## Steps

### Phase A — Driver cleanup: what to remove

**A1. Noise recovery machinery**

Remove everything named `noise_recovery*` or `noise_reject_streak`:

- Struct fields: `noise_reject_streak`, `noise_recovery_attempts`,
  `noise_recovery_clean_frames`, `noise_recovery_active`  (4 fields)
- Defines in `.h`: `EGIS0577_NOISE_RECOVERY_STREAK_*`,
  `EGIS0577_NOISE_RECOVERY_MAX_*`, `EGIS0577_NOISE_RECOVERY_DELAY_*`,
  `EGIS0577_NOISE_RECOVERY_CLEAN_FRAMES_*`  (8 defines)
- Functions: `reset_noise_recovery_state()`,
  `noise_recovery_note_clean_baseline()`,
  `noise_recovery_delay_ms()`, `noise_recovery_required_clean_frames()`,
  `action_is_verify_or_identify()`, `stage2_reject_is_noise_like()`
- All call sites (look for `noise_recovery_active`, `noise_reject_streak`,
  `noise_recovery_*` in `process_imgs` and `save_img`)
- Replacement: on any Stage-2 reject, always call
  `restart_capture_cycle(self, ssm, dev, "stage2 reject", post_poll_delay)`.
  No action-aware branching.

**A2. USB reset recovery path**

Remove the entire `reset_device_and_reclaim()` / USB-reset escalation:

- Function `reset_device_and_reclaim()`
- Function `restart_capture_cycle_with_usb_reset()`
- Struct field `startup_reset_attempts`
- All call sites in `resp_cb` timeout handling and `SM_INIT`
- Env var handling for `EGIS0577_ENABLE_USB_RESET_RECOVERY`
- The `.h` define `EGIS0577_STARTUP_RESET_RECOVERY_MAX` and
  `EGIS0577_STARTUP_RESET_DELAY_MS`
- Replacement: the only recovery on timeout/wedge is the existing
  `restart_capture_cycle()` (claim recycle + PRE_INIT). Keep `startup_timeout_retries`
  and `EGIS0577_STARTUP_TIMEOUT_RECOVERY_MAX` (2 attempts), then hard-fail.

**A3. Unarmed-stuck escalation**

Remove the "sensor stuck unarmed forever" escalation loop:

- Struct fields: `unarmed_finger_first_time`, `unarmed_reinit_attempts`
- Function `report_unarmed_stuck()`
- Define `EGIS0577_UNARMED_REINIT_MAX`
- All call sites in `save_img` / the no-finger poll path
- Replacement: if the sensor looks finger-present but `capture_armed` is
  FALSE for more than one claim cycle, just call `restart_capture_cycle`
  and let the caller retry naturally.  We do not need to escalate to a
  "remove finger" error during debugging.

**A4. Env-var tunable timing struct fields**

The following struct fields exist only to cache env-var overrides that were
useful during early tuning.  The values are now stable; collapse them to
direct use of the defines:

- Remove fields: `pre_frame_delay_ms`, `poll_loop_delay_ms`,
  `no_finger_retry_delay_ms`, `post_capture_poll_delay_ms`,
  `max_frames_per_claim`, `frame_delay_armed`
- In `SM_INIT`, delete the `get_env_ms_or_default()` block that initialises
  these (lines ~2137–2155).
- Inline all callers with the corresponding define value.  The pre-frame
  delay path (`frame_delay_armed`, `pre_frame_delay_ms`) in `SM_REQ` can
  be deleted entirely since `EGIS0577_PRE_FRAME_DELAY_MS` is 0.
- Keep `get_env_ms_or_default` / `get_env_uint_or_default` only if still
  used elsewhere; otherwise delete the helpers too.

**A5. Env-var tunable stage-2 threshold struct fields**

Same pattern — values are known and stable:

- Remove struct fields: `stage2_grain_pct_x1000`, `stage2_min_minutiae`,
  `stage2_max_minutiae`, `stage2_min_ridge_pixels`, `stage2_min_stretch_p5`
- In `SM_INIT`, delete the `get_env_uint_or_default()` block for these
  (lines ~2145–2154).
- In `stage2_snapshot_quality_ok()` and `stage2_reject_is_noise_like()`,
  replace `self->stage2_*` references with the `EGIS0577_STAGE2_*` defines
  directly.

**A6. IDENTIFY action implementation**

`IDENTIFY` is already cleared from `dev_class->features`, so remove the
dead implementation:

- Function `on_frame_accepted_identify()`
- Function `dev_identify()` and its `dev_class->identify = dev_identify`
  registration
- The `FPI_DEVICE_ACTION_IDENTIFY` case in `on_frame_accepted()`
- Pack/unpack helpers used exclusively by identify (check
  `pack_gallery`, `unpack_gallery_frame` — if also used by enroll/verify NCC,
  keep them; if only identify, remove)
- `GPtrArray *enroll_gallery` stays (used by enroll NCC path).

**A7. Enroll geometry fields**

- Struct fields `enroll_frame_size`, `enroll_frame_width`,
  `enroll_frame_height` — confirm whether any of these are read outside
  of their setter and the debug log in `SM_INIT`.  If they are only ever
  written and never affect control flow, remove them.

---

### Phase B — Consolidation: harden what stays

**B1. Confirm stage-2 gate values in `.h`**

After the struct-field removal, all thresholds come from defines.
Verify the `.h` defines match the agreed operating point:

| Parameter | Target | Current define |
|---|---|---|
| `EGIS0577_STAGE2_GRAIN_PCT_X1000` | 6000 (= 6.0%) | 6000 ✓ |
| `EGIS0577_STAGE2_MIN_MINUTIAE` | 2 | 2 ✓ |
| `EGIS0577_STAGE2_MAX_MINUTIAE` | 16 | 16 ✓ |
| `EGIS0577_STAGE2_MIN_RIDGE_PIXELS` | 600 | 600 ✓ |
| `EGIS0577_STAGE2_MIN_STRETCH_P5` | 100 | 100 ✓ |
| `EGIS0577_FINGER_SETTLE_MS` | 400 | 400 ✓ |
| `EGIS0577_TURN_TIMEOUT_MS` | 1400 | 1400 ✓ |

Update the comment block in `.h` to mark these as the production-stable
operating point, not "current best live-tested".

**B2. Simplify `reset_action_state()`**

After removing the dropped fields, `reset_action_state()` should be shorter.
Ensure it still zeros: `stop`, `finger_reported`, `capture_armed`,
`frame_counter`, `frame_reads_this_claim`, `turn_open`, `has_pre_init_run`,
`startup_timeout_retries`, `rearm_not_before_time`,
and clears `capture_frame`, `background`, `best_frame`.

**B3. Simplify on-reject path in `process_imgs()`**

After removing noise recovery, the reject branch should be:
1. Log the stage-2 metrics (keep the `fp_warn` line).
2. Call `restart_capture_cycle(self, ssm, dev, "stage2 reject", EGIS0577_POST_CAPTURE_POLL_DELAY_MS)`.
That is all.  No action-aware delay, no noise streak counter, no recovery
mode gating.

**B4. Verify `save_img` turn-timeout path is intact**

The turn timeout logic in `save_img` (forced commit when
`g_get_monotonic_time() - finger_first_detected_time > EGIS0577_TURN_TIMEOUT_MS * 1000`)
must still work correctly after removing the unarmed-stuck escalation that
previously shared some of the `unarmed_finger_first_time` tracking.

**B5. Mirror cleaned files to `wip-libfprint/`**

After all edits, `cp refs/libfprint/libfprint/drivers/egis0577.{c,h} wip-libfprint/`.

---

### Phase C — PGM capture debug workflow

**C1. Audit `eh577-capture-helper.c`**

Read `refs/libfprint/examples/eh577-capture-helper.c` and confirm it:
- Calls `fp_device_open_sync()`, `fp_device_capture_sync()`,
  `fp_device_close_sync()` in a loop for N captures.
- Saves each accepted image as a `.pgm` file in the output directory.
- Prints stage-2 metrics (available from the driver `fp_warn` log lines).

If it does not loop or does not write PGM, fix the binary to do so.

**C2. Confirm `eh577_capture_debug.sh` covers the workflow**

`tools/eh577_capture_debug.sh` already exists.  Confirm it:
- Stops fprintd before running.
- Sets `EGIS0577_FRAME_DUMP_DIR` so raw frames are saved alongside PGMs.
- Passes a count argument to the helper.
- Saves artifacts under `artifacts/capture12/debug-YYYYMMDD-HHMMSS/`.
- Prints the log path at the end.

If anything is missing (e.g., PGM filenames with a sequence number, or
stage-2 accept/reject counts in the final summary), fix the script.

**C3. Write a leaner `tools/eh577_pgm_loop.sh`**

Simpler than `capture12.sh` — no cue timers, no finger-selection UI.
Intended for rapid iteration:

```
Usage: sudo ./tools/eh577_pgm_loop.sh [N]   # default N=5
```

- Kills fprintd / fprintd.socket.
- Runs `eh577-capture-helper` for N captures.
- For each accepted PGM: copies it to `artifacts/pgm/YYYYMMDD-HHMMSS-NNN.pgm`.
- Summarises: frames attempted, accepted, rejected, per-frame grain/minutiae
  from the driver log.
- Exit 0 if ≥ 1 accepted frame; exit 1 if all rejected.

---

### Phase D — Offline enroll + identify loop

**D1. Audit `eh577-enroll-helper.c`**

Read `refs/libfprint/examples/eh577-enroll-helper.c` and confirm it:
- Accepts a storage path argument (e.g., `enrollment.variant`).
- Calls `fp_device_enroll_sync()` for `NCC_ENROLL_FRAMES` (7) stages,
  prompting the user to touch/lift each time.
- Saves the enrolled `FpPrint` as a `.variant` file on disk.
- On error, exits non-zero with a clear message.

**D2. Audit `eh577-identify-helper.c`**

Read `refs/libfprint/examples/eh577-identify-helper.c` and confirm it:
- Accepts a storage directory or list of `.variant` files.
- Loads the enrolled `FpPrint` objects.
- Calls `fp_device_identify_sync()`.
- Prints the matched finger name (or "NO MATCH") and the NCC score to
  stdout — the shell script key-value form is best: `MATCH=yes NCC=0.73`.

**D3. Update / write `tools/eh577_offline_session.sh`**

This replaces the ad-hoc parts of `enroll_identify.sh` with a single clean
script that owns one session end-to-end:

```
Usage: sudo ./tools/eh577_offline_session.sh
```

Flow:
1. **Pre-flight**: check `1c7a:0577` on USB, kill fprintd/socket, set
   `LD_LIBRARY_PATH` to `refs/libfprint/build/libfprint`.
2. **Enroll phase**: prompt for finger name, run `eh577-enroll-helper`,
   save `artifacts/sessions/SESS/enrollment.variant`.
3. **Identify loop**: repeat until user types `q`:
   - Prompt "touch finger (or 'q' to quit)".
   - Run `eh577-identify-helper artifacts/sessions/SESS/`.
   - Print "MATCH (NCC=X.XX)" or "NO MATCH (NCC=X.XX)".
   - Copy the accepted PGM (if helper dumps one) to
     `artifacts/sessions/SESS/identify-NNN.pgm`.
4. **Summary**: total identify attempts, match count, no-match count,
   path to session log.

All driver debug output goes to `artifacts/sessions/SESS/session.log` via
`G_MESSAGES_DEBUG=libfprint-egis0577`.  Terminal shows only the prompts
and results.

---

## Validation

- `wc -l refs/libfprint/libfprint/drivers/egis0577.c` should drop from ~2415
  to ≤ 1700 lines after Phase A+B.
- Driver still builds: `ninja -C refs/libfprint/build` with zero errors.
- `sudo ./tools/eh577_pgm_loop.sh 3` completes, produces ≥ 1 `.pgm` in
  `artifacts/pgm/`, and the log shows stage-2 `accept` at least once.
- `sudo ./tools/eh577_offline_session.sh`:  enroll 7 frames, then 3 identify
  touches of the same finger all report "MATCH".
- `wip-libfprint/egis0577.c` and `wip-libfprint/egis0577.h` are identical to
  `refs/libfprint/…/egis0577.{c,h}`.

---

## Todo

### Phase A — Removals
- [x] A1: Remove noise recovery (4 fields, 8 defines, 6 functions, all call sites)
- [x] A2: Remove USB reset recovery (`reset_device_and_reclaim`, `restart_capture_cycle_with_usb_reset`, `startup_reset_attempts`, env flag)
- [x] A3: Remove unarmed-stuck escalation (`unarmed_*` fields, `report_unarmed_stuck`, `EGIS0577_UNARMED_REINIT_MAX`)
- [x] A4: Collapse env-var timing fields (`pre_frame_delay_ms`, `poll_loop_delay_ms`, `no_finger_retry_delay_ms`, `post_capture_poll_delay_ms`, `max_frames_per_claim`, `frame_delay_armed`) — inline defines, delete `SM_INIT` env-var block
- [x] A5: Collapse env-var stage-2 threshold fields (5 `stage2_*` struct fields) — use defines directly in `stage2_snapshot_quality_ok()`
- [x] A6: Remove IDENTIFY implementation (`on_frame_accepted_identify`, `dev_identify`, the identify case in `on_frame_accepted`)
- [x] A7: Remove `enroll_frame_size` (not load-bearing); kept `enroll_frame_width/height` (used by `pack_gallery`)

### Phase B — Consolidation
- [x] B1: Verified `.h` define values match agreed operating point
- [x] B2: Simplified `reset_action_state()` to match trimmed struct
- [x] B3: Simplified on-reject in `process_imgs()` to single `restart_capture_cycle` call
- [x] B4: `save_img` turn-timeout path intact (still checks `finger_first_detected_time`)
- [x] B5: Mirrored `egis0577.{c,h}` to `wip-libfprint/`; clean build (zero warnings)

### Phase C — PGM capture workflow
- [x] C1: Audit `eh577-capture-helper.c` — loop + PGM output confirmed (saves capture-NN.pgm, loops N times)
- [x] C2: Audit `eh577_capture_debug.sh` — added PGM accept count to final summary
- [x] C3: Write `tools/eh577_pgm_loop.sh` (lean N-capture loop with summary)

### Phase D — Offline enroll+identify
- [x] D1: Audit `eh577-enroll-helper.c` — `--finger-index N`, saves to `test-storage.variant` in CWD
- [x] D2: Audit `eh577-identify-helper.c` — loads gallery from CWD, prints match/no-match per identify-complete line (NCC score not exposed through public API)
- [x] D3: Write `tools/eh577_offline_session.sh` (enroll + identify loop + session log under `artifacts/sessions/SESS/`)

### Final
- [ ] Run enroll+identify validation (same-finger × 3 MATCH)
- [ ] Commit cleaned driver + new tools
