#ifndef ZV_FILTER_H
#define ZV_FILTER_H

/*
 * Blur and the effects built on it: shadows and glows. The blur is three
 * box blurs, which approximate a gaussian of the given sigma (the Canvas
 * shadowBlur is sigma * 2). Everything works on premultiplied surfaces and
 * on 8-bit coverage masks.
 */

#include "zv_pixel.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Blurs the surface in place with a gaussian of standard deviation
       sigma (pixels). Needs scratch memory from the allocator; returns false
       without it. */
    bool zv_blur_surface(ZvSurface *surface, float sigma);
    /* Same for a coverage mask of width x height bytes. */
    bool zv_blur_mask(uint8_t *mask, int width, int height, float sigma, const ZvAllocator *allocator);

    /* The box radii that approximate a gaussian with three passes. */
    void zv_blur_boxes(float sigma, int boxes[3]);

#ifdef __cplusplus
}
#endif

#endif /* ZV_FILTER_H */
