# EH577 USB Open Crash Research

## Goal

Determine the most plausible root cause of the intermittent EH577 USB-level crash where `img-capture` or other userspace access attempts are followed by `libusb couldn't open USB device ... errno=5`, a kernel-side `usb 3-3: USB disconnect`, and repeated `device descriptor read/64, error -110` re-enumeration failures.

The concrete outcome is:

- the likely crash class is narrowed using source-backed evidence,
- the local symptom timeline is matched against known libusb/xHCI/USB failure patterns,
- the top candidate mitigations are prioritized,
- and the results are written into a local log before further trial-and-error changes.

## Context

- Relevant local files:
  - `AGENTS.md`
  - `docs/research-log.md`
  - `logs/2026-06-04-img-capture-guided-touch-01-failure-summary.txt`
  - `logs/2026-06-04-img-capture-guided-touch-03-failure-summary.txt`
  - `logs/2026-06-04-libfprint-stack-boundary-summary.txt`
  - `logs/2026-06-04-img-capture-preinit-misbranch-summary.txt`
  - `tools/eh577_guided_img_capture.sh`
  - `build/eh577_libusb_smoketest`
- Already observed locally:
  - EH577 can enumerate cleanly after a cold reboot.
  - Plain libusb, usbfs, and GUsb async packet sends can succeed on a healthy device.
  - Some guided or repeated runtime attempts lead to:
    - `libusb: error [get_usbfs_fd] ... errno=5`
    - `Failed to open device ... Input/Output Error [-1]`
    - `usb 3-3: USB disconnect`
    - repeated `device descriptor read/64, error -110`
  - After this failure, the device often disappears until a cold reboot.
- Web-research leads already worth checking:
  - `errno=5` in libusb open paths often maps to disconnect/race behavior rather than driver-logic failure.
  - `error -110` commonly indicates USB timeout at enumeration or controller/device signal failure.
  - autosuspend, controller state, or another USB device/hub reset can sometimes trigger similar disconnect cascades.
- Constraints:
  - The goal is research and diagnosis first, not random kernel tuning.
  - Findings should be treated as hypotheses until checked against local logs.
  - Prefer official docs, upstream source, kernel docs, libusb source, bug trackers, and distro bug reports.

## Steps

1. Summarize the exact local crash signature from existing logs into one compact timeline.
2. Research what libusb `get_usbfs_fd errno=5` means in the Linux backend source and common bug reports.
3. Research what kernel `device descriptor read/64, error -110` means in USB and xHCI contexts.
4. Research whether internal fingerprint readers on shared hubs/controllers can be dropped by hub resets, autosuspend, resume, or competing services.
5. Compare the external evidence against this machine’s symptom pattern and classify the top hypotheses.
6. Convert the research into a short ranked list of next local experiments, from least invasive to most invasive.

## Validation

This plan is successful when all of the following are true:

- a dedicated local log summarizes the USB-open crash pattern with source-backed explanations,
- at least three plausible hypotheses are ranked by fit to the local evidence,
- at least one hypothesis is explicitly ruled weaker than the others,
- and the next local mitigation experiments are listed in priority order.

## Todo

### Phase 0 — lock the local evidence set

- [ ] Re-read the existing guided-touch failure summaries and extract the exact crash signature
- [ ] Re-read the strongest successful healthy-open logs so the contrast is explicit
- [ ] Save a compact symptom timeline under `logs/`

### Phase 1 — research libusb meaning of `errno=5`

- [ ] Read upstream libusb Linux backend references for `get_usbfs_fd`
- [ ] Find at least two issue reports where `errno=5` happened around disconnect/open races
- [ ] Note what those reports say about permissions vs disconnect vs stale-node causes

### Phase 2 — research kernel meaning of `error -110`

- [ ] Read kernel USB error-code documentation for `-110`
- [ ] Find at least two xHCI or USB-enumeration reports matching `device descriptor read/64, error -110`
- [ ] Separate physical-signal explanations from host-controller/power-management explanations

### Phase 3 — investigate fingerprint-reader-specific patterns

- [ ] Search for libfprint/fprintd/fingerprint-reader reports involving shared hub resets or internal USB disconnects
- [ ] Search for autosuspend or runtime-power-management interactions affecting fingerprint readers
- [ ] Search for reports where another internal USB device caused a hub/controller reset that dropped the fingerprint reader

### Phase 4 — synthesize and rank hypotheses

- [ ] Rank the top candidate causes for this machine
- [ ] Identify which causes fit the cold-reboot-only recovery behavior best
- [ ] Identify which causes fit the selective failure during some guided runs best
- [ ] Write a dedicated synthesis log under `logs/`

### Phase 5 — turn research into local experiments

- [ ] Propose a minimal local experiment for each top hypothesis
- [ ] Prioritize experiments that do not require system-wide changes first
- [ ] Link the chosen experiments back into the active runtime/guided-touch plan
