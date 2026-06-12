# 2026-06-03 — full EH575 post-init replay on real EH577

Using `build/eh577_usbfs_probe` with a direct usbfs reset before each run:

### Full `eh575-auto` / post-init sequence result

A complete 18-packet EH575 post-init replay succeeded on EH577 with no protocol errors.

Important observed responses:

- `60 00 fc` -> `SIGE 00 aa 01` on one run, `SIGE 00 ab 01` on later runs
- `60 01 fc` -> `SIGE 01 05 01` on repeated reset-based runs
  - this is different from the earlier one-off observation `SIGE 01 01 01`
- `60 40 fc` -> `SIGE 40 80 01`
- `62 67 03` -> 10-byte response `53 49 47 45 67 03 01 00 00 00`
- `64 14 ec` -> **5356-byte response received successfully**

This is a major milestone:

- EH577 does not just accept a few early EH575 packets
- it accepts the **entire EH575 post-init sequence** that was replayed
- it returns the same response lengths expected by the EH575 driver, including the large `5356`-byte payload

### Large payload observation

The `64 14 ec` payload returned on these runs was all zeros in the visible leading/trailing bytes logged by the probe.

Interpretation is still open:

- the device may need extra state or finger interaction before meaningful image data appears
- the current sequence may only fetch a blank / dark frame when idle
- the EH575 driver's pre-init / repeat path may still matter for getting usable capture data

### Interrupt endpoint observation so far

Two interrupt-poll runs were performed with the standalone probe:

- one after reset while idle
- one after a successful post-init replay

Both runs produced **no interrupt payloads** on endpoints `0x83` or `0x84` within the short polling window used by the probe.

This suggests one of:

- interrupts are only emitted on finger events
- interrupts require a different arming sequence
- the OEM stack may use them opportunistically rather than mandatorily for basic bulk transport

### State variability discovered

Earlier, a one-off manual run observed:

- `60 01 fc` -> `SIGE 01 01 01`

After explicit usbfs resets, repeated runs instead observed:

- `60 01 fc` -> `SIGE 01 05 01`

And `60 00 fc` toggled between:

- `SIGE 00 aa 01`
- `SIGE 00 ab 01`

So EH577 clearly has **internal state-dependent response variation** even while remaining EH575-compatible at the framing/length level.

This is now one of the most important reverse-engineering clues.

### Access / tooling friction note

- `sudo -n` worked for the successful reset and probe runs above.
- a later attempt to automate repeated reset+probe loops failed because sudo credentials were no longer available non-interactively.

So more repetition and `usbmon` capture should proceed with an explicit fresh sudo authentication step.
