# 2026-06-09 — pre-init required to arm sensor; confirmed reproducible nonzero captures

### Context: stuck state investigation

After June 4 sessions, the probe consistently returned all-zero `64 14 ec` frames with
`60 01 fc` → `01 05 01` and `60 40 fc` → `40 80 01`. 15 timing-varied finger-swipe attempts
(loop-capture script) all returned zero frames and `62 67 03` → `ff 00`.

Previously suspected "contaminated device state" — this was **wrong**. The idle readings
`01 05 01` / `40 80 01` / `ff 00` are the normal no-finger idle values and appear in the
June 3 idle/postinit18 logs too. USB resets do not change these values because they reflect
the current sensor arm state, not a corrupted register.

### Root cause: pre-init is required before post-init

The `eh575-preinit` sequence (29 packets) contains register **writes** (e.g. `61 80 00`,
`61 0a fd`, `61 35 02`, `61 0c 22`, `61 09 83`, `61 0b 03`) that arm the sensor's
optical/touch hardware. The post-init sequence has **no writes** — it only reads registers
and then calls `64 14 ec`.

When pre-init was skipped (all June 9 loop-capture runs), the sensor stayed unarmed:
- `60 01 fc` → `01 05 01` (byte 5 = 0x05: sensor not armed)
- `60 40 fc` → `40 80 01` (bit 7 set: scan hardware inactive)

After running pre-init first:
- `60 01 fc` → `01 00 01` (restored)
- `60 40 fc` → `40 00 01` (restored)
- `62 67 03` → `54 13` during finger hold (nonzero: finger detected)
- `64 14 ec` → nonzero frames (1566–2594 nonzero bytes)

### Results (preinit-then-capture experiment, 8 cycles)

| Run | Nonzero bytes | 62 67 03 bytes 8-9 |
|-----|--------------|---------------------|
| 1   | 1            | ff 00 (no finger)   |
| 2   | 1            | ff 00               |
| 3   | 4            | ff 00               |
| 4   | 5            | ff 00               |
| 5   | 1846         | 54 13 (finger!)     |
| 6   | 2594         | (nonzero)           |
| 7   | 1566         | (nonzero)           |
| 8   | 1705         | (nonzero)           |

Run 6 best frame: 2594 nonzero bytes, 245 unique values. PGM saved to
`dumps/preinit-capture-20260609-235532/run-06-best-frame.pgm`.
