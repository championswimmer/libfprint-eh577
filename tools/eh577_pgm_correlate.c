/*
 * Normalized cross-correlation (NCC) matcher for EH577 PGM captures.
 *
 * Prints an NxN score matrix (peak NCC over a small search window) for all
 * pairs of the given PGM files.  No libfprint or external libraries needed —
 * just gcc + libm.
 *
 * Build:
 *   gcc -O2 -o eh577_pgm_correlate eh577_pgm_correlate.c -lm
 *
 * Usage:
 *   ./eh577_pgm_correlate [--search N] [--threshold F] <a.pgm> <b.pgm> ...
 *
 *   --search N    : search window +-N pixels in each axis (default 20)
 *   --threshold F : NCC value considered a match, 0.0-1.0 (default 0.4)
 *
 * NCC ranges from -1 (inverse) to +1 (perfect match).  Genuine same-finger
 * press captures on a press sensor typically score 0.3-0.8 depending on image
 * quality and placement consistency.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define MAX_IMAGES 64
#define MAX_W      1024
#define MAX_H      1024

typedef struct {
    unsigned char *data;
    int            w, h;
    char          *path;
    /* Precomputed per-image stats (over the full image) */
    double         mean;
    double         std;
} Img;

/* ---- PGM loader ---- */

static Img *
load_pgm (const char *path)
{
    FILE *f = fopen (path, "rb");
    if (!f) { fprintf (stderr, "Cannot open %s\n", path); return NULL; }

    char magic[3] = {0};
    int w, h, mx;
    if (fscanf (f, "%2s %d %d %d", magic, &w, &h, &mx) != 4 ||
        magic[0] != 'P' || magic[1] != '5' || w <= 0 || h <= 0 || w > MAX_W || h > MAX_H)
    {
        fprintf (stderr, "Bad PGM header in %s\n", path);
        fclose (f);
        return NULL;
    }
    fgetc (f); /* consume single whitespace after maxval */

    Img *img = calloc (1, sizeof (Img));
    img->data = malloc ((size_t) w * h);
    img->w    = w;
    img->h    = h;
    img->path = strdup (path);

    if (fread (img->data, 1, (size_t) w * h, f) != (size_t) w * h)
    {
        fprintf (stderr, "Short read: %s\n", path);
        free (img->data); free (img->path); free (img);
        fclose (f);
        return NULL;
    }
    fclose (f);

    /* Precompute mean and std */
    double sum = 0.0, sum2 = 0.0;
    int n = w * h;
    for (int i = 0; i < n; i++) {
        double v = img->data[i];
        sum  += v;
        sum2 += v * v;
    }
    img->mean = sum / n;
    double var = sum2 / n - img->mean * img->mean;
    img->std  = (var > 0.0) ? sqrt (var) : 1e-9;

    return img;
}

static void
free_img (Img *img)
{
    if (!img) return;
    free (img->data);
    free (img->path);
    free (img);
}

/* ---- NCC at a specific (dx, dy) offset ---- */
/*
 * Shift B by (dx, dy) relative to A and compute NCC over the overlapping
 * region using each image's full-image mean/std.  Pixels from B are
 * accessed at (x+dx, y+dy); only positions valid in both images are used.
 *
 * Using global mean/std (not per-overlap) keeps the computation O(W*H)
 * per offset and is accurate enough when the overlap is most of the image.
 */
static double
ncc_at_offset (const Img *a, const Img *b, int dx, int dy)
{
    int x0a = (dx >= 0) ? 0       : -dx;
    int x0b = (dx >= 0) ? dx      : 0;
    int y0a = (dy >= 0) ? 0       : -dy;
    int y0b = (dy >= 0) ? dy      : 0;

    int xend = (dx >= 0) ? (a->w - dx) : (b->w + dx);
    int yend = (dy >= 0) ? (a->h - dy) : (b->h + dy);

    if (xend <= 0 || yend <= 0) return -1.0;
    /* Clamp to image bounds */
    if (xend > a->w - x0a) xend = a->w - x0a;
    if (xend > b->w - x0b) xend = b->w - x0b;
    if (yend > a->h - y0a) yend = a->h - y0a;
    if (yend > b->h - y0b) yend = b->h - y0b;
    if (xend <= 0 || yend <= 0) return -1.0;

    double ma = a->mean, mb = b->mean;
    double sa = a->std,  sb = b->std;

    double cross = 0.0;
    int n = 0;
    for (int y = 0; y < yend; y++) {
        const unsigned char *rowA = a->data + (y0a + y) * a->w + x0a;
        const unsigned char *rowB = b->data + (y0b + y) * b->w + x0b;
        for (int x = 0; x < xend; x++) {
            cross += ((double) rowA[x] - ma) * ((double) rowB[x] - mb);
            n++;
        }
    }
    if (n == 0) return -1.0;
    return cross / ((double) n * sa * sb);
}

/* ---- Peak NCC over search window ---- */

static double
peak_ncc (const Img *a, const Img *b, int search,
          int *best_dx, int *best_dy)
{
    double best = -2.0;
    *best_dx = 0; *best_dy = 0;
    for (int dy = -search; dy <= search; dy++) {
        for (int dx = -search; dx <= search; dx++) {
            double v = ncc_at_offset (a, b, dx, dy);
            if (v > best) { best = v; *best_dx = dx; *best_dy = dy; }
        }
    }
    return best;
}

/* ---- main ---- */

int
main (int argc, char **argv)
{
    Img   *imgs[MAX_IMAGES];
    char  *paths[MAX_IMAGES];
    int    n = 0;
    int    search    = 20;
    double threshold = 0.40;

    for (int i = 1; i < argc; i++) {
        if (strcmp (argv[i], "--search") == 0 && i + 1 < argc) {
            search = atoi (argv[++i]);
        } else if (strcmp (argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold = atof (argv[++i]);
        } else {
            if (n >= MAX_IMAGES) { fprintf (stderr, "Too many files\n"); return 1; }
            paths[n++] = argv[i];
        }
    }

    if (n < 2) {
        printf ("usage: %s [--search N] [--threshold F] <a.pgm> <b.pgm> ...\n", argv[0]);
        printf ("  NCC ranges from -1 to +1; same-finger pairs typically 0.3-0.8.\n");
        return 1;
    }

    /* Load all images */
    printf ("Loading images:\n");
    for (int i = 0; i < n; i++) {
        imgs[i] = load_pgm (paths[i]);
        if (!imgs[i]) return 1;
        printf ("  %2d  %s  (%dx%d  mean=%.1f  std=%.1f)\n",
                i + 1, paths[i], imgs[i]->w, imgs[i]->h,
                imgs[i]->mean, imgs[i]->std);
    }

    /* Build score matrix */
    double scores[MAX_IMAGES][MAX_IMAGES];
    int    bdx[MAX_IMAGES][MAX_IMAGES];
    int    bdy[MAX_IMAGES][MAX_IMAGES];

    printf ("\nComputing NCC scores (search window +-%d px)...\n", search);
    for (int i = 0; i < n; i++) {
        for (int j = i; j < n; j++) {
            scores[i][j] = peak_ncc (imgs[i], imgs[j], search,
                                     &bdx[i][j], &bdy[i][j]);
            scores[j][i]  = scores[i][j];
            bdx[j][i]     = -bdx[i][j];
            bdy[j][i]     = -bdy[i][j];
        }
    }

    /* Print matrix */
    printf ("\nNCC score matrix (%d images, threshold=%.2f)\n\n", n, threshold);
    printf ("      ");
    for (int i = 0; i < n; i++) printf ("   %3d", i + 1);
    printf ("\n      ");
    for (int i = 0; i < n; i++) printf (" -----");
    printf ("\n");

    for (int i = 0; i < n; i++) {
        printf (" %3d |", i + 1);
        for (int j = 0; j < n; j++) {
            double s = scores[i][j];
            if (s >= threshold)
                printf (" [%.2f]", s);  /* brackets = match */
            else
                printf ("  %.2f ", s);
        }
        printf ("\n");
    }

    printf ("\n[score] = at or above threshold %.2f\n", threshold);

    /* Print top pairs by score */
    printf ("\nTop pairs (excluding self):\n");
    /* Simple selection sort of off-diagonal pairs */
    typedef struct { int i, j; double s; } Pair;
    Pair top[MAX_IMAGES * MAX_IMAGES];
    int np = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            top[np++] = (Pair) { i, j, scores[i][j] };
    /* Bubble sort descending (small n) */
    for (int a = 0; a < np - 1; a++)
        for (int b = a + 1; b < np; b++)
            if (top[b].s > top[a].s) { Pair t = top[a]; top[a] = top[b]; top[b] = t; }

    int show = (np < 10) ? np : 10;
    for (int k = 0; k < show; k++) {
        int i = top[k].i, j = top[k].j;
        printf ("  %2d vs %2d  ncc=%.4f  best_offset=(%+d,%+d)%s\n",
                i + 1, j + 1, top[k].s,
                bdx[i][j], bdy[i][j],
                top[k].s >= threshold ? "  MATCH" : "");
    }

    /* Files legend */
    printf ("\nFiles:\n");
    for (int i = 0; i < n; i++)
        printf ("  %2d  %s\n", i + 1, paths[i]);
    printf ("\n");

    for (int i = 0; i < n; i++) free_img (imgs[i]);
    return 0;
}
