#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

typedef struct _FpImage FpImage;
extern FpImage *fp_image_new(int width, int height);
extern const unsigned char *fp_image_get_data(FpImage *img, gsize *len);
extern GSList *fp_image_get_minutiae(FpImage *img);

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    char line[256];
    fgets(line, sizeof(line), f); // P5
    fgets(line, sizeof(line), f); // maybe comment
    if (line[0] == '#') fgets(line, sizeof(line), f);
    int w = 0, h = 0;
    sscanf(line, "%d %d", &w, &h);
    fgets(line, sizeof(line), f); // 255
    int img_size = w * h;
    unsigned char *raw_data = malloc(img_size);
    fread(raw_data, 1, img_size, f);
    fclose(f);

    FpImage *img = fp_image_new(w, h);
    gsize len;
    unsigned char *img_data = (unsigned char *)fp_image_get_data(img, &len);
    memcpy(img_data, raw_data, img_size);

    GSList *minutiae = fp_image_get_minutiae(img);
    printf("%s: %dx%d, %d minutiae. First bytes: %02x %02x %02x\n", argv[1], w, h, g_slist_length(minutiae), raw_data[0], raw_data[1], raw_data[2]);
    return 0;
}
