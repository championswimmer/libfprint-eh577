#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct packet {
  const char *name;
  uint8_t bytes[32];
  int out_len;
  int in_len;
};

static const struct packet eh575_pre_init[] = {
  {"60 00 00", {0x45,0x47,0x49,0x53,0x60,0x00,0x00}, 7, 7},
  {"60 01 00", {0x45,0x47,0x49,0x53,0x60,0x01,0x00}, 7, 7},
  {"61 0a fd", {0x45,0x47,0x49,0x53,0x61,0x0a,0xfd}, 7, 7},
  {"61 35 02", {0x45,0x47,0x49,0x53,0x61,0x35,0x02}, 7, 7},
  {"61 80 00", {0x45,0x47,0x49,0x53,0x61,0x80,0x00}, 7, 7},
  {"60 80 00", {0x45,0x47,0x49,0x53,0x60,0x80,0x00}, 7, 7},
  {"61 0a fc", {0x45,0x47,0x49,0x53,0x61,0x0a,0xfc}, 7, 7},
  {"63 01 ...", {0x45,0x47,0x49,0x53,0x63,0x01,0x02,0x0f,0x03}, 9, 9},
  {"61 0c 22", {0x45,0x47,0x49,0x53,0x61,0x0c,0x22}, 7, 7},
  {"61 09 83", {0x45,0x47,0x49,0x53,0x61,0x09,0x83}, 7, 7},
  {"63 26 ...", {0x45,0x47,0x49,0x53,0x63,0x26,0x06,0x06,0x60,0x06,0x05,0x2f,0x06}, 13, 13},
  {"61 0a f4", {0x45,0x47,0x49,0x53,0x61,0x0a,0xf4}, 7, 7},
  {"61 0c 44", {0x45,0x47,0x49,0x53,0x61,0x0c,0x44}, 7, 7},
  {"61 50 03", {0x45,0x47,0x49,0x53,0x61,0x50,0x03}, 7, 7},
  {"60 50 03", {0x45,0x47,0x49,0x53,0x60,0x50,0x03}, 7, 7},
  {"73 14 ec", {0x45,0x47,0x49,0x53,0x73,0x14,0xec}, 7, 5356},
  {"60 40 ec", {0x45,0x47,0x49,0x53,0x60,0x40,0xec}, 7, 7},
  {"63 09 ...", {0x45,0x47,0x49,0x53,0x63,0x09,0x0b,0x83,0x24,0x00,0x44,0x0f,0x08,0x20,0x20,0x01,0x05,0x12}, 18, 18},
  {"63 26 ...", {0x45,0x47,0x49,0x53,0x63,0x26,0x06,0x06,0x60,0x06,0x05,0x2f,0x06}, 13, 13},
  {"61 23 00", {0x45,0x47,0x49,0x53,0x61,0x23,0x00}, 7, 7},
  {"61 24 33", {0x45,0x47,0x49,0x53,0x61,0x24,0x33}, 7, 7},
  {"61 20 00", {0x45,0x47,0x49,0x53,0x61,0x20,0x00}, 7, 7},
  {"61 21 66", {0x45,0x47,0x49,0x53,0x61,0x21,0x66}, 7, 7},
  {"60 00 66", {0x45,0x47,0x49,0x53,0x60,0x00,0x66}, 7, 7},
  {"60 01 66", {0x45,0x47,0x49,0x53,0x60,0x01,0x66}, 7, 7},
  {"60 40 66", {0x45,0x47,0x49,0x53,0x60,0x40,0x66}, 7, 7},
  {"61 0c 22", {0x45,0x47,0x49,0x53,0x61,0x0c,0x22}, 7, 7},
  {"61 0b 03", {0x45,0x47,0x49,0x53,0x61,0x0b,0x03}, 7, 7},
  {"61 0a fc", {0x45,0x47,0x49,0x53,0x61,0x0a,0xfc}, 7, 7},
};

static const struct packet eh575_post_init[] = {
  {"60 00 fc", {0x45,0x47,0x49,0x53,0x60,0x00,0xfc}, 7, 7},
  {"60 01 fc", {0x45,0x47,0x49,0x53,0x60,0x01,0xfc}, 7, 7},
  {"60 40 fc", {0x45,0x47,0x49,0x53,0x60,0x40,0xfc}, 7, 7},
  {"63 09 ...", {0x45,0x47,0x49,0x53,0x63,0x09,0x0b,0x83,0x24,0x00,0x44,0x0f,0x08,0x20,0x20,0x01,0x05,0x12}, 18, 18},
  {"63 26 ...", {0x45,0x47,0x49,0x53,0x63,0x26,0x06,0x06,0x60,0x06,0x05,0x2f,0x06}, 13, 13},
  {"61 23 00", {0x45,0x47,0x49,0x53,0x61,0x23,0x00}, 7, 7},
  {"61 24 33", {0x45,0x47,0x49,0x53,0x61,0x24,0x33}, 7, 7},
  {"61 20 00", {0x45,0x47,0x49,0x53,0x61,0x20,0x00}, 7, 7},
  {"61 21 66", {0x45,0x47,0x49,0x53,0x61,0x21,0x66}, 7, 7},
  {"60 00 66", {0x45,0x47,0x49,0x53,0x60,0x00,0x66}, 7, 7},
  {"60 01 66", {0x45,0x47,0x49,0x53,0x60,0x01,0x66}, 7, 7},
  {"63 2c ...57", {0x45,0x47,0x49,0x53,0x63,0x2c,0x02,0x00,0x57}, 9, 9},
  {"60 2d 02", {0x45,0x47,0x49,0x53,0x60,0x2d,0x02}, 7, 7},
  {"62 67 03", {0x45,0x47,0x49,0x53,0x62,0x67,0x03}, 7, 10},
  {"60 0f 03", {0x45,0x47,0x49,0x53,0x60,0x0f,0x03}, 7, 7},
  {"63 2c ...13", {0x45,0x47,0x49,0x53,0x63,0x2c,0x02,0x00,0x13}, 9, 9},
  {"60 00 02", {0x45,0x47,0x49,0x53,0x60,0x00,0x02}, 7, 7},
  {"64 14 ec", {0x45,0x47,0x49,0x53,0x64,0x14,0xec}, 7, 5356},
};

static const struct packet eh575_repeat[] = {
  {"61 2d 20", {0x45,0x47,0x49,0x53,0x61,0x2d,0x20}, 7, 7},
  {"60 00 20", {0x45,0x47,0x49,0x53,0x60,0x00,0x20}, 7, 7},
  {"60 01 20", {0x45,0x47,0x49,0x53,0x60,0x01,0x20}, 7, 7},
  {"63 2c ...57", {0x45,0x47,0x49,0x53,0x63,0x2c,0x02,0x00,0x57}, 9, 9},
  {"60 2d 02", {0x45,0x47,0x49,0x53,0x60,0x2d,0x02}, 7, 7},
  {"62 67 03", {0x45,0x47,0x49,0x53,0x62,0x67,0x03}, 7, 10},
  {"63 2c ...13", {0x45,0x47,0x49,0x53,0x63,0x2c,0x02,0x00,0x13}, 9, 9},
  {"60 00 02", {0x45,0x47,0x49,0x53,0x60,0x00,0x02}, 7, 7},
  {"64 14 ec", {0x45,0x47,0x49,0x53,0x64,0x14,0xec}, 7, 5356},
};

static void hexdump(const uint8_t *buf, int len) {
  for (int i = 0; i < len; i++) {
    printf("%02x", buf[i]);
    if (i + 1 != len) printf(" ");
  }
  printf("\n");
}

static int bulk_xfer(int fd, unsigned int ep, void *buf, int len, unsigned int timeout_ms) {
  struct usbdevfs_bulktransfer x = {
    .ep = ep,
    .len = (unsigned int)len,
    .timeout = timeout_ms,
    .data = buf,
  };
  return ioctl(fd, USBDEVFS_BULK, &x);
}

static int interrupt_xfer(int fd, unsigned char ep, void *buf, int len, int timeout_ms) {
  struct usbdevfs_urb urb;
  memset(&urb, 0, sizeof(urb));
  urb.type = USBDEVFS_URB_TYPE_INTERRUPT;
  urb.endpoint = ep;
  urb.buffer = buf;
  urb.buffer_length = len;
  urb.usercontext = &urb;

  if (ioctl(fd, USBDEVFS_SUBMITURB, &urb) < 0)
    return -1;

  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pr = poll(&pfd, 1, timeout_ms);
  if (pr == 0) {
    (void)ioctl(fd, USBDEVFS_DISCARDURB, &urb);
    void *done = NULL;
    (void)ioctl(fd, USBDEVFS_REAPURB, &done);
    errno = ETIMEDOUT;
    return -1;
  }
  if (pr < 0) {
    (void)ioctl(fd, USBDEVFS_DISCARDURB, &urb);
    return -1;
  }

  void *done = NULL;
  if (ioctl(fd, USBDEVFS_REAPURB, &done) < 0)
    return -1;
  if (done != &urb) {
    errno = EIO;
    return -1;
  }
  if (urb.status < 0) {
    errno = -urb.status;
    return -1;
  }
  return urb.actual_length;
}

static int claim_iface(int fd, unsigned int iface) {
  return ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
}

static int release_iface(int fd, unsigned int iface) {
  return ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
}

static int set_config(int fd, unsigned int config) {
  return ioctl(fd, USBDEVFS_SETCONFIGURATION, &config);
}

static int reset_dev(int fd) {
  return ioctl(fd, USBDEVFS_RESET, 0);
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "Usage:\n"
          "  %s /dev/bus/usb/BBB/DDD reset\n"
          "  %s /dev/bus/usb/BBB/DDD poll-int [loops]\n"
          "  %s /dev/bus/usb/BBB/DDD eh575-preinit [count]\n"
          "  %s /dev/bus/usb/BBB/DDD eh575-postinit [count]\n"
          "  %s /dev/bus/usb/BBB/DDD eh575-repeat [count]\n"
          "  %s /dev/bus/usb/BBB/DDD eh575-auto [count]\n",
          argv0, argv0, argv0, argv0, argv0, argv0);
}

static void sanitize_name(const char *in, char *out, size_t out_sz) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < out_sz; i++) {
    char c = in[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
      out[j++] = c;
    else
      out[j++] = '_';
  }
  out[j] = '\0';
}

static void maybe_dump_response(const char *label, int index, const char *pkt_name,
                                const uint8_t *buf, int len) {
  const char *dir = getenv("EH577_DUMP_DIR");
  if (!dir || !*dir)
    return;

  char safe[128];
  char path[512];
  sanitize_name(pkt_name, safe, sizeof(safe));
  snprintf(path, sizeof(path), "%s/%s-%02d-%s.bin", dir, label, index, safe);

  FILE *fp = fopen(path, "wb");
  if (!fp) {
    fprintf(stderr, "dump open failed for %s: %s\n", path, strerror(errno));
    return;
  }
  fwrite(buf, 1, len, fp);
  fclose(fp);
  printf("  dumped to %s\n", path);
}

static void print_payload_stats(const uint8_t *buf, int len) {
  int nonzero = 0;
  for (int i = 0; i < len; i++)
    if (buf[i] != 0)
      nonzero++;
  printf("  payload stats: len=%d nonzero=%d zero=%d\n", len, nonzero, len - nonzero);
}

static int do_poll_int(int fd, int loops) {
  uint8_t buf[64];
  for (int i = 0; i < loops; i++) {
    memset(buf, 0, sizeof(buf));
    int r83 = interrupt_xfer(fd, 0x83, buf, 16, 250);
    if (r83 >= 0) {
      printf("ep83 %d bytes: ", r83);
      hexdump(buf, r83);
    } else if (errno != ETIMEDOUT) {
      printf("ep83 err: %s\n", strerror(errno));
    }

    memset(buf, 0, sizeof(buf));
    int r84 = interrupt_xfer(fd, 0x84, buf, 16, 250);
    if (r84 >= 0) {
      printf("ep84 %d bytes: ", r84);
      hexdump(buf, r84);
    } else if (errno != ETIMEDOUT) {
      printf("ep84 err: %s\n", strerror(errno));
    }
  }
  return 0;
}

static int run_sequence(int fd, const struct packet *seq, int total, int count,
                        const char *label) {
  if (count < 1) count = 1;
  if (count > total) count = total;

  for (int i = 0; i < count; i++) {
    const struct packet *p = &seq[i];
    int alloc = p->in_len > 4096 ? p->in_len : 4096;
    uint8_t *resp = calloc(1, alloc);
    if (!resp) {
      perror("calloc");
      return 1;
    }

    printf("TX[%02d] %s (%d): ", i, p->name, p->out_len);
    hexdump(p->bytes, p->out_len);
    int w = bulk_xfer(fd, 0x01, (void *)p->bytes, p->out_len, 1000);
    if (w < 0) {
      printf("  write err: %s\n", strerror(errno));
      free(resp);
      return 2;
    }
    printf("  wrote %d bytes\n", w);

    int r = bulk_xfer(fd, 0x82, resp, p->in_len, p->in_len > 64 ? 3000 : 1000);
    if (r < 0) {
      printf("  read err: %s\n", strerror(errno));
      free(resp);
      return 3;
    }
    printf("  RX[%02d] %d bytes: ", i, r);
    if (r > 128) {
      hexdump(resp, 64);
      printf("  ... (truncated, last 16): ");
      hexdump(resp + r - 16, 16);
      print_payload_stats(resp, r);
    } else {
      hexdump(resp, r);
    }

    maybe_dump_response(label, i, p->name, resp, r);
    free(resp);
  }

  return 0;
}

static int do_auto_init(int fd, int count) {
  int total = (int)(sizeof(eh575_post_init) / sizeof(eh575_post_init[0]));
  if (count < 1) count = total;
  if (count > total) count = total;

  for (int i = 0; i < count; i++) {
    const struct packet *p = &eh575_post_init[i];
    int alloc = p->in_len > 4096 ? p->in_len : 4096;
    uint8_t *resp = calloc(1, alloc);
    if (!resp) {
      perror("calloc");
      return 1;
    }

    printf("TX[%02d] %s (%d): ", i, p->name, p->out_len);
    hexdump(p->bytes, p->out_len);
    int w = bulk_xfer(fd, 0x01, (void *)p->bytes, p->out_len, 1000);
    if (w < 0) {
      printf("  write err: %s\n", strerror(errno));
      free(resp);
      return 2;
    }
    printf("  wrote %d bytes\n", w);

    int r = bulk_xfer(fd, 0x82, resp, p->in_len, p->in_len > 64 ? 3000 : 1000);
    if (r < 0) {
      printf("  read err: %s\n", strerror(errno));
      free(resp);
      return 3;
    }
    printf("  RX[%02d] %d bytes: ", i, r);
    if (r > 128) {
      hexdump(resp, 64);
      printf("  ... (truncated, last 16): ");
      hexdump(resp + r - 16, 16);
      print_payload_stats(resp, r);
    } else {
      hexdump(resp, r);
    }

    maybe_dump_response("eh575-auto", i, p->name, resp, r);

    if (i == 1 && r >= 7 && resp[5] == 0x01) {
      printf("  NOTE: packet 1 returned state byte 0x01; switching to EH575 pre-init sequence as the reference patch does.\n");
      free(resp);
      return run_sequence(fd, eh575_pre_init,
                          (int)(sizeof(eh575_pre_init) / sizeof(eh575_pre_init[0])),
                          (int)(sizeof(eh575_pre_init) / sizeof(eh575_pre_init[0])),
                          "eh575-preinit");
    }

    free(resp);
  }

  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    usage(argv[0]);
    return 1;
  }

  const char *dev = argv[1];
  const char *mode = argv[2];
  int fd = open(dev, O_RDWR);
  if (fd < 0) {
    perror("open");
    return 2;
  }

  if (strcmp(mode, "reset") == 0) {
    if (reset_dev(fd) < 0) {
      perror("reset");
      close(fd);
      return 3;
    }
    printf("device reset ok\n");
    close(fd);
    return 0;
  }

  unsigned int iface = 0;
  (void)set_config(fd, 1);
  if (claim_iface(fd, iface) < 0) {
    perror("claim_interface");
    close(fd);
    return 3;
  }

  int rc = 0;
  if (strcmp(mode, "poll-int") == 0) {
    int loops = argc >= 4 ? atoi(argv[3]) : 20;
    rc = do_poll_int(fd, loops);
  } else if (strcmp(mode, "eh575-preinit") == 0) {
    int count = argc >= 4 ? atoi(argv[3]) : (int)(sizeof(eh575_pre_init) / sizeof(eh575_pre_init[0]));
    rc = run_sequence(fd, eh575_pre_init, (int)(sizeof(eh575_pre_init) / sizeof(eh575_pre_init[0])), count, "eh575-preinit");
  } else if (strcmp(mode, "eh575-postinit") == 0) {
    int count = argc >= 4 ? atoi(argv[3]) : (int)(sizeof(eh575_post_init) / sizeof(eh575_post_init[0]));
    rc = run_sequence(fd, eh575_post_init, (int)(sizeof(eh575_post_init) / sizeof(eh575_post_init[0])), count, "eh575-postinit");
  } else if (strcmp(mode, "eh575-repeat") == 0) {
    int count = argc >= 4 ? atoi(argv[3]) : (int)(sizeof(eh575_repeat) / sizeof(eh575_repeat[0]));
    rc = run_sequence(fd, eh575_repeat, (int)(sizeof(eh575_repeat) / sizeof(eh575_repeat[0])), count, "eh575-repeat");
  } else if (strcmp(mode, "eh575-auto") == 0) {
    int count = argc >= 4 ? atoi(argv[3]) : (int)(sizeof(eh575_post_init) / sizeof(eh575_post_init[0]));
    rc = do_auto_init(fd, count);
  } else {
    usage(argv[0]);
    rc = 4;
  }

  if (release_iface(fd, iface) < 0)
    perror("release_interface");
  close(fd);
  return rc;
}
