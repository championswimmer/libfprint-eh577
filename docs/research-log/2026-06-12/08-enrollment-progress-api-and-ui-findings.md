# Enrollment progress in libfprint/fprintd/UI: yes, the APIs expose enough

Date: 2026-06-12

## Question

Can desktop UI clients using `fprint`/`fprintd` know how many enrollment stages are left, and could they show explicit progress such as `11/15` to the user?

## Short answer

**Yes.**

- **Direct libfprint users** can get exact progress directly from the API.
- **fprintd D-Bus clients** can also derive progress, though they must combine a device property with enrollment status signals.
- Therefore, Ubuntu Settings not showing `11/15` is **not** because libfprint/fprintd lack the information. It is mainly a **UI/implementation choice**.

## libfprint API findings

### 1. libfprint explicitly models enrollment as a multi-stage process

The public libfprint API exposes:

- [`fp_device_get_nr_enroll_stages()`](../../../refs/libfprint/libfprint/fp-device.h)
- the `FpEnrollProgress` callback in [`fp-device.h`](../../../refs/libfprint/libfprint/fp-device.h)

`FpEnrollProgress` has this shape:

- `device`
- `completed_stages`
- `print`
- `user_data`
- `error`

So a direct libfprint client gets the **completed stage count** on every progress callback and can read the **total stage count** from the device.

### 2. libfprint internally describes this as UI-relevant progress

The internal helper [`fpi_device_enroll_progress()`](../../../refs/libfprint/libfprint/fpi-device.c) is documented as:

> "Notify about the progress of the enroll operation. This is important for UI interaction."

It logs:

> `Device reported enroll progress, reported %i of %i have been completed`

That is exactly the data needed for a `completed / total` progress display.

### 3. The local libfprint example already prints numeric progress

[`../../../refs/libfprint/examples/enroll.c`](../../../refs/libfprint/examples/enroll.c) does:

- `fp_device_get_nr_enroll_stages (device)` for the total
- prints `Enroll stage %d of %d passed`

The local EH577 helper does the same in [`../../../refs/libfprint/examples/eh577-enroll-helper.c`](../../../refs/libfprint/examples/eh577-enroll-helper.c):

- `EH577_HELPER enroll-stage completed=%d total=%d`

So at the raw libfprint layer, **numeric user-visible progress is already straightforward**.

## fprintd D-Bus API findings

### 4. fprintd exposes the total stage count as a property

The `net.reactivated.Fprint.Device` D-Bus API documents:

- property: `num-enroll-stages`

with the caveat that it is only valid **after the device is claimed**; otherwise it is undefined / `-1`.

This means a UI can ask fprintd:

- how many total stages the device needs

before or during enrollment, as long as it has claimed the device.

### 5. fprintd exposes enrollment progress as status signals, not as an explicit counter

The same D-Bus API exposes the signal:

- `EnrollStatus (result, done)`

Important values include:

- `enroll-stage-passed`
- `enroll-completed`
- retry statuses such as `enroll-retry-scan`, `enroll-finger-not-centered`, `enroll-remove-and-retry`, etc.

So fprintd does **not** directly emit `current_stage=11` in the signal payload.

But it **does** expose enough to derive it:

1. read `num-enroll-stages`
2. start at `0`
3. increment on each `enroll-stage-passed`
4. do **not** increment on retry statuses
5. mark complete on `enroll-completed`

So a D-Bus UI can compute:

- `passed`
- `remaining = total - passed`
- `progress = passed / total`

### 6. This is slightly weaker than direct libfprint, but still sufficient

Difference between the layers:

- **libfprint API:** gives `completed_stages` directly in the callback
- **fprintd D-Bus API:** gives `total` plus status events, so the client must count passed stages itself

That is still sufficient for an end-user progress indicator such as `11/15`.

## GNOME / Ubuntu Settings findings

### 7. GNOME Control Center already tracks enrollment progress from fprintd

Internet/source results for GNOME Control Center's fingerprint dialog show that it keeps local state like:

- `enroll_stages_passed`
- `enroll_progress`

and on `enroll-stage-passed` it does the equivalent of:

1. `enroll_stages = cc_fprintd_device_get_num_enroll_stages (self->device)`
2. `self->enroll_stages_passed++`
3. `self->enroll_progress = self->enroll_stages_passed / (double) enroll_stages`

It also forces progress to `1.0` on final completion if needed.

So GNOME Settings already knows the progress fraction. It is just apparently not choosing to render it as explicit text like `11/15` in the Ubuntu UI you are seeing.

### 8. Historical GNOME work explicitly adapted to devices with more enroll steps

There is also historical GNOME work titled roughly:

- "Support devices with more than 5 enroll steps"

which is more evidence that the desktop UI stack already understands enrollment as a counted multi-stage process.

## Practical conclusion

### What is possible today

**Yes, it is possible right now to show end-user progress via the existing APIs.**

#### If using libfprint directly

Use:

- `fp_device_get_nr_enroll_stages(device)` for the total
- `completed_stages` from `FpEnrollProgress` for the current count

This is the cleanest path.

#### If using fprintd / D-Bus

Use:

- `num-enroll-stages` property after `Claim`
- count `EnrollStatus == "enroll-stage-passed"`
- ignore retry statuses for the counter
- finalize on `enroll-completed`

This is enough for:

- `3/15 complete`
- `12 stages remaining`
- a determinate progress bar

### Why Ubuntu Settings may not show `11/15`

Most likely reasons are UI policy / presentation, not missing backend data:

- GNOME Settings prefers a visual progress flow instead of numeric text
- the dialog may be designed around finger-position artwork + progress fraction
- `fprintd` does not provide an explicit numeric stage counter in the signal, so the UI must maintain the counter itself (which GNOME already appears to do)

## Implication for EH577 work

If we want an EH577-oriented helper or UI to show clearer enrollment feedback, we do **not** need new libfprint driver APIs for basic progress.

The current stack already exposes enough to show:

- total stages
- completed stages
- remaining stages
- retry-without-advance behavior

The only thing missing at the `fprintd` D-Bus layer is a **direct stage number in the signal**, but that is a convenience issue, not a hard blocker.

## Sources

Local repo:

- [`../../../refs/libfprint/libfprint/fp-device.h`](../../../refs/libfprint/libfprint/fp-device.h)
- [`../../../refs/libfprint/libfprint/fp-device.c`](../../../refs/libfprint/libfprint/fp-device.c)
- [`../../../refs/libfprint/libfprint/fpi-device.h`](../../../refs/libfprint/libfprint/fpi-device.h)
- [`../../../refs/libfprint/libfprint/fpi-device.c`](../../../refs/libfprint/libfprint/fpi-device.c)
- [`../../../refs/libfprint/examples/enroll.c`](../../../refs/libfprint/examples/enroll.c)
- [`../../../refs/libfprint/examples/eh577-enroll-helper.c`](../../../refs/libfprint/examples/eh577-enroll-helper.c)

Internet / upstream docs:

- https://fprint.freedesktop.org/libfprint-dev/FpDevice.html
- https://fprint.freedesktop.org/fprintd-dev/Device.html
- https://lists.gnome.org/archives/commits-list/2020-June/msg11360.html
- https://lists.gnome.org/archives/commits-list/2019-July/msg12023.html
