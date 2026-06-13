/*
 * Egis Technology Inc. (aka. LighTuning) 0577 driver for libfprint
 * Copyright (C) 2026 Arnav Gupta <dev@championsiwmmer.in>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "egis0577"

#include <nbis.h>

#include "egis0577.h"
#include "drivers_api.h"

/*
 * WIP EH577 port:
 * - started from the public EH575 reverse-engineered driver
 * - early EH575 bulk packets are known to work on real EH577 hardware
 * - interrupt endpoints 0x83/0x84 are still unimplemented here
 * - packet/response behavior beyond the early init phase still needs capture
 */

/*
 * ==================== Basic definitions ====================
 */

/* Struct to share data across lifecycle */
struct _FpDeviceEgis0577
{
  FpImageDevice parent;

  gboolean      running;
  gboolean      stop;
  gboolean      finger_reported;
  gboolean      capture_armed;

  guint8       *capture_frame;
  gsize         capture_nonzero;

  guint8       *background;

  const Packet *pkt_array;
  int           pkt_array_len;
  int           current_index;
  guint         frame_counter;
  guint         frame_reads_this_claim;
  guint         max_frames_per_claim;
  guint         pre_frame_delay_ms;
  guint         poll_loop_delay_ms;
  guint         no_finger_retry_delay_ms;
  guint         post_capture_poll_delay_ms;
  gboolean      frame_delay_armed;
  gboolean      has_pre_init_run;

  gint64        finger_first_detected_time;

  /* Per-touch "turn" best-frame selection (see save_img). */
  gboolean      turn_open;     /* a touch is being captured right now */
  guint8       *best_frame;    /* cleanest usable frame seen this turn */
  int           best_sat;      /* saturated-pixel count of best_frame (lower=cleaner) */
  int           best_coverage; /* coverage% of best_frame */

  guint         stage2_grain_pct_x1000;
  guint         stage2_min_minutiae;
  guint         stage2_max_minutiae;
  guint         stage2_min_ridge_pixels;
  guint         stage2_min_stretch_p5;

  guint         noise_reject_streak;
  guint         noise_recovery_attempts;
  guint         noise_recovery_clean_frames;
  gboolean      noise_recovery_active;

  gint64        unarmed_finger_first_time;

};

enum sm_states {
  SM_INIT,
  SM_START,
  SM_REQ,
  SM_RESP,
  SM_PROCESS_IMG,
  SM_DONE,
  SM_STATES_NUM
};

G_DECLARE_FINAL_TYPE (FpDeviceEgis0577, fpi_device_egis0577, FPI, DEVICE_EGIS0577, FpImageDevice);
G_DEFINE_TYPE (FpDeviceEgis0577, fpi_device_egis0577, FP_TYPE_IMAGE_DEVICE);

static const char *
packet_array_name (const Packet *pkt_array)
{
  if (pkt_array == EGIS0577_POST_INIT_PACKETS)
    return "post-init";
  if (pkt_array == EGIS0577_PRE_INIT_PACKETS)
    return "pre-init";

  return "unknown";
}

static void
report_finger_status (FpDeviceEgis0577 *self,
                      FpImageDevice    *img_self,
                      gboolean          present,
                      const char       *reason)
{
  if (self->finger_reported != present)
    fp_dbg ("Reporting finger %s (%s)", present ? "present" : "absent", reason);
  else
    fp_dbg ("Finger remains %s (%s)", present ? "present" : "absent", reason);

  self->finger_reported = present;
  fpi_image_device_report_finger_status (img_self, present);
}

static void
clear_capture_frame (FpDeviceEgis0577 *self)
{
  g_clear_pointer (&self->capture_frame, g_free);
  self->capture_nonzero = 0;
}

static void
clear_background (FpDeviceEgis0577 *self)
{
  g_clear_pointer (&self->background, g_free);
}

/*
 * Keep a rolling copy of the most recent *warm* no-finger frame as the background.
 *
 * The sensor's fixed hot/saturated pixels (and fixed-pattern offset) are present in
 * the no-finger frames too, so subtracting the latest one in process_imgs cancels
 * that fixed-pattern noise without eroding ridge detail. The baseline must be a warm
 * frame that actually carries the hot pixels: a cold all-zero frame has bg=0 at those
 * locations and would fail to subtract them, so only frames that reach this helper
 * (non-zero, no-finger) update it, and they update it every time so the baseline
 * reflects the sensor state right before the finger lands.
 */
static void
update_warm_background (FpDeviceEgis0577 *self, FpiUsbTransfer *transfer)
{
  if (transfer->actual_length != EGIS0577_IMGSIZE)
    return;

  if (!self->background)
    self->background = g_malloc (EGIS0577_IMGSIZE);

  memcpy (self->background, transfer->buffer, EGIS0577_IMGSIZE);
}

static gsize
count_finger_pixels_raw (FpiUsbTransfer *transfer)
{
  gsize count = 0;

  for (gsize i = 0; i < transfer->actual_length; i++)
    {
      guint8 val = transfer->buffer[i];
      if (val > 15 && val < 150)
        count++;
    }

  return count;
}

static gsize
count_nonzero_bytes (FpiUsbTransfer *transfer)
{
  gsize nonzero = 0;

  for (size_t i = 0; i < transfer->actual_length; i++)
    if (transfer->buffer[i] != 0)
      nonzero++;

  return nonzero;
}

/* Saturated/hot pixels (~255). Fewer = cleaner frame; used to pick the best frame
 * within a touch. Saturation is the cheap, validated proxy for the perceived noise
 * ("grain"); the precise grain<0.1 / minutiae gate is enforced separately. */
static gsize
count_saturated_pixels (FpiUsbTransfer *transfer)
{
  gsize n = 0;

  for (size_t i = 0; i < transfer->actual_length; i++)
    if (transfer->buffer[i] >= 250)
      n++;

  return n;
}

static void
clear_best_frame (FpDeviceEgis0577 *self)
{
  g_clear_pointer (&self->best_frame, g_free);
  self->best_sat = -1;
  self->best_coverage = 0;
}

static guint
get_env_uint_or_default (const char *name,
                         guint       default_value)
{
  const gchar *value = g_getenv (name);
  gchar *endptr = NULL;
  guint64 parsed = 0;

  if (!value || value[0] == '\0')
    return default_value;

  parsed = g_ascii_strtoull (value, &endptr, 10);
  if (!endptr || endptr[0] != '\0')
    {
      fp_warn ("Ignoring invalid %s value: %s", name, value);
      return default_value;
    }

  if (parsed > G_MAXUINT)
    {
      fp_warn ("Ignoring too-large %s value: %s", name, value);
      return default_value;
    }

  return (guint) parsed;
}

static guint
get_env_ms_or_default (const char *name,
                       guint       default_value)
{
  return get_env_uint_or_default (name, default_value);
}

static void
dump_frame_if_requested (FpDeviceEgis0577 *self,
                         FpiUsbTransfer    *transfer,
                         gsize              nonzero)
{
  const gchar *dump_dir = g_getenv ("EGIS0577_FRAME_DUMP_DIR");
  g_autofree gchar *path = NULL;
  g_autofree gchar *base = NULL;
  g_autoptr(GError) error = NULL;

  if (!dump_dir || dump_dir[0] == '\0')
    return;

  if (transfer->actual_length != EGIS0577_IMGSIZE)
    return;

  if (g_mkdir_with_parents (dump_dir, 0755) != 0)
    {
      fp_warn ("Failed to create EH577 frame dump dir: %s", dump_dir);
      return;
    }

  base = g_strdup_printf ("%04u-%s-nonzero-%zu.bin",
                          self->frame_counter++,
                          packet_array_name (self->pkt_array),
                          nonzero);
  path = g_build_filename (dump_dir, base, NULL);

  if (!g_file_set_contents (path,
                            (const gchar *) transfer->buffer,
                            transfer->actual_length,
                            &error))
    {
      fp_warn ("Failed to dump EH577 frame to %s: %s", path, error->message);
      return;
    }

  fp_dbg ("Dumped EH577 frame to %s", path);
}

/*
 * ==================== Data processing ====================
 */

static gboolean
valid_data (FpiUsbTransfer *transfer)
{
  return count_nonzero_bytes (transfer) > 0;
}

static void
calculate_finger_heuristics (FpDeviceEgis0577 *self, FpiUsbTransfer *transfer, int *out_coverage, int *out_intensity)
{
  int coverage_pixels = 0;
  long long intensity_sum = 0;

  for (size_t i = 0; i < transfer->actual_length; i++)
    {
      guint8 val = transfer->buffer[i];
      guint8 bg = self->background ? self->background[i] : 0;

      if (val > bg + 2)
        val -= bg;
      else
        val = 0;

      if (val > 15)
        {
          coverage_pixels++;
          intensity_sum += val;
        }
    }

  *out_coverage = (coverage_pixels * 100) / transfer->actual_length;
  *out_intensity = coverage_pixels > 0 ? (intensity_sum / coverage_pixels) : 0;
}

static gboolean
finger_detected (FpDeviceEgis0577 *self, FpiUsbTransfer *transfer)
{
  int coverage = 0, intensity = 0;
  calculate_finger_heuristics (self, transfer, &coverage, &intensity);

  fp_dbg ("finger_detected: coverage=%d%% intensity=%d", coverage, intensity);
  return coverage >= 18 && intensity >= 10; /* Require at least a faint solid touch to avoid getting stuck on latent prints */
}

static gboolean
capture_usable (FpDeviceEgis0577 *self, FpiUsbTransfer *transfer)
{
  int coverage = 0, intensity = 0;
  calculate_finger_heuristics (self, transfer, &coverage, &intensity);
  return coverage >= 25 && intensity >= 20;
}

/* Normalize the processed image into the same polarity used by the offline
 * capture12 analysis tools before applying the Stage-2 quality gate.  This
 * driver only marks snapshot images as COLORS_INVERTED, so the gate runs on the
 * final resized output geometry that NBIS will actually see. */
static void
normalize_snapshot_image_for_stage2 (FpImage *img)
{
  if (!(img->flags & FPI_IMAGE_COLORS_INVERTED))
    return;

  for (gsize i = 0; i < (gsize) img->width * img->height; i++)
    img->data[i] = 0xff - img->data[i];

  img->flags &= ~FPI_IMAGE_COLORS_INVERTED;
}

static guint
histogram_percentile_value (const guint histogram[256],
                            guint       total,
                            guint       pct)
{
  guint target;
  guint cumulative = 0;

  if (total == 0)
    return 0;

  target = (guint) (((guint64) (total - 1) * pct) / 100);

  for (guint i = 0; i < 256; i++)
    {
      cumulative += histogram[i];
      if (cumulative > target)
        return i;
    }

  return 255;
}

/* Offline testing found the ImageMagick-like "stretch5" transform to be the
 * best visibility/minutiae tradeoff so far: map the 5th..99th percentile range
 * of the final normalized 208x104 snapshot into 20..245. This improves ridge
 * contrast without the heavier local-normalization variants that risk creating
 * synthetic minutiae. The submitted image is intentionally the enhanced image so
 * libfprint/NBIS and saved capture PGMs see the same pixels. */
static void
enhance_snapshot_image_stretch5 (FpImage *img,
                                 guint   *out_p5,
                                 guint   *out_p99)
{
  guint histogram[256] = { 0 };
  guint total = img->width * img->height;
  guint lo;
  guint hi;
  guint in_range;
  guint out_range = EGIS0577_ENHANCE_STRETCH_OUT_HI - EGIS0577_ENHANCE_STRETCH_OUT_LO;

  if (total == 0)
    return;

  for (gsize i = 0; i < (gsize) total; i++)
    histogram[img->data[i]]++;

  lo = histogram_percentile_value (histogram, total, EGIS0577_ENHANCE_STRETCH_LO_PCT);
  hi = histogram_percentile_value (histogram, total, EGIS0577_ENHANCE_STRETCH_HI_PCT);

  if (out_p5)
    *out_p5 = lo;
  if (out_p99)
    *out_p99 = hi;

  if (hi <= lo)
    {
      fp_dbg ("Skipping stretch5 enhancement due to flat histogram (lo=%u hi=%u)", lo, hi);
      return;
    }

  if (g_strcmp0 (g_getenv ("EGIS0577_DISABLE_STRETCH"), "1") == 0)
    {
      fp_dbg ("Skipping stretch5 enhancement because EGIS0577_DISABLE_STRETCH=1");
      return;
    }

  in_range = hi - lo;
  for (gsize i = 0; i < (gsize) total; i++)
    {
      gint v = img->data[i];
      gint stretched;

      if (v <= (gint) lo)
        stretched = EGIS0577_ENHANCE_STRETCH_OUT_LO;
      else if (v >= (gint) hi)
        stretched = EGIS0577_ENHANCE_STRETCH_OUT_HI;
      else
        stretched = EGIS0577_ENHANCE_STRETCH_OUT_LO +
                    (((v - (gint) lo) * (gint) out_range + (gint) in_range / 2) /
                     (gint) in_range);

      img->data[i] = (guint8) CLAMP (stretched, 0, 255);
    }

  fp_dbg ("Applied stretch5 enhancement: p%u=%u p%u=%u -> %u..%u",
          EGIS0577_ENHANCE_STRETCH_LO_PCT,
          lo,
          EGIS0577_ENHANCE_STRETCH_HI_PCT,
          hi,
          EGIS0577_ENHANCE_STRETCH_OUT_LO,
          EGIS0577_ENHANCE_STRETCH_OUT_HI);
}

static guint8
median9 (const guint8 *values)
{
  guint8 sorted[9];

  memcpy (sorted, values, sizeof (sorted));

  for (guint i = 1; i < G_N_ELEMENTS (sorted); i++)
    {
      guint8 v = sorted[i];
      gint j = (gint) i - 1;

      while (j >= 0 && sorted[j] > v)
        {
          sorted[j + 1] = sorted[j];
          j--;
        }

      sorted[j + 1] = v;
    }

  return sorted[4];
}

static guint
stage2_grain_pct_x1000 (FpImage *img)
{
  guint64 noisy_pixels = 0;
  guint64 interior_pixels = 0;
  guint8 window[9];

  if (img->width < 3 || img->height < 3)
    return G_MAXUINT;

  for (guint y = 1; y + 1 < img->height; y++)
    {
      for (guint x = 1; x + 1 < img->width; x++)
        {
          guint idx = 0;
          guint8 med;

          for (gint dy = -1; dy <= 1; dy++)
            for (gint dx = -1; dx <= 1; dx++)
              window[idx++] = img->data[(y + dy) * img->width + (x + dx)];

          med = median9 (window);
          if (ABS ((gint) img->data[y * img->width + x] - (gint) med) >
              EGIS0577_STAGE2_GRAIN_DIFF_THRESHOLD)
            noisy_pixels++;

          interior_pixels++;
        }
    }

  if (interior_pixels == 0)
    return G_MAXUINT;

  return (guint) ((noisy_pixels * 100000ULL) / interior_pixels);
}

static guint
stage2_ridge_pixels (FpImage *img)
{
  guint count = 0;

  for (gsize i = 0; i < (gsize) img->width * img->height; i++)
    if (img->data[i] < EGIS0577_STAGE2_RIDGE_PIXEL_THRESHOLD)
      count++;

  return count;
}

static gboolean
stage2_minutiae_count (FpImage *img, guint *out_minutiae)
{
  MINUTIAE *minutiae = NULL;
  g_autofree int *quality_map = NULL;
  g_autofree int *direction_map = NULL;
  g_autofree int *low_contrast_map = NULL;
  g_autofree int *low_flow_map = NULL;
  g_autofree int *high_curve_map = NULL;
  g_autofree unsigned char *binarized = NULL;
  g_autofree LFSPARMS *lfsparms = NULL;
  int map_w = 0, map_h = 0;
  int bw = 0, bh = 0, bd = 0;
  int r;

  lfsparms = g_memdup2 (&g_lfsparms_V2, sizeof (LFSPARMS));
  lfsparms->remove_perimeter_pts = (img->flags & FPI_IMAGE_PARTIAL) ? TRUE : FALSE;

  r = get_minutiae (&minutiae,
                    &quality_map,
                    &direction_map,
                    &low_contrast_map,
                    &low_flow_map,
                    &high_curve_map,
                    &map_w,
                    &map_h,
                    &binarized,
                    &bw,
                    &bh,
                    &bd,
                    img->data,
                    img->width,
                    img->height,
                    8,
                    img->ppmm,
                    lfsparms);
  if (r)
    {
      fp_warn ("Stage-2 minutiae scan failed, code %d", r);
      if (minutiae)
        free_minutiae (minutiae);
      return FALSE;
    }

  *out_minutiae = minutiae ? minutiae->num : 0;
  if (minutiae)
    free_minutiae (minutiae);

  return TRUE;
}

static gboolean
stage2_snapshot_quality_ok (FpDeviceEgis0577 *self,
                            FpImage          *img,
                            guint            *out_grain_pct_x1000,
                            guint            *out_ridge_pixels,
                            guint            *out_minutiae,
                            guint            *out_stretch_p5,
                            guint            *out_stretch_p99)
{
  guint grain_pct_x1000;
  guint ridge_pixels;
  guint minutiae = 0;
  guint stretch_p5 = 0;
  guint stretch_p99 = 0;

  normalize_snapshot_image_for_stage2 (img);
  enhance_snapshot_image_stretch5 (img, &stretch_p5, &stretch_p99);

  grain_pct_x1000 = stage2_grain_pct_x1000 (img);
  ridge_pixels = stage2_ridge_pixels (img);

  if (!stage2_minutiae_count (img, &minutiae))
    {
      if (out_grain_pct_x1000)
        *out_grain_pct_x1000 = grain_pct_x1000;
      if (out_ridge_pixels)
        *out_ridge_pixels = ridge_pixels;
      if (out_minutiae)
        *out_minutiae = 0;
      if (out_stretch_p5)
        *out_stretch_p5 = stretch_p5;
      if (out_stretch_p99)
        *out_stretch_p99 = stretch_p99;
      return FALSE;
    }

  if (out_grain_pct_x1000)
    *out_grain_pct_x1000 = grain_pct_x1000;
  if (out_ridge_pixels)
    *out_ridge_pixels = ridge_pixels;
  if (out_minutiae)
    *out_minutiae = minutiae;
  if (out_stretch_p5)
    *out_stretch_p5 = stretch_p5;
  if (out_stretch_p99)
    *out_stretch_p99 = stretch_p99;

  return stretch_p5 >= self->stage2_min_stretch_p5 &&
         grain_pct_x1000 < self->stage2_grain_pct_x1000 &&
         minutiae >= self->stage2_min_minutiae &&
         /* Too many minutiae on this tiny sensor usually means noise, not a
          * better fingerprint. Cap the count so noisy stretch5 frames do not
          * pass just because they manufacture many false minutiae. */
         minutiae <= self->stage2_max_minutiae &&
         ridge_pixels >= self->stage2_min_ridge_pixels;
}

static gboolean
stage2_reject_is_noise_like (FpDeviceEgis0577 *self,
                             guint             grain_pct_x1000,
                             guint             minutiae,
                             guint             stretch_p5)
{
  return stretch_p5 < self->stage2_min_stretch_p5 ||
         grain_pct_x1000 >= self->stage2_grain_pct_x1000 ||
         minutiae > self->stage2_max_minutiae;
}

static gboolean
action_is_verify_or_identify (FpDevice *dev)
{
  FpiDeviceAction action = fpi_device_get_current_action (dev);

  return action == FPI_DEVICE_ACTION_VERIFY || action == FPI_DEVICE_ACTION_IDENTIFY;
}

static guint
noise_recovery_streak_threshold (FpDevice *dev)
{
  return action_is_verify_or_identify (dev) ?
         EGIS0577_NOISE_RECOVERY_STREAK_VERIFY_IDENTIFY :
         EGIS0577_NOISE_RECOVERY_STREAK_ENROLL_CAPTURE;
}

static guint
noise_recovery_max_attempts (FpDevice *dev)
{
  return action_is_verify_or_identify (dev) ?
         EGIS0577_NOISE_RECOVERY_MAX_VERIFY_IDENTIFY :
         EGIS0577_NOISE_RECOVERY_MAX_ENROLL_CAPTURE;
}

static guint
noise_recovery_delay_ms (FpDevice *dev)
{
  return action_is_verify_or_identify (dev) ?
         EGIS0577_NOISE_RECOVERY_DELAY_VERIFY_IDENTIFY_MS :
         EGIS0577_NOISE_RECOVERY_DELAY_ENROLL_CAPTURE_MS;
}

static guint
noise_recovery_required_clean_frames (FpDevice *dev)
{
  return action_is_verify_or_identify (dev) ?
         EGIS0577_NOISE_RECOVERY_CLEAN_FRAMES_VERIFY_IDENTIFY :
         EGIS0577_NOISE_RECOVERY_CLEAN_FRAMES_ENROLL_CAPTURE;
}

static void
reset_noise_recovery_state (FpDeviceEgis0577 *self)
{
  self->noise_reject_streak = 0;
  self->noise_recovery_attempts = 0;
  self->noise_recovery_clean_frames = 0;
  self->noise_recovery_active = FALSE;
}

static gboolean
noise_recovery_note_clean_baseline (FpDeviceEgis0577 *self,
                                    FpDevice         *dev,
                                    const char       *source)
{
  guint required;

  if (!self->noise_recovery_active)
    return TRUE;

  self->noise_recovery_clean_frames++;
  required = noise_recovery_required_clean_frames (dev);

  fp_dbg ("Noise recovery clean baseline %u/%u from %s",
          self->noise_recovery_clean_frames,
          required,
          source);

  if (self->noise_recovery_clean_frames < required)
    return FALSE;

  fp_warn ("Noise recovery completed after %u clean baseline frames; capture can arm again",
           self->noise_recovery_clean_frames);
  self->noise_reject_streak = 0;
  self->noise_recovery_clean_frames = 0;
  self->noise_recovery_active = FALSE;
  return TRUE;
}

/* Used inside resp_cb to advance to the next packet in the current sequence. */
static void
jump_to_req_with_optional_delay (FpDeviceEgis0577 *self,
                                  FpiSsm           *ssm,
                                  const char       *reason)
{
  if (self->poll_loop_delay_ms > 0)
    {
      fp_dbg ("Delaying next packet by %u ms (%s)", self->poll_loop_delay_ms, reason);
      fpi_ssm_jump_to_state_delayed (ssm, SM_REQ, self->poll_loop_delay_ms);
    }
  else
    {
      fpi_ssm_jump_to_state (ssm, SM_REQ);
    }
}

/* Used in save_img / process_imgs to restart with a full PRE_INIT → POST_INIT
 * reset cycle.  Re-running POST_INIT immediately without a reset times out at
 * packet 5; a full SM_INIT gives the device the reset it needs. */
static void
jump_to_init_with_optional_delay (FpDeviceEgis0577 *self,
                                   FpiSsm           *ssm,
                                   const char       *reason)
{
  if (self->poll_loop_delay_ms > 0)
    {
      fp_dbg ("Delaying next init cycle by %u ms (%s)", self->poll_loop_delay_ms, reason);
      fpi_ssm_jump_to_state_delayed (ssm, SM_INIT, self->poll_loop_delay_ms);
    }
  else
    {
      fpi_ssm_jump_to_state (ssm, SM_INIT);
    }
}

static gboolean
recycle_interface_claim (FpDevice *dev,
                         const char *reason,
                         GError **error)
{
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);

  fp_dbg ("Releasing interface %d before fresh claim (%s)", EGIS0577_INTERFACE, reason);
  if (!g_usb_device_release_interface (usb_dev,
                                       EGIS0577_INTERFACE,
                                       0,
                                       error))
    return FALSE;

  fp_dbg ("Re-claiming interface %d (%s)", EGIS0577_INTERFACE, reason);
  if (!g_usb_device_claim_interface (usb_dev,
                                     EGIS0577_INTERFACE,
                                     0,
                                     error))
    return FALSE;

  return TRUE;
}

/* EH577 appears to budget only ~8 large 64 14 ec reads per interface claim.
 * Re-running REPEAT as a "flush" spends the same budget and wedges the next
 * stage. Instead, recycle the claim and restart from PRE_INIT so every snapshot
 * is captured from a fresh transport session, like the standalone probe does. */
static void
restart_capture_cycle (FpDeviceEgis0577 *self,
                       FpiSsm           *ssm,
                       FpDevice         *dev,
                       const char       *reason,
                       guint             delay)
{
  g_autoptr(GError) error = NULL;

  if (!recycle_interface_claim (dev, reason, &error))
    {
      fp_dbg ("Failed to recycle EH577 claim: %s", error->message);
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  self->frame_reads_this_claim = 0;
  self->frame_delay_armed = FALSE;
  fp_dbg ("Restarting capture cycle from fresh claim in %u ms (%s)", delay, reason);
  fpi_ssm_jump_to_state_delayed (ssm, SM_INIT, delay);
}

static gboolean
claim_needs_recycle (FpDeviceEgis0577 *self)
{
  return self->frame_reads_this_claim >= self->max_frames_per_claim;
}

static void
restart_for_next_poll (FpDeviceEgis0577 *self,
                       FpiSsm           *ssm,
                       FpDevice         *dev,
                       const char       *reason)
{
  if (claim_needs_recycle (self))
    {
      fp_dbg ("Frame-read budget reached on this claim (%u/%u), recycling before next poll",
              self->frame_reads_this_claim,
              self->max_frames_per_claim);
      restart_capture_cycle (self, ssm, dev, reason, self->no_finger_retry_delay_ms);
      return;
    }

  if (self->no_finger_retry_delay_ms > 0)
    {
      fp_dbg ("Retrying next capture in %u ms without recycling claim (%s)",
              self->no_finger_retry_delay_ms,
              reason);
      fpi_ssm_jump_to_state_delayed (ssm, SM_INIT, self->no_finger_retry_delay_ms);
      return;
    }

  fp_dbg ("Retrying next capture immediately without recycling claim (%s)", reason);
  fpi_ssm_jump_to_state (ssm, SM_INIT);
}

/* End a touch "turn": submit the cleanest frame collected during the window, or
 * report a retry if none qualified. Called when the 1s window has elapsed (from any
 * frame, finger present or not), guaranteeing a turn never runs longer than ~1s. */
static void
finalize_turn (FpDeviceEgis0577 *self, FpiSsm *ssm, FpDevice *dev)
{
  FpImageDevice *img_self = FP_IMAGE_DEVICE (dev);
  FpiImageDeviceState state;

  self->turn_open = FALSE;
  self->capture_armed = FALSE;

  g_object_get (dev, "fpi-image-device-state", &state, NULL);

  if (self->best_frame && state == FPI_IMAGE_DEVICE_STATE_CAPTURE)
    {
      clear_capture_frame (self);
      self->capture_frame = g_steal_pointer (&self->best_frame);
      self->capture_nonzero = self->best_coverage;
      fp_dbg ("Turn complete: submitting best frame (cov=%d%% sat=%d)",
              self->best_coverage, self->best_sat);
      clear_best_frame (self);
      fpi_ssm_next_state (ssm); /* -> SM_PROCESS_IMG */
      return;
    }

  fp_dbg ("Turn complete with no usable frame (state=%d); reporting retry", state);
  clear_best_frame (self);
  if (state == FPI_IMAGE_DEVICE_STATE_CAPTURE)
    fpi_image_device_retry_scan (img_self, FP_DEVICE_RETRY_GENERAL);
  restart_for_next_poll (self, ssm, dev, "turn window expired without usable frame");
}

static void
save_img (FpiUsbTransfer *transfer, FpDevice *dev)
{
  FpImageDevice *img_self = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  FpiImageDeviceState state;
  gboolean has_valid_data = valid_data (transfer);
  gboolean detected_finger = FALSE;
  int coverage = 0, intensity = 0;

  calculate_finger_heuristics (self, transfer, &coverage, &intensity);

  fp_dbg ("Frame received from %s[%d]: len=%zu cov=%d%% int=%d ready=%d stop=%d",
          packet_array_name (self->pkt_array),
          self->current_index,
          transfer->actual_length,
          coverage,
          intensity,
          self->capture_frame != NULL,
          self->stop);

  dump_frame_if_requested (self, transfer, coverage);
  self->frame_reads_this_claim += 1;
  fp_dbg ("Frame reads in current claim: %u/%u",
          self->frame_reads_this_claim,
          self->max_frames_per_claim);

  /* Hard 1.2s cap: once a touch's window has elapsed, finalize the turn on the very
   * next frame (finger present or not) so a turn never blocks for more than ~1.2s. */
  if (self->turn_open && !self->stop &&
      g_get_monotonic_time () - self->finger_first_detected_time > 1200 * 1000)
    {
      finalize_turn (self, transfer->ssm, dev);
      return;
    }

  /*
   * EH577 idle captures are often all-zero, including the first 5356-byte
   * post-init frame when no finger is present. Treat that as "no finger yet"
   * rather than as a fatal transport/data error.
   */
  if (!has_valid_data)
    {
      fp_dbg ("Zero frame seen; continuing polling loop");

      if (self->stop)
        {
          fp_dbg ("Stop requested while handling zero frame");
          fpi_ssm_jump_to_state (transfer->ssm, SM_DONE);
          clear_capture_frame (self);
          return;
        }

      if (self->finger_reported && g_get_monotonic_time () - self->finger_first_detected_time < 300 * 1000)
        {
          fp_dbg ("Ignoring zero frame during 300ms settle time");
          restart_for_next_poll (self, transfer->ssm, dev, "zero frame during settle");
          return;
        }

      if (!noise_recovery_note_clean_baseline (self, dev, "all-zero frame"))
        {
          report_finger_status (self, img_self, FALSE, "noise recovery waiting for additional clean zero frame");
          restart_for_next_poll (self, transfer->ssm, dev, "noise recovery clean zero frame");
          return;
        }

      if (!self->capture_armed)
        fp_dbg ("Capture armed after clean zero frame");
      self->capture_armed = TRUE;
      self->unarmed_finger_first_time = 0;

      /* A cold all-zero frame lacks the sensor's hot pixels, so it is a poor
       * baseline — do not use it. The warm no-finger branch below owns the
       * rolling background. */

      report_finger_status (self, img_self, FALSE, "all-zero frame");
      restart_for_next_poll (self, transfer->ssm, dev, "all-zero frame");
      return;
    }

  if (self->stop)
    {
      fp_dbg ("Stop requested while handling non-zero frame");
      fpi_ssm_jump_to_state (transfer->ssm, SM_DONE);
      clear_capture_frame (self);
      return;
    }

  detected_finger = finger_detected (self, transfer);
  fp_dbg ("Finger heuristic for current frame: %s", detected_finger ? "present" : "absent");

  if (!detected_finger)
    {
      if (self->finger_reported && g_get_monotonic_time () - self->finger_first_detected_time < 300 * 1000)
        {
          fp_dbg ("Ignoring no-finger frame during 300ms settle time");
          restart_for_next_poll (self, transfer->ssm, dev, "no-finger frame during settle");
          return;
        }

      /* Roll the warm no-finger baseline forward on every truly-empty frame so it
       * tracks the sensor's current fixed-pattern/hot-pixel state right up until
       * the finger lands. */
      if (count_finger_pixels_raw (transfer) < 200)
        {
          fp_dbg ("Updating rolling warm background");
          update_warm_background (self, transfer);
        }

      if (!noise_recovery_note_clean_baseline (self, dev, "warm no-finger frame"))
        {
          report_finger_status (self, img_self, FALSE, "noise recovery waiting for additional clean no-finger frame");
          restart_for_next_poll (self, transfer->ssm, dev, "noise recovery clean no-finger frame");
          return;
        }

      if (!self->capture_armed)
        fp_dbg ("Capture armed after clean no-finger frame (cov=%d%%)", coverage);
      self->capture_armed = TRUE;
      self->unarmed_finger_first_time = 0;

      report_finger_status (self, img_self, FALSE, "non-zero frame below finger heuristic threshold");
      restart_for_next_poll (self, transfer->ssm, dev, "non-zero frame below finger heuristic threshold");
      return;
    }

  g_object_get (dev, "fpi-image-device-state", &state, NULL);
  if (!self->capture_armed &&
      state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      gint64 now = g_get_monotonic_time ();

      if (self->unarmed_finger_first_time == 0)
        self->unarmed_finger_first_time = now;

      if (now - self->unarmed_finger_first_time < 750 * 1000)
        {
          fp_dbg ("Ignoring finger-like startup/transient frame with cov=%d%% until a clean no-finger baseline is observed",
                  coverage);
          report_finger_status (self, img_self, FALSE, "ignoring startup transient before capture is armed");
          restart_for_next_poll (self, transfer->ssm, dev, "startup transient before capture is armed");
          return;
        }

      fp_warn ("Persistent finger-like startup frames for >750ms; arming capture to avoid retry wedge");
      self->capture_armed = TRUE;
      self->unarmed_finger_first_time = 0;
    }

  if (!self->turn_open)
    {
      /* Finger just landed: open a fresh turn. Guarded by turn_open (not just
       * finger_reported) so a transient no-finger frame mid-turn — e.g. right
       * after a claim recycle — cannot restart the 1s window. */
      self->finger_first_detected_time = g_get_monotonic_time ();
      self->unarmed_finger_first_time = 0;
      self->turn_open = TRUE;
      clear_best_frame (self);
    }

  report_finger_status (self, img_self, TRUE, "snapshot frame exceeded threshold");
  g_object_get (dev, "fpi-image-device-state", &state, NULL);
  fp_dbg ("Image-device state after finger-present report=%d", state);

  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF)
    {
      fp_dbg ("Finger still down while upstream waits for lift/reset; recycling claim before next finger-off poll");
      restart_capture_cycle (self,
                             transfer->ssm,
                             dev,
                             "await finger-off with finger still present",
                             self->post_capture_poll_delay_ms);
      return;
    }

  if (state != FPI_IMAGE_DEVICE_STATE_CAPTURE)
    {
      fp_dbg ("Finger present outside capture state %d; not attempting a new capture", state);
      restart_for_next_poll (self, transfer->ssm, dev, "finger still present outside capture state");
      return;
    }

  {
    gint64 elapsed = g_get_monotonic_time () - self->finger_first_detected_time;

    /* Skip the first 300ms while the finger settles. */
    if (elapsed < 300 * 1000)
      {
        fp_dbg ("Finger settling (%lld ms), skipping frame", (long long) (elapsed / 1000));
        restart_for_next_poll (self, transfer->ssm, dev, "finger settling");
        return;
      }

    /* In the 300..1200ms window, keep the cleanest usable frame (lowest saturation).
     * The 1.2s cap at the top of save_img finalizes the turn and submits this best
     * frame; we never accept the first frame greedily anymore unless it passes stage 2. */
    if (capture_usable (self, transfer))
      {
        int sat = (int) count_saturated_pixels (transfer);
        gboolean quality_ok = FALSE;

        {
          g_autoptr(FpImage) img = fp_image_new (EGIS0577_PADDED_IMGWIDTH, EGIS0577_IMGHEIGHT);
          g_autoptr(FpImage) resized_image = NULL;
          img->width = EGIS0577_PADDED_IMGWIDTH;
          img->height = EGIS0577_IMGHEIGHT;
          img->flags = FPI_IMAGE_COLORS_INVERTED;

          for (guint src_y = 0; src_y < EGIS0577_SENSOR_STRIDE_Y; src_y++)
            {
              for (guint src_x = 0; src_x < EGIS0577_SENSOR_ACTIVE_WIDTH; src_x++)
                {
                  guint8 val = transfer->buffer[src_y * EGIS0577_SENSOR_STRIDE_X + src_x];
                  guint8 bg = self->background ? self->background[src_y * EGIS0577_SENSOR_STRIDE_X + src_x] : 0;
                  if (val > bg + 2)
                    val -= bg;
                  else
                    val = 0;

                  guint dest_x = src_x;
                  guint dest_y = src_y;

                  img->data[dest_y * EGIS0577_PADDED_IMGWIDTH + dest_x] = val;
                }
            }

          resized_image = fpi_image_resize (img, EGIS0577_RESIZE, EGIS0577_RESIZE);
          quality_ok = stage2_snapshot_quality_ok (self,
                                                   resized_image,
                                                   NULL, NULL, NULL, NULL, NULL);
        }

        if (quality_ok)
          {
            fp_dbg ("Fast exit: early frame passed Stage-2 quality gate");
            g_clear_pointer (&self->best_frame, g_free);
            self->best_frame = g_memdup2 (transfer->buffer, transfer->actual_length);
            self->best_sat = sat;
            self->best_coverage = coverage;
            finalize_turn (self, transfer->ssm, dev);
            return;
          }

        if (!self->best_frame || sat < self->best_sat)
          {
            g_clear_pointer (&self->best_frame, g_free);
            self->best_frame = g_memdup2 (transfer->buffer, transfer->actual_length);
            self->best_sat = sat;
            self->best_coverage = coverage;
            fp_dbg ("New best turn frame: cov=%d%% sat=%d", coverage, sat);
          }
      }

    restart_for_next_poll (self, transfer->ssm, dev, "collecting best turn frame");
    return;
  }
}

static void
process_imgs (FpiSsm *ssm, FpDevice *dev)
{
  FpImageDevice *img_self = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  FpiImageDeviceState state;

  report_finger_status (self, img_self, TRUE, "processing snapshot frame");

  g_object_get (dev, "fpi-image-device-state", &state, NULL);
  fp_dbg ("Processing snapshot frame while image-device state=%d", state);
  if (state == FPI_IMAGE_DEVICE_STATE_CAPTURE)
    {
      gboolean submitted = FALSE;
      gboolean recovery_triggered = FALSE;
      guint restart_delay = self->post_capture_poll_delay_ms;
      const char *restart_reason = "stage2 quality retry await finger-off";

      if (!self->stop && self->capture_frame)
        {
          g_autoptr(FpImage) img = NULL;
          g_autoptr(FpImage) resized_image = NULL;
          guint grain_pct_x1000 = 0;
          guint ridge_pixels = 0;
          guint minutiae = 0;
          guint stretch_p5 = 0;
          guint stretch_p99 = 0;
          gboolean quality_ok;


          img = fp_image_new (EGIS0577_PADDED_IMGWIDTH, EGIS0577_IMGHEIGHT);
          img->width = EGIS0577_PADDED_IMGWIDTH;
          img->height = EGIS0577_IMGHEIGHT;
          img->flags = FPI_IMAGE_COLORS_INVERTED;

          for (guint src_y = 0; src_y < EGIS0577_SENSOR_STRIDE_Y; src_y++)
            {
              /* Crop to the active region: only src_x 0..ACTIVE_WIDTH-1 carry
               * sensor pixels; columns ACTIVE_WIDTH..STRIDE_X-1 are firmware
               * zero padding and must not reach the matcher. The raw buffer is
               * still read at the full STRIDE_X (103) row stride. */
              for (guint src_x = 0; src_x < EGIS0577_SENSOR_ACTIVE_WIDTH; src_x++)
                {
                  guint8 val = self->capture_frame[src_y * EGIS0577_SENSOR_STRIDE_X + src_x];
                  guint8 bg = self->background ? self->background[src_y * EGIS0577_SENSOR_STRIDE_X + src_x] : 0;
                  if (val > bg + 2)
                    val -= bg;
                  else
                    val = 0;

                  /* Native landscape orientation (70x52), no rotation. */
                  guint dest_x = src_x;
                  guint dest_y = src_y;

                  img->data[dest_y * EGIS0577_PADDED_IMGWIDTH + dest_x] = val;
                }
            }

          resized_image = fpi_image_resize (img, EGIS0577_RESIZE, EGIS0577_RESIZE);
          quality_ok = stage2_snapshot_quality_ok (self,
                                                   resized_image,
                                                   &grain_pct_x1000,
                                                   &ridge_pixels,
                                                   &minutiae,
                                                   &stretch_p5,
                                                   &stretch_p99);

          fp_warn ("Stage-2 snapshot gate: stretch_p5=%u/%u stretch_p99=%u grain=%u.%03u%%/%u.%03u%% ridge_pixels=%u/%u minutiae=%u/%u..%u => %s",
                  stretch_p5,
                  self->stage2_min_stretch_p5,
                  stretch_p99,
                  grain_pct_x1000 / 1000,
                  grain_pct_x1000 % 1000,
                  self->stage2_grain_pct_x1000 / 1000,
                  self->stage2_grain_pct_x1000 % 1000,
                  ridge_pixels,
                  self->stage2_min_ridge_pixels,
                  minutiae,
                  self->stage2_min_minutiae,
                  self->stage2_max_minutiae,
                  quality_ok ? "accept" : "retry");

          if (quality_ok)
            {
              reset_noise_recovery_state (self);
              fp_dbg ("Submitting snapshot image from frame with nonzero=%zu", self->capture_nonzero);
              fpi_image_device_image_captured (img_self, g_steal_pointer (&resized_image));
              submitted = TRUE;
            }
          else
            {
              g_autoptr(GString) reject_reason = g_string_new (NULL);
              gboolean noise_like = stage2_reject_is_noise_like (self,
                                                                 grain_pct_x1000,
                                                                 minutiae,
                                                                 stretch_p5);

              if (noise_like)
                self->noise_reject_streak++;

              if (noise_like &&
                  self->noise_reject_streak >= noise_recovery_streak_threshold (dev) &&
                  self->noise_recovery_attempts < noise_recovery_max_attempts (dev))
                {
                  recovery_triggered = TRUE;
                  restart_delay = noise_recovery_delay_ms (dev);
                  restart_reason = "noise recovery fresh baseline";
                  self->noise_recovery_attempts++;
                  self->noise_reject_streak = 0;
                  self->noise_recovery_clean_frames = 0;
                  self->noise_recovery_active = TRUE;
                  self->capture_armed = FALSE;
                  self->turn_open = FALSE;
                  self->unarmed_finger_first_time = 0;
                  self->has_pre_init_run = FALSE;
                  clear_background (self);
                  clear_best_frame (self);
                  fp_warn ("Stage-2 noisy reject streak triggered fresh baseline recovery (attempt %u/%u, delay=%ums)",
                           self->noise_recovery_attempts,
                           noise_recovery_max_attempts (dev),
                           restart_delay);
                }
              else if (noise_like &&
                       self->noise_recovery_attempts >= noise_recovery_max_attempts (dev))
                {
                  fp_warn ("Stage-2 noisy reject streak seen but recovery is capped for current action (%u/%u)",
                           self->noise_recovery_attempts,
                           noise_recovery_max_attempts (dev));
                }

              if (stretch_p5 < self->stage2_min_stretch_p5)
                g_string_append_printf (reject_reason,
                                        "%sstretch_p5=%u < %u",
                                        reject_reason->len ? "; " : "",
                                        stretch_p5,
                                        self->stage2_min_stretch_p5);

              if (grain_pct_x1000 >= self->stage2_grain_pct_x1000)
                g_string_append_printf (reject_reason,
                                        "%sgrain=%u.%03u%% >= %u.%03u%%",
                                        reject_reason->len ? "; " : "",
                                        grain_pct_x1000 / 1000,
                                        grain_pct_x1000 % 1000,
                                        self->stage2_grain_pct_x1000 / 1000,
                                        self->stage2_grain_pct_x1000 % 1000);

              if (ridge_pixels < self->stage2_min_ridge_pixels)
                g_string_append_printf (reject_reason,
                                        "%sridge_pixels=%u < %u",
                                        reject_reason->len ? "; " : "",
                                        ridge_pixels,
                                        self->stage2_min_ridge_pixels);

              if (minutiae < self->stage2_min_minutiae)
                g_string_append_printf (reject_reason,
                                        "%sminutiae=%u < %u",
                                        reject_reason->len ? "; " : "",
                                        minutiae,
                                        self->stage2_min_minutiae);

              if (minutiae > self->stage2_max_minutiae)
                g_string_append_printf (reject_reason,
                                        "%sminutiae=%u > %u (likely noise)",
                                        reject_reason->len ? "; " : "",
                                        minutiae,
                                        self->stage2_max_minutiae);

              if (reject_reason->len == 0)
                g_string_assign (reject_reason, "unknown Stage-2 failure");

              if (recovery_triggered)
                g_string_append_printf (reject_reason,
                                        "%snoise recovery: fresh baseline/reinit requested",
                                        reject_reason->len ? "; " : "");

              fp_warn ("Stage-2 reject: %s", reject_reason->str);
              fpi_image_device_retry_scan (img_self, FP_DEVICE_RETRY_GENERAL);
            }
        }

      clear_capture_frame (self);

      /* Keep libfprint in AWAIT_FINGER_OFF until we observe a real lift on a
       * later poll. Recycle the claim now so those polls happen on a fresh
       * transport session. */
      fp_dbg (submitted ?
              "Image submitted; keep waiting for real finger-off and recycle claim" :
              "Stage-2 rejected image; keep waiting for real finger-off and recycle claim");
      restart_capture_cycle (self, ssm, dev,
                             submitted ? "post-capture await finger-off" : restart_reason,
                             submitted ? self->post_capture_poll_delay_ms : restart_delay);
    }
  else
    {
      clear_capture_frame (self);
      fp_dbg ("Image-device state is not CAPTURE yet, returning to polling loop");
      jump_to_init_with_optional_delay (self, ssm, "image-device state not CAPTURE yet");
    }
}

/*
 * ==================== IO ====================
 */

static void
resp_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  if (error)
    {
      if (!self->stop &&
          g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          fp_dbg ("Timeout at %s[%d], recycling claim and restarting capture",
                  packet_array_name (self->pkt_array),
                  self->current_index);
          clear_capture_frame (self);
          restart_capture_cycle (self, transfer->ssm, dev, "timeout recovery", self->no_finger_retry_delay_ms);
          g_error_free (error);
          return;
        }

      fp_dbg ("Error occurred at index %d of %s array",
              self->current_index,
              packet_array_name (self->pkt_array));
      fpi_ssm_mark_failed (transfer->ssm, error);

      clear_capture_frame (self);
      return;
    }

  fp_dbg ("RX complete for %s[%d]: actual=%zu first-bytes=%02x %02x %02x %02x %02x %02x %02x",
          packet_array_name (self->pkt_array),
          self->current_index,
          transfer->actual_length,
          transfer->actual_length > 0 ? transfer->buffer[0] : 0,
          transfer->actual_length > 1 ? transfer->buffer[1] : 0,
          transfer->actual_length > 2 ? transfer->buffer[2] : 0,
          transfer->actual_length > 3 ? transfer->buffer[3] : 0,
          transfer->actual_length > 4 ? transfer->buffer[4] : 0,
          transfer->actual_length > 5 ? transfer->buffer[5] : 0,
          transfer->actual_length > 6 ? transfer->buffer[6] : 0);

  if (self->current_index == self->pkt_array_len - 1)
    {
      if (self->pkt_array == EGIS0577_POST_INIT_PACKETS)
        {
          /* Post-init complete: a full 5356-byte snapshot frame is in
           * transfer->buffer. save_img decides whether to recycle the claim
           * for another attempt or move to SM_PROCESS_IMG. */
          fp_dbg ("Completed post-init sequence, passing frame to save_img");
          self->current_index = 0;
          save_img (transfer, dev);
          return;
        }
      else
        {
          /* Pre-init complete — switch to post-init for the frame capture. */
          fp_dbg ("Completed pre-init sequence, switching to post-init");
          self->has_pre_init_run = TRUE;
          self->pkt_array = EGIS0577_POST_INIT_PACKETS;
          self->pkt_array_len = EGIS0577_POST_INIT_PACKETS_LENGTH;
          self->current_index = 0;
        }
    }
  else if (self->pkt_array == EGIS0577_POST_INIT_PACKETS &&
           self->current_index == 1 &&
           transfer->actual_length >= 7 &&
           transfer->buffer[4] == 0x01 &&
           transfer->buffer[5] == 0x01 &&
           transfer->buffer[6] == 0x01)
    {
      /*
       * EH575 switches to pre-init on SIGE 01 01 01.
       * For EH577, the strongest capture evidence remains on the post-init
       * path, while the PRE_INIT-specific 73 14 ec command has so far returned
       * only a short status reply instead of the meaningful 5356-byte frame
       * seen on post-init 64 14 ec.
       */
      fp_dbg ("Exact 01 01 01 state observed, but EH577 stays on post-init path");
      self->current_index += 1;
    }
  else
    {
      self->current_index += 1;
    }

  jump_to_req_with_optional_delay (self, transfer->ssm, "advance packet sequence");
}

static void
recv_resp (FpiSsm *ssm, FpDevice *dev, int response_length)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  fp_dbg ("Submitting bulk IN for %s[%d], expecting %d bytes",
          packet_array_name (self->pkt_array),
          self->current_index,
          response_length);

  fpi_usb_transfer_fill_bulk (transfer, EGIS0577_EPIN, response_length);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  fpi_usb_transfer_submit (transfer, EGIS0577_TIMEOUT, NULL, resp_cb, NULL);
}

static void
send_req (FpiSsm *ssm, FpDevice *dev, const Packet *pkt)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  fp_dbg ("Submitting bulk OUT for %s[%d/%d]: %02x %02x %02x len=%d expect=%d",
          packet_array_name (self->pkt_array),
          self->current_index,
          self->pkt_array_len,
          pkt->sequence[4],
          pkt->sequence[5],
          pkt->sequence[6],
          pkt->length,
          pkt->response_length);

  fpi_usb_transfer_fill_bulk_full (transfer, EGIS0577_EPOUT, pkt->sequence, pkt->length, NULL);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;

  fpi_usb_transfer_submit (transfer, EGIS0577_TIMEOUT, NULL, fpi_ssm_usb_transfer_cb, NULL);
}

/*
 * ==================== SSM loopback ====================
 */

static void
ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SM_INIT:
      fp_dbg ("Starting capture");
      if (self->has_pre_init_run)
        {
          self->pkt_array = EGIS0577_POST_INIT_PACKETS;
          self->pkt_array_len = EGIS0577_POST_INIT_PACKETS_LENGTH;
        }
      else
        {
          self->pkt_array = EGIS0577_PRE_INIT_PACKETS;
          self->pkt_array_len = EGIS0577_PRE_INIT_PACKETS_LENGTH;
        }
      self->current_index = 0;
      self->pre_frame_delay_ms = get_env_ms_or_default ("EGIS0577_PRE_FRAME_DELAY_MS", 0);
      self->poll_loop_delay_ms = get_env_ms_or_default ("EGIS0577_POLL_LOOP_DELAY_MS", 0);
      self->no_finger_retry_delay_ms = get_env_ms_or_default ("EGIS0577_NO_FINGER_RETRY_DELAY_MS",
                                                              EGIS0577_NO_FINGER_RETRY_DELAY_MS);
      self->post_capture_poll_delay_ms = get_env_ms_or_default ("EGIS0577_POST_CAPTURE_POLL_DELAY_MS",
                                                                EGIS0577_POST_CAPTURE_POLL_DELAY_MS);
      self->max_frames_per_claim = get_env_ms_or_default ("EGIS0577_MAX_FRAMES_PER_CLAIM",
                                                          EGIS0577_MAX_FRAMES_PER_CLAIM);
      self->stage2_grain_pct_x1000 = get_env_uint_or_default ("EGIS0577_STAGE2_GRAIN_PCT_X1000",
                                                              EGIS0577_STAGE2_GRAIN_PCT_X1000);
      self->stage2_min_minutiae = get_env_uint_or_default ("EGIS0577_STAGE2_MIN_MINUTIAE",
                                                           EGIS0577_STAGE2_MIN_MINUTIAE);
      self->stage2_max_minutiae = get_env_uint_or_default ("EGIS0577_STAGE2_MAX_MINUTIAE",
                                                           EGIS0577_STAGE2_MAX_MINUTIAE);
      self->stage2_min_ridge_pixels = get_env_uint_or_default ("EGIS0577_STAGE2_MIN_RIDGE_PIXELS",
                                                               EGIS0577_STAGE2_MIN_RIDGE_PIXELS);
      self->stage2_min_stretch_p5 = get_env_uint_or_default ("EGIS0577_STAGE2_MIN_STRETCH_P5",
                                                             EGIS0577_STAGE2_MIN_STRETCH_P5);
      self->frame_delay_armed = FALSE;

      clear_capture_frame (self);
      fp_dbg ("Initial packet array: %s", packet_array_name (self->pkt_array));
      fp_dbg ("EH577 pacing config: pre_frame_delay_ms=%u poll_loop_delay_ms=%u no_finger_retry_delay_ms=%u post_capture_poll_delay_ms=%u max_frames_per_claim=%u current_claim_frames=%u stage2_stretch_p5>=%u stage2_grain<%u.%03u%% stage2_minutiae=%u..%u stage2_ridge_pixels>=%u",
              self->pre_frame_delay_ms,
              self->poll_loop_delay_ms,
              self->no_finger_retry_delay_ms,
              self->post_capture_poll_delay_ms,
              self->max_frames_per_claim,
              self->frame_reads_this_claim,
              self->stage2_min_stretch_p5,
              self->stage2_grain_pct_x1000 / 1000,
              self->stage2_grain_pct_x1000 % 1000,
              self->stage2_min_minutiae,
              self->stage2_max_minutiae,
              self->stage2_min_ridge_pixels);
      fpi_ssm_next_state (ssm);
      break;

    case SM_START:
      if (self->stop)
        {
          fp_dbg ("Stopping, completed capture");
          fpi_ssm_mark_completed (ssm);
          fpi_image_device_deactivate_complete (img_dev, NULL);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case SM_REQ:
      fp_dbg ("SSM_REQ using %s[%d/%d]",
              packet_array_name (self->pkt_array),
              self->current_index,
              self->pkt_array_len);

      if (self->pkt_array[self->current_index].response_length == EGIS0577_IMGSIZE &&
          self->pre_frame_delay_ms > 0 &&
          !self->frame_delay_armed)
        {
          self->frame_delay_armed = TRUE;
          fp_dbg ("Delaying frame request %s[%d] by %u ms before sending 64 14 ec",
                  packet_array_name (self->pkt_array),
                  self->current_index,
                  self->pre_frame_delay_ms);
          fpi_ssm_jump_to_state_delayed (ssm, SM_REQ, self->pre_frame_delay_ms);
          break;
        }

      self->frame_delay_armed = FALSE;
      send_req (ssm, dev, &self->pkt_array[self->current_index]);
      break;

    case SM_RESP:
      fp_dbg ("SSM_RESP waiting on %s[%d/%d]",
              packet_array_name (self->pkt_array),
              self->current_index,
              self->pkt_array_len);
      recv_resp (ssm, dev, self->pkt_array[self->current_index].response_length);
      break;

    case SM_PROCESS_IMG:
      process_imgs (ssm, dev);
      break;

    case SM_DONE:
      fpi_ssm_jump_to_state (ssm, SM_START);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  self->running = FALSE;

  if (error)
    {
      fp_dbg ("Capture loop completed with error: %s", error->message);
      fpi_image_device_session_error (img_dev, error);
    }
  else
    {
      fp_dbg ("Capture loop completed cleanly");
    }
}

/*
 * ==================== Top-level command callback & meta-data ====================
 */

static void
dev_init (FpImageDevice *dev)
{
  GError *error = NULL;

  fp_dbg ("Opening EH577 device");
  fp_dbg ("Claiming interface %d", EGIS0577_INTERFACE);
  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                     EGIS0577_INTERFACE,
                                     0,
                                     &error))
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
  fp_dbg ("Releasing interface %d", EGIS0577_INTERFACE);
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  EGIS0577_INTERFACE,
                                  0,
                                  &error);

  fpi_image_device_close_complete (dev, error);
}

static void
dev_stop (FpImageDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  fp_dbg ("Deactivating");

  self->stop = TRUE;

  clear_capture_frame (self);
  clear_background (self);
  clear_best_frame (self);
  self->turn_open = FALSE;
  self->unarmed_finger_first_time = 0;
  reset_noise_recovery_state (self);

  self->has_pre_init_run = FALSE;

  if (self->running)
    self->stop = TRUE;
  else
    fpi_image_device_deactivate_complete (dev, NULL);
}

static void
dev_start (FpImageDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (dev), ssm_run_state, SM_STATES_NUM);

  fp_dbg ("Activate requested");
  self->stop = FALSE;
  self->finger_reported = FALSE;
  self->capture_armed = FALSE;
  self->unarmed_finger_first_time = 0;
  reset_noise_recovery_state (self);
  self->frame_counter = 0;
  self->frame_reads_this_claim = 0;
  self->turn_open = FALSE;
  clear_best_frame (self);

  fpi_ssm_start (ssm, loop_complete);

  self->running = TRUE;

  fpi_image_device_activate_complete (dev, NULL);
}

static const FpIdEntry id_table[] = {{
                                       .vid = 0x1c7a,
                                       .pid = 0x0577,
                                     }};

static void
fpi_device_egis0577_init (FpDeviceEgis0577 *self)
{
}

static void
fpi_device_egis0577_class_init (FpDeviceEgis0577Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = "egis0577";
  dev_class->full_name = "LighTuning Technology Inc. EgisTec EH577";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = 8;
  dev_class->temp_hot_seconds = -1;

  img_class->img_open = dev_init;
  img_class->img_close = dev_deinit;
  img_class->activate = dev_start;
  img_class->deactivate = dev_stop;

  img_class->img_width = EGIS0577_PADDED_IMGWIDTH * EGIS0577_RESIZE;
  img_class->img_height = EGIS0577_IMGHEIGHT * EGIS0577_RESIZE;

  img_class->bz3_threshold = EGIS0577_BZ3_THRESHOLD;
}
