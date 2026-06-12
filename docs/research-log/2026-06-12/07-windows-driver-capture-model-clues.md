# Windows driver capture-model clues: press-like flow, no obvious swipe/stitch evidence

Date: 2026-06-12

## Question

Can the Windows EH577 reference driver tell us whether the OEM stack treats the sensor as a **press** reader or a **swipe** reader, and whether it appears to **stitch** multiple strips/images together?

## Method

Examined the local Windows package under [`../../../windows-driver/`](../../../windows-driver/):

- [`../../../windows-driver/EgisTouchFP0577.dll`](../../../windows-driver/EgisTouchFP0577.dll)
- [`../../../windows-driver/EgisTouchFPEngine0577.dll`](../../../windows-driver/EgisTouchFPEngine0577.dll)
- [`../../../windows-driver/EgisTouchFPSensor0577.dll`](../../../windows-driver/EgisTouchFPSensor0577.dll)
- [`../../../windows-driver/EgisTouchFP0577.inf`](../../../windows-driver/EgisTouchFP0577.inf)

using string extraction, PE section scanning, export inspection, and constant searches via [`../../../tools/windows_pe_scan.py`](../../../tools/windows_pe_scan.py).

## Findings

### 1. The Windows stack looks more like a **finger-down -> get one image/sample** flow than a swipe-strip flow

The main capture DLL contains strings consistent with an interrupt-driven touch event followed by image capture:

- `get_image send EGIS_WAIT_INTERRUPT`
- `get_image receive EGIS_WAIT_INTERRUPT`
- `get_image receive EGIS_TZ_STATE_NOTIFY_FINGER_DOWN`
- `get_image finger touch`
- `confirm_finger_stable`
- `get_image finger_quality = %d`
- `check_finger_remove`
- `finger removed`

These strongly suggest:

1. enter a detect mode,
2. wait for a finger-down notification,
3. confirm the finger is stable,
4. capture/evaluate an image.

That is much more **press/touch-like** than a continuous swipe-strip acquisition model.

### 2. I found **no positive string evidence** for swipe/strip/stitch assembly

Across all three DLLs, searches for terms such as:

- `swipe`
- `strip`
- `stitch`
- `assemble`
- `mosaic`
- `merge`
- `concat`
- `rolling`

returned **no hits**.

Negative evidence is not proof, but if the public strings were exposing a swipe/strip pipeline, I would have expected at least some terminology around strips, rows, reconstruction, or assembly. None showed up.

### 3. The engine side looks like it accepts a **single 2-D sample** with width/height metadata

The engine DLL exposes:

- `CTouchSensor::AcceptSampleData`
- `HorizontalLineLength = %d, VerticalLineLength = %d`
- `EnrollmentCount = [%d], FSize = [%d]`
- `Verify_Orininal`
- `Verify_Skeleton`
- `Enroll_Orininal_%02d`
- `Enroll_Skeleton_%02d`

This looks like the matcher is handed one image/sample at a time with explicit 2-D geometry, then extracts skeleton/features from that sample. That again looks more like **snapshot processing** than host-side strip stitching.

### 4. The package still suggests **multiple frames** may be used internally around one touch

There are strings such as:

- `calculate_statistic_by_frames`
- `get_image qty=%d, cover_count = %d, try_cnt = %d`
- `Getting Zone1 Image, height width=(%d,%d)`
- `Getting Zone2 Image, height width=(%d,%d)`
- `Image`
- `Image_not_bkg`

Best interpretation:

- the OEM stack likely does more than one raw read around a touch,
- may compute statistics / background-subtracted variants,
- and may evaluate subregions (`Zone1` / `Zone2`).

But this is still **not the same thing as swipe-strip stitching**. It looks more like **multi-frame quality/detect processing around a press image**.

### 5. The Windows engine also knows the same raw geometry clue we already use on Linux

A direct constant scan found a contiguous `52, 103` `u32` pair inside [`../../../windows-driver/EgisTouchFPEngine0577.dll`](../../../windows-driver/EgisTouchFPEngine0577.dll).

That matches the current Linux raw-frame interpretation:

- `103 x 52 = 5356`

which is the exact bulk payload size already observed for `64 14 ec`.

This does **not** prove the entire Windows pipeline is one-frame-only, but it is good corroboration that the OEM stack also knows about the same small rectangular raw image geometry rather than some obviously taller swipe canvas.

## Conclusion

Best current read from the Windows package:

- **More evidence for PRESS/touch than swipe.**
- **No direct evidence of host-side strip stitching/assembly.**
- **Some evidence of multiple frames / statistics / zone handling around capture**, likely for detect, stability, quality, or background handling.

So the Windows reference stack does **not** give us a reason to reintroduce EH575-style swipe assembly. If anything, it reinforces the current direction:

- detect mode
- wait for finger-down / stable touch
- capture a small rectangular image sample
- do quality / feature extraction on that sample

## Practical implication for EH577 bringup

The most useful Windows-derived ideas are probably:

1. better **detect-mode / finger-stable** handling,
2. possible **interrupt-assisted finger-down** behavior,
3. possible **multi-frame selection/statistics/background handling** within the **press** model.

The least supported idea is reintroducing Linux-side **swipe-strip assembly**.
