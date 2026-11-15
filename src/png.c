/*
 * png.c - the little PNG support the clipboard needs: a minimal encoder and a
 * decoder (stb_image, PNG only, compiled static so it does not clash with the copy
 * zen_ui instantiates).
 */
#include "png_internal.h"

#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR    /* HDR and the linear helpers pull in pow(): the core links no libm */
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS (1 << 15)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ---- encoder ---- */

/* CRC-32 by slicing-by-8: eight bytes per step through eight tables, instead of one byte through one (the IDAT chunk is
   the whole image, and its CRC was most of the time of the encoder). */
static uint32_t g_crc_table[8][256];
static bool g_crc_ready;

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++)
    {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[0][n] = c;
    }
    for (uint32_t n = 0; n < 256; n++)
        for (int t = 1; t < 8; t++)
        {
            uint32_t c = g_crc_table[t - 1][n];
            g_crc_table[t][n] = (c >> 8) ^ g_crc_table[0][c & 0xFF];
        }
    g_crc_ready = true;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (; n >= 8; p += 8, n -= 8)
    {
        uint32_t lo = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        uint32_t hi = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        lo ^= crc;
        crc = g_crc_table[7][lo & 0xFF] ^ g_crc_table[6][(lo >> 8) & 0xFF] ^ g_crc_table[5][(lo >> 16) & 0xFF] ^
              g_crc_table[4][lo >> 24] ^ g_crc_table[3][hi & 0xFF] ^ g_crc_table[2][(hi >> 8) & 0xFF] ^
              g_crc_table[1][(hi >> 16) & 0xFF] ^ g_crc_table[0][hi >> 24];
    }
    for (; n; n--)
        crc = g_crc_table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Writes "length type data crc" and returns the new write position. */
static uint8_t *put_chunk(uint8_t *out, const char type[4], const uint8_t *data, size_t len)
{
    put_be32(out, (uint32_t)len);
    memcpy(out + 4, type, 4);
    if (len)
        memcpy(out + 8, data, len);
    uint32_t crc = crc_update(0xFFFFFFFFu, out + 4, 4 + len) ^ 0xFFFFFFFFu;
    put_be32(out + 8 + len, crc);
    return out + 12 + len;
}

/* One scanline of the raw stream: the filter byte 0, then R G B A of each 0xAARRGGBB pixel. */
static void fill_line(uint8_t *line, const uint32_t *pixels, size_t w)
{
    line[0] = 0;
    for (size_t px = 0; px < w; px++)
    {
        uint32_t c = pixels[px];
        uint8_t *o = line + 1 + px * 4;
        o[0] = (uint8_t)(c >> 16);
        o[1] = (uint8_t)(c >> 8);
        o[2] = (uint8_t)c;
        o[3] = (uint8_t)(c >> 24);
    }
}

uint8_t *png_encode(const Framebuffer *fb, size_t *out_size)
{
    if (!fb || !fb->pixels || fb->width <= 0 || fb->height <= 0 || !out_size)
        return NULL;
    if (!g_crc_ready)
        crc_init();

    size_t w = (size_t)fb->width, h = (size_t)fb->height;
    size_t row = 1 + w * 4;     /* filter byte + RGBA */
    size_t raw = row * h;
    size_t blocks = (raw + 65534) / 65535;
    size_t zlen = 2 + raw + blocks * 5 + 4; /* zlib header, stored blocks, adler32 */
    if (zlen > 0x7FFFFFFFu)
        return NULL;

    uint8_t *z = malloc(zlen);
    uint8_t *file = malloc(8 + 25 + (12 + zlen) + 12);
    if (!z || !file)
    {
        free(z);
        free(file);
        return NULL;
    }

    /* zlib stream: stored blocks over the filtered scanlines, then Adler-32. One scanline at a time is turned into
       its bytes (the filter byte 0, then R G B A of each pixel) and the blocks take their bytes from it. */
    uint8_t *zp = z;
    *zp++ = 0x78;
    *zp++ = 0x01;
    uint32_t a = 1, b = 0;
    uint8_t *line = malloc(row);
    if (!line)
    {
        free(z);
        free(file);
        return NULL;
    }
    size_t y = 0, x = 0; /* scanline and byte within it, of the raw stream */
    size_t left = raw;
    fill_line(line, fb->pixels, w);
    while (left)
    {
        size_t n = left > 65535 ? 65535 : left;
        *zp++ = (uint8_t)(left == n); /* BFINAL, BTYPE = 00 */
        *zp++ = (uint8_t)(n & 0xFF);
        *zp++ = (uint8_t)(n >> 8);
        *zp++ = (uint8_t)(~n & 0xFF);
        *zp++ = (uint8_t)((~n >> 8) & 0xFF);
        left -= n;
        while (n)
        {
            size_t take = row - x < n ? row - x : n;
            memcpy(zp, line + x, take);
            /* Adler-32 with the two sums reduced once per 5552 bytes: they cannot overflow 32 bits before that */
            for (const uint8_t *p = line + x; take;)
            {
                size_t k = take < 5552 ? take : 5552;
                take -= k;
                n -= k;
                zp += k;
                x += k;
                while (k--)
                {
                    a += *p++;
                    b += a;
                }
                a %= 65521u;
                b %= 65521u;
            }
            if (x == row)
            {
                x = 0;
                if (++y < h)
                    fill_line(line, fb->pixels + y * (size_t)fb->stride, w);
            }
        }
    }
    free(line);
    put_be32(zp, (b << 16) | a);
    zp += 4;

    uint8_t *p = file;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(p, sig, 8);
    p += 8;
    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* RGBA */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    p = put_chunk(p, "IHDR", ihdr, 13);
    p = put_chunk(p, "IDAT", z, (size_t)(zp - z));
    p = put_chunk(p, "IEND", NULL, 0);

    free(z);
    *out_size = (size_t)(p - file);
    return file;
}

/* ---- decoder ---- */

bool png_decode(const void *data, size_t size, Framebuffer *out)
{
    if (!data || !out || size == 0 || size > 0x7FFFFFFFu)
        return false;
    int w, h, comp;
    unsigned char *rgba = stbi_load_from_memory((const unsigned char *)data, (int)size, &w, &h, &comp, 4);
    if (!rgba)
        return false;
    if (!framebuffer_alloc(out, w, h))
    {
        stbi_image_free(rgba);
        return false;
    }
    for (int i = 0; i < w * h; i++)
    {
        const unsigned char *s = rgba + (size_t)i * 4;
        out->pixels[i] = ((uint32_t)s[3] << 24) | ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
    }
    stbi_image_free(rgba);
    return true;
}
