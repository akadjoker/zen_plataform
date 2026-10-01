#include "bmpcmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr, "usage: vcmp a.bmp b.bmp [--tol N] [--diff out.bmp]\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return usage();
    int tolerance = 0;
    const char *diff_path = NULL;
    for (int i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "--tol") == 0 && i + 1 < argc)
            tolerance = atoi(argv[++i]);
        else if (strcmp(argv[i], "--diff") == 0 && i + 1 < argc)
            diff_path = argv[++i];
        else
            return usage();
    }

    ZvBitmap a, b, diff;
    if (!zv_bitmap_load_bmp(&a, argv[1]))
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    if (!zv_bitmap_load_bmp(&b, argv[2]))
    {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        zv_bitmap_free(&a);
        return 2;
    }

    ZvCompare r;
    int status = 2;
    memset(&diff, 0, sizeof diff);
    if (!zv_compare(&a, &b, tolerance, &r, diff_path ? &diff : NULL))
    {
        fprintf(stderr, "size mismatch: %dx%d against %dx%d\n", a.width, a.height, b.width, b.height);
    }
    else
    {
        printf("size %dx%d  pixels %lld  over tolerance(%d) %lld  max channel %d  (A %d R %d G %d B %d)\n",
               r.width, r.height, r.pixels, tolerance, r.over_tolerance, r.max_channel,
               r.max_alpha, r.max_red, r.max_green, r.max_blue);
        status = r.over_tolerance == 0 ? 0 : 1;
        if (diff_path && !zv_bitmap_save_bmp(&diff, diff_path))
            fprintf(stderr, "cannot write %s\n", diff_path);
    }
    if (diff_path)
        zv_bitmap_free(&diff);
    zv_bitmap_free(&a);
    zv_bitmap_free(&b);
    return status;
}
