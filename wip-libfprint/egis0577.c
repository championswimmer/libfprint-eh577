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
  if (pkt_array == EGIS0577_REPEAT_PACKETS)
    return "repeat";
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

static guint
get_env_ms_or_default (const char *name,
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

      if (self->finger_reported && g_get_monotonic_time () - self->finger_first_detected_time < 200 * 1000)
        {
          fp_dbg ("Ignoring zero frame during 200ms settle time");
          restart_for_next_poll (self, transfer->ssm, dev, "zero frame during settle");
          return;
        }

      if (!self->capture_armed)
        fp_dbg ("Capture armed after clean zero frame");
      self->capture_armed = TRUE;

      if (!self->background && count_finger_pixels_raw (transfer) < 200)
        {
          fp_dbg ("Capturing clean background for session");
          self->background = g_malloc0 (transfer->actual_length);
          memcpy (self->background, transfer->buffer, transfer->actual_length);
        }

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
      if (self->finger_reported && g_get_monotonic_time () - self->finger_first_detected_time < 200 * 1000)
        {
          fp_dbg ("Ignoring no-finger frame during 200ms settle time");
          restart_for_next_poll (self, transfer->ssm, dev, "no-finger frame during settle");
          return;
        }

      if (!self->capture_armed)
        fp_dbg ("Capture armed after clean no-finger frame (cov=%d%%)", coverage);
      self->capture_armed = TRUE;

      if (!self->background && count_finger_pixels_raw (transfer) < 200)
        {
          fp_dbg ("Capturing clean background for session");
          self->background = g_malloc0 (transfer->actual_length);
          memcpy (self->background, transfer->buffer, transfer->actual_length);
        }

      report_finger_status (self, img_self, FALSE, "non-zero frame below finger heuristic threshold");
      restart_for_next_poll (self, transfer->ssm, dev, "non-zero frame below finger heuristic threshold");
      return;
    }

  g_object_get (dev, "fpi-image-device-state", &state, NULL);
  if (!self->capture_armed &&
      state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      fp_dbg ("Ignoring finger-like startup/transient frame with cov=%d%% until a clean no-finger baseline is observed",
              coverage);
      report_finger_status (self, img_self, FALSE, "ignoring startup transient before capture is armed");
      restart_for_next_poll (self, transfer->ssm, dev, "startup transient before capture is armed");
      return;
    }

  if (!self->finger_reported)
    self->finger_first_detected_time = g_get_monotonic_time ();

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

  if (g_get_monotonic_time () - self->finger_first_detected_time < 200 * 1000)
    {
      fp_dbg ("Finger detected but waiting 200ms to settle");
      restart_for_next_poll (self, transfer->ssm, dev, "waiting for finger to settle");
      return;
    }

  if (!capture_usable (self, transfer))
    {
      fp_dbg ("Finger detected but frame cov=%d%% is below usable threshold, reporting retry",
              coverage);
      self->capture_armed = FALSE;
      fpi_image_device_retry_scan (img_self, FP_DEVICE_RETRY_GENERAL);
      restart_for_next_poll (self, transfer->ssm, dev, "weak snapshot frame");
      return;
    }

  self->capture_armed = FALSE;
  clear_capture_frame (self);
  self->capture_frame = g_memdup2 (transfer->buffer, transfer->actual_length);
  self->capture_nonzero = coverage; // Use coverage for logging later

  fp_dbg ("Accepted snapshot frame with cov=%d%%, moving to image processing", coverage);
  fpi_ssm_next_state (transfer->ssm);
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
      if (!self->stop && self->capture_frame)
        {
          g_autoptr(FpImage) img = NULL;
          FpImage *resizedImage = NULL;
          guint row;

          img = fp_image_new (EGIS0577_PADDED_IMGWIDTH, EGIS0577_IMGHEIGHT);
          img->width = EGIS0577_PADDED_IMGWIDTH;
          img->height = EGIS0577_IMGHEIGHT;
          img->flags = FPI_IMAGE_COLORS_INVERTED;

          for (row = 0; row < EGIS0577_IMGHEIGHT; row++)
            {
              for (guint col = 0; col < EGIS0577_IMGWIDTH; col++)
                {
                  guint8 val = self->capture_frame[row * EGIS0577_IMGWIDTH + col];
                  guint8 bg = self->background ? self->background[row * EGIS0577_IMGWIDTH + col] : 0;
                  if (val > bg + 2)
                    val -= bg;
                  else
                    val = 0;
                  img->data[row * EGIS0577_PADDED_IMGWIDTH + col] = val;
                }
            }

          resizedImage = fpi_image_resize (img, EGIS0577_RESIZE, EGIS0577_RESIZE);

          fp_dbg ("Submitting snapshot image from frame with nonzero=%zu", self->capture_nonzero);
          fpi_image_device_image_captured (img_self, resizedImage);
        }

      clear_capture_frame (self);

      /* Keep libfprint in AWAIT_FINGER_OFF until we observe a real lift on a
       * later poll. Recycle the claim now so those polls happen on a fresh
       * transport session. */
      fp_dbg ("Image submitted; keep waiting for real finger-off and recycle claim");
      restart_capture_cycle (self, ssm, dev, "post-capture await finger-off", self->post_capture_poll_delay_ms);
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
      else if (self->pkt_array == EGIS0577_REPEAT_PACKETS)
        {
          /* Legacy REPEAT path kept for debugging/reference.  The current EH577
           * runtime should not enter it because REPEAT burns the same large-read
           * budget as a real frame capture. */
          fp_dbg ("Completed repeat flush, restarting post-init");
          self->pkt_array = EGIS0577_POST_INIT_PACKETS;
          self->pkt_array_len = EGIS0577_POST_INIT_PACKETS_LENGTH;
          self->current_index = 0;
          jump_to_req_with_optional_delay (self, transfer->ssm, "post-repeat restart");
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
      self->frame_delay_armed = FALSE;

      clear_capture_frame (self);
      fp_dbg ("Initial packet array: %s", packet_array_name (self->pkt_array));
      fp_dbg ("EH577 pacing config: pre_frame_delay_ms=%u poll_loop_delay_ms=%u no_finger_retry_delay_ms=%u post_capture_poll_delay_ms=%u max_frames_per_claim=%u current_claim_frames=%u",
              self->pre_frame_delay_ms,
              self->poll_loop_delay_ms,
              self->no_finger_retry_delay_ms,
              self->post_capture_poll_delay_ms,
              self->max_frames_per_claim,
              self->frame_reads_this_claim);
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
  self->frame_counter = 0;
  self->frame_reads_this_claim = 0;

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
  dev_class->nr_enroll_stages = 15;
  dev_class->temp_hot_seconds = -1;

  img_class->img_open = dev_init;
  img_class->img_close = dev_deinit;
  img_class->activate = dev_start;
  img_class->deactivate = dev_stop;

  img_class->img_width = EGIS0577_PADDED_IMGWIDTH * EGIS0577_RESIZE;
  img_class->img_height = EGIS0577_IMGHEIGHT * EGIS0577_RESIZE;

  img_class->bz3_threshold = EGIS0577_BZ3_THRESHOLD;
}
