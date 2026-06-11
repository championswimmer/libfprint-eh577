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

  GSList       *strips;
  gsize         strips_len;

  const Packet *pkt_array;
  int           pkt_array_len;
  int           current_index;
  guint         frame_counter;
  guint         pre_frame_delay_ms;
  guint         poll_loop_delay_ms;
  gboolean      frame_delay_armed;

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

static unsigned char
egis_get_pixel (struct fpi_frame_asmbl_ctx *ctx, struct fpi_frame *frame, unsigned int x, unsigned int y)
{
  return frame->data[x + y * ctx->frame_width];
}

static struct fpi_frame_asmbl_ctx assembling_ctx = {
  .frame_width = EGIS0577_IMGWIDTH,
  .frame_height = EGIS0577_RFMGHEIGHT,
  .image_width = (EGIS0577_IMGWIDTH / 3) * 4,   /* PIXMAN expects width/stride to be multiple of 4 */
  .get_pixel = egis_get_pixel,
};

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

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

static gboolean
valid_data (FpiUsbTransfer *transfer)
{
  return count_nonzero_bytes (transfer) > 0;
}

static gboolean
finger_present (FpiUsbTransfer *transfer)
{
  gsize nonzero = count_nonzero_bytes (transfer);

  fp_dbg ("finger_present: nonzero=%zu threshold=%d", nonzero, EGIS0577_MIN_ACTIVE_PIXELS);
  return nonzero >= EGIS0577_MIN_ACTIVE_PIXELS;
}

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

static void
save_img (FpiUsbTransfer *transfer, FpDevice *dev)
{
  FpImageDevice *img_self = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);
  gsize nonzero = count_nonzero_bytes (transfer);
  gboolean has_valid_data = valid_data (transfer);
  gboolean detected_finger = FALSE;

  fp_dbg ("Frame received from %s[%d]: len=%zu nonzero=%zu strips=%zu stop=%d",
          packet_array_name (self->pkt_array),
          self->current_index,
          transfer->actual_length,
          nonzero,
          self->strips_len,
          self->stop);

  dump_frame_if_requested (self, transfer, nonzero);

  /*
   * EH577 idle captures are often all-zero, including the first 5356-byte
   * post-init frame when no finger is present. Treat that as "no finger yet"
   * rather than as a fatal transport/data error.
   */
  if (!has_valid_data)
    {
      fp_dbg ("Zero frame seen; %s",
              self->strips_len > 0 ? "processing previously collected strips" : "continuing polling loop");

      if (self->stop)
        {
          fp_dbg ("Stop requested while handling zero frame");
          fpi_ssm_jump_to_state (transfer->ssm, SM_DONE);
          g_slist_free_full (self->strips, g_free);
          self->strips_len = 0;
          self->strips = NULL;
          return;
        }

      if (self->strips_len > 0)
        goto START_PROCESSING;

      report_finger_status (self, img_self, FALSE, "all-zero frame");
      jump_to_init_with_optional_delay (self, transfer->ssm, "all-zero frame");
      return;
    }

  if (self->stop)
    {
      fp_dbg ("Stop requested while handling non-zero frame");
      fpi_ssm_jump_to_state (transfer->ssm, SM_DONE);
      g_slist_free_full (self->strips, g_free);
      self->strips_len = 0;
      self->strips = NULL;
      return;
    }

  detected_finger = finger_present (transfer);
  fp_dbg ("Finger heuristic for current frame: %s", detected_finger ? "present" : "absent");

  if (!detected_finger)
    {
      if (self->strips_len >= EGIS0577_MIN_STRIPS_FOR_MATCH)
        {
          fp_dbg ("Finger no longer detected after %zu strips, processing image", self->strips_len);
          goto START_PROCESSING;
        }
      else if (self->strips_len > 0)
        {
          fp_dbg ("Only %zu strip(s) collected (need %d), discarding spurious capture",
                  self->strips_len, EGIS0577_MIN_STRIPS_FOR_MATCH);
          g_slist_free_full (self->strips, g_free);
          self->strips = NULL;
          self->strips_len = 0;
          report_finger_status (self, img_self, FALSE, "too few strips — spurious frame discarded");
        }
      else
        {
          report_finger_status (self, img_self, FALSE, "non-zero frame below finger heuristic threshold");
        }
    }
  else
    {
      struct fpi_frame *stripe = g_malloc (EGIS0577_IMGWIDTH * EGIS0577_RFMGHEIGHT + sizeof (struct fpi_frame));
      stripe->delta_x = 0;
      stripe->delta_y = 0;
      memcpy (stripe->data, (transfer->buffer) + (EGIS0577_IMGWIDTH * EGIS0577_RFMDIS), EGIS0577_IMGWIDTH * EGIS0577_RFMGHEIGHT);
      self->strips = g_slist_prepend (self->strips, stripe);
      self->strips_len += 1;

      report_finger_status (self, img_self, TRUE, "active pixel count exceeded threshold");
      fp_dbg ("Appended strip %zu/%d", self->strips_len, EGIS0577_CONSECUTIVE_CAPTURES);
    }

  if (self->strips_len < EGIS0577_CONSECUTIVE_CAPTURES)
    {
      fp_dbg ("Continuing polling loop with %zu strips buffered", self->strips_len);
      jump_to_init_with_optional_delay (self, transfer->ssm, "collecting more strips");
    }
  else
START_PROCESSING:
    {
      fp_dbg ("Enough data collected, moving to image processing with %zu strips", self->strips_len);
      fpi_ssm_next_state (transfer->ssm);
    }
}

static void
process_imgs (FpiSsm *ssm, FpDevice *dev)
{
  FpImageDevice *img_self = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  FpiImageDeviceState state;

  report_finger_status (self, img_self, TRUE, "processing buffered strips");

  g_object_get (dev, "fpi-image-device-state", &state, NULL);
  fp_dbg ("Processing %zu strips while image-device state=%d", self->strips_len, state);
  if (state == FPI_IMAGE_DEVICE_STATE_CAPTURE)
    {
      if (!self->stop)
        {
          g_autoptr(FpImage) img = NULL;

          self->strips = g_slist_reverse (self->strips);
          fpi_do_movement_estimation (&assembling_ctx, self->strips);

          img = fpi_assemble_frames (&assembling_ctx, self->strips);
          img->flags |= (FPI_IMAGE_COLORS_INVERTED | FPI_IMAGE_PARTIAL);

          FpImage *resizedImage = fpi_image_resize (img, EGIS0577_RESIZE, EGIS0577_RESIZE);

          fp_dbg ("Submitting assembled image from %zu strips", self->strips_len);
          fpi_image_device_image_captured (img_self, resizedImage);
        }

      g_slist_free_full (self->strips, g_free);
      self->strips = NULL;
      self->strips_len = 0;

      /* Report finger-off and wait EGIS0577_INTER_STAGE_DELAY_MS before the
       * next stage.  The delay gives the user time to lift their finger and
       * resets the sensor via SM_INIT (PRE_INIT → POST_INIT), which clears the
       * AGC shift that makes idle frames look like partial touches. */
      report_finger_status (self, img_self, FALSE, "image submitted — inter-stage gap");
      fp_dbg ("Image submitted; pausing %d ms then resetting for next stage",
              EGIS0577_INTER_STAGE_DELAY_MS);
      fpi_ssm_jump_to_state_delayed (ssm, SM_INIT, EGIS0577_INTER_STAGE_DELAY_MS);
    }
  else
    {
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
      fp_dbg ("Error occurred at index %d of %s array",
              self->current_index,
              packet_array_name (self->pkt_array));
      fpi_ssm_mark_failed (transfer->ssm, error);

      g_slist_free_full (self->strips, g_free);
      self->strips_len = 0;
      self->strips = NULL;
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
          /* Post-init complete: frame (5356 bytes) is in transfer->buffer.
           * save_img decides whether to jump to SM_INIT for another cycle
           * (zero/sub-threshold frame, or collecting more strips) or to
           * SM_PROCESS_IMG when enough strips are assembled. */
          fp_dbg ("Completed post-init sequence, passing frame to save_img");
          self->current_index = 0;
          save_img (transfer, dev);
          return;
        }
      else
        {
          /* Pre-init complete — switch to post-init for the frame capture. */
          fp_dbg ("Completed pre-init sequence, switching to post-init");
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

  jump_to_init_with_optional_delay (self, transfer->ssm, "advance packet sequence");
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
      self->pkt_array = EGIS0577_PRE_INIT_PACKETS;
      self->pkt_array_len = EGIS0577_PRE_INIT_PACKETS_LENGTH;
      self->current_index = 0;
      self->finger_reported = FALSE;
      self->frame_counter = 0;
      self->pre_frame_delay_ms = get_env_ms_or_default ("EGIS0577_PRE_FRAME_DELAY_MS", 0);
      self->poll_loop_delay_ms = get_env_ms_or_default ("EGIS0577_POLL_LOOP_DELAY_MS", 0);
      self->frame_delay_armed = FALSE;

      /* Do NOT clear strips here — they must survive across reinit cycles when
       * we are mid-collection (collecting CONSECUTIVE_CAPTURES strips, each
       * requiring a full PRE_INIT → POST_INIT reset to get a fresh frame).
       * Strips are freed by: save_img stop paths, and process_imgs after submit. */
      fp_dbg ("Initial packet array: %s", packet_array_name (self->pkt_array));
      fp_dbg ("EH577 pacing config: pre_frame_delay_ms=%u poll_loop_delay_ms=%u",
              self->pre_frame_delay_ms,
              self->poll_loop_delay_ms);
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

  fp_dbg ("Deactivate requested, running=%d", self->running);
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
  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;
  dev_class->nr_enroll_stages = 10;

  img_class->img_open = dev_init;
  img_class->img_close = dev_deinit;
  img_class->activate = dev_start;
  img_class->deactivate = dev_stop;

  img_class->img_width = EGIS0577_IMGWIDTH;
  img_class->img_height = -1;

  img_class->bz3_threshold = EGIS0577_BZ3_THRESHOLD;
}
