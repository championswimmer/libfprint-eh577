/*
 * EH577 multi-capture helper: capture N images (one per finger touch) and save
 * each as a numbered PGM, using the public fp_device_capture() path so the saved
 * images are exactly what the driver produces (rolling baseline subtraction +
 * resize) — the same data the matcher would see.
 *
 * Copyright (C) 2026 workspace contributors
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version.
 */

#define FP_COMPONENT "example-eh577-capture-helper"

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <libfprint/fprint.h>
#include <glib-unix.h>

#include "storage.h"
#include "utilities.h"

/* If fd 3 is open (the wrapper script wires it to the terminal), write
 * user-facing prompts there so stdout/stderr can be redirected to a log
 * file without losing terminal interaction.  Falls back to stdout if fd 3
 * is not available so the helper still works when run directly. */
G_GNUC_PRINTF (1, 2)
static void
capture_message (const char *fmt, ...)
{
  va_list args;

  va_start (args, fmt);
  if (fcntl (3, F_GETFD) != -1)
    vdprintf (3, fmt, args);
  else
    g_vprintf (fmt, args);
  va_end (args);
}

typedef struct CaptureData
{
  GMainLoop    *loop;
  GCancellable *cancellable;
  unsigned int  sigint_handler;
  gint          total;
  gint          done;
  gchar        *dir;
  gint          ret_value;
} CaptureData;

static void
capture_data_free (CaptureData *cd)
{
  g_clear_handle_id (&cd->sigint_handler, g_source_remove);
  g_clear_object (&cd->cancellable);
  g_clear_pointer (&cd->dir, g_free);
  g_main_loop_unref (cd->loop);
  g_free (cd);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CaptureData, capture_data_free)

static gboolean
verbose_finger_status_enabled (void)
{
  const char *value = g_getenv ("EH577_CAPTURE_VERBOSE_STATUS");

  return value && value[0] && g_strcmp0 (value, "0") != 0;
}

static void
on_finger_status_changed (FpDevice *dev, GParamSpec *pspec, gpointer user_data)
{
  FpFingerStatusFlags status;
  const char *s;

  if (!verbose_finger_status_enabled ())
    return;

  status = fp_device_get_finger_status (dev);
  s = (status & FP_FINGER_STATUS_PRESENT) ? "present" :
      (status & FP_FINGER_STATUS_NEEDED) ? "needed" : "none";

  capture_message ("EH577_CAPTURE finger-status status=%s\n", s);
}

static void
on_device_closed (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  CaptureData *cd = user_data;

  g_autoptr(GError) error = NULL;

  fp_device_close_finish (dev, res, &error);
  if (error)
    g_warning ("Failed closing device %s", error->message);

  g_main_loop_quit (cd->loop);
}

static void
capture_quit (FpDevice *dev, CaptureData *cd)
{
  if (!fp_device_is_open (dev))
    {
      g_main_loop_quit (cd->loop);
      return;
    }
  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_device_closed, cd);
}

static void start_next_capture (FpDevice *dev, CaptureData *cd);

typedef struct RetryRestart
{
  FpDevice    *dev;
  CaptureData *cd;
} RetryRestart;

static void
retry_restart_free (RetryRestart *rr)
{
  g_clear_object (&rr->dev);
  g_free (rr);
}

static gboolean
retry_after_lift_cb (gpointer user_data)
{
  RetryRestart *rr = user_data;

  start_next_capture (rr->dev, rr->cd);
  return G_SOURCE_REMOVE;
}

static void
schedule_retry_after_lift (FpDevice *dev, CaptureData *cd)
{
  RetryRestart *rr = g_new0 (RetryRestart, 1);

  rr->dev = g_object_ref (dev);
  rr->cd = cd;

  capture_message ("EH577_CAPTURE lift your finger before retry\n");
  g_timeout_add_full (G_PRIORITY_DEFAULT,
                      1500,
                      retry_after_lift_cb,
                      rr,
                      (GDestroyNotify) retry_restart_free);
}

static void
dev_capture_cb (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  CaptureData *cd = user_data;

  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;

  image = fp_device_capture_finish (dev, res, &error);

  if (!image)
    {
      /* Retryable scan errors: re-issue the same touch instead of advancing. */
      if (error && error->domain == FP_DEVICE_RETRY)
        {
          capture_message ("EH577_CAPTURE retry message=%s\n", error->message);
          schedule_retry_after_lift (dev, cd);
          return;
        }

      capture_message ("EH577_CAPTURE error message=%s\n", error ? error->message : "unknown");
      cd->ret_value = EXIT_FAILURE;
      capture_quit (dev, cd);
      return;
    }

  cd->done += 1;
  {
    g_autofree gchar *base = g_strdup_printf ("capture-%02d.pgm", cd->done);
    g_autofree gchar *path = g_build_filename (cd->dir, base, NULL);
    if (save_image_to_pgm (image, path))
      capture_message ("EH577_CAPTURE saved %d/%d path=%s\n", cd->done, cd->total, path);
    else
      capture_message ("EH577_CAPTURE save-failed %d/%d path=%s\n", cd->done, cd->total, path);
  }

  if (cd->done < cd->total)
    {
      capture_message ("EH577_CAPTURE lift your finger\n");
      start_next_capture (dev, cd);
    }
  else
    {
      capture_message ("EH577_CAPTURE complete count=%d\n", cd->done);
      cd->ret_value = EXIT_SUCCESS;
      capture_quit (dev, cd);
    }
}

static void
start_next_capture (FpDevice *dev, CaptureData *cd)
{
  capture_message ("EH577_CAPTURE touch %d/%d — press and hold\n", cd->done + 1, cd->total);
  fp_device_capture (dev, TRUE, cd->cancellable,
                     (GAsyncReadyCallback) dev_capture_cb, cd);
}

static void
on_device_opened (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  CaptureData *cd = user_data;

  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      capture_message ("EH577_CAPTURE device-open-failed message=%s\n", error->message);
      cd->ret_value = EXIT_FAILURE;
      capture_quit (dev, cd);
      return;
    }

  capture_message ("EH577_CAPTURE device-opened total=%d\n", cd->total);
  start_next_capture (dev, cd);
}

static gboolean
sigint_cb (void *user_data)
{
  CaptureData *cd = user_data;

  g_cancellable_cancel (cd->cancellable);
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  g_autoptr(FpContext) ctx = NULL;
  g_autoptr(CaptureData) cd = NULL;
  GPtrArray *devices;
  FpDevice *dev;

  /* args: <output-dir> [count] */
  const char *dir = (argc >= 2) ? argv[1] : ".";
  gint total = (argc >= 3) ? atoi (argv[2]) : 12;

  if (total < 1)
    total = 1;

  setbuf (stdout, NULL);

  ctx = fp_context_new ();
  devices = fp_context_get_devices (ctx);
  if (!devices)
    {
      g_printerr ("Impossible to get devices\n");
      return EXIT_FAILURE;
    }

  dev = discover_device (devices);
  if (!dev)
    {
      g_printerr ("No supported device found\n");
      return EXIT_FAILURE;
    }

  if (!fp_device_has_feature (dev, FP_DEVICE_FEATURE_CAPTURE))
    {
      g_printerr ("Device %s does not support capture\n", fp_device_get_name (dev));
      return EXIT_FAILURE;
    }

  cd = g_new0 (CaptureData, 1);
  cd->loop = g_main_loop_new (NULL, FALSE);
  cd->cancellable = g_cancellable_new ();
  cd->total = total;
  cd->done = 0;
  cd->dir = g_strdup (dir);
  cd->ret_value = EXIT_FAILURE;
  cd->sigint_handler = g_unix_signal_add_full (G_PRIORITY_HIGH, SIGINT,
                                               sigint_cb, cd, NULL);

  g_signal_connect (dev, "notify::finger-status",
                    G_CALLBACK (on_finger_status_changed), cd);

  fp_device_open (dev, cd->cancellable,
                  (GAsyncReadyCallback) on_device_opened, cd);
  g_main_loop_run (cd->loop);

  return cd->ret_value;
}
