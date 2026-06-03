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

### Practical next actions after this batch

1. Replay the remainder of the EH575 post-init sequence on EH577.
2. Check whether packet `64 14 ec` yields a `5356`-byte image-like payload on EH577.
3. Poll interrupt endpoints `0x83` and `0x84` before/after init.
4. Capture `usbmon` traces for these experiments.
5. Start a new EH577 driver skeleton by adapting the EH575 transport logic.
