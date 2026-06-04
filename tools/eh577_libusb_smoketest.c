#include <errno.h>
#include <libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EH577_VID 0x1c7a
#define EH577_PID 0x0577
#define EH577_CONFIG 1
#define EH577_IFACE 0
#define EH577_EP_OUT 0x01
#define EH577_EP_IN 0x82

static const uint8_t first_post_init[] = {
  0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0xfc
};

static void hexdump(const uint8_t *buf, int len)
{
  for (int i = 0; i < len; i++)
    {
      printf("%02x", buf[i]);
      if (i + 1 != len)
        printf(" ");
    }
  printf("\n");
}

static const char *libusb_errname_local(int rc)
{
  const char *name = libusb_error_name(rc);
  return name ? name : "unknown";
}

static void usage(const char *argv0)
{
  fprintf(stderr,
          "Usage: %s [--set-config] [--delay-ms N] [--read] [--detach] [--timeout-ms N] [--debug N]\n",
          argv0);
}

int main(int argc, char **argv)
{
  bool set_config = false;
  bool do_read = false;
  bool detach = false;
  int delay_ms = 0;
  unsigned int timeout_ms = 1000;
  int debug_level = LIBUSB_LOG_LEVEL_INFO;

  for (int i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "--set-config") == 0)
        set_config = true;
      else if (strcmp(argv[i], "--read") == 0)
        do_read = true;
      else if (strcmp(argv[i], "--detach") == 0)
        detach = true;
      else if (strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc)
        delay_ms = atoi(argv[++i]);
      else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc)
        timeout_ms = (unsigned int) atoi(argv[++i]);
      else if (strcmp(argv[i], "--debug") == 0 && i + 1 < argc)
        debug_level = atoi(argv[++i]);
      else
        {
          usage(argv[0]);
          return 2;
        }
    }

  libusb_context *ctx = NULL;
  libusb_device_handle *handle = NULL;
  int rc = libusb_init(&ctx);
  if (rc != 0)
    {
      fprintf(stderr, "libusb_init failed: %s\n", libusb_errname_local(rc));
      return 1;
    }

  libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, debug_level);

  handle = libusb_open_device_with_vid_pid(ctx, EH577_VID, EH577_PID);
  if (!handle)
    {
      fprintf(stderr, "Could not open %04x:%04x\n", EH577_VID, EH577_PID);
      libusb_exit(ctx);
      return 3;
    }

  libusb_device *dev = libusb_get_device(handle);
  printf("Opened %04x:%04x at bus %u address %u\n",
         EH577_VID, EH577_PID,
         libusb_get_bus_number(dev),
         libusb_get_device_address(dev));

  int active_cfg = -1;
  rc = libusb_get_configuration(handle, &active_cfg);
  if (rc == 0)
    printf("Active configuration: %d\n", active_cfg);
  else
    printf("libusb_get_configuration failed: %s\n", libusb_errname_local(rc));

  rc = libusb_kernel_driver_active(handle, EH577_IFACE);
  if (rc >= 0)
    printf("Kernel driver active on iface %d: %d\n", EH577_IFACE, rc);
  else
    printf("libusb_kernel_driver_active failed: %s\n", libusb_errname_local(rc));

  if (detach)
    {
      rc = libusb_set_auto_detach_kernel_driver(handle, 1);
      printf("set_auto_detach_kernel_driver: %d (%s)\n", rc, libusb_errname_local(rc));
    }

  if (set_config)
    {
      rc = libusb_set_configuration(handle, EH577_CONFIG);
      printf("set_configuration(%d): %d (%s)\n", EH577_CONFIG, rc, libusb_errname_local(rc));
      if (rc != 0)
        goto out;
    }

  rc = libusb_claim_interface(handle, EH577_IFACE);
  printf("claim_interface(%d): %d (%s)\n", EH577_IFACE, rc, libusb_errname_local(rc));
  if (rc != 0)
    goto out;

  if (delay_ms > 0)
    {
      printf("Sleeping %d ms before first bulk OUT\n", delay_ms);
      usleep((useconds_t) delay_ms * 1000);
    }

  printf("TX (%zu bytes): ", sizeof(first_post_init));
  hexdump(first_post_init, (int) sizeof(first_post_init));

  int transferred = 0;
  rc = libusb_bulk_transfer(handle,
                            EH577_EP_OUT,
                            (unsigned char *) first_post_init,
                            (int) sizeof(first_post_init),
                            &transferred,
                            timeout_ms);
  printf("bulk OUT ep=0x%02x timeout=%u -> rc=%d (%s), transferred=%d\n",
         EH577_EP_OUT,
         timeout_ms,
         rc,
         libusb_errname_local(rc),
         transferred);

  if (rc == 0 && do_read)
    {
      uint8_t resp[64] = {0};
      transferred = 0;
      rc = libusb_bulk_transfer(handle,
                                EH577_EP_IN,
                                resp,
                                7,
                                &transferred,
                                timeout_ms);
      printf("bulk IN ep=0x%02x timeout=%u -> rc=%d (%s), transferred=%d\n",
             EH577_EP_IN,
             timeout_ms,
             rc,
             libusb_errname_local(rc),
             transferred);
      if (transferred > 0)
        {
          printf("RX: ");
          hexdump(resp, transferred);
        }
    }

  libusb_release_interface(handle, EH577_IFACE);

out:
  libusb_close(handle);
  libusb_exit(ctx);
  return rc == 0 ? 0 : 4;
}
