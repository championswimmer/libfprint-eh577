# 2026-06-12 — enroll wedge root cause: ~8 large reads per USB claim, REPEAT flush is self-defeating

Analysed `logs/identify-session-20260612-013023.txt`. Enroll stage 1 passes (3 strips
assembled), then the device **stops ACKing** at the start of stage 2: post-init[0]
(`60 00 fc`, a 7-byte read) — its OUT even took 272 ms vs ~0 ms normally — and the
following bulk IN times out after the full 10 s `EGIS0577_TIMEOUT`, aborting the whole
enroll with "transfer timed out" → "Enrollment incomplete (1/10)".

### Confirmed root cause — it is a *count* problem, not a timing problem

Counting the 5356-byte (`64 14 ec`) reads before the wedge: **exactly 8** —
alternating `post-init[17]` (real strip) and `repeat[8]` (flush):

    post-init[17], repeat[8], post-init[17], repeat[8],
    post-init[17], repeat[8], post-init[17], repeat[8]  → next command wedges

This matches the ceiling noted in commit `592d455` ("after ~8 consecutive real captures
the device DMA/FIFO saturates"). Two consequences:

1. **The REPEAT flush is self-defeating.** `repeat[8]` is itself a 5356-byte `64 14 ec`
   read, so every flush spends one unit of the very budget it was added to protect. Net
   real strips before wedge ≈ 4. With 3 strips/stage + a flush per strip and per stage,
   the budget is exhausted early in stage 2.
2. **10 enroll stages is structurally unreachable** under the current design — not a
   value you can dial in with more delay. Every recent commit (`8becfde`, `592d455`,
   `90386e9`) attacked this as a timing/pacing problem (more delay, more flush); the
   device still wedged because spacing was never the issue.

### Why the standalone probe did 8 full cycles fine but the driver wedges after ~4

`tools/eh577_preinit_then_capture.sh` runs each of its 8 cycles as a **separate probe
process**: `open(dev)` → `USBDEVFS_CLAIMINTERFACE` → preinit+postinit → `RELEASEINTERFACE`
→ `close(fd)`. Every cycle is a **fresh interface claim**. The driver claims interface 0
once in `dev_init` and holds it for the entire enroll. Strong hypothesis: **the ~8-large-read
DMA/FIFO budget is per-claim (per-open) session state, and releasing+re-claiming the
interface — or `USBDEVFS_RESET` — clears it.** This also explains why running PRE_INIT
again *inside the same claim* never un-wedged the device (commits `90386e9`: stalls after
2 SM_INIT cycles; `80974a9`: SM_INIT after a capture stalls at packet ~10). PRE_INIT
resends config registers but does not reset the claim/DMA session; a fresh claim does.
The probe also already exposes `USBDEVFS_RESET` (`reset` mode) and a claim→reset→sequence
path (`eh575-postinit-reset`).

### Implications / next steps (not yet validated on hardware)

- Stop treating this as timing. Do **not** add more delay or another flush variant.
- Most promising fix: between captures/stages, **release + re-claim interface 0**
  (libfprint `g_usb_device_release_interface` + `g_usb_device_claim_interface`) or issue
  `g_usb_device_reset()` to clear the per-claim DMA budget. Needs a hardware run to confirm
  a fresh claim actually un-wedges mid-session.
- Drop the REPEAT-as-flush (it consumes the budget). Test whether removing it ~doubles
  real strips before the ceiling.
- Make the restart-path timeout **recoverable** (reset + retry a bounded number of times)
  instead of `fpi_ssm_mark_failed` aborting the whole enroll on the first timeout.
- Product call: fewer enroll stages and **require a finger-off→finger-on transition
  between stages** (bounds reads/stage, adds placement diversity, also helps the
  open false-match blocker).
