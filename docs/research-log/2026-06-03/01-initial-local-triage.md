# 2026-06-03 — initial local triage

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
