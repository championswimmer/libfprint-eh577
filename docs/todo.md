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
- [ ] Replay the full EH575 post-init sequence on EH577
- [ ] Determine whether `64 14 ec` returns a `5356`-byte payload on EH577
- [ ] Poll interrupt endpoints `0x83` / `0x84` during idle and init
- [x] Build an initial EH577 libfprint driver skeleton by adapting `egis0575`
- [ ] Reconcile the `01 01 01` response on `EGIS 60 01 fc` with the EH575 pre-init branch logic

## Likely deeper reverse-engineering work

- [ ] Capture USB traffic for probe/init/idling
- [ ] Capture USB traffic for finger-on-sensor events
- [ ] Capture USB traffic for enroll/verify flows from a working proprietary stack
- [ ] Identify framing, checksums, sequence fields, and interrupt semantics
- [ ] Turn the current EH577 skeleton in `wip-libfprint/` into an applyable/buildable libfprint patch
- [ ] Get probe/open working against real hardware
- [ ] Implement activation/deactivation and event handling
- [ ] Implement first image / payload acquisition path
