/*
 * Egis Technology Inc. (aka. LighTuning) 0577 driver for libfprint
 * Copyright (C) 2021 Animesh Sahu <animeshsahu19@yahoo.com>
 * Local EH577 adaptation work (C) 2026 workspace contributors
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

  GSList       *strips;
  gsize         strips_len;

  const Packet *pkt_array;
  int           pkt_array_len;
  int           current_index;
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

/*
 * ==================== Data processing ====================
 */

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

static gboolean
valid_data (FpiUsbTransfer *transfer)
{
  int sum = 0;

  for (size_t i = 0; i < MIN (100, transfer->actual_length); i++)
    sum |= transfer->buffer[i];
  return sum;
}

static gboolean
finger_present (FpiUsbTransfer *transfer)
{
  unsigned char *buffer = transfer->buffer;
  int length = transfer->actual_length;
  double mean = 0;
  double variance = 0;

  for (size_t i = 0; i < length; i++)
    mean += buffer[i];
  mean /= length;

  for (size_t i = 0; i < length; i++)
    variance += (buffer[i] - mean) * (buffer[i] - mean);
  variance /= length;

  return variance > EGIS0577_MIN_SD * EGIS0577_MIN_SD;
}

static void
save_img (FpiUsbTransfer *transfer, FpDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  if (!valid_data (transfer))
    {
      GError *error = fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID, "All zero data received!");
      fpi_ssm_mark_failed (transfer->ssm, error);
      g_error_free (error);
      goto CLEANUP;
    }

  if (self->stop)
    {
      fpi_ssm_jump_to_state (transfer->ssm, SM_DONE);
CLEANUP:
      g_slist_free_full (self->strips, g_free);
      self->strips_len = 0;
      self->strips = NULL;
      return;
    }

  if (!finger_present (transfer))
    {
      if (self->strips_len > 0)
        goto START_PROCESSING;
    }
  else
    {
      struct fpi_frame *stripe = g_malloc (EGIS0577_IMGWIDTH * EGIS0577_RFMGHEIGHT + sizeof (struct fpi_frame));
      stripe->delta_x = 0;
      stripe->delta_y = 0;
      memcpy (stripe->data, (transfer->buffer) + (EGIS0577_IMGWIDTH * EGIS0577_RFMDIS), EGIS0577_IMGWIDTH * EGIS0577_RFMGHEIGHT);
      self->strips = g_slist_prepend (self->strips, stripe);
      self->strips_len += 1;
    }

  if (self->strips_len < EGIS0577_CONSECUTIVE_CAPTURES)
    fpi_ssm_jump_to_state (transfer->ssm, SM_REQ);
  else
START_PROCESSING:
    fpi_ssm_next_state (transfer->ssm);
}

static void
process_imgs (FpiSsm *ssm, FpDevice *dev)
{
  FpImageDevice *img_self = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

  FpiImageDeviceState state;

  fpi_image_device_report_finger_status (img_self, TRUE);

  g_object_get (dev, "fpi-image-device-state", &state, NULL);
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

          fpi_image_device_image_captured (img_self, resizedImage);
        }

      g_slist_free_full (self->strips, g_free);
      self->strips = NULL;
      self->strips_len = 0;

      fpi_image_device_report_finger_status (img_self, FALSE);
      fpi_ssm_next_state (ssm);
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
      const char *array_name = "pre-init";
      if (self->pkt_array == EGIS0577_POST_INIT_PACKETS)
        array_name = "post-init";
      else if (self->pkt_array == EGIS0577_REPEAT_PACKETS)
        array_name = "repeat";

      fp_dbg ("Error occurred at index %d of %s array", self->current_index, array_name);
      fpi_ssm_mark_failed (transfer->ssm, error);

      g_slist_free_full (self->strips, g_free);
      self->strips_len = 0;
      self->strips = NULL;
      return;
    }

  if (self->current_index == self->pkt_array_len - 1)
    {
      if (self->pkt_array == EGIS0577_REPEAT_PACKETS || self->pkt_array == EGIS0577_POST_INIT_PACKETS)
        {
          self->pkt_array = EGIS0577_REPEAT_PACKETS;
          self->pkt_array_len = EGIS0577_REPEAT_PACKETS_LENGTH;
          self->current_index = 0;

          save_img (transfer, dev);
          return;
        }
      else
        {
          self->pkt_array = EGIS0577_POST_INIT_PACKETS;
          self->pkt_array_len = EGIS0577_POST_INIT_PACKETS_LENGTH;
          self->current_index = 0;
        }
    }
  else if (self->pkt_array == EGIS0577_POST_INIT_PACKETS && self->current_index == 1 && transfer->buffer[5] == 0x01)
    {
      fp_dbg ("Pre initialization required, switching to pre-init packets");
      self->pkt_array = EGIS0577_PRE_INIT_PACKETS;
      self->pkt_array_len = EGIS0577_PRE_INIT_PACKETS_LENGTH;
      self->current_index = 0;
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

  fpi_usb_transfer_fill_bulk (transfer, EGIS0577_EPIN, response_length);

  transfer->ssm = ssm;

  fpi_usb_transfer_submit (transfer, EGIS0577_TIMEOUT, NULL, resp_cb, NULL);
}

static void
send_req (FpiSsm *ssm, FpDevice *dev, const Packet *pkt)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

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
      self->pkt_array = EGIS0577_POST_INIT_PACKETS;
      self->pkt_array_len = EGIS0577_POST_INIT_PACKETS_LENGTH;
      self->current_index = 0;

      self->strips_len = 0;
      self->strips = NULL;
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
      send_req (ssm, dev, &self->pkt_array[self->current_index]);
      break;

    case SM_RESP:
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
    fpi_image_device_session_error (img_dev, error);
}

/*
 * ==================== Top-level command callback & meta-data ====================
 */

static void
dev_init (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);

  fpi_image_device_open_complete (dev, error);
}

static void
dev_deinit (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);

  fpi_image_device_close_complete (dev, error);
}

static void
dev_stop (FpImageDevice *dev)
{
  FpDeviceEgis0577 *self = FPI_DEVICE_EGIS0577 (dev);

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

  img_class->img_open = dev_init;
  img_class->img_close = dev_deinit;
  img_class->activate = dev_start;
  img_class->deactivate = dev_stop;

  img_class->img_width = EGIS0577_IMGWIDTH;
  img_class->img_height = -1;

  img_class->bz3_threshold = EGIS0577_BZ3_THRESHOLD;
}
