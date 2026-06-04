#include <gio/gio.h>
#include <gusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EH577_VID 0x1c7a
#define EH577_PID 0x0577
#define EH577_CONFIG 1
#define EH577_IFACE 0
#define EH577_EP_OUT 0x01
#define EH577_EP_IN 0x82

static const guint8 first_post_init[] = {
  0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0xfc
};

typedef struct {
  GMainLoop *loop;
  gboolean do_read;
  guint timeout_ms;
  guint8 in_buf[64];
  guint8 out_buf[sizeof(first_post_init)];
  gboolean ok;
} AsyncCtx;

static void hexdump(const guint8 *buf, gsize len)
{
  for (gsize i = 0; i < len; i++)
    {
      printf("%02x", buf[i]);
      if (i + 1 != len)
        printf(" ");
    }
  printf("\n");
}

static void in_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  AsyncCtx *ctx = user_data;
  g_autoptr(GError) error = NULL;
  gssize actual = g_usb_device_bulk_transfer_finish(G_USB_DEVICE(source_object), res, &error);

  if (error)
    {
      printf("async bulk IN failed: %s\n", error->message);
      ctx->ok = FALSE;
    }
  else
    {
      printf("async bulk IN ok, actual=%zd\n", actual);
      if (actual > 0)
        {
          printf("RX: ");
          hexdump(ctx->in_buf, (gsize) actual);
        }
      ctx->ok = TRUE;
    }

  g_main_loop_quit(ctx->loop);
}

static void out_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  AsyncCtx *ctx = user_data;
  g_autoptr(GError) error = NULL;
  gssize actual = g_usb_device_bulk_transfer_finish(G_USB_DEVICE(source_object), res, &error);

  if (error)
    {
      printf("async bulk OUT failed: %s\n", error->message);
      ctx->ok = FALSE;
      g_main_loop_quit(ctx->loop);
      return;
    }

  printf("async bulk OUT ok, actual=%zd\n", actual);

  if (!ctx->do_read)
    {
      ctx->ok = TRUE;
      g_main_loop_quit(ctx->loop);
      return;
    }

  g_usb_device_bulk_transfer_async(G_USB_DEVICE(source_object),
                                   EH577_EP_IN,
                                   ctx->in_buf,
                                   7,
                                   ctx->timeout_ms,
                                   NULL,
                                   in_cb,
                                   ctx);
}

static void usage(const char *argv0)
{
  fprintf(stderr,
          "Usage: %s [--set-config] [--sync] [--read] [--timeout-ms N] [--debug]\n",
          argv0);
}

int main(int argc, char **argv)
{
  gboolean set_config = FALSE;
  gboolean do_sync = FALSE;
  gboolean do_read = FALSE;
  gboolean debug = FALSE;
  guint timeout_ms = 1000;

  for (int i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "--set-config") == 0)
        set_config = TRUE;
      else if (strcmp(argv[i], "--sync") == 0)
        do_sync = TRUE;
      else if (strcmp(argv[i], "--read") == 0)
        do_read = TRUE;
      else if (strcmp(argv[i], "--debug") == 0)
        debug = TRUE;
      else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc)
        timeout_ms = (guint) g_ascii_strtoull(argv[++i], NULL, 10);
      else
        {
          usage(argv[0]);
          return 2;
        }
    }

  g_autoptr(GError) error = NULL;
  g_autoptr(GUsbContext) usb_ctx = g_usb_context_new(&error);
  if (!usb_ctx)
    {
      fprintf(stderr, "g_usb_context_new failed: %s\n", error->message);
      return 1;
    }

  if (debug)
    g_usb_context_set_debug(usb_ctx, G_LOG_LEVEL_DEBUG);

  g_usb_context_enumerate(usb_ctx);

  g_autoptr(GUsbDevice) dev = g_usb_context_find_by_vid_pid(usb_ctx, EH577_VID, EH577_PID, &error);
  if (!dev)
    {
      fprintf(stderr, "find_by_vid_pid failed: %s\n", error->message);
      return 3;
    }

  printf("Found %04x:%04x at bus %u addr %u port %u\n",
         EH577_VID,
         EH577_PID,
         g_usb_device_get_bus(dev),
         g_usb_device_get_address(dev),
         g_usb_device_get_port_number(dev));

  if (!g_usb_device_open(dev, &error))
    {
      fprintf(stderr, "open failed: %s\n", error->message);
      return 4;
    }

  printf("Current configuration: %d\n", g_usb_device_get_configuration(dev, NULL));

  if (set_config)
    {
      if (!g_usb_device_set_configuration(dev, EH577_CONFIG, &error))
        {
          fprintf(stderr, "set_configuration(%d) failed: %s\n", EH577_CONFIG, error->message);
          return 5;
        }
      printf("set_configuration(%d) ok\n", EH577_CONFIG);
    }

  if (!g_usb_device_claim_interface(dev, EH577_IFACE, G_USB_DEVICE_CLAIM_INTERFACE_NONE, &error))
    {
      fprintf(stderr, "claim_interface(%d) failed: %s\n", EH577_IFACE, error->message);
      return 6;
    }

  printf("TX (%zu bytes): ", sizeof(first_post_init));
  hexdump(first_post_init, sizeof(first_post_init));

  if (do_sync)
    {
      gsize actual = 0;
      guint8 out_buf[sizeof(first_post_init)];
      memcpy(out_buf, first_post_init, sizeof(first_post_init));
      if (!g_usb_device_bulk_transfer(dev, EH577_EP_OUT, out_buf, sizeof(out_buf), &actual, timeout_ms, NULL, &error))
        {
          fprintf(stderr, "sync bulk OUT failed: %s\n", error->message);
          return 7;
        }
      printf("sync bulk OUT ok, actual=%zu\n", actual);

      if (do_read)
        {
          guint8 in_buf[64] = {0};
          actual = 0;
          if (!g_usb_device_bulk_transfer(dev, EH577_EP_IN, in_buf, 7, &actual, timeout_ms, NULL, &error))
            {
              fprintf(stderr, "sync bulk IN failed: %s\n", error->message);
              return 8;
            }
          printf("sync bulk IN ok, actual=%zu\n", actual);
          if (actual > 0)
            {
              printf("RX: ");
              hexdump(in_buf, actual);
            }
        }
    }
  else
    {
      AsyncCtx ctx = {
        .loop = g_main_loop_new(NULL, FALSE),
        .do_read = do_read,
        .timeout_ms = timeout_ms,
        .ok = FALSE,
      };
      memcpy(ctx.out_buf, first_post_init, sizeof(first_post_init));

      g_usb_device_bulk_transfer_async(dev,
                                       EH577_EP_OUT,
                                       ctx.out_buf,
                                       sizeof(ctx.out_buf),
                                       timeout_ms,
                                       NULL,
                                       out_cb,
                                       &ctx);
      g_main_loop_run(ctx.loop);
      g_main_loop_unref(ctx.loop);

      if (!ctx.ok)
        return 9;
    }

  if (!g_usb_device_release_interface(dev, EH577_IFACE, G_USB_DEVICE_CLAIM_INTERFACE_NONE, &error))
    {
      fprintf(stderr, "release_interface failed: %s\n", error->message);
      return 10;
    }

  if (!g_usb_device_close(dev, &error))
    {
      fprintf(stderr, "close failed: %s\n", error->message);
      return 11;
    }

  return 0;
}
