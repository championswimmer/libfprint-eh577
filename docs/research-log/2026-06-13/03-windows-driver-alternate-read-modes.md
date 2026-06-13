# Windows driver alternate-read-mode clues

Date: 2026-06-13

## Question

The current EH577 libfprint path reads the EH575-derived bulk snapshot command:

- `EGIS 64 14 ec` -> `5356` bytes (`103 x 52`)

This works, but it is a very small raw window. The question was whether the
Windows reference driver points to other ways to read more data from the device.

## Inputs checked

- `windows-driver/EgisTouchFP0577.dll`
- `windows-driver/EgisTouchFPSensor0577.dll`
- `windows-driver/EgisTouchFPEngine0577.dll`
- `windows-driver/EgisTouchFP0577.inf`
- `windows-driver/*_strings.txt`
- `refs/EgisTec-EH575/findings/output.json`
- `refs/EgisTec-EH575/findings/EgisTec-EH575/decompiled_source/ghidra/EgisTouchFP0575.c`

Tools used:

- `tools/windows_pe_scan.py`
- `strings`
- `objdump -p`
- `objdump -d -Mintel`
- direct PE/RIP-relative string-reference scans

## Findings

### 1. Windows still looks press/touch-driven, not swipe-driven

The EH577 Windows DLL still has the same press-like capture strings found on
2026-06-12:

- `get_image send EGIS_WAIT_INTERRUPT`
- `get_image receive EGIS_WAIT_INTERRUPT`
- `get_image receive EGIS_TZ_STATE_NOTIFY_FINGER_DOWN`
- `confirm_finger_stable`
- `get_image finger_quality = %d`
- `check_finger_remove`

I found no new positive evidence for swipe/strip/stitch assembly. Alternate
read modes should therefore be treated as alternate press-frame acquisition or
quality/detect modes, not as a reason to reintroduce swipe assembly.

### 2. EH577 INF pins `FetchImageMode` to `0x80000003`

The INF contains:

- `FetchImageMode = 0x80000003`
- `FingerOnMode = 0`
- `FingerOnThresholdLoose = 2`
- `FingerOnThreshold = 6`

Disassembly of `EgisTouchFP0577.dll` shows the main capture object reads
`FetchImageMode` and passes it into a lower device-layer method before register
writes and image reads.

Relevant disassembly shape:

- read `FetchImageMode`
- store it at an object field
- call a device-layer method at vtable offset `0x68` with that mode
- write registers `3`, `9`, `2`, `5`, then `8`
- run the image acquisition loop

So Windows has a capture-mode knob, but the public package configures only one
EH577 mode: `0x80000003`.

### 3. Windows allocates and processes a larger internal image buffer

The main EH577 DLL constructor allocates a `0x4000` byte buffer and initializes
working image dimensions around `0x80 x 0x80`.

Concrete static clues:

- `LocalAlloc(..., 0x4000)`
- object image buffer pointer stored after allocation
- width/height-like fields initialized to `0x80`
- the get-image path calls image-read and image-quality methods with width and
  height fields, then logs `Image` and `Image_not_bkg`
- non-mode-2 images are background-subtracted/inverted after read

This is stronger evidence than the string scan alone: the Windows stack has a
larger internal image path than the raw `103 x 52` frame that the current Linux
driver submits.

However, this does **not** yet prove the USB device directly returns a single
`0x4000` frame. It may be:

- a different hardware read command,
- multiple hardware reads fused by the Windows stack,
- a decrypted/depacked representation,
- or a processed frame derived from the smaller sensor data.

### 4. The older EH575 decompilation exposes candidate command families beyond `64`

The EH575 decompilation has low-level helpers for more command families than the
current Linux driver uses:

- `0x60` status/register read:
  - sends 7 bytes, expects 7 bytes
- `0x73` short command + follow-up read:
  - current probe already showed `73 14 ec` differs by context
- `0x80` read:
  - packet shape uses `EGIS 80 <start> <count>`
  - response length is `(count & 0xff) << 8`
  - requires `(start + count) < 0x101`
- `0x81` two-stage read:
  - sends `EGIS 81 <start> <count>`
  - then reads `(count & 0xff) << 8` bytes and verifies `SIGE`
- `0x90` variable-length query/read:
  - sends `EGIS 90 00 00`
  - parses a length from the returned `SIGE` payload
  - then reads that many bytes
- `0xe0` write/config path:
  - builds a payload containing `0x14ec`
  - likely calibration/config data, not a simple frame fetch

The `0x80`/`0x81` helpers are the most interesting for "more frame data":
`count = 0x40` implies a `0x4000` byte transfer, matching the Windows internal
buffer size. This is a hypothesis to probe, not a validated EH577 command yet.

### 5. The engine DLL still contains a `52, 103` geometry pair

The existing `52 x 103` raw-geometry clue remains present in
`EgisTouchFPEngine0577.dll`, matching the `5356` byte bulk frame. That means the
small raw frame is not imaginary; the Windows stack knows about that geometry
too.

The new conclusion is not "the current frame is wrong". It is:

- `103 x 52` is a real raw geometry,
- Windows also appears to maintain a larger image buffer,
- we need live USB evidence to decide whether that larger image comes from an
  alternate USB read or from Windows-side expansion/processing.

## Candidate live probes

Safe next probe direction, after normal pre-init and post-init arming:

1. Try `EGIS 90 00 00` as a metadata/length query and log whether it returns a
   valid `SIGE` length.
2. Try small `0x80` reads first, for example:
   - `EGIS 80 00 01` -> expect up to `0x0100` bytes if supported
   - `EGIS 80 00 04` -> expect up to `0x0400` bytes
   - `EGIS 80 00 10` -> expect up to `0x1000` bytes
3. Only if those behave sanely, try:
   - `EGIS 80 00 40` -> candidate `0x4000` byte image-buffer read
4. Repeat the same shape for `0x81` only if `0x80` gives evidence it is safe and
   meaningful.
5. Keep monitoring interrupt endpoints `0x83` / `0x84`, but treat them as
   detect/finger-down signals unless they actually deliver frame-sized payloads.

Avoid jumping straight to full-size reads in the production driver. These should
first be added as standalone probe modes with short timeouts, dump capture, and
clear logging of partial transfers.

## Practical conclusion

There is now static evidence for **possible alternate read modes** beyond the
current `64 14 ec` bulk snapshot:

- `FetchImageMode = 0x80000003` is a real Windows capture-mode knob.
- The Windows stack allocates a `0x4000` image buffer.
- The EH575-family decompilation exposes `0x80`/`0x81` reads whose response
  length scales as `count << 8`, which can naturally reach `0x4000`.

But there is still no evidence for swipe capture or host-side strip assembly.
The next step is live probing of `0x90`, then conservative `0x80`/`0x81` reads,
not changing the libfprint model yet.
