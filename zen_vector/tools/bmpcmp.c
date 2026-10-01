#include "bmpcmp.h"

#include <stdlib.h>

static int channel_diff(uint32_t a, uint32_t b, int shift)
{
    int x = (int)((a >> shift) & 0xFF);
    int y = (int)((b >> shift) & 0xFF);
    return x > y ? x - y : y - x;
}

bool zv_compare(const Framebuffer *a, const Framebuffer *b, int tolerance, ZvCompare *out, Framebuffer *diff)
{
    if (!a || !b || !out || a->width != b->width || a->height != b->height)
        return false;

    if (diff && !framebuffer_alloc(diff, a->width, a->height))
        return false;

    ZvCompare r = {a->width, a->height, (long long)a->width * a->height, 0, 0, 0, 0, 0, 0};
    for (int y = 0; y < a->height; y++)
    {
        const uint32_t *pa = a->pixels + (size_t)y * (size_t)a->stride;
        const uint32_t *pb = b->pixels + (size_t)y * (size_t)b->stride;
        for (int x = 0; x < a->width; x++)
        {
            int da = channel_diff(pa[x], pb[x], 24);
            int dr = channel_diff(pa[x], pb[x], 16);
            int dg = channel_diff(pa[x], pb[x], 8);
            int db = channel_diff(pa[x], pb[x], 0);
            int worst = da;
            if (dr > worst)
                worst = dr;
            if (dg > worst)
                worst = dg;
            if (db > worst)
                worst = db;
            if (da > r.max_alpha)
                r.max_alpha = da;
            if (dr > r.max_red)
                r.max_red = dr;
            if (dg > r.max_green)
                r.max_green = dg;
            if (db > r.max_blue)
                r.max_blue = db;
            if (worst > r.max_channel)
                r.max_channel = worst;
            if (worst > tolerance)
                r.over_tolerance++;
            if (diff)
            {
                uint32_t *pd = diff->pixels + (size_t)y * (size_t)diff->stride;
                if (worst == 0)
                    pd[x] = 0xFF000000u;
                else if (worst <= tolerance)
                    pd[x] = 0xFF006000u;
                else
                {
                    int red = 96 + worst;
                    pd[x] = 0xFF000000u | ((uint32_t)(red > 255 ? 255 : red) << 16);
                }
            }
        }
    }
    *out = r;
    return true;
}
