# EH577 Usbmon Runtime Comparison

## Goal

Capture a side-by-side USB bus trace of the EH577 under:

1. the patched `libfprint` runtime path (`img-capture`), and
2. the known-good standalone usbfs probe path (`eh575-postinit 18` with guided finger hold),

so the first meaningful divergence can be identified from actual USB traffic instead of inferred from runtime logs alone.

The concrete outcome is:

- a clean `usbmon` trace exists for the patched runtime path,
- a comparable `usbmon` trace exists for the standalone successful or near-successful post-init finger-hold path,
- both runs have matching human-readable logs and raw frame dumps,
- and the next session can compare the two traces to locate the earliest sequence/timing/state difference before `64 14 ec` stays stuck on the idle zero payload.

## Context

- Relevant files:
  - `AGENTS.md`
  - `docs/research-log.md`
  - `.agents/plans/03-eh577-runtime-capture-timeout.md`
  - `.agents/plans/05-eh577-guided-touch-continuation.md`
  - `tools/eh577_guided_capture.sh`
  - `tools/eh577_guided_img_capture.sh`
  - `tools/eh577_usbfs_probe.c`
  - `build/eh577_usbfs_probe`
  - `build/eh577_libusb_smoketest`
  - `refs/libfprint/build/examples/img-capture`
- Current validated facts:
  - EH577 is on USB bus `3` on this machine when healthy.
  - Device identity is `1c7a:0577` at `/sys/bus/usb/devices/3-3`.
  - `tcpdump` is available locally.
  - `tshark` / `wireshark` / `dumpcap` were not confirmed installed, so this plan uses `tcpdump` directly.
  - Guided touch 05, 06, and 07 proved that patched `libfprint` runtime frames are byte-identical to the standalone idle zero frame.
  - Conservative and strong pacing did not move the runtime off that idle-identical zero-frame path.
- Working hypothesis now:
  - the remaining blocker is not finger heuristic tuning and not small loop pacing alone;
  - the most valuable next artifact is a direct USB trace comparison between patched runtime and known-good standalone behavior.
- Constraints:
  - EH577 can wedge and disappear until recovery or reboot.
  - `sudo -n` can expire; refresh sudo before every capture batch.
  - Use fresh filenames for every run.
  - If EH577 is unhealthy, do not waste a `usbmon` capture on a bad run.

## Steps

1. Refresh sudo and verify EH577 health before any capture.
2. Enable `usbmon` and verify that `tcpdump` can capture from `usbmon3`.
3. Capture one patched-runtime guided `img-capture` session with:
   - `usbmon` trace
   - driver log
   - raw runtime frame dumps
4. Capture one standalone guided `eh575-postinit 18` session with:
   - `usbmon` trace
   - probe stdout/stderr log
   - raw probe dump directory via `EH577_DUMP_DIR`
5. If the standalone capture misses the meaningful non-zero frame, retry that standalone run before changing code.
6. After both traces exist, record exact commands, filenames, and whether each run was healthy, zero-only, or meaningful.
7. Leave the next session enough artifacts to compare the first divergence point.

## Validation

This plan is successful when all of the following exist:

- at least one runtime `usbmon` trace file under `logs/`
- at least one standalone probe `usbmon` trace file under `logs/`
- a matching runtime text log and runtime frame-dump directory
- a matching standalone text log and standalone probe dump directory
- a short summary log stating whether each capture stayed zero-only or produced meaningful data
- enough metadata to compare the two traces in the next session without rerunning captures unnecessarily

## Todo

### Phase 0 — preflight and capture environment

- [ ] Run `sudo -v` before starting any capture batch from an interactive terminal
- [x] Verify EH577 is present with `lsusb | rg '1c7a:0577|EgisTec EH577'`
- [x] Verify live libusb health with `sudo -n ./build/eh577_libusb_smoketest`
- [x] Load usbmon with `sudo modprobe usbmon`
- [x] Verify `tcpdump` sees the monitor interface with `sudo tcpdump -D | rg 'usbmon3|usbmon0'`
- [x] Record harness blocker: generic `sudo tcpdump` may still require interactive auth even when targeted device probes succeed non-interactively
- [ ] If EH577 is not healthy, recover first or cold reboot before continuing

### Phase 1 — prepare artifact paths

Use fresh date-prefixed names. Example names below assume the next session continues this same stream.

- [x] Create runtime capture directories:
  - `mkdir -p logs`
  - `mkdir -p dumps/2026-06-04-libfprint-usbmon-runtime-01`
- [x] Create standalone capture directories:
  - `mkdir -p dumps/2026-06-04-usbfs-usbmon-postinit-01`
- [x] Decide final filenames before starting:
  - runtime trace: `logs/2026-06-04-usbmon-libfprint-runtime-01.pcap`
  - runtime tcpdump stderr: `logs/2026-06-04-usbmon-libfprint-runtime-01-tcpdump.txt`
  - runtime app log: `logs/2026-06-04-img-capture-usbmon-runtime-01.txt`
  - standalone trace: `logs/2026-06-04-usbmon-usbfs-postinit-01.pcap`
  - standalone tcpdump stderr: `logs/2026-06-04-usbmon-usbfs-postinit-01-tcpdump.txt`
  - standalone probe log: `logs/2026-06-04-usbfs-postinit-usbmon-01.txt`
- [x] Add `tools/eh577_usbmon_capture.sh` to start/stop `tcpdump` cleanly around one capture command
- [x] Make `tools/eh577_guided_capture.sh` tolerate non-interactive harness runs when upfront sudo validation is unavailable

### Phase 2 — capture patched libfprint runtime usbmon trace

Recommended runtime capture command sequence:

1. Start the runtime run through the usbmon wrapper from an interactive terminal:

```bash
./tools/eh577_usbmon_capture.sh \
  --iface usbmon3 \
  --pcap "$PWD/logs/2026-06-04-usbmon-libfprint-runtime-01.pcap" \
  --tcpdump-log "$PWD/logs/2026-06-04-usbmon-libfprint-runtime-01-tcpdump.txt" \
  -- ./tools/eh577_guided_img_capture.sh \
       --log "$PWD/logs/2026-06-04-img-capture-usbmon-runtime-01.txt" \
       --label "EH577 usbmon runtime 01" \
       --hold 14 \
       --timeout 30 \
       --frame-dump-dir "$PWD/dumps/2026-06-04-libfprint-usbmon-runtime-01"
```

If `usbmon3` is unavailable but `usbmon0` exists, use `usbmon0` as a fallback.

Notes:

- Do **not** add pacing env vars for the first usbmon runtime trace unless there is a specific reason.
- Finger off until `TOUCH`, then hold firmly until `REMOVE`.
- Let the wrapper do the live preflight.

2. The wrapper should stop `tcpdump` cleanly after the guided run finishes.

3. Immediately record whether the runtime run was:

- preflight failure
- USB/open crash
- clean zero-only runtime
- or unexpected non-zero runtime

### Phase 3 — capture standalone usbfs probe usbmon trace

This capture is meant to represent the EH575-compatible post-init path outside libfprint.

1. Start the standalone run through the usbmon wrapper from an interactive terminal:

```bash
./tools/eh577_usbmon_capture.sh \
  --iface usbmon3 \
  --pcap "$PWD/logs/2026-06-04-usbmon-usbfs-postinit-01.pcap" \
  --tcpdump-log "$PWD/logs/2026-06-04-usbmon-usbfs-postinit-01-tcpdump.txt" \
  -- bash -lc './tools/eh577_guided_capture.sh \
       --sudo-upfront \
       --delay-before-start 3 \
       --delay-before-touch 1 \
       --hold 10 \
       --delay-after-remove 2 \
       --cycles 1 \
       --label "EH577 usbfs post-init usbmon 01" \
       -- sudo -n bash -lc '\''export EH577_DUMP_DIR="$PWD/dumps/2026-06-04-usbfs-usbmon-postinit-01"; ./build/eh577_usbfs_probe /dev/bus/usb/003/004 eh575-postinit 18'\'' \
       > "$PWD/logs/2026-06-04-usbfs-postinit-usbmon-01.txt" 2>&1'
```

2. The wrapper should stop `tcpdump` cleanly when the probe run completes.

3. Check whether the standalone dump directory contains a meaningful `eh575-postinit-17-64_14_ec.bin` frame.

If the standalone run stays zero-only, retry the standalone capture with a fresh suffix such as `...-02` before abandoning the comparison.

### Phase 4 — minimal immediate post-capture checks

- [ ] Confirm runtime trace file exists and is non-empty
- [ ] Confirm standalone trace file exists and is non-empty
- [ ] Confirm runtime frame-dump directory contains `.bin` files
- [ ] Confirm standalone dump directory contains `eh575-postinit-17-64_14_ec.bin`
- [ ] Save `sha256sum` and non-zero-byte counts for:
  - runtime first dumped frame
  - standalone packet-17 frame
- [ ] Save a one-paragraph note saying whether the standalone run reproduced a meaningful non-zero frame

Suggested quick checks:

```bash
ls -lh logs/2026-06-04-usbmon-libfprint-runtime-01.pcap \
      logs/2026-06-04-usbmon-usbfs-postinit-01.pcap

find dumps/2026-06-04-libfprint-usbmon-runtime-01 -maxdepth 1 -type f | wc -l
find dumps/2026-06-04-usbfs-usbmon-postinit-01 -maxdepth 1 -type f | sort
```

And for payload comparison:

```bash
python3 - <<'PY'
import hashlib, sys
paths = sys.argv[1:]
for p in paths:
    data = open(p, 'rb').read()
    print(p, len(data), sum(1 for b in data if b), hashlib.sha256(data).hexdigest())
PY \
  dumps/2026-06-04-libfprint-usbmon-runtime-01/0000-post-init-nonzero-0.bin \
  dumps/2026-06-04-usbfs-usbmon-postinit-01/eh575-postinit-17-64_14_ec.bin
```

### Phase 5 — handoff artifacts for the next session

- [ ] Write a short runtime summary log under `logs/`
- [ ] Write a short standalone summary log under `logs/`
- [ ] Record the exact commands used, including whether capture ran on `usbmon3` or `usbmon0`
- [ ] Record whether EH577 needed recovery or reboot before the traces were captured
- [ ] Leave both `.pcap` files, both text logs, and both dump directories untouched for the next session

## Operational notes

### When to abort and retry

Abort and retry the specific run if:

- wrapper preflight fails
- EH577 disappears from `lsusb`
- libusb smoketest fails before starting capture
- the device crashes at open time
- the `tcpdump` trace file is empty or clearly failed to start

Do **not** reuse the same filename after a failed attempt. Increment the suffix (`01` -> `02`).

### If EH577 wedges mid-session

1. stop the current `tcpdump` cleanly
2. save any partial logs
3. record that the run is invalid as a health blocker
4. recover EH577 or cold reboot
5. restart the capture with a fresh suffix

### Reading traces later

Because `tshark` was not confirmed installed, the next session may need to inspect captures with either:

- `tcpdump -nn -r file.pcap`
- Wireshark on another machine
- or newly installed `tshark` if available later

The most important thing now is to **capture clean traces**, not to decode them fully in the same session.
