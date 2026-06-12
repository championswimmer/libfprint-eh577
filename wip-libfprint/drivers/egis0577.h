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

#pragma once

/*
 * Device data
 */

#define EGIS0577_CONFIGURATION 1
#define EGIS0577_INTERFACE 0

/*
 * Device endpoints
 */

#define EGIS0577_EPOUT 0x01 /* ( 1 | FPI_USB_ENDPOINT_OUT ) */
#define EGIS0577_EPIN 0x82  /* ( 2 | FPI_USB_ENDPOINT_IN ) */

/*
 * Image polling sequences
 *
 * First 4 bytes of packet to be sent is "EGIS", rest are unknown but a specific pattern was observed.
 * First 4 bytes of response is "SIGE".
 *
 * This EH577 draft currently reuses the EH575 sequence tables verbatim.
 * That is intentional as a starting point: real EH577 hardware has already accepted
 * the early EH575 post-init packets on the wire.
 *
 * Important nuance:
 * - on real EH577 hardware, `EGIS 60 01 fc` has returned `SIGE 01 01 01`,
 *   `SIGE 01 05 01`, and `SIGE 01 00 01`
 * - the strongest successful capture evidence so far came from the post-init-led
 *   snapshot capture path
 * - EH577 also accepts the EH575 PRE_INIT family, but its PRE_INIT payload
 *   command `73 14 ec` has so far returned only a short status reply rather
 *   than the meaningful 5356-byte frame seen on post-init `64 14 ec`
 * - because of that, this EH577 draft currently prefers staying on the
 *   post-init capture path even when `60 01 fc` returns `01 01 01`
 */

typedef struct Packet
{
  int            length;
  unsigned char *sequence;

  int            response_length;
} Packet;

#define EGIS0577_PRE_INIT_PACKETS_LENGTH 29
static const Packet EGIS0577_PRE_INIT_PACKETS[] = {
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfd}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x35, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x80, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x80, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfc}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x01, 0x02, 0x0f, 0x03}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x22}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x09, 0x83}, .response_length = 7},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06}, .response_length = 13},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xf4}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x44}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x50, 0x03}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x50, 0x03}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x73, 0x14, 0xec}, .response_length = 7},  /* returns 7 bytes in pre-init context, not 5356 */
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x40, 0xec}, .response_length = 7},
  {.length = 18, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x09, 0x0b, 0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12}, .response_length = 18},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06}, .response_length = 13},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x23, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x24, 0x33}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x20, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x21, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x40, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x22}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0b, 0x03}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x0a, 0xfc}, .response_length = 7},   /* to EGIS0577_POST_INIT_PACKETS */
};

#define EGIS0577_POST_INIT_PACKETS_LENGTH 18
static const Packet EGIS0577_POST_INIT_PACKETS[] = {
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0xfc}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0xfc}, .response_length = 7},   /* EH577 currently stays on post-init even if response is exact SIGE 01 01 01 */
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x40, 0xfc}, .response_length = 7},
  {.length = 18, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x09, 0x0b, 0x83, 0x24, 0x00, 0x44, 0x0f, 0x08, 0x20, 0x20, 0x01, 0x05, 0x12}, .response_length = 18},
  {.length = 13, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05, 0x2f, 0x06}, .response_length = 13},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x23, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x24, 0x33}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x20, 0x00}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x21, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x66}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0x66}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x57}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x2d, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x62, 0x67, 0x03}, .response_length = 10},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x0f, 0x03}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x13}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x64, 0x14, 0xec}, .response_length = 5356},   /* to EGIS0577_REPEAT_PACKETS */
};

#define EGIS0577_REPEAT_PACKETS_LENGTH 9
static const Packet EGIS0577_REPEAT_PACKETS[] = {
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x61, 0x2d, 0x20}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x20}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0x20}, .response_length = 7},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x57}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x2d, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x62, 0x67, 0x03}, .response_length = 10},
  {.length = 9, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x13}, .response_length = 9},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x02}, .response_length = 7},
  {.length = 7, .sequence = (unsigned char[]){0x45, 0x47, 0x49, 0x53, 0x64, 0x14, 0xec}, .response_length = 5356},
};

#define EGIS0577_IMGWIDTH 103
#define EGIS0577_IMGHEIGHT 52
#define EGIS0577_IMGSIZE (EGIS0577_IMGWIDTH * EGIS0577_IMGHEIGHT)
/*
 * pixman's A8 helpers used by fpi_image_resize() require rowstride alignment
 * to 32 bits, so the snapshot image must be padded before resizing.
 */
#define EGIS0577_PADDED_IMGWIDTH (((EGIS0577_IMGWIDTH + 3) / 4) * 4)

#define EGIS0577_BZ3_THRESHOLD 40
#define EGIS0577_RESIZE 2

/*
 * Minimum number of nonzero pixels required to classify a frame as finger-present.
 *
 * Measured on real hardware (2026-06-11):
 *   Idle (no finger, cold):         ~111 nonzero pixels at value ~190 (hot pixels).
 *   Post-capture AGC shift (no finger): 519–755 nonzero (sensor AGC warms up; may
 *                                       persist even after SM_INIT reset in some runs).
 *   Real finger:                    1305–1594 nonzero pixels at value 1–105.
 *
 * For the snapshot path we distinguish:
 * - likely finger contact
 * - good enough frame to submit upstream
 */
#define EGIS0577_MIN_ACTIVE_PIXELS_PRESENT 700
#define EGIS0577_MIN_ACTIVE_PIXELS_STRICT 1000
/*
 * Successful commands complete well below 1 s. Keep timeout recovery tight so
 * a wedged claim does not stall stage transitions for 10 seconds.
 */
#define EGIS0577_TIMEOUT 2000

/*
 * Keep missed-touch retries responsive: poll again immediately within the same
 * claim until the claim is near its observed large-read limit.
 */
#define EGIS0577_NO_FINGER_RETRY_DELAY_MS 0

/*
 * Milliseconds to pause after submitting a good image before restarting
 * polling to observe the real finger-off transition.
 */
#define EGIS0577_POST_CAPTURE_POLL_DELAY_MS 200

/*
 * EH577 appears to allow only a small number of 64 14 ec frame reads per
 * interface claim before transport starts wedging. Stay below that ceiling and
 * recycle the claim proactively after a handful of missed-touch polls.
 */
#define EGIS0577_MAX_FRAMES_PER_CLAIM 6
