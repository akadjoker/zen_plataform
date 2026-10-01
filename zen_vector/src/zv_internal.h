#ifndef ZV_INTERNAL_H
#define ZV_INTERNAL_H

#include "zv_pixel.h"

#include <math.h>
#include <string.h>

/* Grows *buffer (capacity *capacity elements of size elem) so it holds at
   least needed elements, copying the first used elements. Returns false and
   changes nothing when memory runs out. */
static inline bool zv_grow(const ZvAllocator *allocator, void **buffer, int *capacity, int used, int needed, size_t elem)
{
    if (needed <= *capacity)
        return true;
    if (needed > 0x3FFFFFFF)
        return false;
    int cap = *capacity < 16 ? 16 : *capacity;
    while (cap < needed)
        cap = cap > 0x1FFFFFFF ? needed : cap * 2;
    if ((size_t)cap > SIZE_MAX / elem)
        return false;
    void *fresh = allocator->allocate(allocator->context, (size_t)cap * elem);
    if (!fresh)
        return false;
    if (used > 0)
        memcpy(fresh, *buffer, (size_t)used * elem);
    if (*buffer)
        allocator->release(allocator->context, *buffer);
    *buffer = fresh;
    *capacity = cap;
    return true;
}

static inline float zv_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int zv_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int zv_floor_int(float v)
{
    return (int)floorf(v);
}

static inline int zv_ceil_int(float v)
{
    return (int)ceilf(v);
}

#endif /* ZV_INTERNAL_H */
