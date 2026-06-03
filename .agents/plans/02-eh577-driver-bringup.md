# EH577 Driver Bringup

## Goal

Turn the current EH577 reverse-engineering results into a buildable, testable `libfprint` driver path with enough evidence to support open-source bringup on real hardware.

The concrete target is not just “some code compiles”, but:

- a repeatable probe/capture workflow,
- a documented understanding of EH577 init/state behavior,
- an EH577 driver patch derived from the EH575 work,
- and an end-to-end validation path from local build to `fprintd` device recognition.

## Context

- Relevant local files:
  - `AGENTS.md`
  - `docs/research-log.md`
  - `docs/protocol-comparison.md`
  - `docs/todo.md`
  - `logs/2026-06-03-eh577-postinit18-analysis.txt`
  - `tools/eh577_usbfs_probe.c`
  - `wip-libfprint/drivers/egis0577.c`
  - `wip-libfprint/drivers/egis0577.h`
  - `refs/EgisTec-EH575/libfprint.patch`
  - `refs/libfprint/`
- Current validated hardware facts:
  - device is `1c7a:0577`
  - endpoints are `0x01` bulk OUT, `0x82` bulk IN, `0x83` interrupt IN, `0x84` interrupt IN
  - EH577 accepts the full EH575 post-init bulk sequence
  - `64 14 ec` returns a `5356`-byte payload
  - current idle payload dump is all zeros
- Current constraints:
  - direct USB access requires `sudo`
  - `sudo -n` can expire during longer sessions
  - the current Linux install has no OEM Egis userspace to observe
  - state bytes vary across runs, so hardcoded assumptions are risky

### External research notes already gathered

- Web search indicates Windows packages for `USB\VID_1C7A&PID_0577` likely use an INF named `EgisTouchFP0577s.inf` and a driver version around `3.6.1.8`.
- `libfprint` developer docs confirm the normal bringup path is:
  - enumerate devices via `FpContext`
  - open the device
  - validate device operations through examples and API flows
- Wireshark / usbmon docs confirm Linux USB capture workflow:
  - `sudo modprobe usbmon`
  - capture on `usbmonX` for the matching bus or `usbmon0`
  - make `/dev/usbmon*` readable if non-root capture is desired

These web-search clues should be treated as research leads, not yet as proven EH577 protocol facts.

## Steps

1. Consolidate the known-good local workflow and preserve the current baseline.
2. Capture more live EH577 behavior under controlled conditions:
   - reset-only
   - post-init idle
   - pre-init path
   - repeat path
   - finger touch / hold / remove
3. Characterize state variation in small replies and determine whether EH577 really wants the EH575 pre-init branch.
4. Save and analyze more `5356`-byte payloads to determine when they gain entropy or image structure.
5. Determine whether interrupts `0x83` / `0x84` are required, optional, or only event-driven.
6. Update the EH577 WIP driver to match the observed state machine instead of merely renaming EH575.
7. Turn the WIP files into a buildable patch against the local `refs/libfprint` tree.
8. Validate the patched build locally and record exactly what works and what still fails.
9. Keep research notes, logs, dumps, and plan checkboxes updated as each phase advances.

## Validation

The bringup plan is considered materially successful when all of the following exist:

- a repeatable capture log for idle and finger-interaction runs under `logs/`
- raw payload dumps under `dumps/` for at least:
  - idle post-init
  - finger-present post-init or repeat path
- a documented interpretation of variable response bytes in `docs/research-log.md`
- an EH577 driver patch or patch-like diff that builds in `refs/libfprint/`
- a recorded local validation result showing one of:
  - the patched stack now recognizes EH577, or
  - a narrower, precisely documented failure point remains

## Todo

### Phase 0 — baseline and plan hygiene

- [x] Create an EH577-focused agent workspace under `.agents/`
- [x] Record current repo state and reverse-engineering status in `AGENTS.md`
- [x] Create this detailed bringup plan under `.agents/plans/`
- [x] Keep this plan updated as the live todo list for EH577 bringup

### Phase 1 — capture and probe baseline

- [x] Prove EH575 bulk-protocol compatibility on EH577
- [x] Replay the full EH575 post-init sequence on EH577
- [x] Save a full post-init dump set under `dumps/`
- [x] Confirm that `64 14 ec` returns `5356` bytes
- [x] Confirm that the current idle `64 14 ec` payload dump is all-zero
- [x] Run and log `eh575-preinit` explicitly on EH577
- [x] Run and log `eh575-repeat` explicitly on EH577
- [x] Capture a side-by-side comparison of `eh575-postinit`, `eh575-preinit`, and `eh575-repeat`

### Phase 2 — state-machine characterization

- [x] Repeat reset + first 2 packets enough times to characterize `aa` vs `ab` and `01` vs `05`
- [ ] Determine whether the earlier `01 01 01` response is reproducible
- [x] Record which commands show stable replies vs stateful replies
- [ ] Decide whether EH577 should follow EH575 post-init directly, pre-init branch logic, or both
- [x] Update `docs/research-log.md` with a dedicated state-variation table

### Phase 3 — finger interaction and interrupt behavior

- [x] Poll interrupts while no finger is present
- [ ] Poll interrupts while placing a finger on the sensor
- [ ] Poll interrupts while holding a finger steady on the sensor
- [ ] Poll interrupts while removing a finger
- [ ] Record whether `0x83`, `0x84`, both, or neither emit data during finger interaction
- [ ] If interrupts remain silent, record that clearly and continue using bulk-only testing as the primary path

### Phase 4 — payload analysis

- [ ] Capture `64 14 ec` payloads during idle, touch, hold, and remove scenarios
- [ ] Compare payload nonzero counts and entropy across those scenarios
- [ ] Check whether any payload resembles image-strip structure rather than pure zeros/noise
- [ ] If needed, add a small analysis helper under `tools/` to summarize payload bytes or render them for inspection
- [ ] Record payload conclusions in a new log or doc entry

### Phase 5 — external research leads

- [ ] Search for OEM Windows driver packages or archives containing `EgisTouchFP0577s.inf`
- [ ] If a package is found, extract binary names, INF metadata, and any useful strings into `docs/` or `logs/`
- [ ] Search for any public reports/issues mentioning EH577, `1c7a:0577`, or related Egis swipe/image devices
- [ ] Cross-check any web findings against local captures before changing driver logic

### Phase 6 — driver patch bringup

- [x] Create `wip-libfprint/drivers/egis0577.c`
- [x] Create `wip-libfprint/drivers/egis0577.h`
- [x] Set the EH577 USB ID in the WIP driver skeleton
- [ ] Review the EH577 WIP driver for any leftover EH575 assumptions that conflict with observed EH577 behavior
- [ ] Relax reply parsing wherever EH577 state bytes are known to vary
- [ ] Decide whether interrupt integration belongs in the first patch or a later phase
- [ ] Turn the WIP files into an applyable patch against `refs/libfprint/`
- [ ] Document patch assumptions directly in code comments where behavior is not yet fully proven

### Phase 7 — build and integration validation

- [ ] Set up a local `refs/libfprint/build` directory with Meson
- [ ] Build the modified libfprint tree successfully
- [ ] Record any dependency/package issues encountered during build
- [ ] Test whether the built stack enumerates EH577 as a supported device
- [ ] If enumeration works, test probe/open/activate behavior
- [ ] If image or capture code fails, record the exact failure stage rather than generalizing

### Phase 8 — finish criteria for the first bringup milestone

- [ ] Have a documented answer for whether EH577 is operationally “EH575-compatible enough” for a first driver patch
- [ ] Have at least one buildable EH577 patch artifact
- [ ] Have enough logs/dumps/docs that a future agent can resume without rediscovering the protocol family
- [ ] Update `AGENTS.md`, `docs/research-log.md`, and this plan with the final status of the milestone
