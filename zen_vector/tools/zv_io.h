#ifndef ZV_IO_H
#define ZV_IO_H

/* Small helpers for the tools and tests: a straight-alpha bitmap, BMP files,
   whole-file reading and a monotonic clock. The library itself needs none of
   this. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t *pixels; /* straight 0xAARRGGBB */
    int width, height;
    int stride; /* pixels per row */
} ZvBitmap;

bool zv_bitmap_alloc(ZvBitmap *b, int width, int height); /* zeroed */
void zv_bitmap_free(ZvBitmap *b);
void zv_bitmap_clear(ZvBitmap *b, uint32_t color);
uint32_t zv_bitmap_get(const ZvBitmap *b, int x, int y); /* 0 outside */

/* 24-bit and 32-bit BI_RGB, bottom-up or top-down. Always writes 32-bit. */
bool zv_bitmap_load_bmp(ZvBitmap *b, const char *path);
bool zv_bitmap_save_bmp(const ZvBitmap *b, const char *path);

/* Whole files; release with zv_io_free. zv_io_read_text adds a terminating zero. */
uint8_t *zv_io_read(const char *path, size_t *size);
char *zv_io_read_text(const char *path);
void zv_io_free(void *memory);

uint64_t zv_time_nanos(void);

#endif /* ZV_IO_H */
