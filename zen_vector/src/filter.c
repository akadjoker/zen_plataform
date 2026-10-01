#include "zv_filter.h"
#include "zv_internal.h"

/* Three box blurs that approximate a gaussian (Kovesi, "Fast almost-gaussian
   filtering"): box widths from the ideal width for sigma. */
void zv_blur_boxes(float sigma, int boxes[3])
{
    float ideal = sqrtf(12.0f * sigma * sigma / 3.0f + 1.0f);
    int wl = (int)floorf(ideal);
    if (wl % 2 == 0)
        wl--;
    if (wl < 1)
        wl = 1;
    int wu = wl + 2;
    float m = (12.0f * sigma * sigma - 3.0f * (float)wl * (float)wl - 12.0f * (float)wl - 9.0f) / (-4.0f * (float)wl - 4.0f);
    int mi = (int)floorf(m + 0.5f);
    for (int i = 0; i < 3; i++)
        boxes[i] = ((i < mi ? wl : wu) - 1) / 2;
}

/* One box pass on 8-bit channels, 4 per pixel interleaved (a surface) or 1
   (a mask), along a line with a stride. */
static void box_line(const uint8_t *src, uint8_t *dst, int count, int stride, int channels, int radius)
{
    int window = 2 * radius + 1;
    for (int ch = 0; ch < channels; ch++)
    {
        int sum = 0;
        for (int i = -radius; i <= radius; i++)
        {
            int k = zv_clampi(i, 0, count - 1);
            sum += src[(size_t)k * (size_t)stride + (size_t)ch];
        }
        for (int i = 0; i < count; i++)
        {
            dst[(size_t)i * (size_t)stride + (size_t)ch] = (uint8_t)((sum + window / 2) / window);
            int out = zv_clampi(i - radius, 0, count - 1);
            int in = zv_clampi(i + radius + 1, 0, count - 1);
            sum += src[(size_t)in * (size_t)stride + (size_t)ch] - src[(size_t)out * (size_t)stride + (size_t)ch];
        }
    }
}

static void box_pass(uint8_t *a, uint8_t *b, int width, int height, int stride_bytes, int channels, int radius)
{
    /* horizontal a -> b, vertical b -> a */
    for (int y = 0; y < height; y++)
        box_line(a + (size_t)y * (size_t)stride_bytes, b + (size_t)y * (size_t)stride_bytes, width, channels, channels, radius);
    for (int x = 0; x < width; x++)
        box_line(b + (size_t)x * (size_t)channels, a + (size_t)x * (size_t)channels, height, stride_bytes, channels, radius);
}

static bool blur_bytes(uint8_t *pixels, int width, int height, int stride_bytes, int channels, float sigma, const ZvAllocator *alloc)
{
    if (!(sigma > 0.0f) || width <= 0 || height <= 0)
        return true;
    int boxes[3];
    zv_blur_boxes(sigma, boxes);
    uint8_t *tmp = alloc->allocate(alloc->context, (size_t)stride_bytes * (size_t)height);
    if (!tmp)
        return false;
    for (int i = 0; i < 3; i++)
    {
        if (boxes[i] > 0)
            box_pass(pixels, tmp, width, height, stride_bytes, channels, boxes[i]);
    }
    alloc->release(alloc->context, tmp);
    return true;
}

bool zv_blur_surface(ZvSurface *s, float sigma)
{
    return blur_bytes((uint8_t *)s->pixels, s->width, s->height, s->stride * 4, 4, sigma, s->allocator);
}

bool zv_blur_mask(uint8_t *mask, int width, int height, float sigma, const ZvAllocator *allocator)
{
    return blur_bytes(mask, width, height, width, 1, sigma, allocator ? allocator : zv_default_allocator());
}
