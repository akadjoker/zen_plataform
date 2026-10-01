#ifndef ZV_BMPCMP_H
#define ZV_BMPCMP_H

#include "zv_io.h"

typedef struct
{
    int width, height;
    long long pixels;
    long long over_tolerance;
    int max_alpha, max_red, max_green, max_blue;
    int max_channel;
} ZvCompare;

/* Compares two framebuffers channel by channel (0xAARRGGBB, straight alpha).
   Returns false, with out untouched, when the sizes differ. A pixel is over the
   tolerance when any channel differs by more than tolerance. If diff is not NULL
   it receives a newly allocated picture: black where equal, green where within
   the tolerance, and red (brighter for larger differences) where over it.
   Release it with zv_bitmap_free. */
bool zv_compare(const ZvBitmap *a, const ZvBitmap *b, int tolerance, ZvCompare *out, ZvBitmap *diff);

#endif /* ZV_BMPCMP_H */
