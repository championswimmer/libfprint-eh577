# 2026-06-03 — guided finger-interaction captures

A timed helper script was added to decouple finger prompts from the LLM loop:

- `tools/eh577_guided_capture.sh`

A payload-render helper was also added:

- `tools/eh577_dump_to_pgm.py`

Artifacts created in this phase:

- `logs/2026-06-03-eh577-guided-interrupt-finger-2cycle.txt`
- `logs/2026-06-03-eh577-repeat-fingerhold-01.txt`
- `logs/2026-06-03-eh577-guided-postinit-fingerhold-01.txt`
- `logs/2026-06-03-eh577-postinit-fingerhold-01-ascii.txt`
- `logs/2026-06-03-eh577-finger-analysis.txt`
- `dumps/2026-06-03-repeat-fingerhold-01/`
- `dumps/2026-06-03-postinit-fingerhold-01/`

### Guided interrupt result

A two-cycle guided interrupt poll was run while the user touched, held, and removed a finger on cue.

Result:

- no interrupt payloads were observed on `0x83`
- no interrupt payloads were observed on `0x84`

So interrupts are still silent even during explicit finger interaction in the tested window.

### Guided repeat-path finger-hold result

A repeat-path run was performed while the user held a finger on the sensor.

Result:

- `64 14 ec` still returned `5356` bytes of all zeros
- its SHA-256 matched the idle payload exactly

This means the repeat path alone is **not** sufficient to produce a meaningful payload in the currently tested state/timing.

### Guided post-init finger-hold result

A full post-init run was then performed while the user held a finger on the sensor.

This produced the first major breakthrough of the session:

- packet `64 14 ec` returned a `5356`-byte payload with `1305` non-zero bytes
- the payload had `80` unique byte values
- the SHA-256 changed from the idle zero-frame hash to:
  - `51e6dcd08a54f9d1aeed5269e362e4720c2c3835994c586bdac299df569c95c3`

This is the first strong evidence that EH577 is returning meaningful image-like data during finger contact.

### Structural observations on the non-zero payload

Assuming EH575 geometry (`103 x 52`):

- the payload length matches exactly one `103 * 52` frame
- the non-zero bounding box spans all rows but only columns `0..69`
- columns `70..102` were zero in this capture

This pattern strongly suggests structured sensor data rather than random noise.

A PGM rendering was generated at:

- `dumps/2026-06-03-postinit-fingerhold-01/eh575-postinit-17-64_14_ec.pgm`

And an ASCII preview was saved at:

- `logs/2026-06-03-eh577-postinit-fingerhold-01-ascii.txt`

### Status-byte changes during meaningful capture

Compared with the earlier idle post-init capture, the finger-hold post-init run also changed several smaller responses:

- `60 01 fc`: `... 05 01` -> `... 00 01`
- `60 40 fc`: `... 80 01` -> `... 00 01`
- `60 2d 02`: `... c7 01` -> `... 47 01`
- `62 67 03`: `... ff 00` -> `... 22 08`

This reinforces the idea that some EH577 reply bytes are reflecting internal capture/finger state.

### Updated interpretation after finger testing

At this point the best working model is:

1. EH577 is an EH575-family image device using the bulk protocol.
2. The **post-init path** is important for meaningful capture.
3. The **repeat path alone** is not enough in the currently tested scenario.
4. Interrupt endpoints `0x83` / `0x84` appear optional, inactive, or OEM-armed rather than required for the observed bulk capture path.
