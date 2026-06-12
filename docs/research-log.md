# EH577 Research Log Index

This repository contains the detailed chronological logs of investigations, experiments, and findings for the EgisTec EH577 fingerprint sensor. To keep logs manageable, the individual details are split into daily folders.

## [2026-06-12](research-log/2026-06-12)
* [01-enroll-wedge-root-cause.md](research-log/2026-06-12/01-enroll-wedge-root-cause.md): Traced the enroll wedge to an 8-large-read DMA ceiling per claim, identifying interface reset as the solution.
* [02-windows-inf-and-dll-guard-clues.md](research-log/2026-06-12/02-windows-inf-and-dll-guard-clues.md): Extracted registry INF keys and DLL function/variable strings pointing to detect/sensor splits.
* [03-sensor-size-research.md](research-log/2026-06-12/03-sensor-size-research.md): Compared EH577 raw/frame dimensions with other touch readers and concluded that `103x52` is likely the real raw frame while the current `208x104` snapshot output is not yet proven to be the best final matching geometry.
* [04-capture-noise-timing-and-denoise-findings.md](research-log/2026-06-12/04-capture-noise-timing-and-denoise-findings.md): Found same-finger score-0 is caused by fixed-pattern saturated hot-pixel noise (present in the no-finger baseline, rising with finger pressure); 3x3 median denoising removes it but erodes ridges, so the direction is fresh baseline subtraction + moderate-contact frame timing instead.
* [05-press-sensor-timing-strategies.md](research-log/2026-06-12/05-press-sensor-timing-strategies.md): Analyzed other libfprint PRESS type drivers to understand how they handle capture timing and finger settlement delays.
* [06-probe-vs-driver-init-diff.md](research-log/2026-06-12/06-probe-vs-driver-init-diff.md): Diffed the standalone probe's init against the driver's and found the PRE_INIT (29) and POST_INIT (18) register sequences are byte-for-byte identical — so saturation is not a config diff; the real difference is read cadence (probe reads one frame and exits; driver loops hundreds of reads, runs pre-init first, never USB-resets). Decisive next test is an empirical same-finger probe-vs-driver frame comparison.

## [2026-06-10](research-log/2026-06-10)
* [01-libfprint-driver-produces-assembled-fingerprint-image-end-to-end.md](research-log/2026-06-10/01-libfprint-driver-produces-assembled-fingerprint-image-end-to-end.md): Successfully produced a fully assembled PGM fingerprint image from the libfprint driver after adding a pre-init open hook.

## [2026-06-09](research-log/2026-06-09)
* [01-pre-init-required-to-arm-sensor.md](research-log/2026-06-09/01-pre-init-required-to-arm-sensor.md): Discovered that running the pre-init register-write sequence is strictly required to arm the sensor hardware.

## [2026-06-04](research-log/2026-06-04)
* [01-libfprint-guided-touch-05-runtime-dump-comparison.md](research-log/2026-06-04/01-libfprint-guided-touch-05-runtime-dump-comparison.md): Confirmed the libfprint runtime is receiving identical byte-for-byte idle frames as the standalone probe.
* [02-libfprint-guided-touch-06-conservative-pacing-result.md](research-log/2026-06-04/02-libfprint-guided-touch-06-conservative-pacing-result.md): Tested mild pacing delays (20ms/10ms) but found no change in all-zero frame generation.
* [03-libfprint-guided-touch-07-strong-pacing-result.md](research-log/2026-06-04/03-libfprint-guided-touch-07-strong-pacing-result.md): Tried stronger pacing (100ms/50ms), confirming timing alone does not resolve the unarmed sensor state.
* [04-usbmon-capture-prep-for-runtime-vs-usbfs-comparison.md](research-log/2026-06-04/04-usbmon-capture-prep-for-runtime-vs-usbfs-comparison.md): Created scripts and planning logs to perform usbmon comparisons between runtime and standalone paths.

## [2026-06-03](research-log/2026-06-03)
* [01-initial-local-triage.md](research-log/2026-06-03/01-initial-local-triage.md): Established host environment information, identified device endpoints, and analyzed archived EH575 reference code.
* [02-deeper-protocol-shape-comparison.md](research-log/2026-06-03/02-deeper-protocol-shape-comparison.md): Compared command wrappers, endpoint layout, and image size constraints between `egis0570`, `egismoc`, and EH575.
* [03-first-live-protocol-probe-on-eh577.md](research-log/2026-06-03/03-first-live-protocol-probe-on-eh577.md): Demonstrated that EH577 accepts EH575 initialization packets and returns compatible `SIGE` bulk responses.
* [04-full-eh575-post-init-replay-on-real-eh577.md](research-log/2026-06-03/04-full-eh575-post-init-replay-on-real-eh577.md): Replayed the full 18 post-init packets and observed state-dependent variations in early status registers.
* [05-explicit-eh575-pre-init-and-repeat-path-replays.md](research-log/2026-06-03/05-explicit-eh575-pre-init-and-repeat-path-replays.md): Successfully tested pre-init and repeat bulk paths using the standalone usbfs probe tool.
* [06-repeated-early-state-sampling.md](research-log/2026-06-03/06-repeated-early-state-sampling.md): Sampled early response sequences to map stateful register variances across reset events.
* [07-guided-finger-interaction-captures.md](research-log/2026-06-03/07-guided-finger-interaction-captures.md): Captured the first non-zero frame during a timed finger-present hold on the sensor.
* [08-post-init-finger-hold-reproducibility-confirmed.md](research-log/2026-06-03/08-post-init-finger-hold-reproducibility-confirmed.md): Verified the capture mechanism with a second independent non-zero frame showing distinct ridges.
* [09-first-successful-upstream-libfprint-integration-build.md](research-log/2026-06-03/09-first-successful-upstream-libfprint-integration-build.md): Integrated the first draft driver into libfprint and successfully built and tested it via meson.
