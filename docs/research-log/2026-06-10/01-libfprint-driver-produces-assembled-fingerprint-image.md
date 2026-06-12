# 2026-06-10 — libfprint driver produces assembled fingerprint image end-to-end

### Summary

After fixing the driver to run pre-init first, the patched libfprint produced a complete
assembled fingerprint image end-to-end through the full libfprint stack.

### What happened

1. Driver opened device, claimed interface 0
2. SM_INIT → started with `EGIS0577_PRE_INIT_PACKETS` (29 packets, sensor armed)
   - Pre-init packet 15 (`73 14 ec`) was originally set to expect 5356 bytes but device
     returns 7 bytes in pre-init context — fixed by setting response_length = 7 in header
3. After pre-init completed, `resp_cb` automatically switched to `EGIS0577_POST_INIT_PACKETS`
4. Post-init polling loop ran; `60 01 fc` → `01 00 01` (sensor armed), `60 40 fc` → `40 00 01`
5. 8 consecutive frames captured, all with ~1067–1071 nonzero bytes (finger heuristic: present)
6. 8 strips collected → `fpi_do_movement_estimation` → `fpi_assemble_frames` → `fpi_image_device_image_captured`
7. Minutiae scan completed in 3ms
8. Image saved as `finger.pgm` (136×178 pixels, PGM P5 format)

Note: `Libfprint compiled without pixman support, impossible to resize` — non-fatal; resize
was skipped, image still assembled correctly.

### Artifacts

- Log: `logs/libfprint-guided-20260610-000413.txt`
- Raw frame dumps: `dumps/libfprint-guided-20260610-000413/` (8 × `.bin` files, ~1067 nonzero bytes each)
- Assembled fingerprint: `dumps/libfprint-guided-20260610-000413/finger-assembled-136x178.pgm`

### Fix applied: pre-init packet 15 response_length

`73 14 ec` in `EGIS0577_PRE_INIT_PACKETS` had `response_length = 5356`. The device returns
7 bytes for this command in the pre-init context (confirmed in both probe and driver runs).
Changed to `response_length = 7` in `egis0577.h`.

### Implication for the libfprint driver

The current `egis0577` driver starts with `dev_init` → `fpi_image_device_open_complete` and
then loops post-init continuously. It **skips pre-init entirely**. This is why the runtime
driver has never produced a nonzero frame.

**Fix**: the driver's `SM_INIT` (or a new `SM_PREINIT`) state must send the 29 pre-init
packets once at device open before entering the post-init loop. The `eh575-auto` probe mode
already has logic to detect `resp[5] == 0x01` and switch to pre-init — but that only triggers
on a literal "first boot" response value. A cleaner fix is to always run pre-init on open.

### Confirmed register-state semantics

| Register read        | Armed (capture works) | Unarmed (zeros)     |
|---------------------|-----------------------|---------------------|
| `60 01 fc` byte 5   | `0x00`                | `0x05`              |
| `60 40 fc` byte 5   | `0x00`                | `0x80`              |
| `62 67 03` bytes 8-9| `22 08`/`2b 0c`/`54 13` (nonzero) | `ff 00` / `00 00` |
