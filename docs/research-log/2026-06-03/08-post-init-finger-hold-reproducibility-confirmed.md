# 2026-06-03 — post-init finger-hold reproducibility confirmed

A second correctly timed guided post-init finger-hold capture was performed.

Artifacts created:

- `logs/2026-06-03-eh577-guided-postinit-fingerhold-03.txt`
- `logs/2026-06-03-eh577-postinit-fingerhold-03-ascii.txt`
- `logs/2026-06-03-eh577-postinit-fingerhold-reproducibility.txt`
- `dumps/2026-06-03-postinit-fingerhold-03/`

Important note:

- an intermediate run `fingerhold-02` was mistimed because the capture command finished before the touch cue
- that run is useful as a timing lesson, but it is **not** the reproducibility result to rely on

### Second successful non-zero frame

The corrected `fingerhold-03` run again produced a non-zero `64 14 ec` payload:

- length `5356`
- nonzero bytes `1594`
- unique values `104`
- SHA-256 `826fc28994c7371d70761ad6444b2ddcd9322bab0547a2dd74d5f4eeb09f4e28`

This confirms that the earlier non-zero `fingerhold-01` frame was not a one-off accident.

### Comparison against the first successful frame

`fingerhold-01`:

- nonzero `1305`
- unique values `80`
- SHA-256 `51e6dcd08a54f9d1aeed5269e362e4720c2c3835994c586bdac299df569c95c3`
- bbox `x=0..69`, `y=0..51`

`fingerhold-03`:

- nonzero `1594`
- unique values `104`
- SHA-256 `826fc28994c7371d70761ad6444b2ddcd9322bab0547a2dd74d5f4eeb09f4e28`
- bbox `x=0..69`, `y=0..51`

The two frames differ at `2322` byte positions.

### Interpretation

This is strong evidence that EH577 is returning real, variable finger-dependent image-like data rather than a fixed template block or random deterministic noise.

At this point the evidence strongly supports all of the following:

1. EH577 is an EH575-family bulk image device.
2. The **post-init path** is the current reliable path for meaningful capture.
3. The **repeat path alone** still does not produce meaningful payloads in the tested conditions.
4. Frame content varies between acquisitions, as expected for real scans.
