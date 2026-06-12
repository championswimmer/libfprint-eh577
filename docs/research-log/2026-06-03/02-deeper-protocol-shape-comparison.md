# 2026-06-03 — deeper protocol-shape comparison

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
