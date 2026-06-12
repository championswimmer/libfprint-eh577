# 2026-06-03 — first successful upstream libfprint integration build

After confirming the latest upstream `libfprint` clone was current, the local EH577 driver draft was wired into the real source tree under `refs/libfprint/`.

### Code integration changes

Added / updated:

- `refs/libfprint/libfprint/drivers/egis0577.c`
- `refs/libfprint/libfprint/drivers/egis0577.h`
- `refs/libfprint/libfprint/meson.build`
- `refs/libfprint/meson.build`

Important EH577-specific logic changes made before build validation:

1. idle all-zero `5356`-byte frames are no longer treated as fatal errors
2. the PRE_INIT branch is only taken on the exact `SIGE 01 01 01` pattern
3. comments were updated to document real EH577 state variation and the observed post-init-led capture behavior

### Build environment issues encountered

The machine initially lacked several build dependencies/tools:

- `meson`
- `ninja`
- `gusb` development files
- `gudev` / udev-related development support for full default builds

For EH577 bringup, a reduced local build configuration was used to avoid unrelated integration requirements:

```bash
meson setup refs/libfprint/build refs/libfprint \
  -Ddrivers=egis0577,virtual_image \
  -Dudev_rules=disabled \
  -Dudev_hwdb=disabled \
  -Dintrospection=false \
  -Ddoc=false \
  -Dinstalled-tests=false
```

### Metadata mismatch fixed during integration

The first post-integration test run exposed a consistency issue:

- `1c7a:0577` was still present in the autosuspend-only allowlist/hwdb generator input
- once `egis0577` became a real driver, this caused the `udev-hwdb` helper to warn that `0577` was now implemented by a driver

Fix applied:

- removed `1c7a:0577` from:
  - `refs/libfprint/libfprint/fprint-list-udev-hwdb.c`
  - `refs/libfprint/data/autosuspend.hwdb`

### Build/test validation result

Artifact created:

- `logs/2026-06-03-libfprint-build-validation.txt`

Results:

- `meson setup` succeeded
- `meson compile -C refs/libfprint/build` succeeded
- `meson test -C refs/libfprint/build --print-errorlogs` succeeded in the reduced configured environment

Observed test summary:

- `Ok: 6`
- `Fail: 0`
- `Skipped: 28`

The skipped tests are expected in this reduced driver configuration.

### Important build-process note

A false failure was seen when `compile` and `test` were accidentally started in parallel while the shared library was still being relinked.

That produced misleading linker errors like:

- `file not recognized: file format not recognized`

This was a tooling race, not an EH577 code problem.

Future runs should keep `meson compile` and `meson test` strictly sequential.

### Interpretation

This is the first successful upstream-integration milestone for EH577.

At this point we now know:

1. the EH577 driver skeleton can be integrated into the latest upstream tree,
2. the current EH577 draft is buildable in a real `libfprint` environment,
3. the first patch can likely stay bulk-first and defer interrupt integration,
4. the next stage should move toward runtime enumeration and behavior testing of the built stack.

### Practical next actions after this batch

1. Test whether the built stack enumerates EH577 as a supported device.
2. If enumeration works, test probe/open/activate behavior against the real device.
3. Investigate whether the smaller state-byte shifts can be used as finger/capture readiness indicators.
4. Decide explicitly whether the first libfprint patch should stay bulk-only and ignore interrupts.
5. Capture `usbmon` traces for a successful non-zero post-init finger-hold run if runtime integration still leaves behavior gaps.
