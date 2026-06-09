# TODO

## Short term

- [x] `git init` the repository
- [x] Clone/reference `Animeshz/EgisTec-EH575`
- [x] Inspect upstream `libfprint` Egis drivers (`egis0570`, `egismoc`)
- [x] Determine whether EH577 looks image-based or MOC-based
- [x] Search for existing USB IDs / protocol notes for `1c7a:0577`
- [x] Check for existing local Linux OEM blobs / userspace components
- [x] Prove or disprove EH575 bulk-protocol compatibility on live EH577 hardware
- [ ] Check for OEM Windows driver packages that mention EH577
- [x] Replay the full EH575 post-init sequence on EH577
- [x] Determine whether `64 14 ec` returns a `5356`-byte payload on EH577
- [x] Poll interrupt endpoints `0x83` / `0x84` during idle and init
- [x] Build an initial EH577 libfprint driver skeleton by adapting `egis0575`
- [x] Characterize EH577 state variation on `EGIS 60 00/01 fc` (`aa` vs `ab`, `01` vs `05`)
- [x] Save and analyze the raw `5356`-byte payload from `64 14 ec`
- [x] Poll interrupts while physically touching/removing a finger
- [x] Reproduce the earlier `01 01 01` response and reconcile it with the EH575 pre-init branch logic
- [x] Repeat the successful non-zero post-init finger-hold capture to confirm reproducibility (runs 5-8 all produced 1500+ nonzero bytes after pre-init)
- [x] Compare multiple non-zero payload frames for image stability (4 good frames, 245 unique values in best)
- [x] **Fix the egis0577 libfprint driver to run pre-init sequence once at device open before the post-init loop** — SM_INIT now starts with EGIS0577_PRE_INIT_PACKETS; both wip-libfprint and refs/libfprint updated and rebuilt
- [x] Test the rebuilt libfprint driver end-to-end: confirmed 8 nonzero frames captured, 8 strips assembled into 136×178 fingerprint image, saved to finger.pgm
- [ ] Build libfprint with pixman support to enable the resize step (currently skipped with a CRITICAL warning)
- [x] Test enrollment and verification flow with the patched driver — see [`docs/plan-enroll-verify.md`](plan-enroll-verify.md)
- [ ] Consider upstreaming the driver as a patch to libfprint

## Likely deeper reverse-engineering work

- [ ] Capture USB traffic for probe/init/idling
- [ ] Capture USB traffic for finger-on-sensor events
- [ ] Capture USB traffic for enroll/verify flows from a working proprietary stack
- [ ] Identify framing, checksums, sequence fields, and interrupt semantics
- [ ] Turn the current EH577 skeleton in `wip-libfprint/` into an applyable/buildable libfprint patch
- [ ] Get probe/open working against real hardware
- [ ] Implement activation/deactivation and event handling
- [ ] Implement first image / payload acquisition path
- [ ] Decide whether the first EH577 driver patch should stay bulk-only and omit interrupt handling
