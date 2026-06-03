# EH577 research log

This file is a running log of what was investigated on the local machine and what was learned.

## 2026-06-03 — initial local triage

### Repository state

- Working directory: `/home/championswimmer/Development/Cpp/libfprint-eh577`
- The directory started effectively empty and was **not** a git repository yet.

### Host environment

- Kernel: `Linux beelink 6.17.0-22-generic x86_64`
- Installed fingerprint stack packages:
  - `fprintd 1.94.5-2`
  - `libfprint-2-2 1:1.94.9+tod1-1ubuntu0.2`
  - `libfprint-2-tod1 1:1.94.9+tod1-1ubuntu0.2`
  - `libpam-fprintd 1.94.5-2`

### Target device identification

Observed with `lsusb` and sysfs:

- USB ID: `1c7a:0577`
- USB vendor string: `EgisTec`
- USB product string: `EgisTec EH577`
- Serial: `07B74AD1`
- Sysfs path: `/sys/bus/usb/devices/3-3`
- `usb-devices` reports:
  - `Cls=ff(vend.)`
  - `Sub=00`
  - one interface with `Sub=ff Prot=00`
  - **no kernel driver bound** (`Driver=(none)`)

### Full USB interface shape

`lsusb -d 1c7a:0577 -v` reported:

- Device class: vendor specific (`0xff`)
- `bNumConfigurations = 1`
- `bNumInterfaces = 1`
- Interface 0 has 4 endpoints:
  - `0x01` bulk OUT, 512 bytes
  - `0x82` bulk IN, 512 bytes
  - `0x83` interrupt IN, 16 bytes, interval 8
  - `0x84` interrupt IN, 16 bytes, interval 8
- Power: 100mA
- Speed: high-speed USB 2.0 (480 Mbps)

This endpoint layout is useful because it strongly suggests a proprietary command/response protocol over bulk endpoints plus asynchronous notifications over one or two interrupt endpoints.

### Current Linux support status

- `fprintd-enroll -f right-index-finger` failed with:
  - `NoSuchDevice: No devices available`
- So the currently installed stack does **not** expose EH577 as a usable device.

### What exists in the installed libfprint stack

String inspection of `/usr/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0` shows:

- Egis-related strings and drivers present:
  - `EgisTec ES603`
  - `FpDeviceEgis0570`
  - `libfprint-egis0570`
  - `libfprint-egismoc`
  - `fpi_device_egismoc_init`
- Explicit source references embedded in the shared object:
  - `../libfprint/drivers/egis0570.c`
  - `../libfprint/drivers/egismoc/egismoc.c`
- No obvious string match for `0577` support was found.

Working hypothesis:

- EH577 is likely related to other Egis USB devices, but is not covered by the current Ubuntu libfprint build.
- It may be closer either to:
  - the older image-based Egis path (`egis0570` style), or
  - the newer match-on-chip path (`egismoc` style),
  - or a distinct protocol family.

### Other observations

- `lsusb` names vendor `1c7a` as `LighTuning Technology Inc.` while the device strings identify as `EgisTec`.
- This matches known Egis/LighTuning naming overlap seen in existing libfprint strings.

### Access constraints encountered

- `dmesg` requires elevated privileges here (`Operation not permitted`).
- If kernel logs or USB bus capture are needed, sudo will be required.

### EH575 reference repository pulled locally

Reference clones created:

- `refs/EgisTec-EH575` at commit `57fa58a`
- `refs/libfprint` at commit `d79f157`

#### EH575 repository high-level takeaways

From `refs/EgisTec-EH575/README.md`:

- The EH575 effort is archived but contains:
  - `libfprint.patch`
  - `archive/` experiments
  - `findings/` from reverse engineering and USB traffic analysis
- The author states the **USB sequence has been found**.
- The intended direction was libfprint integration.

#### EH575 patch takeaways

From `refs/EgisTec-EH575/libfprint.patch`:

- Adds a new libfprint image driver `egis0575.c`.
- Treats EH575 as an **image/swipe-style** device rather than MOC.
- Driver structure is notably similar to `egis0570.c`:
  - bulk command/response loop
  - repeated frame capture
  - strip assembly into a final image
  - finger-presence heuristic based on image statistics
- This is relevant because EH577 may be protocol-adjacent to EH575 rather than to the newer Egis MOC family.

#### Windows driver naming from EH575 notes

From `refs/EgisTec-EH575/findings/WindowsDrivers.txt`:

- `EgisTouchFP0575.dll`
- `EgisTouchFPEngine0575.dll`
- `EgisTouchFPSensor0575.dll`
- Transport is via WinUSB / UMDF according to the notes.

This is important because EH577 may have similarly named Windows binaries (`...0577...`) that can be harvested for strings, exports, or protocol clues.

#### EH575 reverse-engineering output clues

From `refs/EgisTec-EH575/findings/output.json` (decompiler output):

- Many commands appear to use a 7-byte command/response framing.
- Responses are repeatedly validated against magic `0x45474953` = ASCII `EGIS`.
- Decompiled command IDs include at least:
  - `0x03`, `0x04`, `0x05`
  - `0x60`, `0x61`
  - `0x62`, `0x63`, `0x70`, `0x71`, `0x72`, `0x73`
  - `0x80`, `0x81`, `0x90`, `0x97`
  - `0xE0`
- There are strong signs of:
  - register-like read/write operations
  - block transfers
  - a checksum over message payloads
  - request/response transport wrappers in the Windows driver

This does **not** prove EH577 uses the same protocol, but it gives a concrete command vocabulary to test against captures.

#### Upstream libfprint comparison points

- `egis0570.c` is clearly an image driver with captured strip assembly.
- `egismoc/egismoc.c` is clearly a match-on-chip family using:
  - bulk command endpoint(s)
  - interrupt endpoint for finger events
  - explicit response prefix/suffix validation

EH577 has:

- one bulk OUT
- one bulk IN
- **two** interrupt IN endpoints

This makes EH577 especially interesting because its USB topology could fit a hybrid or more complex notification scheme than EH575/0570.

### Immediate next steps recorded

1. Mine EH575 patch/header files for packet arrays, dimensions, and transport assumptions.
2. Inspect libfprint `egis0570.h` and `egismoc.h` for endpoint sizes and framing constants.
3. Search for EH577-specific Windows package names / strings online and locally.
4. Try to obtain live USB captures from the actual EH577 hardware stack.
5. If needed, use sudo for `dmesg`, `usbmon`, or descriptor/bus captures.

## 2026-06-03 — deeper protocol-shape comparison

### Repository state after setup

- Workspace is now a git repository (`git init` completed).
- Uncommitted files currently include:
  - `README.md`
  - `docs/research-log.md`
  - `docs/todo.md`
  - `refs/EgisTec-EH575/`
  - `refs/libfprint/`

### EH575 packet/header details extracted from the patch

From `refs/EgisTec-EH575/libfprint.patch` / `egis0575.h`:

- EH575 driver uses only two endpoints:
  - bulk OUT `0x01`
  - bulk IN `0x82`
- Packet framing is still clearly Egis-branded:
  - host requests begin with ASCII `EGIS`
  - responses are expected to begin with `SIGE`
- Packet sizes are **not fixed** the way `egis0570` is:
  - 7-byte packets
  - 9-byte packets
  - 13-byte packets
  - 18-byte packets
  - large image/data response `5356` bytes
- Important command bytes seen in the EH575 sequences include:
  - `0x60`, `0x61`, `0x62`, `0x63`, `0x64`, `0x73`
- EH575 image geometry in the patch:
  - width `103`
  - height `52`
  - capture size `5356`
  - strip height `24`
  - `8` consecutive captures assembled into a final image

This makes EH575 materially different from upstream `egis0570`, and closer to the command vocabulary found in the EH575 Windows reverse-engineering notes.

### Upstream libfprint driver shape comparison

#### `egis0570`

- Endpoints:
  - bulk OUT `0x04`
  - bulk IN `0x83`
- Uses only 7-byte request packets.
- Repeatedly receives a large `32512`-byte buffer.
- Treats the payload as 5 frames of `114x57` each.
- No interrupt endpoints are involved.

#### `egis0575` (patch from reference repo)

- Endpoints used by the Linux patch/prototype:
  - bulk OUT `0x01`
  - bulk IN `0x82`
- The patch's own test fixture descriptor shows the EH575 hardware itself also exposes:
  - interrupt IN `0x83`
  - interrupt IN `0x84`
- Mixed-length commands.
- Large data response size around `5356` bytes.
- The public Linux patch does not use interrupt endpoints, but OEM Windows artifacts mention endpoint `0x83` reads.

#### `egismoc`

- Endpoints:
  - bulk OUT `0x02`
  - bulk IN `0x81`
  - interrupt IN `0x83`
- Uses an 8-byte `EGIS`/`SIGE` wrapper with dynamically computed check bytes.
- Semantics are clearly match-on-chip:
  - list enrolled IDs
  - identify
  - enroll
  - delete
  - capture/status polling

### EH577 hardware shape relative to known families

Current EH577 descriptor facts:

- bulk OUT `0x01`
- bulk IN `0x82`
- interrupt IN `0x83`
- interrupt IN `0x84`
- high-speed USB 2.0
- remote wakeup advertised
- vendor-specific class/subclass (`ff/ff` at interface level)

Interpretation after correcting the EH575 comparison:

- EH577 shares **bulk endpoint numbering** with the EH575 patch (`0x01`/`0x82`).
- EH577 also matches the broader EH575-family descriptor shape, because the EH575 test fixture in the patch includes the same extra interrupt endpoints `0x83` and `0x84`.
- EH577 still does **not** match the exact endpoint numbering used by upstream `egismoc` (`0x02`/`0x81`/`0x83`).

Revised working hypothesis:

- EH577 is very likely in the **EH575 protocol family**.
- The first implementation attempt should start from an EH575-like bulk transport skeleton.
- Interrupts `0x83` and `0x84` may still matter, but they are no longer a sign that EH577 is unlike EH575.

### libfprint / udev support observations

#### Installed system libfprint rules

From `/usr/lib/udev/hwdb.d/60-autosuspend-libfprint-2.hwdb`:

- supported Egis devices currently listed are:
  - `0570`
  - `0571`
  - `0582`
  - `0583`
  - `0586`
  - `0587`
  - `05A1`
- `0577` is **not** in the installed generated hwdb for supported libfprint devices.

From `/usr/lib/udev/rules.d/70-libfprint-2.rules`:

- nothing USB/Egis-specific is present for this sensor
- only Elan SPI helper rules are present in this packaging

#### Upstream source tree nuance

From the cloned `refs/libfprint/data/autosuspend.hwdb` source file:

- `usb:v1C7Ap0577*` **is present**
- nearby IDs also include `0575`, `0576`, and `057E`

This is an important nuance:

- upstream source data knows `0577` is a fingerprint-reader-class device worth autosuspend handling,
- but the installed/generated supported-device list still does **not** expose it through an actual libfprint driver.

That suggests `0577` is at least known in libfprint-maintainer data, even if no public driver has landed.

### Runtime power-management observations for the live EH577 device

From `udevadm info -a -p /sys/bus/usb/devices/3-3`:

- `power/control = auto`
- `power/persist = 0`
- `power/runtime_status = suspended`
- `power/autosuspend_delay_ms = 2000`
- `supports_autosuspend = 1`
- remote wakeup is advertised at the USB descriptor level

This means the device normally idles suspended under runtime PM. Capturing protocol traffic may need active user interaction or a driver/userspace client that wakes it first.

### Searches for vendor userspace on this Linux install

Searched under `/usr`, `/usr/lib`, `/usr/libexec`, `/usr/bin`, `/usr/sbin`, and `/opt` for Egis/fingerprint-related artifacts.

Results:

- found only generic Linux fingerprint stack pieces:
  - `libfprint-2.so.2.0.0`
  - `libfprint-2-tod.so.1`
  - `fprintd` tools and service
  - udev/hwdb support files
- found **no obvious Egis OEM user-space binaries** on this Linux install
  - no `EgisTouch*`
  - no EH577-named blobs
  - no separate TOD plugin directory entries

### Online-search attempts

- Automated lookups against `grep.app` were rate-limited (`HTTP 429`).
- Automated DuckDuckGo HTML queries were blocked by bot-challenge pages.

So internet search from this environment is possible in principle, but unauthenticated automated code/web search will likely require alternate sources or a manual browser-assisted step.

## 2026-06-03 — first live protocol probe on EH577

A small standalone usbfs probe was added:

- source: `tools/eh577_usbfs_probe.c`
- binary: `build/eh577_usbfs_probe`

This probe talks directly to `/dev/bus/usb/...` using `USBDEVFS_BULK`, so it does not depend on libusb headers or pyusb.

### Probe setup

- Device power control was forced to `on` via sysfs.
- Runtime status became `active`.
- The probe claimed interface 0 and sent the first EH575 post-init bulk packets over:
  - OUT `0x01`
  - IN `0x82`

### Critical result: EH577 accepts EH575 packets

Observed successful exchanges:

1. `EGIS 60 00 fc` -> `SIGE 00 aa 01`
2. `EGIS 60 01 fc` -> `SIGE 01 01 01`
3. `EGIS 60 40 fc` -> `SIGE 40 80 01`
4. `EGIS 63 09 0b 83 24 00 44 0f 08 20 20 01 05 12`
   -> `SIGE 09 0b 01 83 24 00 44 0f 08 20 20 01 05 12`
5. `EGIS 63 26 06 06 60 06 05 2f 06`
   -> `SIGE 26 06 01 06 60 06 05 2f 06`

#### Important nuance discovered after comparing with the EH575 driver logic

The EH575 patch does **not** simply continue after packet 1.

- In `egis0575.c`, when the response to `EGIS 60 01 fc` has byte 5 equal to `0x01`, the driver switches into the longer `PRE_INIT` sequence.
- Our first raw probe kept sending later post-init packets anyway, and EH577 still accepted them.

This means one of two things is true:

1. EH577 is permissive and accepts both paths, or
2. our first probe proved wire compatibility but not yet the *canonical* init path.

So future probing should prefer an **auto-init** mode that follows the EH575 branch rule.

This is the strongest finding so far.

#### What this proves

- EH577 definitely understands **EH575-style `EGIS` bulk commands**.
- EH577 definitely returns **EH575-style `SIGE` responses**.
- The `60 01 fc -> 01 01 01` response exactly matches a state cue already documented in the EH575 patch comments.

So EH577 is no longer just *suspected* to be similar to EH575; it has now been shown to be **wire-compatible with at least the early EH575 init sequence**.

### Notes on access friction

- Running the probe as the unprivileged user fails with `open: Permission denied` on `/dev/bus/usb/003/004`.
- Some `sudo` invocations worked during probing, but later attempts became inconsistent and started requiring interactive authentication again.
- Because of that, a full replay of all 18 EH575 post-init packets and interrupt polling is still pending.

### Additional artifacts created after the first live probe

#### Improved standalone probe

`tools/eh577_usbfs_probe.c` was expanded to include:

- `reset` mode via `USBDEVFS_RESET`
- proper interrupt URB polling for endpoints `0x83` / `0x84`
- explicit `eh575-preinit`, `eh575-postinit`, and `eh575-repeat` modes
- `eh575-auto` mode that follows the EH575 branch rule and switches into pre-init when packet `60 01 fc` returns the `01 01 01` state

This should reduce ambiguity in future captures and make it easier to identify the first real divergence.

#### WIP libfprint port base

A local driver work area now exists under `wip-libfprint/`:

- `wip-libfprint/drivers/egis0575.c`
- `wip-libfprint/drivers/egis0575.h`
  - extracted from the EH575 patch for direct local reference
- `wip-libfprint/drivers/egis0577.c`
- `wip-libfprint/drivers/egis0577.h`
  - first EH577 draft derived from the EH575 driver skeleton

Current status of the EH577 draft:

- device ID changed to `1c7a:0577`
- component / full-name strings renamed to EH577
- sequence tables still intentionally mirror EH575 until live captures show otherwise
- interrupt endpoints are still not integrated into the libfprint draft yet

## 2026-06-03 — full EH575 post-init replay on real EH577

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

## 2026-06-03 — explicit EH575 pre-init and repeat path replays on EH577

After the successful post-init replay, the standalone probe was used to run the other EH575 bulk paths explicitly:

- `eh575-preinit`
- `eh575-repeat`

Artifacts created:

- `logs/2026-06-03-eh577-preinit-run.txt`
- `logs/2026-06-03-eh577-repeat-run.txt`
- `logs/2026-06-03-eh577-sequence-comparison.txt`
- `dumps/2026-06-03-preinit/`
- `dumps/2026-06-03-repeat/`

### Repeat path result

The full EH575 repeat path succeeded on EH577.

Important points:

- all 9 packets completed successfully
- `64 14 ec` again returned `5356` bytes
- the payload was again all zeros in the idle capture
- the smaller replies matched the same general EH575-compatible shape already seen in post-init

Notable replies:

- `61 2d 20` -> `53 49 47 45 2d 20 01`
- `60 00 20` -> `53 49 47 45 00 aa 01`
- `60 01 20` -> `53 49 47 45 01 05 01`
- `60 2d 02` -> `53 49 47 45 2d c7 01`
- `62 67 03` -> `53 49 47 45 67 03 01 00 ff 00`
- `64 14 ec` -> `5356` bytes, all zero in this run

### Pre-init path result

The full EH575 pre-init path also succeeded on EH577.

Important points:

- all 29 packets completed successfully
- EH577 accepted pre-init-specific commands without transport/protocol failure
- the pre-init path's `73 14 ec` command **did not** return a large payload
- instead, `73 14 ec` returned exactly 7 zero bytes

Notable replies:

- `60 00 00` -> `53 49 47 45 00 aa 01`
- `60 01 00` -> `53 49 47 45 01 05 01`
- `60 80 00` -> `53 49 47 45 80 02 01`
- `73 14 ec` -> `00 00 00 00 00 00 00`
- `60 40 ec` -> `53 49 47 45 40 00 01`
- `60 00 66` -> `53 49 47 45 00 ab 01`
- `60 01 66` -> `53 49 47 45 01 00 01`
- `60 40 66` -> `53 49 47 45 40 00 01`

### Interpretation of the new path coverage

This is an important strengthening of the EH575-family model.

EH577 now appears to accept not only the EH575 post-init path, but also:

- the EH575 pre-init path
- the EH575 repeat path

That means the main reverse-engineering question is no longer whether EH577 belongs to the EH575 protocol family; it almost certainly does.

The more precise open questions are now:

1. which path is the canonical capture path for EH577,
2. when the large payload becomes meaningful instead of zero-filled,
3. and how the variable state bytes should influence driver branching.

### Tooling note

A direct `sudo -n env EH577_DUMP_DIR=...` invocation behaved inconsistently once and triggered an unrelated-looking warning before sudo rejected the command.

Using the more explicit form below worked reliably for the pre-init replay:

```bash
sudo -n bash -lc 'export EH577_DUMP_DIR="$PWD/dumps/2026-06-03-preinit"; \
  ./build/eh577_usbfs_probe /dev/bus/usb/003/004 eh575-preinit'
```

So future scripted runs that need environment variables should prefer `sudo -n bash -lc '...'`.

## 2026-06-03 — repeated early-state sampling

To characterize the earliest varying status bytes, the first two EH575 post-init packets were sampled repeatedly in two modes:

1. 10 runs with a fresh usbfs reset before each run
2. 6 runs without resets between runs

Artifacts created:

- `logs/2026-06-03-eh577-first2-state-samples.txt`
- `logs/2026-06-03-eh577-first2-noreset-samples.txt`
- `logs/2026-06-03-eh577-first2-state-analysis.txt`

### Observed replies in the repeated sampling series

Across both repeated series, the replies were fully stable:

- `60 00 fc` -> `53 49 47 45 00 ab 01`
- `60 01 fc` -> `53 49 47 45 01 00 01`

This is different from earlier successful captures, which had shown:

- `60 00 fc` -> `... aa 01`
- `60 01 fc` -> `... 05 01`
- and one early one-off `60 01 fc` -> `... 01 01`

### Interpretation

EH577 clearly has multiple internal reply states for the earliest status-like commands.

At this point at least three packet-1 variants have been observed across the session history:

- `01 00 01`
- `01 05 01`
- `01 01 01`

And packet-0 has at least two observed variants:

- `00 aa 01`
- `00 ab 01`

This means the driver should not assume a single fixed early-state reply. Instead it should:

- treat those bytes as stateful
- branch conservatively
- and only rely on stronger invariants such as transport framing, packet lengths, and larger behavior patterns

### Early-state variation table

| Command | Observed replies so far | Notes |
|---|---|---|
| `60 00 fc` | `... 00 aa 01`, `... 00 ab 01` | varies across session history |
| `60 01 fc` | `... 01 00 01`, `... 01 05 01`, `... 01 01 01` | most important current branch/state indicator |
| `60 00 66` | `... 00 aa 01`, `... 00 ab 01` | also stateful |
| `60 01 66` | `... 01 05 01`, `... 01 00 01` | seen in post-init vs pre-init captures |
| `60 2d 02` | `... 2d 47 01`, `... 2d c7 01` | later status variation |
| `62 67 03` | `... 67 03 01 00 00 00`, `... 67 03 01 00 ff 00` | later status variation |

## 2026-06-03 — guided finger-interaction captures

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

### Practical next actions after this batch

1. Repeat the post-init finger-hold capture to check reproducibility.
2. Compare multiple non-zero captures for spatial similarity and stability.
3. Capture `usbmon` traces for a successful non-zero post-init finger-hold run.
4. Update the EH577 libfprint draft to prefer the post-init-led capture path.
5. Investigate whether the smaller state-byte shifts can be used as finger/capture readiness indicators.
