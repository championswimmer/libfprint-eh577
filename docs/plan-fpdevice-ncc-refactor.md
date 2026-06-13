# Plan: Refactor egis0577 from FpImageDevice to FpDevice with embedded NCC matching

## Background

The current driver inherits `FpImageDevice`. That base class handles the enroll/verify/identify
action lifecycle automatically, calling NBIS `mindtct` + `bozorth3` after every
`fpi_image_device_image_captured()` call. The EH577 produces too few minutiae (3–12 per image)
for bozorth3 to be useful; all scores are 0 in practice.

Normalized cross-correlation (NCC) on raw pixel buffers works well on this sensor.
Two-finger testing (folders `20260612-233340` vs `20260612-233841`, 24 images total) shows:
- Genuine pairs: mean NCC ≈ 0.40, peak 0.81
- Impostor pairs: mean NCC ≈ 0.31, max 0.50

A rule of "NCC ≥ 0.50 **AND** ≥ 2 gallery frames match" eliminates the one edge-case impostor
pair from this dataset.

**Important caveat**: this threshold was validated on exactly one impostor finger pair.
Treat it as a provisional starting point, not a proven security threshold.
The ±20 pixel search window used during analysis hit the boundary on several genuine pairs
(e.g. pair 1 vs 3: best offset was at the edge of the window). Widen the search window to ±30
or ±40 before collecting new data, then re-derive the threshold.

The goal of this refactor is to implement all matching logic inside the driver so that
applications call `fp_device_enroll()` / `fp_device_verify()` / `fp_device_identify()`
exactly as with any other libfprint device, with no application-side changes.

---

## Files touched

- `refs/libfprint/libfprint/drivers/egis0577.h` — one-line parent class change
- `refs/libfprint/libfprint/drivers/egis0577.c` — majority of the work

---

## Part 1 — egis0577.h

### 1.1 Change parent class in G_DECLARE_FINAL_TYPE

Line 100 currently:
```c
G_DECLARE_FINAL_TYPE (FpDeviceEgis0577, fpi_device_egis0577, FPI, DEVICE_EGIS0577, FpImageDevice);
```
Change `FpImageDevice` → `FpDevice`:
```c
G_DECLARE_FINAL_TYPE (FpDeviceEgis0577, fpi_device_egis0577, FPI, DEVICE_EGIS0577, FpDevice);
```

No other changes to the header are needed.

---

## Part 2 — egis0577.c

### 2.1 Includes

Remove any include of `fpi-image-device.h` that is pulled in via `drivers_api.h` or directly.
`drivers_api.h` typically covers both `FpImageDevice` and `FpDevice` headers, so no explicit
add should be needed — but verify by checking what `drivers_api.h` includes and add
`#include "fpi-device.h"` explicitly if needed after the refactor causes undefined-type errors.

The existing `#include <nbis.h>` (line 22) is still needed for the stage-2 quality gate
(`stage2_snapshot_quality_ok` uses NBIS minutiae detection). It can stay.

### 2.2 Device struct — `struct _FpDeviceEgis0577`

Lines 41–88. Make two changes:

**a) Parent class field** (line 42):
```c
/* before */
FpImageDevice parent;
/* after */
FpDevice      parent;
```

**b) Add NCC enrollment state fields** after the existing fields, before the closing `}`:
```c
  /* NCC enrollment accumulator */
  GPtrArray    *enroll_gallery;       /* GPtrArray<guint8*> pixel buffers, one per press */
  guint         enroll_stage;         /* how many presses collected so far */
  gsize         enroll_frame_size;    /* w * h bytes, set on first accepted frame */
  guint         enroll_frame_width;
  guint         enroll_frame_height;
```

`enroll_gallery` holds one heap-allocated `guint8*` pixel buffer per successfully enrolled
press. Each buffer is `enroll_frame_size` bytes. The array is cleared and rebuilt at the start
of every enroll action.

### 2.3 G_DEFINE_TYPE

Line 101:
```c
/* before */
G_DEFINE_TYPE (FpDeviceEgis0577, fpi_device_egis0577, FP_TYPE_IMAGE_DEVICE);
/* after */
G_DEFINE_TYPE (FpDeviceEgis0577, fpi_device_egis0577, FP_TYPE_DEVICE);
```

### 2.4 Constants — add near the top of the file with other `#define`s

```c
#define NCC_ENROLL_FRAMES   7      /* distinct presses required for enrollment */
#define NCC_THRESHOLD       0.50f  /* per-frame NCC score to count as a hit    */
#define NCC_MIN_MATCHES     2      /* minimum gallery hits to declare a match   */
#define NCC_SEARCH_WINDOW   30     /* ±N pixel search window; ≥30 to avoid     */
                                   /* peak-at-boundary artifacts seen at ±20   */
```

`NCC_ENROLL_FRAMES` also replaces the hardcoded `8` in `class_init` (see §2.12).

### 2.5 NCC helper functions — add as new static functions

These are a direct port of `tools/eh577_pgm_correlate.c`'s `ncc_at_offset` and `peak_ncc`.
Add them near the top of the file, before any function that calls them.

```c
static double
ncc_at_offset (const guint8 *a, const guint8 *b,
               guint w, guint h,
               double a_mean, double a_std,
               double b_mean, double b_std,
               int dx, int dy)
{
  int x0a = (dx >= 0) ? 0 : -dx;
  int x0b = (dx >= 0) ? dx : 0;
  int y0a = (dy >= 0) ? 0 : -dy;
  int y0b = (dy >= 0) ? dy : 0;
  int xend = (int) w - MAX (x0a, x0b);
  int yend = (int) h - MAX (y0a, y0b);
  if (xend <= 0 || yend <= 0)
    return -1.0;

  double cross = 0.0;
  int    n     = 0;
  for (int y = 0; y < yend; y++)
    {
      const guint8 *rowA = a + (y0a + y) * w + x0a;
      const guint8 *rowB = b + (y0b + y) * w + x0b;
      for (int x = 0; x < xend; x++)
        {
          cross += ((double) rowA[x] - a_mean) * ((double) rowB[x] - b_mean);
          n++;
        }
    }
  if (n == 0)
    return -1.0;
  return cross / ((double) n * a_std * b_std);
}

static double
peak_ncc (const guint8 *a, const guint8 *b,
          guint w, guint h, int search)
{
  gsize   n     = (gsize) w * h;
  double  a_sum = 0, a_sq = 0, b_sum = 0, b_sq = 0;

  for (gsize i = 0; i < n; i++)
    {
      a_sum += a[i];  a_sq += (double) a[i] * a[i];
      b_sum += b[i];  b_sq += (double) b[i] * b[i];
    }
  double a_mean = a_sum / n,  b_mean = b_sum / n;
  double a_var  = a_sq / n - a_mean * a_mean;
  double b_var  = b_sq / n - b_mean * b_mean;
  double a_std  = (a_var > 0.0) ? sqrt (a_var) : 1e-9;
  double b_std  = (b_var > 0.0) ? sqrt (b_var) : 1e-9;

  double best = -2.0;
  for (int dy = -search; dy <= search; dy++)
    for (int dx = -search; dx <= search; dx++)
      {
        double v = ncc_at_offset (a, b, w, h,
                                  a_mean, a_std, b_mean, b_std,
                                  dx, dy);
        if (v > best)
          best = v;
      }
  return best;
}
```

`math.h` must be included for `sqrt`. It is almost certainly already pulled in transitively;
add `#include <math.h>` explicitly if the build fails.

### 2.6 GVariant gallery serialisation helpers

The enrolled gallery is stored in `FpPrint.fpi-data` as GVariant type `(uuaay)`:
- First `u` — frame width
- Second `u` — frame height
- `aay` — array of raw pixel byte arrays, one per enrolled press

Add these two helpers as static functions:

```c
static GVariant *
pack_gallery (GPtrArray *frames,
              guint      frame_width,
              guint      frame_height)
{
  GVariantBuilder builder;
  gsize           frame_size = (gsize) frame_width * frame_height;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));
  for (guint i = 0; i < frames->len; i++)
    g_variant_builder_add_value (
      &builder,
      g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                 g_ptr_array_index (frames, i),
                                 frame_size, sizeof (guint8)));

  return g_variant_new ("(uu@aay)",
                        frame_width, frame_height,
                        g_variant_builder_end (&builder));
}

static gboolean
unpack_gallery (GVariant  *data,
                guint8  ***out_frames,
                guint     *out_count,
                guint     *out_width,
                guint     *out_height)
{
  GVariant *frames_var = NULL;

  if (!g_variant_is_of_type (data, G_VARIANT_TYPE ("(uuaay)")))
    return FALSE;

  g_variant_get (data, "(uu@aay)", out_width, out_height, &frames_var);
  *out_count = (guint) g_variant_n_children (frames_var);

  if (*out_count == 0 || *out_width == 0 || *out_height == 0)
    {
      g_variant_unref (frames_var);
      return FALSE;
    }

  gsize frame_size = (gsize) (*out_width) * (*out_height);
  *out_frames = g_new (guint8 *, *out_count);

  for (guint i = 0; i < *out_count; i++)
    {
      GVariant   *child = g_variant_get_child_value (frames_var, i);
      gsize       len;
      const void *raw   = g_variant_get_fixed_array (child, &len, sizeof (guint8));

      if (len != frame_size)
        {
          /* Size mismatch — corrupt print or resolution changed. */
          for (guint j = 0; j < i; j++)
            g_free ((*out_frames)[j]);
          g_free (*out_frames);
          g_variant_unref (child);
          g_variant_unref (frames_var);
          return FALSE;
        }
      (*out_frames)[i] = g_memdup2 (raw, len);
      g_variant_unref (child);
    }

  g_variant_unref (frames_var);
  return TRUE;
}
```

### 2.7 Replace `report_finger_status` signature

The existing `report_finger_status` (line ~115) takes an `FpImageDevice *img_self` parameter
alongside `FpDeviceEgis0577 *self`, because it calls `fpi_image_device_report_finger_status`.

Replace the signature and body:
```c
/* before */
static void
report_finger_status (FpDeviceEgis0577 *self,
                      FpImageDevice    *img_self,
                      gboolean          present,
                      const char       *reason)
{
  if (self->finger_reported != present)
    {
      self->finger_reported = present;
      fpi_image_device_report_finger_status (img_self, present);
    }
}

/* after */
static void
report_finger_status (FpDeviceEgis0577 *self,
                      gboolean          present,
                      const char       *reason)
{
  if (self->finger_reported != present)
    {
      self->finger_reported = present;
      fp_dbg ("Finger %s (%s)", present ? "present" : "absent", reason);
    }
}
```

All existing call sites pass `img_self` as the second argument. Remove that argument from every
call site. The `img_self` local variable in `ssm_run_state` (see §2.8) will be removed.

### 2.8 `ssm_run_state` — remove FpImageDevice local, update stop path

Line 1427 currently declares:
```c
FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);
```
Remove this line. Replace every use of `img_dev` in `ssm_run_state` with `dev`.

The only remaining `img_dev` use after that is in the `SM_START` stop path (line 1487):
```c
case SM_START:
  if (self->stop)
    {
      fp_dbg ("Stopping, completed capture");
      fpi_ssm_mark_completed (ssm);
      fpi_image_device_deactivate_complete (img_dev, NULL);   /* REMOVE */
    }
```
After the refactor this becomes:
```c
case SM_START:
  if (self->stop)
    {
      fp_dbg ("Stopping, completed capture");
      fpi_ssm_mark_completed (ssm);
    }
```
The action completion call (enroll/verify/identify complete) is issued from
`on_frame_accepted_*` before `self->stop` is set. `loop_complete` only handles the error path.

### 2.9 Replace `fpi_image_device_image_captured` with `on_frame_accepted`

Line 1169:
```c
fpi_image_device_image_captured (img_self, g_steal_pointer (&resized_image));
submitted = TRUE;
```
Replace with:
```c
on_frame_accepted (dev, g_steal_pointer (&resized_image));
submitted = TRUE;
```

The `img_self` local variable at the top of the enclosing function (`FpImageDevice *img_self = FP_IMAGE_DEVICE (dev)`) should be removed once all its uses are gone.

### 2.10 New `on_frame_accepted` dispatcher and per-action helpers

Add these four functions. They are called from the SSM when a quality frame passes stage 2.

#### 2.10.1 `on_frame_accepted_enroll`

```c
static void
on_frame_accepted_enroll (FpDevice *dev, FpImage *img)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  FpPrint          *enroll_print = NULL;

  fpi_device_get_enroll_data (dev, &enroll_print);

  gsize   frame_size = (gsize) img->width * img->height;
  guint8 *pixels     = g_memdup2 (img->data, frame_size);

  if (self->enroll_stage == 0)
    {
      self->enroll_frame_size   = frame_size;
      self->enroll_frame_width  = img->width;
      self->enroll_frame_height = img->height;
    }

  g_object_unref (img);

  g_ptr_array_add (self->enroll_gallery, pixels);
  self->enroll_stage++;

  /* Disarm until next lift+press cycle */
  self->capture_armed = FALSE;
  self->turn_open     = FALSE;

  fp_info ("Enroll stage %u/%u captured", self->enroll_stage, NCC_ENROLL_FRAMES);

  if (self->enroll_stage < NCC_ENROLL_FRAMES)
    {
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);
      return;
    }

  /* All stages done: pack gallery into print */
  GVariant *gallery = pack_gallery (self->enroll_gallery,
                                    self->enroll_frame_width,
                                    self->enroll_frame_height);
  fpi_print_set_type (enroll_print, FPI_PRINT_RAW);
  g_object_set (enroll_print, "fpi-data", gallery, NULL);

  self->stop = TRUE;
  fpi_device_enroll_complete (dev, g_object_ref (enroll_print), NULL);
}
```

One press = one accepted frame (the stage-2 quality gate and the `capture_armed`/`turn_open`
reset guarantee distinct presses). Because `capture_armed` is reset to `FALSE` here,
the loop will wait for the finger to lift and re-press before it arms and captures another frame.

#### 2.10.2 `on_frame_accepted_verify`

```c
static void
on_frame_accepted_verify (FpDevice *dev, FpImage *img)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  FpPrint          *verify_print = NULL;

  fpi_device_get_verify_data (dev, &verify_print);

  guint    probe_w = img->width, probe_h = img->height;
  gsize    probe_size = (gsize) probe_w * probe_h;
  guint8  *probe = g_memdup2 (img->data, probe_size);
  g_object_unref (img);

  GVariant *stored = NULL;
  g_object_get (verify_print, "fpi-data", &stored, NULL);
  if (!stored)
    {
      g_free (probe);
      self->stop = TRUE;
      fpi_device_verify_complete (
        dev, fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID, "no fpi-data"));
      return;
    }

  guint8 **gallery;
  guint    gallery_count, gw, gh;
  if (!unpack_gallery (stored, &gallery, &gallery_count, &gw, &gh))
    {
      g_variant_unref (stored);
      g_free (probe);
      self->stop = TRUE;
      fpi_device_verify_complete (
        dev, fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                       "bad gallery variant"));
      return;
    }
  g_variant_unref (stored);

  guint   hits       = 0;
  double  best_score = -1.0;

  if (gw == probe_w && gh == probe_h)
    {
      for (guint i = 0; i < gallery_count; i++)
        {
          double s = peak_ncc (probe, gallery[i], probe_w, probe_h,
                               NCC_SEARCH_WINDOW);
          if (s > best_score)
            best_score = s;
          if (s >= NCC_THRESHOLD)
            hits++;
        }
    }

  for (guint i = 0; i < gallery_count; i++)
    g_free (gallery[i]);
  g_free (gallery);
  g_free (probe);

  gboolean match = (best_score >= NCC_THRESHOLD) && (hits >= NCC_MIN_MATCHES);
  fp_info ("Verify: best_ncc=%.3f hits=%u/%u => %s",
           best_score, hits, gallery_count, match ? "MATCH" : "NO-MATCH");

  self->stop = TRUE;
  fpi_device_verify_report (dev,
                             match ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                             NULL, NULL);
  fpi_device_verify_complete (dev, NULL);
}
```

#### 2.10.3 `on_frame_accepted_identify`

```c
static void
on_frame_accepted_identify (FpDevice *dev, FpImage *img)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  GPtrArray        *prints = NULL;

  fpi_device_get_identify_data (dev, &prints);

  guint   probe_w = img->width, probe_h = img->height;
  gsize   probe_size = (gsize) probe_w * probe_h;
  guint8 *probe = g_memdup2 (img->data, probe_size);
  g_object_unref (img);

  FpPrint *best_print   = NULL;
  double   best_score   = -1.0;

  for (guint pi = 0; pi < prints->len; pi++)
    {
      FpPrint  *candidate = g_ptr_array_index (prints, pi);
      GVariant *stored    = NULL;

      g_object_get (candidate, "fpi-data", &stored, NULL);
      if (!stored)
        continue;

      guint8 **gallery;
      guint    gallery_count, gw, gh;
      if (!unpack_gallery (stored, &gallery, &gallery_count, &gw, &gh)
          || gw != probe_w || gh != probe_h)
        {
          g_variant_unref (stored);
          continue;
        }
      g_variant_unref (stored);

      guint  hits = 0;
      double top  = -1.0;
      for (guint i = 0; i < gallery_count; i++)
        {
          double s = peak_ncc (probe, gallery[i], probe_w, probe_h,
                               NCC_SEARCH_WINDOW);
          if (s > top) top = s;
          if (s >= NCC_THRESHOLD) hits++;
          g_free (gallery[i]);
        }
      g_free (gallery);

      if (top >= NCC_THRESHOLD && hits >= NCC_MIN_MATCHES && top > best_score)
        {
          best_score = top;
          best_print = candidate;
        }
    }

  g_free (probe);

  fp_info ("Identify: best_ncc=%.3f => %s",
           best_score, best_print ? "MATCH" : "NO-MATCH");

  self->stop = TRUE;
  fpi_device_identify_report (dev, best_print, NULL, NULL);
  fpi_device_identify_complete (dev, NULL);
}
```

#### 2.10.4 `on_frame_accepted` dispatcher

```c
static void
on_frame_accepted (FpDevice *dev, FpImage *img)
{
  switch (fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_ENROLL:
      on_frame_accepted_enroll (dev, img);
      break;
    case FPI_DEVICE_ACTION_VERIFY:
      on_frame_accepted_verify (dev, img);
      break;
    case FPI_DEVICE_ACTION_IDENTIFY:
      on_frame_accepted_identify (dev, img);
      break;
    default:
      g_object_unref (img);
      break;
    }
}
```

### 2.11 `loop_complete` — replace session error call

Lines 1539–1556:
```c
/* before */
static void
loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice    *img_dev = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0577 *self    = FPI_DEVICE_EGIS0577 (dev);

  self->running = FALSE;

  if (error)
    {
      fp_dbg ("Capture loop completed with error: %s", error->message);
      fpi_image_device_session_error (img_dev, error);
    }
  else
    fp_dbg ("Capture loop completed cleanly");
}

/* after */
static void
loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  self->running = FALSE;

  if (error)
    {
      fp_dbg ("Capture loop completed with error: %s", error->message);
      fpi_device_action_error (dev, error);
    }
  else
    fp_dbg ("Capture loop completed cleanly");
}
```

`fpi_device_action_error` terminates whatever action (enroll/verify/identify) is in progress.
It is only reached when the SSM fails mid-capture before a completion call has been made.

### 2.12 Replace `dev_init`/`dev_deinit` with `dev_open`/`dev_close`

Rename and change the parameter type from `FpImageDevice *dev` to `FpDevice *dev`.
Replace the completion calls:

```c
/* before */
static void
dev_init (FpImageDevice *dev)
{
  GError *error = NULL;
  if (!g_usb_device_claim_interface (...))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }
  fpi_image_device_open_complete (dev, error);
}

static void
dev_deinit (FpImageDevice *dev)
{
  GError *error = NULL;
  clear_capture_frame (FPI_DEVICE_EGIS0577 (dev));
  g_usb_device_release_interface (...);
  fpi_image_device_close_complete (dev, error);
}

/* after */
static void
dev_open (FpDevice *dev)
{
  GError *error = NULL;
  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     EGIS0577_INTERFACE, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }
  fpi_device_open_complete (dev, NULL);
}

static void
dev_close (FpDevice *dev)
{
  GError *error = NULL;
  clear_capture_frame (FPI_DEVICE_EGIS0577 (dev));
  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  EGIS0577_INTERFACE, 0, &error);
  fpi_device_close_complete (dev, error);
}
```

### 2.13 Remove `dev_stop` and `dev_start`

Delete both functions entirely. Their responsibilities are distributed as follows:

- `dev_start`'s SSM kick-off moves into `dev_enroll`/`dev_verify`/`dev_identify` (see §2.14).
- `dev_stop`'s state cleanup moves into `dev_enroll`/`dev_verify`/`dev_identify` (each resets
  state before starting its SSM) and into `dev_close` (which already calls `clear_capture_frame`).
- The `fpi_image_device_activate_complete` and `fpi_image_device_deactivate_complete` calls
  disappear; they are FpImageDevice lifecycle concepts with no FpDevice equivalent.

### 2.14 New `dev_enroll`, `dev_verify`, `dev_identify` action handlers

These replace `dev_start`. Each starts a fresh SSM.

```c
static void
dev_enroll (FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  g_clear_pointer (&self->enroll_gallery, g_ptr_array_unref);
  self->enroll_gallery    = g_ptr_array_new_with_free_func (g_free);
  self->enroll_stage      = 0;
  self->enroll_frame_size = 0;

  self->stop                   = FALSE;
  self->finger_reported        = FALSE;
  self->capture_armed          = FALSE;
  self->unarmed_finger_first_time = 0;
  reset_noise_recovery_state (self);
  self->frame_counter          = 0;
  self->frame_reads_this_claim = 0;
  self->turn_open              = FALSE;
  clear_best_frame (self);

  FpiSsm *ssm = fpi_ssm_new (dev, ssm_run_state, SM_STATES_NUM);
  fpi_ssm_start (ssm, loop_complete);
  self->running = TRUE;
}

static void
dev_verify (FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  self->stop                   = FALSE;
  self->finger_reported        = FALSE;
  self->capture_armed          = FALSE;
  self->unarmed_finger_first_time = 0;
  reset_noise_recovery_state (self);
  self->frame_counter          = 0;
  self->frame_reads_this_claim = 0;
  self->turn_open              = FALSE;
  clear_best_frame (self);

  FpiSsm *ssm = fpi_ssm_new (dev, ssm_run_state, SM_STATES_NUM);
  fpi_ssm_start (ssm, loop_complete);
  self->running = TRUE;
}

static void
dev_identify (FpDevice *dev)
{
  /* identical initialisation to dev_verify */
  dev_verify (dev);   /* or copy the body — either is fine */
}
```

### 2.15 `fpi_device_egis0577_class_init` — rewrite

Lines 1654–1677. The current body assigns to both `FpDeviceClass` and `FpImageDeviceClass`.
The new body only uses `FpDeviceClass`:

```c
static void
fpi_device_egis0577_class_init (FpDeviceEgis0577Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id        = "egis0577";
  dev_class->full_name = "LighTuning Technology Inc. EgisTec EH577";
  dev_class->type      = FP_DEVICE_TYPE_USB;
  dev_class->id_table  = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  dev_class->nr_enroll_stages = NCC_ENROLL_FRAMES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open     = dev_open;
  dev_class->close    = dev_close;
  dev_class->enroll   = dev_enroll;
  dev_class->verify   = dev_verify;
  dev_class->identify = dev_identify;

  /* img_class assignments removed:
   *   img_class->img_open, img_close, activate, deactivate
   *   img_class->img_width, img_height
   *   img_class->bz3_threshold
   * All gone — FpDevice does not have these. */
}
```

Remove the `FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass)` line that was there.

---

## Part 3 — meson.build / build system

No changes needed. `egis0577.c` already links against `libfprint` and gets the GLib/GObject
dependencies from there. The NCC code uses only `math.h` which is part of the C standard library.

If the build system enforces that every driver listed in `drivers/meson.build` inherits from
`FpImageDevice`, find and remove that enforcement for `egis0577`. (Upstream libfprint does not
have such a constraint; all drivers in the tree derive from either `FpImageDevice` or `FpDevice`.)

---

## Part 4 — Testing checklist

After implementing:

1. **Build**: `ninja -C builddir` must succeed with no new warnings.

2. **Enroll**: Run `refs/libfprint/examples/eh577-enroll-helper`. You should see
   `NCC_ENROLL_FRAMES` (7) progress messages, each requiring a distinct lift-and-repress cycle.
   A print file should appear in the storage directory.

3. **Verify (match)**: Run `refs/libfprint/examples/verify`. Present the same finger that was
   enrolled. Should report `MATCH`.

4. **Verify (no-match)**: Present a different finger. Should report `NO-MATCH`.

5. **Identify**: Run `refs/libfprint/examples/eh577-identify-helper` with the enrolled print
   in the gallery. Same-finger: should return the matched print. Different finger: no match.

6. **Check `self->stop` timing**: Add a temporary `fp_dbg` in `loop_complete` and in the action
   complete calls to confirm that `loop_complete` is always called *after* the action complete
   function, never before and never twice.

7. **Threshold re-derivation**: After confirming the driver works end-to-end, collect a larger
   impostor set (≥5 different fingers, multiple captures each) and re-run
   `tools/eh577_pgm_correlate.c` with `--search 30` to verify the 0.50 threshold still holds,
   or adjust `NCC_THRESHOLD` and `NCC_MIN_MATCHES` accordingly. The current threshold is
   validated on exactly one impostor pair.

---

## Summary of all call sites to update

| Old call | New call | Location |
|---|---|---|
| `FpImageDevice parent` (struct) | `FpDevice parent` | `struct _FpDeviceEgis0577` |
| `G_DEFINE_TYPE(...FP_TYPE_IMAGE_DEVICE)` | `...FP_TYPE_DEVICE` | line 101 |
| `G_DECLARE_FINAL_TYPE(...FpImageDevice)` | `...FpDevice` | egis0577.h |
| `fpi_image_device_image_captured(img_self, img)` | `on_frame_accepted(dev, img)` | ~line 1169 |
| `fpi_image_device_deactivate_complete(img_dev, NULL)` | *(remove)* | SM_START stop path |
| `fpi_image_device_session_error(img_dev, error)` | `fpi_device_action_error(dev, error)` | `loop_complete` |
| `fpi_image_device_open_complete(dev, error)` | `fpi_device_open_complete(dev, error)` | `dev_open` |
| `fpi_image_device_close_complete(dev, error)` | `fpi_device_close_complete(dev, error)` | `dev_close` |
| `fpi_image_device_report_finger_status(img_self, present)` | `fp_dbg(...)` | `report_finger_status` |
| `FpImageDevice *img_dev = FP_IMAGE_DEVICE(dev)` | *(remove)* | `ssm_run_state` |
| `FpImageDevice *img_self = FP_IMAGE_DEVICE(dev)` | *(remove)* | image-processing block |
| `img_class->img_open = dev_init` | `dev_class->open = dev_open` | `class_init` |
| `img_class->img_close = dev_deinit` | `dev_class->close = dev_close` | `class_init` |
| `img_class->activate = dev_start` | `dev_class->enroll = dev_enroll` (+ verify/identify) | `class_init` |
| `img_class->deactivate = dev_stop` | *(remove)* | `class_init` |
| `img_class->img_width = ...` | *(remove)* | `class_init` |
| `img_class->img_height = ...` | *(remove)* | `class_init` |
| `img_class->bz3_threshold = ...` | *(remove)* | `class_init` |
| `dev_class->nr_enroll_stages = 8` | `dev_class->nr_enroll_stages = NCC_ENROLL_FRAMES` | `class_init` |
