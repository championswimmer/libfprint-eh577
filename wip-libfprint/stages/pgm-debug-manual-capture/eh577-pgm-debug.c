/*
 * EH577 interactive PGM debug runner.
 *
 * This is intentionally not an enroll/verify/identify flow.  It starts the
 * driver's capture loop in EGIS0577_PGM_DEBUG_* mode, then uses single-key
 * commands to toggle whether the driver saves every processed snapshot it polls.
 *
 * Keys (raw terminal, no Enter):
 *   f  start saving enhanced PGMs + metrics
 *   s  stop saving
 *   x  exit gracefully
 */

#define FP_COMPONENT "example-eh577-pgm-debug"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <libfprint/fprint.h>
#include <glib-unix.h>

#include "storage.h"
#include "utilities.h"

static struct termios g_term_saved;
static gboolean       g_term_is_raw = FALSE;

static void
restore_term (void)
{
  if (g_term_is_raw)
    {
      tcsetattr (STDIN_FILENO, TCSANOW, &g_term_saved);
      g_term_is_raw = FALSE;
    }
}

static void
set_term_raw (void)
{
  struct termios raw;

  tcgetattr (STDIN_FILENO, &g_term_saved);
  raw = g_term_saved;
  raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr (STDIN_FILENO, TCSANOW, &raw);
  g_term_is_raw = TRUE;
}

typedef struct
{
  GMainLoop    *loop;
  GCancellable *cancellable;
  FpDevice     *dev;
  const gchar  *control_path;
  gboolean      capture_active;
  gboolean      quitting;
  gboolean      cancel_requested;
  gint          ret;
} DebugData;

G_GNUC_PRINTF (2, 3)
static void
debug_print (DebugData *dd, const gchar *fmt, ...)
{
  va_list args;
  g_autofree gchar *msg = NULL;
  int ttyfd = (fcntl (3, F_GETFD) != -1) ? 3 : STDERR_FILENO;

  va_start (args, fmt);
  msg = g_strdup_vprintf (fmt, args);
  va_end (args);

  dprintf (ttyfd, "%s\r\n", msg);
}

static void
write_control (DebugData *dd, gboolean active)
{
  g_autoptr(GError) error = NULL;

  if (!dd->control_path || !dd->control_path[0])
    return;

  if (!g_file_set_contents (dd->control_path, active ? "1\n" : "0\n", -1, &error))
    debug_print (dd, "[pgm-debug] failed to write control file: %s", error->message);
}

static void
on_device_closed (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  DebugData *dd = user_data;
  g_autoptr(GError) error = NULL;

  fp_device_close_finish (dev, res, &error);
  if (error)
    debug_print (dd, "[pgm-debug] close error: %s", error->message);

  restore_term ();
  g_main_loop_quit (dd->loop);
}

static void
quit_debug (FpDevice *dev, DebugData *dd)
{
  write_control (dd, FALSE);

  if (!fp_device_is_open (dev))
    {
      restore_term ();
      g_main_loop_quit (dd->loop);
      return;
    }

  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_device_closed, dd);
}

static void start_capture (FpDevice *dev, DebugData *dd);

static void
on_capture_done (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  DebugData *dd = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpImage) image = NULL;

  dd->capture_active = FALSE;
  image = fp_device_capture_finish (dev, res, &error);

  if (dd->quitting)
    {
      quit_debug (dev, dd);
      return;
    }

  if (image)
    {
      debug_print (dd, "[pgm-debug] capture callback returned an image; restarting debug loop");
      start_capture (dev, dd);
      return;
    }

  if (dd->cancel_requested || (error && error->domain == FP_DEVICE_RETRY))
    {
      dd->cancel_requested = FALSE;
      start_capture (dev, dd);
      return;
    }

  debug_print (dd, "[pgm-debug] capture error: %s", error ? error->message : "unknown");
  dd->ret = 1;
  quit_debug (dev, dd);
}

static void
start_capture (FpDevice *dev, DebugData *dd)
{
  g_clear_object (&dd->cancellable);
  dd->cancellable = g_cancellable_new ();
  dd->capture_active = TRUE;
  fp_device_capture (dev, TRUE, dd->cancellable,
                     (GAsyncReadyCallback) on_capture_done, dd);
}

static gboolean
on_key (GIOChannel *src, GIOCondition cond, gpointer user_data)
{
  DebugData *dd = user_data;
  gchar ch;
  gsize nr = 0;

  if (g_io_channel_read_chars (src, &ch, 1, &nr, NULL) != G_IO_STATUS_NORMAL || nr == 0)
    return G_SOURCE_CONTINUE;

  switch (ch)
    {
    case 'f':
      write_control (dd, TRUE);
      debug_print (dd, "fingerprint captures started");
      break;

    case 's':
      write_control (dd, FALSE);
      debug_print (dd, "fingerprint captures stopped");
      break;

    case 'x':
    case '\x03':
      debug_print (dd, "[pgm-debug] exit requested");
      dd->quitting = TRUE;
      dd->ret = 0;
      if (dd->capture_active)
        {
          dd->cancel_requested = TRUE;
          g_cancellable_cancel (dd->cancellable);
        }
      else
        quit_debug (dd->dev, dd);
      break;

    default:
      break;
    }

  return G_SOURCE_CONTINUE;
}

static gboolean
on_sigint (gpointer user_data)
{
  DebugData *dd = user_data;

  if (dd->quitting)
    return G_SOURCE_CONTINUE;

  debug_print (dd, "[pgm-debug] SIGINT — exiting");
  dd->quitting = TRUE;
  dd->ret = 0;
  if (dd->capture_active)
    {
      dd->cancel_requested = TRUE;
      g_cancellable_cancel (dd->cancellable);
    }
  else
    quit_debug (dd->dev, dd);

  return G_SOURCE_CONTINUE;
}

static void
on_device_opened (FpDevice *dev, GAsyncResult *res, gpointer user_data)
{
  DebugData *dd = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      debug_print (dd, "[pgm-debug] open failed: %s", error->message);
      dd->ret = 1;
      restore_term ();
      g_main_loop_quit (dd->loop);
      return;
    }

  debug_print (dd, "[pgm-debug] device opened");
  debug_print (dd, "[pgm-debug] keys: f=start  s=stop  x=exit");
  start_capture (dev, dd);
}

int
main (int argc, char **argv)
{
  g_autoptr(FpContext) ctx = NULL;
  GPtrArray *devices;
  FpDevice *dev;
  GIOChannel *stdin_ch;
  const gchar *control_path = g_getenv ("EGIS0577_PGM_DEBUG_CONTROL");

  setbuf (stdout, NULL);
  setbuf (stderr, NULL);

  if (!g_getenv ("EGIS0577_PGM_DEBUG_DIR"))
    g_printerr ("warning: EGIS0577_PGM_DEBUG_DIR is not set; driver will not save PGMs\n");

  if (control_path && control_path[0])
    g_file_set_contents (control_path, "0\n", -1, NULL);

  atexit (restore_term);
  set_term_raw ();

  ctx = fp_context_new ();
  devices = fp_context_get_devices (ctx);
  if (!devices)
    {
      restore_term ();
      g_printerr ("No devices found\n");
      return 1;
    }

  dev = discover_device (devices);
  if (!dev)
    {
      restore_term ();
      g_printerr ("No EH577 device found\n");
      return 1;
    }

  DebugData dd = {
    .loop = g_main_loop_new (NULL, FALSE),
    .cancellable = g_cancellable_new (),
    .dev = dev,
    .control_path = control_path,
    .capture_active = FALSE,
    .quitting = FALSE,
    .cancel_requested = FALSE,
    .ret = 0,
  };

  stdin_ch = g_io_channel_unix_new (STDIN_FILENO);
  g_io_channel_set_encoding (stdin_ch, NULL, NULL);
  g_io_channel_set_buffered (stdin_ch, FALSE);
  g_io_add_watch (stdin_ch, G_IO_IN, on_key, &dd);
  g_io_channel_unref (stdin_ch);

  g_unix_signal_add (SIGINT, on_sigint, &dd);

  fp_device_open (dev, dd.cancellable,
                  (GAsyncReadyCallback) on_device_opened, &dd);

  g_main_loop_run (dd.loop);

  g_object_unref (dd.cancellable);
  g_main_loop_unref (dd.loop);

  return dd.ret;
}
