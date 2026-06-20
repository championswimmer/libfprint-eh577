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
#include <stdio.h>
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
  gboolean      waiting_for_lift; /* set on turn timeout or accepted frame; cleared on lift */

  guint         pgm_debug_counter;
  gint64        pgm_debug_last_capture_time;

  guint8       *capture_frame;

  guint8       *background;
  guint         background_warmup_remaining; /* idle frames still to grab as baseline */

  const Packet *pkt_array;
  int           pkt_array_len;
  int           current_index;
  guint         frame_counter;
  guint         frame_reads_this_claim;
  gboolean      has_pre_init_run;

  gint64        finger_first_detected_time;
  gboolean      turn_open;

  GPtrArray    *enroll_gallery;
  guint         enroll_stage;
  guint         enroll_frame_width;
  guint         enroll_frame_height;

  guint         startup_timeout_retries;
};

enum sm_states {
  SM_INIT,
  SM_START,
  SM_REQ,
  SM_RESP,
  SM_DONE,
  SM_STATES_NUM
};

G_DECLARE_FINAL_TYPE (FpDeviceEgis0577, fpi_device_egis0577, FPI, DEVICE_EGIS0577, FpDevice);
G_DEFINE_TYPE (FpDeviceEgis0577, fpi_device_egis0577, FP_TYPE_DEVICE);

#define NCC_ENROLL_FRAMES 12
#define NCC_THRESHOLD 0.50
#define NCC_MIN_MATCHES 2
#define NCC_SEARCH_WINDOW 30

/* Startup: recycle the interface claim up to this many times if the first
 * pre-init packet times out. If still stuck after that, fail cleanly. */
#define EGIS0577_STARTUP_TIMEOUT_RECOVERY_MAX        2
#define EGIS0577_STARTUP_TIMEOUT_RECOVERY_DELAY_MS 250
#define EGIS0577_STARTUP_SETTLE_DELAY_MS           150

/* Per-touch timing. After a finger first lands we ignore frames for SETTLE_MS
 * so the press stabilises before evaluation. If no valid frame is captured
 * within TURN_TIMEOUT_MS the turn fails and the driver waits for a real lift
 * before arming a new attempt. */
#define EGIS0577_FINGER_SETTLE_MS                  400
#define EGIS0577_TURN_TIMEOUT_MS                  1400

/* Startup: grab this many valid (non-zero) idle frames as the warm background
 * before arming finger detection. Without a baseline, finger_detected compares
 * against bg=0 and the sensor's hot idle frame reads as a finger, deadlocking
 * detection. The capture workflow guarantees no finger is present at startup. */
#define EGIS0577_BACKGROUND_WARMUP_FRAMES            3

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
 * the no-finger frames too, so subtracting the latest one in save_img cancels
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


static void
maybe_write_live_frame_pgm (FpImage *img)
{
  const gchar *path = g_getenv ("EGIS0577_LIVE_FRAME_PATH");
  gchar header[64];
  int header_len;
  gsize data_len;
  g_autofree gchar *buf = NULL;
  g_autoptr(GError) error = NULL;

  if (!path || !path[0])
    return;

  header_len = g_snprintf (header, sizeof (header), "P5 %u %u 255\n", img->width, img->height);
  data_len = (gsize) img->width * img->height;
  buf = g_malloc (header_len + data_len);
  memcpy (buf, header, header_len);
  memcpy (buf + header_len, img->data, data_len);

  if (!g_file_set_contents (path, buf, (gssize) (header_len + data_len), &error))
    fp_dbg ("live frame write failed: %s", error->message);
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

static gboolean
write_image_pgm (FpImage     *img,
                 const gchar *path,
                 GError     **error)
{
  gchar header[64];
  int header_len;
  gsize data_len;
  g_autofree gchar *buf = NULL;

  header_len = g_snprintf (header, sizeof (header), "P5 %u %u 255\n", img->width, img->height);
  data_len = (gsize) img->width * img->height;
  buf = g_malloc (header_len + data_len);
  memcpy (buf, header, header_len);
  memcpy (buf + header_len, img->data, data_len);

  return g_file_set_contents (path, buf, (gssize) (header_len + data_len), error);
}

static gboolean
pgm_debug_enabled (void)
{
  const gchar *dir = g_getenv ("EGIS0577_PGM_DEBUG_DIR");

  return dir && dir[0];
}

static gboolean
pgm_debug_control_active (void)
{
  const gchar *control_path = g_getenv ("EGIS0577_PGM_DEBUG_CONTROL");
  g_autofree gchar *contents = NULL;
  gsize len = 0;

  if (!control_path || !control_path[0])
    return TRUE;

  if (!g_file_get_contents (control_path, &contents, &len, NULL) || len == 0)
    return FALSE;

  return contents[0] == '1' || contents[0] == 'f' || contents[0] == 'F' ||
         g_str_has_prefix (contents, "running");
}

static guint
pgm_debug_interval_ms (void)
{
  const gchar *value = g_getenv ("EGIS0577_PGM_DEBUG_INTERVAL_MS");
  gint64 parsed;

  if (!value || !value[0])
    return 100;

  parsed = g_ascii_strtoll (value, NULL, 10);
  if (parsed < 1)
    return 100;

  return (guint) CLAMP (parsed, 1, 10000);
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
  gsize raw_finger_pixels;

  calculate_finger_heuristics (self, transfer, &coverage, &intensity);
  raw_finger_pixels = count_finger_pixels_raw (transfer);

  fp_dbg ("finger_detected: coverage=%d%% intensity=%d raw_finger_pixels=%zu",
          coverage, intensity, raw_finger_pixels);
  return coverage >= EGIS0577_PRESENCE_MIN_COVERAGE_PCT &&
         intensity >= EGIS0577_PRESENCE_MIN_INTENSITY &&
         raw_finger_pixels >= EGIS0577_PRESENCE_MIN_RAW_FINGER_PIXELS;
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

/* In-place 3x3 median denoise. The press snapshot carries high-frequency
 * speckle that stretch5 then amplifies (a narrow p5..p99 range means a large
 * stretch gain, turning a few grey levels of sensor wobble into >25-level jumps
 * that the grain metric counts as noise). A median filter removes that speckle
 * while preserving ridge edges, so it runs *before* the stretch. Interior
 * pixels only; the 1px border is left untouched (the grain metric ignores it
 * too). Operates on a snapshot copy so each output reads only original pixels. */
static void
denoise_snapshot_median3x3 (FpImage *img)
{
  guint w = img->width;
  guint h = img->height;
  g_autofree guint8 *src = NULL;
  guint8 window[9];

  if (w < 3 || h < 3)
    return;

  src = g_memdup2 (img->data, (gsize) w * h);

  for (guint y = 1; y + 1 < h; y++)
    for (guint x = 1; x + 1 < w; x++)
      {
        guint idx = 0;

        for (gint dy = -1; dy <= 1; dy++)
          for (gint dx = -1; dx <= 1; dx++)
            window[idx++] = src[(y + dy) * w + (x + dx)];

        img->data[y * w + x] = median9 (window);
      }
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
  denoise_snapshot_median3x3 (img);
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

  return grain_pct_x1000 < EGIS0577_STAGE2_GRAIN_PCT_X1000 &&
         minutiae > EGIS0577_STAGE2_MIN_MINUTIAE &&
         minutiae < EGIS0577_STAGE2_MAX_MINUTIAE &&
         ridge_pixels > EGIS0577_STAGE2_MIN_RIDGE_PIXELS;
}

static FpImage *
create_processed_snapshot (FpDeviceEgis0577 *self,
                           FpiUsbTransfer   *transfer)
{
  g_autoptr(FpImage) img = fp_image_new (EGIS0577_PADDED_IMGWIDTH, EGIS0577_IMGHEIGHT);

  img->width = EGIS0577_PADDED_IMGWIDTH;
  img->height = EGIS0577_IMGHEIGHT;
  img->flags = FPI_IMAGE_COLORS_INVERTED;

  for (guint src_y = 0; src_y < EGIS0577_SENSOR_STRIDE_Y; src_y++)
    for (guint src_x = 0; src_x < EGIS0577_SENSOR_ACTIVE_WIDTH; src_x++)
      {
        guint8 val = transfer->buffer[src_y * EGIS0577_SENSOR_STRIDE_X + src_x];
        guint8 bg = self->background ? self->background[src_y * EGIS0577_SENSOR_STRIDE_X + src_x] : 0;

        img->data[src_y * EGIS0577_PADDED_IMGWIDTH + src_x] = (val > bg + 2) ? val - bg : 0;
      }

  return fpi_image_resize (img, EGIS0577_RESIZE, EGIS0577_RESIZE);
}

static void
image_basic_stats (FpImage *img,
                   guint   *out_min,
                   guint   *out_max,
                   guint   *out_mean)
{
  gsize n = (gsize) img->width * img->height;
  guint min = 255;
  guint max = 0;
  guint64 sum = 0;

  for (gsize i = 0; i < n; i++)
    {
      guint v = img->data[i];
      min = MIN (min, v);
      max = MAX (max, v);
      sum += v;
    }

  if (out_min)
    *out_min = min;
  if (out_max)
    *out_max = max;
  if (out_mean)
    *out_mean = n ? (guint) (sum / n) : 0;
}

static void
pgm_debug_maybe_capture (FpDeviceEgis0577 *self,
                         FpiUsbTransfer   *transfer,
                         gboolean          has_valid_data)
{
  const gchar *dir = g_getenv ("EGIS0577_PGM_DEBUG_DIR");
  const gchar *log_path = g_getenv ("EGIS0577_PGM_DEBUG_LOG");
  gint64 now;
  guint interval_ms;
  int coverage = 0;
  int intensity = 0;
  guint raw_nonzero;
  guint raw_finger_pixels;
  gboolean present;
  g_autoptr(FpImage) img = NULL;
  guint grain_pct_x1000 = 0;
  guint ridge_pixels = 0;
  guint minutiae = 0;
  guint stretch_p5 = 0;
  guint stretch_p99 = 0;
  guint pixel_min = 0;
  guint pixel_max = 0;
  guint pixel_mean = 0;
  gboolean quality_ok;
  guint seq;
  gint64 t_ms;
  g_autofree gchar *base = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr(GError) error = NULL;
  FILE *logf;
  gboolean need_header = FALSE;

  if (!pgm_debug_enabled () || !pgm_debug_control_active ())
    return;

  now = g_get_monotonic_time ();
  interval_ms = pgm_debug_interval_ms ();
  if (self->pgm_debug_last_capture_time != 0 &&
      now - self->pgm_debug_last_capture_time < (gint64) interval_ms * 1000)
    return;

  if (transfer->actual_length != EGIS0577_IMGSIZE)
    return;

  if (g_mkdir_with_parents (dir, 0755) != 0)
    {
      fp_warn ("PGM debug: failed to create %s", dir);
      return;
    }

  calculate_finger_heuristics (self, transfer, &coverage, &intensity);
  raw_nonzero = (guint) count_nonzero_bytes (transfer);
  raw_finger_pixels = (guint) count_finger_pixels_raw (transfer);
  present = has_valid_data &&
            coverage >= EGIS0577_PRESENCE_MIN_COVERAGE_PCT &&
            intensity >= EGIS0577_PRESENCE_MIN_INTENSITY &&
            raw_finger_pixels >= EGIS0577_PRESENCE_MIN_RAW_FINGER_PIXELS;

  img = create_processed_snapshot (self, transfer);
  quality_ok = stage2_snapshot_quality_ok (self, img,
                                           &grain_pct_x1000,
                                           &ridge_pixels,
                                           &minutiae,
                                           &stretch_p5,
                                           &stretch_p99);
  image_basic_stats (img, &pixel_min, &pixel_max, &pixel_mean);

  self->pgm_debug_last_capture_time = now;
  seq = ++self->pgm_debug_counter;
  t_ms = now / 1000;
  base = g_strdup_printf ("frame-%06u-t%lld.pgm", seq, (long long) t_ms);
  path = g_build_filename (dir, base, NULL);

  if (!write_image_pgm (img, path, &error))
    {
      fp_warn ("PGM debug: failed to write %s: %s", path, error->message);
      return;
    }

  if (!log_path || !log_path[0])
    return;

  need_header = !g_file_test (log_path, G_FILE_TEST_EXISTS);
  logf = fopen (log_path, "a");
  if (!logf)
    {
      fp_warn ("PGM debug: failed to open metrics log %s", log_path);
      return;
    }

  if (need_header)
    fprintf (logf, "seq,pgm,t_ms,raw_nonzero,raw_finger_pixels,presence,coverage_pct,intensity,grain_pct_x1000,grain_pct,ridge_pixels,minutiae,stretch_p5,stretch_p99,pixel_min,pixel_max,pixel_mean,quality_ok\n");

  fprintf (logf,
           "%u,%s,%lld,%u,%u,%d,%d,%d,%u,%u.%03u,%u,%u,%u,%u,%u,%u,%u,%d\n",
           seq,
           base,
           (long long) t_ms,
           raw_nonzero,
           raw_finger_pixels,
           present ? 1 : 0,
           coverage,
           intensity,
           grain_pct_x1000,
           grain_pct_x1000 / 1000,
           grain_pct_x1000 % 1000,
           ridge_pixels,
           minutiae,
           stretch_p5,
           stretch_p99,
           pixel_min,
           pixel_max,
           pixel_mean,
           quality_ok ? 1 : 0);
  fclose (logf);
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
  self->has_pre_init_run = FALSE;
  fp_dbg ("Restarting capture cycle from fresh claim in %u ms (%s)", delay, reason);
  fpi_ssm_jump_to_state_delayed (ssm, SM_INIT, delay);
}

static gboolean
claim_needs_recycle (FpDeviceEgis0577 *self)
{
  return self->frame_reads_this_claim >= EGIS0577_MAX_FRAMES_PER_CLAIM;
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
              EGIS0577_MAX_FRAMES_PER_CLAIM);
      restart_capture_cycle (self, ssm, dev, reason, 0);
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
      self->enroll_frame_width = img->width;
      self->enroll_frame_height = img->height;
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
  self->waiting_for_lift = TRUE;

  fp_info ("Enroll stage %u/%u captured; waiting for lift before next turn",
           self->enroll_stage, NCC_ENROLL_FRAMES);

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

    case FPI_DEVICE_ACTION_CAPTURE:
      on_frame_accepted_capture (dev, img);
      break;

    case FPI_DEVICE_ACTION_IDENTIFY:
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

/*
 * save_img: called once per completed post-init sequence with the raw 5356-byte
 * frame in transfer->buffer.  Implements the per-touch turn flow:
 *
 *  1. No finger / zero frame → close any open turn, clear waiting_for_lift if
 *     set, arm for next touch. A lift mid-turn (settle or evaluate window) thus
 *     immediately ends the turn and re-arms.
 *  2. Finger present, waiting_for_lift or not armed → ignore.
 *  3. Finger present, armed → open turn; skip frames until FINGER_SETTLE_MS.
 *  4. Past settle, still within TURN_TIMEOUT_MS → run quality gate once per frame.
 *     First frame that passes: enhance, submit to on_frame_accepted, done.
 *  5. TURN_TIMEOUT_MS elapsed without a valid frame → report absent, set
 *     waiting_for_lift so no new turn starts until the finger is actually lifted.
 */
static void
save_img (FpiUsbTransfer *transfer, FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  FpiSsm *ssm = transfer->ssm;
  gboolean has_valid_data = valid_data (transfer);

  dump_frame_if_requested (self, transfer, count_nonzero_bytes (transfer));
  self->frame_reads_this_claim += 1;
  fp_dbg ("Frame reads in current claim: %u/%u",
          self->frame_reads_this_claim,
          EGIS0577_MAX_FRAMES_PER_CLAIM);

  if (self->stop)
    {
      fpi_ssm_jump_to_state (ssm, SM_DONE);
      return;
    }

  /* ---- Startup warmup: capture the idle baseline as the warm background. ----
   * Until a baseline exists, finger_detected compares against bg=0 and the hot
   * idle frame reads as a finger, deadlocking detection. Grab the first few
   * valid (non-zero) frames unconditionally; skip cold/zero frames (a zero
   * background fails to cancel the sensor's hot pixels). Don't run finger
   * detection until this completes. */
  if (self->background_warmup_remaining > 0)
    {
      if (has_valid_data)
        {
          update_warm_background (self, transfer);
          self->background_warmup_remaining--;
          fp_dbg ("Background warmup: grabbed idle baseline (%u frame(s) left)",
                  self->background_warmup_remaining);
        }
      else
        {
          fp_dbg ("Background warmup: skipping cold/zero frame");
        }
      restart_for_next_poll (self, ssm, dev, "background warmup");
      return;
    }

  if (pgm_debug_enabled ())
    {
      gboolean present_now;

      pgm_debug_maybe_capture (self, transfer, has_valid_data);
      present_now = has_valid_data && finger_detected (self, transfer);
      if (!present_now && has_valid_data && count_finger_pixels_raw (transfer) < 200)
        update_warm_background (self, transfer);
      report_finger_status (self, present_now, present_now ? "pgm debug present" : "pgm debug absent");
      restart_for_next_poll (self, ssm, dev, "pgm debug poll");
      return;
    }

  /* ---- No-finger path (zero frame or below detection threshold) ---- */
  if (!has_valid_data || !finger_detected (self, transfer))
    {
      if (has_valid_data && count_finger_pixels_raw (transfer) < 200)
        update_warm_background (self, transfer);

      /* Any lift clears the "wait for lift" block and arms a fresh turn. */
      if (self->waiting_for_lift)
        {
          self->waiting_for_lift = FALSE;
          fp_dbg ("Lift detected; re-arming for next touch");
        }

      /* Finger absent: dump any open turn so the next touch starts a fresh
       * settle + timeout window anchored on the new finger-present moment.
       * This covers a lift at any point during a turn (settle or evaluate
       * window), not just after a timeout. */
      self->turn_open = FALSE;
      self->capture_armed = TRUE;
      report_finger_status (self, FALSE, has_valid_data ? "below threshold" : "zero frame");
      restart_for_next_poll (self, ssm, dev, "no finger");
      return;
    }

  /* ---- Finger present ---- */
  /* If they haven't lifted yet... */
  if (self->waiting_for_lift)
    {
      fp_dbg ("Finger still on sensor; waiting for lift before next turn");
      restart_for_next_poll (self, ssm, dev, "waiting for lift");
      return;
    }

  if (!self->turn_open)
    {
      self->finger_first_detected_time = g_get_monotonic_time ();
      self->turn_open = TRUE;
      report_finger_status (self, TRUE, "finger detected");
    }

  {
    gint64 elapsed = g_get_monotonic_time () - self->finger_first_detected_time;

    /* Turn timeout: fail, require lift before next attempt. */
    if (elapsed > EGIS0577_TURN_TIMEOUT_MS * 1000)
      {
        fp_warn ("Turn timed out after %lld ms; waiting for lift before retry",
                 (long long) (elapsed / 1000));
        self->turn_open = FALSE;
        self->capture_armed = FALSE;
        self->waiting_for_lift = TRUE;
        report_finger_status (self, FALSE, "turn timeout");
        restart_for_next_poll (self, ssm, dev, "turn timeout");
        return;
      }

    /* Wait for a lift before arming for the FIRST capture */
    if (!self->capture_armed)
      {
        fp_dbg ("Capture not armed; ignoring finger frame");
        restart_for_next_poll (self, ssm, dev, "not armed");
        return;
      }

    /* Settle window: finger just landed, let the press stabilise. */
    if (elapsed < EGIS0577_FINGER_SETTLE_MS * 1000)
      {
        fp_dbg ("Finger settling (%lld ms / %d ms)", (long long) (elapsed / 1000), EGIS0577_FINGER_SETTLE_MS);
        restart_for_next_poll (self, ssm, dev, "finger settling");
        return;
      }

    /* Past settle, within turn window: evaluate quality. */
    {
      g_autoptr(FpImage) resized = NULL;
      guint grain_pct_x1000 = 0;
      guint ridge_pixels = 0;
      guint minutiae = 0;
      guint stretch_p5 = 0;
      guint stretch_p99 = 0;
      gboolean quality_ok;

      resized = create_processed_snapshot (self, transfer);

      /* stage2_snapshot_quality_ok normalises and applies stretch5 in-place. */
      quality_ok = stage2_snapshot_quality_ok (self, resized,
                                               &grain_pct_x1000,
                                               &ridge_pixels,
                                               &minutiae,
                                               &stretch_p5,
                                               &stretch_p99);

      fp_warn ("Stage-2 at %lld ms: grain=%u.%03u%%/<%u.%03u%% ridge=%u/>%u minutiae=%u (%u..%u) => %s",
               (long long) (elapsed / 1000),
               grain_pct_x1000 / 1000, grain_pct_x1000 % 1000,
               EGIS0577_STAGE2_GRAIN_PCT_X1000 / 1000, EGIS0577_STAGE2_GRAIN_PCT_X1000 % 1000,
               ridge_pixels, EGIS0577_STAGE2_MIN_RIDGE_PIXELS,
               minutiae, EGIS0577_STAGE2_MIN_MINUTIAE, EGIS0577_STAGE2_MAX_MINUTIAE,
               quality_ok ? "accept" : "retry");

      maybe_write_live_frame_pgm (resized);

      if (quality_ok)
        {
          fp_dbg ("Frame accepted at %lld ms", (long long) (elapsed / 1000));
          self->turn_open = FALSE;
          self->capture_armed = FALSE;
          /* resized is already normalised and stretch-enhanced by stage2. */
          on_frame_accepted (dev, g_steal_pointer (&resized));

          if (self->stop)
            fpi_ssm_jump_to_state (ssm, SM_DONE);
          else
            restart_capture_cycle (self, ssm, dev, "post-accept poll", EGIS0577_POST_CAPTURE_POLL_DELAY_MS);
          return;
        }
    }

    restart_for_next_poll (self, ssm, dev, "quality gate pending");
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
          restart_capture_cycle (self, transfer->ssm, dev, "timeout recovery", 0);
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
           * transfer->buffer. save_img evaluates it and either accepts it or
           * recycles the claim for another poll. */
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

  fpi_ssm_jump_to_state (transfer->ssm, SM_REQ);
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

              /* Claim-recycle retries exhausted; the device is transport-wedged.
               * Fail cleanly — the reliable recovery is unplug/replug. */
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
          restart_capture_cycle (self, transfer->ssm, dev, "timeout recovery", 0);
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

  if (first_preinit && self->startup_timeout_retries > 0)
    {
      fp_dbg ("Recovered startup after %u claim retr%s",
              self->startup_timeout_retries,
              self->startup_timeout_retries == 1 ? "y" : "ies");
      self->startup_timeout_retries = 0;
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

      clear_capture_frame (self);
      fp_dbg ("Initial packet array: %s (claim_frames=%u/%u, stage2: grain<%u.%03u%% minutiae=%u..%u ridge>%u)",
              packet_array_name (self->pkt_array),
              self->frame_reads_this_claim,
              EGIS0577_MAX_FRAMES_PER_CLAIM,
              EGIS0577_STAGE2_GRAIN_PCT_X1000 / 1000,
              EGIS0577_STAGE2_GRAIN_PCT_X1000 % 1000,
              EGIS0577_STAGE2_MIN_MINUTIAE,
              EGIS0577_STAGE2_MAX_MINUTIAE,
              EGIS0577_STAGE2_MIN_RIDGE_PIXELS);
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

      send_req (ssm, dev, &self->pkt_array[self->current_index]);
      break;

    case SM_RESP:
      fp_dbg ("SSM_RESP waiting on %s[%d/%d]",
              packet_array_name (self->pkt_array),
              self->current_index,
              self->pkt_array_len);
      recv_resp (ssm, dev, self->pkt_array[self->current_index].response_length);
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
  GCancellable *cancellable = NULL;
  g_autoptr(GError) cancelled = NULL;

  self->running = FALSE;

  if (error)
    {
      fp_dbg ("Capture loop completed with error: %s", error->message);
      fpi_device_action_error (dev, error);
      return;
    }

  if (fpi_device_get_current_action (dev) != FPI_DEVICE_ACTION_NONE)
    cancellable = fpi_device_get_cancellable (dev);

  if (cancellable && g_cancellable_set_error_if_cancelled (cancellable, &cancelled))
    {
      fp_dbg ("Capture loop completed after cancellation");
      fpi_device_action_error (dev, g_steal_pointer (&cancelled));
      return;
    }

  fp_dbg ("Capture loop completed cleanly");
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
  self->waiting_for_lift = FALSE;
  self->frame_counter = 0;
  self->pgm_debug_counter = 0;
  self->pgm_debug_last_capture_time = 0;
  self->frame_reads_this_claim = 0;
  self->turn_open = FALSE;
  self->has_pre_init_run = FALSE;
  self->startup_timeout_retries = 0;
  self->background_warmup_remaining = EGIS0577_BACKGROUND_WARMUP_FRAMES;
  clear_capture_frame (self);
  clear_background (self);
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
dev_cancel (FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  fp_dbg ("Cancel requested (running=%d stop=%d)", self->running, self->stop);
  self->stop = TRUE;
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

  /* Keep open lightweight; startup hardening happens via a short settle delay
   * before the first pre-init packet and claim-recycle on timeout. */
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
  dev_class->cancel = dev_cancel;
  dev_class->enroll = dev_enroll;
  dev_class->verify = dev_verify;
  dev_class->capture = dev_capture;

  fpi_device_class_auto_initialize_features (dev_class);
  dev_class->features &= ~FP_DEVICE_FEATURE_IDENTIFY;
}
