#include "zv_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

bool zv_bitmap_alloc(ZvBitmap *b, int width, int height)
{
    memset(b, 0, sizeof *b);
    if (width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / sizeof(uint32_t) / (size_t)height)
        return false;
    b->pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    if (!b->pixels)
        return false;
    b->width = width;
    b->height = height;
    b->stride = width;
    return true;
}

void zv_bitmap_free(ZvBitmap *b)
{
    if (!b)
        return;
    free(b->pixels);
    memset(b, 0, sizeof *b);
}

void zv_bitmap_clear(ZvBitmap *b, uint32_t color)
{
    for (int y = 0; y < b->height; y++)
        for (int x = 0; x < b->width; x++)
            b->pixels[(size_t)y * (size_t)b->stride + (size_t)x] = color;
}

uint32_t zv_bitmap_get(const ZvBitmap *b, int x, int y)
{
    if (x < 0 || y < 0 || x >= b->width || y >= b->height)
        return 0;
    return b->pixels[(size_t)y * (size_t)b->stride + (size_t)x];
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

bool zv_bitmap_load_bmp(ZvBitmap *b, const char *path)
{
    size_t size;
    uint8_t *d = zv_io_read(path, &size);
    if (!d)
        return false;
    bool ok = false;
    if (size >= 54 && d[0] == 'B' && d[1] == 'M')
    {
        uint32_t off = rd32(d + 10);
        int32_t w = (int32_t)rd32(d + 18), h = (int32_t)rd32(d + 22);
        uint16_t bpp = rd16(d + 28);
        uint32_t compression = rd32(d + 30);
        bool top_down = h < 0;
        if (h < 0)
            h = -h;
        if ((bpp == 24 || bpp == 32) && (compression == 0 || (compression == 3 && bpp == 32)) && w > 0 && h > 0)
        {
            size_t row = ((size_t)w * (bpp / 8) + 3) & ~(size_t)3;
            if (off <= size && row * (size_t)h <= size - off && zv_bitmap_alloc(b, w, h))
            {
                for (int y = 0; y < h; y++)
                {
                    const uint8_t *src = d + off + (size_t)(top_down ? y : h - 1 - y) * row;
                    uint32_t *dst = b->pixels + (size_t)y * (size_t)b->stride;
                    for (int x = 0; x < w; x++)
                    {
                        const uint8_t *p = src + (size_t)x * (bpp / 8);
                        uint32_t a = bpp == 32 ? p[3] : 255u;
                        dst[x] = a << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
                    }
                }
                ok = true;
            }
        }
    }
    zv_io_free(d);
    return ok;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool zv_bitmap_save_bmp(const ZvBitmap *b, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    uint8_t h[54];
    memset(h, 0, sizeof h);
    uint32_t row = (uint32_t)b->width * 4u;
    h[0] = 'B';
    h[1] = 'M';
    wr32(h + 2, 54u + row * (uint32_t)b->height);
    wr32(h + 10, 54);
    wr32(h + 14, 40);
    wr32(h + 18, (uint32_t)b->width);
    wr32(h + 22, (uint32_t)b->height);
    h[26] = 1;
    h[28] = 32;
    wr32(h + 34, row * (uint32_t)b->height);
    bool ok = fwrite(h, 1, 54, f) == 54;
    for (int y = b->height - 1; ok && y >= 0; y--)
    {
        const uint32_t *src = b->pixels + (size_t)y * (size_t)b->stride;
        for (int x = 0; ok && x < b->width; x++)
        {
            uint8_t p[4] = {(uint8_t)src[x], (uint8_t)(src[x] >> 8), (uint8_t)(src[x] >> 16), (uint8_t)(src[x] >> 24)};
            ok = fwrite(p, 1, 4, f) == 4;
        }
    }
    return fclose(f) == 0 && ok;
}

uint8_t *zv_io_read(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    uint8_t *d = malloc((size_t)len + 1);
    if (!d)
    {
        fclose(f);
        return NULL;
    }
    size_t got = fread(d, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len)
    {
        free(d);
        return NULL;
    }
    d[len] = 0;
    if (size)
        *size = (size_t)len;
    return d;
}

char *zv_io_read_text(const char *path)
{
    return (char *)zv_io_read(path, NULL);
}

void zv_io_free(void *memory)
{
    free(memory);
}

uint64_t zv_time_nanos(void)
{
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
