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

#include <math.h>
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
  FpDevice      parent;

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
  guint         unarmed_reinit_attempts; /* fresh init+rearm escalations while stuck unarmed */

  GPtrArray    *enroll_gallery;
  guint         enroll_stage;
  gsize         enroll_frame_size;
  guint         enroll_frame_width;
  guint         enroll_frame_height;

  guint         startup_timeout_retries;
  guint         startup_reset_attempts;

  gint64        rearm_not_before_time;
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

G_DECLARE_FINAL_TYPE (FpDeviceEgis0577, fpi_device_egis0577, FPI, DEVICE_EGIS0577, FpDevice);
G_DEFINE_TYPE (FpDeviceEgis0577, fpi_device_egis0577, FP_TYPE_DEVICE);

#define NCC_ENROLL_FRAMES 7
#define NCC_THRESHOLD 0.50
#define NCC_MIN_MATCHES 2
#define NCC_SEARCH_WINDOW 30

#define EGIS0577_STARTUP_TIMEOUT_RECOVERY_MAX        2
#define EGIS0577_STARTUP_TIMEOUT_RECOVERY_DELAY_MS 250
#define EGIS0577_STARTUP_RESET_RECOVERY_MAX          1
#define EGIS0577_STARTUP_RESET_DELAY_MS            500
#define EGIS0577_STARTUP_SETTLE_DELAY_MS           150
#define EGIS0577_ENROLL_REARM_SETTLE_MS            800

/* When the poll loop is stuck unarmed (persistent finger-like frames, no clean
 * no-finger baseline to arm from), escalate by forcing a fresh init+rearm this
 * many times before giving up and asking the caller to lift and retry. Each
 * escalation recycles the interface claim and re-runs PRE_INIT (fresh transport
 * session, like the standalone probe) — it never performs a USB-level reset,
 * which is known to wedge this hardware until a cold reboot. A single PRE_INIT
 * recycle already runs on the Stage-2 reject that precedes this state, so keep
 * the extra escalations low — the actionable outcome when the sensor stays
 * noisy is the lift+retry prompt, and the user should reach it quickly. */
#define EGIS0577_UNARMED_REINIT_MAX                  2

/* Per-touch timing. After a finger is first detected we skip frames for a short
 * settle window so the press stabilizes before it is evaluated. A touch "turn"
 * is force-finalized once the turn timeout elapses, so a single press can never
 * poll indefinitely. */
#define EGIS0577_FINGER_SETTLE_MS                  400
#define EGIS0577_TURN_TIMEOUT_MS                  1400

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
                      gboolean          present,
                      const char       *reason)
{
  if (self->finger_reported != present)
    fp_dbg ("Reporting finger %s (%s)", present ? "present" : "absent", reason);
  else
    fp_dbg ("Finger remains %s (%s)", present ? "present" : "absent", reason);

  self->finger_reported = present;
  fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                           present ? FP_FINGER_STATUS_PRESENT : FP_FINGER_STATUS_NONE,
                                           present ? FP_FINGER_STATUS_NONE : FP_FINGER_STATUS_PRESENT);
}

static double
ncc_at_offset (const guint8 *a,
               const guint8 *b,
               guint         w,
               guint         h,
               double        a_mean,
               double        a_std,
               double        b_mean,
               double        b_std,
               int           dx,
               int           dy)
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
  int n = 0;

  for (int y = 0; y < yend; y++)
    {
      const guint8 *row_a = a + (y0a + y) * w + x0a;
      const guint8 *row_b = b + (y0b + y) * w + x0b;

      for (int x = 0; x < xend; x++)
        {
          cross += ((double) row_a[x] - a_mean) * ((double) row_b[x] - b_mean);
          n++;
        }
    }

  if (n == 0)
    return -1.0;

  return cross / ((double) n * a_std * b_std);
}

static double
peak_ncc (const guint8 *a,
          const guint8 *b,
          guint         w,
          guint         h,
          int           search)
{
  gsize n = (gsize) w * h;
  double a_sum = 0.0, a_sq = 0.0, b_sum = 0.0, b_sq = 0.0;

  for (gsize i = 0; i < n; i++)
    {
      a_sum += a[i];
      a_sq += (double) a[i] * a[i];
      b_sum += b[i];
      b_sq += (double) b[i] * b[i];
    }

  double a_mean = a_sum / n;
  double b_mean = b_sum / n;
  double a_var = a_sq / n - a_mean * a_mean;
  double b_var = b_sq / n - b_mean * b_mean;
  double a_std = (a_var > 0.0) ? sqrt (a_var) : 1e-9;
  double b_std = (b_var > 0.0) ? sqrt (b_var) : 1e-9;
  double best = -2.0;

  for (int dy = -search; dy <= search; dy++)
    for (int dx = -search; dx <= search; dx++)
      {
        double v = ncc_at_offset (a, b, w, h,
                                  a_mean, a_std,
                                  b_mean, b_std,
                                  dx, dy);
        if (v > best)
          best = v;
      }

  return best;
}

static GVariant *
pack_gallery (GPtrArray *frames,
              guint      frame_width,
              guint      frame_height)
{
  GVariantBuilder builder;
  gsize frame_size = (gsize) frame_width * frame_height;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));
  for (guint i = 0; i < frames->len; i++)
    g_variant_builder_add_value (&builder,
                                 g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                            g_ptr_array_index (frames, i),
                                                            frame_size,
                                                            sizeof (guint8)));

  return g_variant_new ("(uu@aay)",
                        frame_width,
                        frame_height,
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
  *out_frames = g_new0 (guint8 *, *out_count);

  for (guint i = 0; i < *out_count; i++)
    {
      GVariant *child = g_variant_get_child_value (frames_var, i);
      gsize len = 0;
      const void *raw = g_variant_get_fixed_array (child, &len, sizeof (guint8));

      if (len != frame_size)
        {
          for (guint j = 0; j < i; j++)
            g_free ((*out_frames)[j]);
          g_free (*out_frames);
          *out_frames = NULL;
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

static FpPrint *
create_raw_frame_print (FpDevice *dev,
                        FpImage  *img)
{
  g_autoptr(GPtrArray) frames = g_ptr_array_new_with_free_func (g_free);
  gsize frame_size = (gsize) img->width * img->height;
  FpPrint *print = fp_print_new (dev);
  GVariant *gallery;

  g_ptr_array_add (frames, g_memdup2 (img->data, frame_size));
  gallery = pack_gallery (frames, img->width, img->height);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", gallery, NULL);

  return print;
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

static gboolean
get_env_bool_or_default (const char *name,
                         gboolean    default_value)
{
  const gchar *value = g_getenv (name);

  if (!value || value[0] == '\0')
    return default_value;

  if (g_ascii_strcasecmp (value, "1") == 0 ||
      g_ascii_strcasecmp (value, "true") == 0 ||
      g_ascii_strcasecmp (value, "yes") == 0 ||
      g_ascii_strcasecmp (value, "on") == 0)
    return TRUE;

  if (g_ascii_strcasecmp (value, "0") == 0 ||
      g_ascii_strcasecmp (value, "false") == 0 ||
      g_ascii_strcasecmp (value, "no") == 0 ||
      g_ascii_strcasecmp (value, "off") == 0)
    return FALSE;

  fp_warn ("Ignoring invalid %s value: %s", name, value);
  return default_value;
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

static gboolean
reset_device_and_reclaim (FpDevice *dev,
                          const char *reason,
                          GError **error)
{
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);
  g_autoptr(GError) release_error = NULL;

  fp_dbg ("Releasing interface %d before USB reset (%s)", EGIS0577_INTERFACE, reason);
  if (!g_usb_device_release_interface (usb_dev,
                                       EGIS0577_INTERFACE,
                                       0,
                                       &release_error))
    fp_dbg ("Ignoring release failure before USB reset: %s", release_error->message);

  fp_dbg ("Resetting USB device (%s)", reason);
  if (!g_usb_device_reset (usb_dev, error))
    return FALSE;

  fp_dbg ("Re-claiming interface %d after USB reset (%s)", EGIS0577_INTERFACE, reason);
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

static void
restart_capture_cycle_with_usb_reset (FpDeviceEgis0577 *self,
                                      FpiSsm           *ssm,
                                      FpDevice         *dev,
                                      const char       *reason,
                                      guint             delay)
{
  g_autoptr(GError) error = NULL;

  if (!reset_device_and_reclaim (dev, reason, &error))
    {
      fp_dbg ("Failed to USB-reset EH577 device: %s", error->message);
      fpi_ssm_mark_failed (ssm, g_steal_pointer (&error));
      return;
    }

  self->frame_reads_this_claim = 0;
  self->frame_delay_armed = FALSE;
  self->has_pre_init_run = FALSE;
  fp_dbg ("Restarting capture cycle after USB reset in %u ms (%s)", delay, reason);
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

static void
on_frame_accepted_enroll (FpDevice *dev,
                          FpImage  *img)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  FpPrint *enroll_print = NULL;
  gsize frame_size = (gsize) img->width * img->height;
  guint8 *pixels = g_memdup2 (img->data, frame_size);

  fpi_device_get_enroll_data (dev, &enroll_print);

  if (self->enroll_stage == 0)
    {
      self->enroll_frame_size = frame_size;
      self->enroll_frame_width = img->width;
      self->enroll_frame_height = img->height;
    }
  else if (self->enroll_frame_size != frame_size)
    {
      g_free (pixels);
      g_object_unref (img);
      self->stop = TRUE;
      fpi_device_action_error (dev,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                         "inconsistent enrollment frame size"));
      return;
    }

  g_object_unref (img);

  g_ptr_array_add (self->enroll_gallery, pixels);
  self->enroll_stage++;

  /* The enrollment template starts as FPI_PRINT_UNDEFINED and is reused across
   * progress callbacks. Only set the final print type once, right before
   * completion, otherwise repeated fpi_print_set_type() calls trip the internal
   * assertion that the type must still be undefined. */
  fpi_device_enroll_progress (dev, self->enroll_stage, enroll_print, NULL);

  self->capture_armed = FALSE;
  self->turn_open = FALSE;
  self->rearm_not_before_time = g_get_monotonic_time () + EGIS0577_ENROLL_REARM_SETTLE_MS * 1000;

  fp_info ("Enroll stage %u/%u captured; re-arm blocked for %u ms to require a real lift/reset",
           self->enroll_stage, NCC_ENROLL_FRAMES, EGIS0577_ENROLL_REARM_SETTLE_MS);

  if (self->enroll_stage < NCC_ENROLL_FRAMES)
    return;

  GVariant *gallery = pack_gallery (self->enroll_gallery,
                                    self->enroll_frame_width,
                                    self->enroll_frame_height);
  fpi_print_set_type (enroll_print, FPI_PRINT_RAW);
  g_object_set (enroll_print, "fpi-data", gallery, NULL);

  self->stop = TRUE;
  fpi_device_enroll_complete (dev, g_object_ref (enroll_print), NULL);
}

static void
on_frame_accepted_verify (FpDevice *dev,
                          FpImage  *img)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  FpPrint *verify_print = NULL;
  g_autoptr(FpPrint) probe_print = NULL;
  g_autoptr(GVariant) stored = NULL;
  guint probe_w = img->width;
  guint probe_h = img->height;
  gsize probe_size = (gsize) probe_w * probe_h;
  guint8 *probe = g_memdup2 (img->data, probe_size);

  fpi_device_get_verify_data (dev, &verify_print);
  probe_print = create_raw_frame_print (dev, img);
  g_object_get (verify_print, "fpi-data", &stored, NULL);
  g_object_unref (img);

  if (!stored)
    {
      g_free (probe);
      self->stop = TRUE;
      fpi_device_verify_complete (dev,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                            "no fpi-data in verify print"));
      return;
    }

  guint8 **gallery = NULL;
  guint gallery_count = 0, gw = 0, gh = 0;
  if (!unpack_gallery (stored, &gallery, &gallery_count, &gw, &gh))
    {
      g_free (probe);
      self->stop = TRUE;
      fpi_device_verify_complete (dev,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                            "bad gallery variant"));
      return;
    }

  guint hits = 0;
  double best_score = -1.0;

  if (gw == probe_w && gh == probe_h)
    {
      for (guint i = 0; i < gallery_count; i++)
        {
          double s = peak_ncc (probe, gallery[i], probe_w, probe_h, NCC_SEARCH_WINDOW);
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

  gboolean match = gw == probe_w && gh == probe_h &&
                   best_score >= NCC_THRESHOLD &&
                   hits >= NCC_MIN_MATCHES;

  fp_info ("Verify: best_ncc=%.3f hits=%u/%u => %s",
           best_score, hits, gallery_count, match ? "MATCH" : "NO-MATCH");

  self->stop = TRUE;
  fpi_device_verify_report (dev,
                            match ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                            g_steal_pointer (&probe_print),
                            NULL);
  fpi_device_verify_complete (dev, NULL);
}

static void
on_frame_accepted_identify (FpDevice *dev,
                            FpImage  *img)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  GPtrArray *prints = NULL;
  g_autoptr(FpPrint) probe_print = NULL;
  guint probe_w = img->width;
  guint probe_h = img->height;
  gsize probe_size = (gsize) probe_w * probe_h;
  guint8 *probe = g_memdup2 (img->data, probe_size);
  FpPrint *best_print = NULL;
  double best_score = -1.0;

  fpi_device_get_identify_data (dev, &prints);
  probe_print = create_raw_frame_print (dev, img);
  g_object_unref (img);

  for (guint pi = 0; pi < prints->len; pi++)
    {
      FpPrint *candidate = g_ptr_array_index (prints, pi);
      g_autoptr(GVariant) stored = NULL;
      guint8 **gallery = NULL;
      guint gallery_count = 0, gw = 0, gh = 0;
      guint hits = 0;
      double top = -1.0;

      g_object_get (candidate, "fpi-data", &stored, NULL);
      if (!stored)
        continue;

      if (!unpack_gallery (stored, &gallery, &gallery_count, &gw, &gh) ||
          gw != probe_w || gh != probe_h)
        {
          if (gallery)
            {
              for (guint i = 0; i < gallery_count; i++)
                g_free (gallery[i]);
              g_free (gallery);
            }
          continue;
        }

      for (guint i = 0; i < gallery_count; i++)
        {
          double s = peak_ncc (probe, gallery[i], probe_w, probe_h, NCC_SEARCH_WINDOW);
          if (s > top)
            top = s;
          if (s >= NCC_THRESHOLD)
            hits++;
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
  fpi_device_identify_report (dev, best_print, g_steal_pointer (&probe_print), NULL);
  fpi_device_identify_complete (dev, NULL);
}

/* Single-shot like verify: hand the stage-2-qualifying image straight back to
 * fp_device_capture_finish(). This is the exact resized snapshot the NCC matcher
 * sees, so the capture12 tooling stores the same pixels the matcher would.
 * Setting stop=TRUE lets the SSM wind down to SM_DONE after we complete; without
 * it the poll loop would keep running past the completed action. */
static void
on_frame_accepted_capture (FpDevice *dev,
                           FpImage  *img)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  self->stop = TRUE;
  fpi_device_capture_complete (dev, img, NULL);
}

static void
on_frame_accepted (FpDevice *dev,
                   FpImage  *img)
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

    case FPI_DEVICE_ACTION_CAPTURE:
      on_frame_accepted_capture (dev, img);
      break;

    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
    default:
      g_object_unref (img);
      break;
    }
}

/* Last-resort exit when the device stays stuck unarmed after exhausting the
 * fresh init+rearm escalations: the sensor keeps returning finger-like frames
 * and we never observe a clean no-finger baseline. Rather than spin forever,
 * surface a retryable "remove finger" condition so the caller can prompt the
 * user to lift and try again. Dispatched by action so we complete the right
 * one (capture is single-shot; enroll just nudges the current stage and keeps
 * polling so a clean baseline can still complete it). */
static void
report_unarmed_stuck (FpDeviceEgis0577 *self, FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_device_get_current_action (dev))
    {
    case FPI_DEVICE_ACTION_CAPTURE:
      self->stop = TRUE;
      fpi_device_capture_complete (dev, NULL,
                                   fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
      fpi_ssm_jump_to_state (ssm, SM_DONE);
      break;

    case FPI_DEVICE_ACTION_ENROLL:
      /* Retry convention: report the retry with no partial print. */
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
      /* Keep enrollment alive: allow another fresh init+rearm cycle. */
      self->unarmed_reinit_attempts = 0;
      self->unarmed_finger_first_time = 0;
      restart_capture_cycle (self, ssm, dev,
                             "enroll stuck unarmed: fresh init+rearm after lift prompt",
                             self->no_finger_retry_delay_ms);
      break;

    case FPI_DEVICE_ACTION_VERIFY:
    case FPI_DEVICE_ACTION_IDENTIFY:
    case FPI_DEVICE_ACTION_NONE:
    case FPI_DEVICE_ACTION_PROBE:
    case FPI_DEVICE_ACTION_OPEN:
    case FPI_DEVICE_ACTION_CLOSE:
    case FPI_DEVICE_ACTION_LIST:
    case FPI_DEVICE_ACTION_DELETE:
    case FPI_DEVICE_ACTION_CLEAR_STORAGE:
    default:
      /* verify/identify force-arm before reaching here; just keep polling. */
      restart_for_next_poll (self, ssm, dev, "stuck unarmed (non-enroll/capture)");
      break;
    }
}

/* End a touch "turn": submit the cleanest frame collected during the window, or
 * restart polling if none qualified. Called when the 1s window has elapsed (from any
 * frame, finger present or not), guaranteeing a turn never runs longer than ~1s. */
static void
finalize_turn (FpDeviceEgis0577 *self, FpiSsm *ssm, FpDevice *dev)
{
  self->turn_open = FALSE;
  self->capture_armed = FALSE;

  if (self->best_frame)
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

  fp_dbg ("Turn complete with no usable frame; restarting poll loop");
  clear_best_frame (self);
  restart_for_next_poll (self, ssm, dev, "turn window expired without usable frame");
}

static void
save_img (FpiUsbTransfer *transfer, FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
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

  if (self->turn_open && !self->stop &&
      g_get_monotonic_time () - self->finger_first_detected_time > EGIS0577_TURN_TIMEOUT_MS * 1000)
    {
      finalize_turn (self, transfer->ssm, dev);
      return;
    }

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

      if (self->finger_reported &&
          g_get_monotonic_time () - self->finger_first_detected_time < EGIS0577_FINGER_SETTLE_MS * 1000)
        {
          fp_dbg ("Ignoring zero frame during %ums settle time", EGIS0577_FINGER_SETTLE_MS);
          restart_for_next_poll (self, transfer->ssm, dev, "zero frame during settle");
          return;
        }

      if (!noise_recovery_note_clean_baseline (self, dev, "all-zero frame"))
        {
          report_finger_status (self, FALSE, "noise recovery waiting for additional clean zero frame");
          restart_for_next_poll (self, transfer->ssm, dev, "noise recovery clean zero frame");
          return;
        }

      if (!action_is_verify_or_identify (dev) &&
          self->rearm_not_before_time > 0 &&
          g_get_monotonic_time () < self->rearm_not_before_time)
        {
          report_finger_status (self, FALSE, "enroll re-arm settle after all-zero baseline");
          restart_for_next_poll (self, transfer->ssm, dev, "enroll re-arm settle after all-zero baseline");
          return;
        }

      if (!self->capture_armed)
        fp_dbg ("Capture armed after clean zero frame");
      self->capture_armed = TRUE;
      self->unarmed_finger_first_time = 0;
      self->unarmed_reinit_attempts = 0;
      self->rearm_not_before_time = 0;

      report_finger_status (self, FALSE, "all-zero frame");
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
      if (self->finger_reported &&
          g_get_monotonic_time () - self->finger_first_detected_time < EGIS0577_FINGER_SETTLE_MS * 1000)
        {
          fp_dbg ("Ignoring no-finger frame during %ums settle time", EGIS0577_FINGER_SETTLE_MS);
          restart_for_next_poll (self, transfer->ssm, dev, "no-finger frame during settle");
          return;
        }

      if (count_finger_pixels_raw (transfer) < 200)
        {
          fp_dbg ("Updating rolling warm background");
          update_warm_background (self, transfer);
        }

      if (!noise_recovery_note_clean_baseline (self, dev, "warm no-finger frame"))
        {
          report_finger_status (self, FALSE, "noise recovery waiting for additional clean no-finger frame");
          restart_for_next_poll (self, transfer->ssm, dev, "noise recovery clean no-finger frame");
          return;
        }

      if (!action_is_verify_or_identify (dev) &&
          self->rearm_not_before_time > 0 &&
          g_get_monotonic_time () < self->rearm_not_before_time)
        {
          report_finger_status (self, FALSE, "enroll re-arm settle after warm baseline");
          restart_for_next_poll (self, transfer->ssm, dev, "enroll re-arm settle after warm baseline");
          return;
        }

      if (!self->capture_armed)
        fp_dbg ("Capture armed after clean no-finger frame (cov=%d%%)", coverage);
      self->capture_armed = TRUE;
      self->unarmed_finger_first_time = 0;
      self->unarmed_reinit_attempts = 0;
      self->rearm_not_before_time = 0;

      report_finger_status (self, FALSE, "non-zero frame below finger heuristic threshold");
      restart_for_next_poll (self, transfer->ssm, dev, "non-zero frame below finger heuristic threshold");
      return;
    }

  if (!self->capture_armed)
    {
      gint64 now = g_get_monotonic_time ();

      if (self->unarmed_finger_first_time == 0)
        self->unarmed_finger_first_time = now;

      if (now - self->unarmed_finger_first_time < 750 * 1000)
        {
          fp_dbg ("Ignoring finger-like startup/transient frame with cov=%d%% until a clean no-finger baseline is observed",
                  coverage);
          report_finger_status (self, FALSE, "ignoring startup transient before capture is armed");
          restart_for_next_poll (self, transfer->ssm, dev, "startup transient before capture is armed");
          return;
        }

      if (action_is_verify_or_identify (dev))
        {
          fp_warn ("Persistent finger-like startup frames for >750ms during verify/identify; arming capture to avoid login wedge");
          self->capture_armed = TRUE;
          self->unarmed_finger_first_time = 0;
        }
      else if (self->unarmed_reinit_attempts < EGIS0577_UNARMED_REINIT_MAX)
        {
          /* Stuck unarmed during enroll/capture: a Stage-2 noise reject cleared
           * the warm background and left the sensor in a noisy state, so every
           * frame reads finger-like and we can never observe the clean no-finger
           * baseline needed to (re)arm. Force a fresh transport session — claim
           * recycle + PRE_INIT re-run — so the sensor settles and a clean
           * baseline can rebuild. This is the "reset the init and rearm" path; it
           * deliberately does NOT USB-reset the device (that wedges this hardware
           * until a cold reboot). Bounded by EGIS0577_UNARMED_REINIT_MAX. */
          self->unarmed_reinit_attempts++;
          fp_warn ("Persistent finger-like frames for >750ms during enroll/capture; forcing fresh init+rearm (attempt %u/%u, no USB reset)",
                   self->unarmed_reinit_attempts, EGIS0577_UNARMED_REINIT_MAX);
          self->capture_armed = FALSE;
          self->turn_open = FALSE;
          self->unarmed_finger_first_time = 0;
          self->has_pre_init_run = FALSE;       /* force PRE_INIT on next SM_INIT */
          reset_noise_recovery_state (self);
          clear_background (self);               /* drop the noisy baseline; rebuild fresh */
          report_finger_status (self, FALSE, "forcing fresh init+rearm while stuck unarmed");
          restart_capture_cycle (self, transfer->ssm, dev,
                                 "stuck unarmed: fresh init+rearm",
                                 self->no_finger_retry_delay_ms);
          return;
        }
      else
        {
          fp_warn ("Still finger-like after %u init+rearm attempts; asking caller to remove finger and retry",
                   self->unarmed_reinit_attempts);
          report_finger_status (self, FALSE, "remove finger and retry");
          report_unarmed_stuck (self, transfer->ssm, dev);
          return;
        }
    }

  if (!self->turn_open)
    {
      self->finger_first_detected_time = g_get_monotonic_time ();
      self->unarmed_finger_first_time = 0;
      self->turn_open = TRUE;
      clear_best_frame (self);
    }

  report_finger_status (self, TRUE, "snapshot frame exceeded threshold");

  {
    gint64 elapsed = g_get_monotonic_time () - self->finger_first_detected_time;

    if (elapsed < EGIS0577_FINGER_SETTLE_MS * 1000)
      {
        fp_dbg ("Finger settling (%lld ms), skipping frame", (long long) (elapsed / 1000));
        restart_for_next_poll (self, transfer->ssm, dev, "finger settling");
        return;
      }

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
            for (guint src_x = 0; src_x < EGIS0577_SENSOR_ACTIVE_WIDTH; src_x++)
              {
                guint8 val = transfer->buffer[src_y * EGIS0577_SENSOR_STRIDE_X + src_x];
                guint8 bg = self->background ? self->background[src_y * EGIS0577_SENSOR_STRIDE_X + src_x] : 0;

                if (val > bg + 2)
                  val -= bg;
                else
                  val = 0;

                img->data[src_y * EGIS0577_PADDED_IMGWIDTH + src_x] = val;
              }

          resized_image = fpi_image_resize (img, EGIS0577_RESIZE, EGIS0577_RESIZE);
          quality_ok = stage2_snapshot_quality_ok (self, resized_image, NULL, NULL, NULL, NULL, NULL);
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
  }
}

static void
process_imgs (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  gboolean submitted = FALSE;
  gboolean recovery_triggered = FALSE;
  guint restart_delay = self->post_capture_poll_delay_ms;
  const char *restart_reason = "stage2 quality retry await finger-off";

  report_finger_status (self, TRUE, "processing snapshot frame");

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
        for (guint src_x = 0; src_x < EGIS0577_SENSOR_ACTIVE_WIDTH; src_x++)
          {
            guint8 val = self->capture_frame[src_y * EGIS0577_SENSOR_STRIDE_X + src_x];
            guint8 bg = self->background ? self->background[src_y * EGIS0577_SENSOR_STRIDE_X + src_x] : 0;

            if (val > bg + 2)
              val -= bg;
            else
              val = 0;

            img->data[src_y * EGIS0577_PADDED_IMGWIDTH + src_x] = val;
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
          on_frame_accepted (dev, g_steal_pointer (&resized_image));
          submitted = TRUE;
        }
      else
        {
          g_autoptr(GString) reject_reason = g_string_new (NULL);
          gboolean noise_like = stage2_reject_is_noise_like (self,
                                                             grain_pct_x1000,
                                                             minutiae,
                                                             stretch_p5);

          /* Only noisy rejects (high grain, too many minutiae, or a washed-out
           * stretch) mean the warm baseline itself is suspect and worth wiping
           * for a fresh transport session. A clean-but-insufficient frame — e.g.
           * low minutiae from a light or partial press — leaves the baseline
           * perfectly good; forcing a baseline wipe + PRE_INIT reinit there just
           * costs time and risks the noisy-rearm loop. Keep the baseline and
           * simply wait for a better press. */
          if (noise_like)
            {
              self->noise_reject_streak = 0;
              recovery_triggered = TRUE;
              restart_delay = noise_recovery_delay_ms (dev);
              restart_reason = "noise recovery fresh baseline";
              self->noise_recovery_attempts++;
              self->noise_recovery_clean_frames = 0;
              self->noise_recovery_active = TRUE;
              self->capture_armed = FALSE;
              self->turn_open = FALSE;
              self->unarmed_finger_first_time = 0;
              self->has_pre_init_run = FALSE;
              if (!action_is_verify_or_identify (dev))
                self->rearm_not_before_time = g_get_monotonic_time () + EGIS0577_ENROLL_REARM_SETTLE_MS * 1000;
              clear_background (self);
              clear_best_frame (self);
              fp_warn ("Stage-2 noise-like reject -> fresh baseline recovery (delay=%ums)",
                       restart_delay);
            }
          else
            {
              restart_delay = self->no_finger_retry_delay_ms;
              restart_reason = "stage2 non-noise retry (keep baseline)";
              self->capture_armed = FALSE;
              self->turn_open = FALSE;
              self->unarmed_finger_first_time = 0;
              if (!action_is_verify_or_identify (dev))
                self->rearm_not_before_time = g_get_monotonic_time () + EGIS0577_ENROLL_REARM_SETTLE_MS * 1000;
              clear_best_frame (self);
              fp_warn ("Stage-2 non-noise reject (e.g. low minutiae) -> retry without fresh baseline (delay=%ums)",
                       restart_delay);
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
        }
    }

  clear_capture_frame (self);

  if (self->stop)
    {
      fp_dbg ("Action completed after accepted frame");
      fpi_ssm_jump_to_state (ssm, SM_DONE);
      return;
    }

  fp_dbg (submitted ?
          "Image submitted; waiting for real finger-off and restarting polling" :
          "Stage-2 rejected image; waiting for real finger-off and restarting polling");
  restart_capture_cycle (self, ssm, dev,
                         submitted ? "post-capture await finger-off" : restart_reason,
                         submitted ? self->post_capture_poll_delay_ms : restart_delay);
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
req_cb (FpiUsbTransfer *transfer, FpDevice *dev, gpointer user_data, GError *error)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  gboolean first_preinit = self->pkt_array == EGIS0577_PRE_INIT_PACKETS &&
                           self->current_index == 0 &&
                           !self->has_pre_init_run;

  if (error)
    {
      if (!self->stop &&
          g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          if (first_preinit)
            {
              if (self->startup_timeout_retries < EGIS0577_STARTUP_TIMEOUT_RECOVERY_MAX)
                {
                  self->startup_timeout_retries++;
                  fp_warn ("Timeout sending first pre-init packet; recycling claim and retrying startup (%u/%u)",
                           self->startup_timeout_retries,
                           EGIS0577_STARTUP_TIMEOUT_RECOVERY_MAX);
                  clear_capture_frame (self);
                  restart_capture_cycle (self,
                                         transfer->ssm,
                                         dev,
                                         "startup timeout recovery",
                                         EGIS0577_STARTUP_TIMEOUT_RECOVERY_DELAY_MS);
                  g_error_free (error);
                  return;
                }

              if (get_env_bool_or_default ("EGIS0577_ENABLE_USB_RESET_RECOVERY", FALSE) &&
                  self->startup_reset_attempts < EGIS0577_STARTUP_RESET_RECOVERY_MAX)
                {
                  self->startup_reset_attempts++;
                  self->startup_timeout_retries = 0;
                  fp_warn ("Timeout sending first pre-init packet after claim recycle; USB-resetting device and retrying startup (%u/%u)",
                           self->startup_reset_attempts,
                           EGIS0577_STARTUP_RESET_RECOVERY_MAX);
                  clear_capture_frame (self);
                  restart_capture_cycle_with_usb_reset (self,
                                                       transfer->ssm,
                                                       dev,
                                                       "startup USB reset recovery",
                                                       EGIS0577_STARTUP_RESET_DELAY_MS);
                  g_error_free (error);
                  return;
                }

              /* Claim-recycle retries are exhausted and USB-reset recovery is
               * off by default (it wedges this hardware until a cold reboot).
               * The device is present but not accepting the first pre-init bulk
               * write — it is transport-wedged. Fail cleanly with an actionable
               * message instead of silently re-recycling forever (which makes
               * the capture/enroll scripts hang with no further output). The
               * reliable recovery is a physical unplug/replug of the sensor. */
              fp_warn ("EH577 not responding to init after %u claim recycles; sensor is transport-wedged — unplug and replug it, then retry",
                       EGIS0577_STARTUP_TIMEOUT_RECOVERY_MAX);
              clear_capture_frame (self);
              fpi_ssm_mark_failed (transfer->ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "EH577 did not respond to init (transport-wedged); unplug and replug the sensor, then retry"));
              g_error_free (error);
              return;
            }

          fp_dbg ("Timeout at %s[%d], recycling claim and restarting capture",
                  packet_array_name (self->pkt_array),
                  self->current_index);
          clear_capture_frame (self);
          restart_capture_cycle (self, transfer->ssm, dev, "timeout recovery", self->no_finger_retry_delay_ms);
          g_error_free (error);
          return;
        }

      fp_dbg ("Error occurred sending packet at index %d of %s array",
              self->current_index,
              packet_array_name (self->pkt_array));
      fpi_ssm_mark_failed (transfer->ssm, error);
      clear_capture_frame (self);
      return;
    }

  if (first_preinit && (self->startup_timeout_retries > 0 || self->startup_reset_attempts > 0))
    {
      fp_dbg ("Recovered startup after %u claim retr%s and %u USB reset attempt%s",
              self->startup_timeout_retries,
              self->startup_timeout_retries == 1 ? "y" : "ies",
              self->startup_reset_attempts,
              self->startup_reset_attempts == 1 ? "" : "s");
      self->startup_timeout_retries = 0;
      self->startup_reset_attempts = 0;
    }

  fpi_ssm_next_state (transfer->ssm);
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

  fpi_usb_transfer_submit (transfer, EGIS0577_TIMEOUT, NULL, req_cb, NULL);
}

/*
 * ==================== SSM loopback ====================
 */

static void
ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

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
      if (!self->has_pre_init_run && self->pkt_array == EGIS0577_PRE_INIT_PACKETS && self->current_index == 0)
        {
          fp_dbg ("Applying %u ms startup settle delay before first pre-init packet",
                  EGIS0577_STARTUP_SETTLE_DELAY_MS);
          fpi_ssm_jump_to_state_delayed (ssm, SM_START, EGIS0577_STARTUP_SETTLE_DELAY_MS);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case SM_START:
      if (self->stop)
        {
          fp_dbg ("Stopping, completed capture");
          fpi_ssm_mark_completed (ssm);
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
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  self->running = FALSE;

  if (error)
    {
      fp_dbg ("Capture loop completed with error: %s", error->message);
      fpi_device_action_error (dev, error);
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
reset_action_state (FpDeviceEgis0577 *self)
{
  self->stop = FALSE;
  self->finger_reported = FALSE;
  self->capture_armed = FALSE;
  self->unarmed_finger_first_time = 0;
  self->unarmed_reinit_attempts = 0;
  reset_noise_recovery_state (self);
  self->frame_counter = 0;
  self->frame_reads_this_claim = 0;
  self->turn_open = FALSE;
  self->has_pre_init_run = FALSE;
  self->startup_timeout_retries = 0;
  self->startup_reset_attempts = 0;
  self->rearm_not_before_time = 0;
  clear_capture_frame (self);
  clear_background (self);
  clear_best_frame (self);
}

static void
start_capture_action (FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  FpiSsm *ssm = fpi_ssm_new (dev, ssm_run_state, SM_STATES_NUM);

  reset_action_state (self);
  fpi_ssm_start (ssm, loop_complete);
  self->running = TRUE;
}

static void
dev_open (FpDevice *dev)
{
  GError *error = NULL;
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);

  fp_dbg ("Opening EH577 device");
  fp_dbg ("Claiming interface %d", EGIS0577_INTERFACE);
  if (!g_usb_device_claim_interface (usb_dev,
                                     EGIS0577_INTERFACE,
                                     0,
                                     &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  /* The unconditional reset-on-open path proved too aggressive and could make
   * the device disappear from lsusb on this hardware. Keep open lightweight;
   * startup hardening now happens via a short settle delay before the first
   * pre-init packet plus timeout-triggered claim recycle / USB-reset recovery. */
  fp_dbg ("Settling %u ms after claim before first action", EGIS0577_STARTUP_SETTLE_DELAY_MS);
  g_usleep (EGIS0577_STARTUP_SETTLE_DELAY_MS * 1000);

  fpi_device_open_complete (dev, NULL);
}

static void
dev_close (FpDevice *dev)
{
  GError *error = NULL;
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  clear_capture_frame (self);
  clear_background (self);
  clear_best_frame (self);
  g_clear_pointer (&self->enroll_gallery, g_ptr_array_unref);
  fp_dbg ("Releasing interface %d", EGIS0577_INTERFACE);
  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  EGIS0577_INTERFACE,
                                  0,
                                  &error);

  fpi_device_close_complete (dev, error);
}

static void
dev_enroll (FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  fp_dbg ("Enroll requested");
  g_clear_pointer (&self->enroll_gallery, g_ptr_array_unref);
  self->enroll_gallery = g_ptr_array_new_with_free_func (g_free);
  self->enroll_stage = 0;
  self->enroll_frame_size = 0;
  self->enroll_frame_width = 0;
  self->enroll_frame_height = 0;

  start_capture_action (dev);
}

static void
dev_verify (FpDevice *dev)
{
  fp_dbg ("Verify requested");
  start_capture_action (dev);
}

static void
dev_identify (FpDevice *dev)
{
  fp_dbg ("Identify requested");
  start_capture_action (dev);
}

static void
dev_capture (FpDevice *dev)
{
  fp_dbg ("Capture requested");
  start_capture_action (dev);
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

  dev_class->id = "egis0577";
  dev_class->full_name = "LighTuning Technology Inc. EgisTec EH577";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = NCC_ENROLL_FRAMES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->enroll = dev_enroll;
  dev_class->verify = dev_verify;
  dev_class->identify = dev_identify;
  dev_class->capture = dev_capture;

  fpi_device_class_auto_initialize_features (dev_class);

  /* For desktop login flows (fprintd/pam_fprintd/GDM), verify is the critical
   * path.  Our live tests show verify works, but identify is still less stable
   * (one enrolled finger failed to identify from a 2-print gallery). Keep the
   * private identify implementation available for targeted testing, but do not
   * advertise IDENTIFY support to user space until it is proven reliable. */
  dev_class->features &= ~FP_DEVICE_FEATURE_IDENTIFY;
}
