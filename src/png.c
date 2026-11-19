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
/* stb_image's zlib reader goes on feeding zeros after the end of its input and expands its output without a limit (a
   70-byte file ran for minutes and overflowed an int): every allocation it makes goes through png_realloc, which refuses
   to grow past what the image in the header can need (png_decode sets g_png_cap from IHDR before it calls stb). */
static _Thread_local size_t g_png_cap;
static void *png_realloc(void *p, size_t oldsz, size_t newsz)
{
    (void)oldsz;
    if (g_png_cap && newsz > g_png_cap)
        return NULL;
    return realloc(p, newsz);
}
#define STBI_MALLOC(sz) malloc(sz)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) png_realloc(p, oldsz, newsz)
#define STBI_FREE(p) free(p)
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

/* The most pixels png_decode takes (8192 x 8192): the decoder allocates the whole image from the size in the header, and
   a header is a few bytes. */
#define PNG_MAX_PIXELS ((uint64_t)1 << 26)

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* What stb_image does not check before it allocates: it grows its buffer to the length a chunk declares (a 100-byte file
   with an IDAT of 1.9 GB asked for 1.9 GB) and sizes the image from IHDR. A file whose chunks declare more than the file
   holds, or whose image is too large, cannot be a PNG worth decoding: refused here, before stb sees it. */
static bool png_plausible(const uint8_t *d, size_t n, size_t *cap)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (n < 8 + 12 + 13 || memcmp(d, sig, 8) != 0)
        return false;
    size_t pos = 8;
    bool header = false, data = false;
    uint64_t idat_total = 0, raw_least = 0;
    while (pos + 12 <= n)
    {
        uint32_t len = be32(d + pos);
        if (len > n - pos - 12)
            return false; /* the chunk says it is longer than what is left of the file */
        if (!header)
        {
            if (len != 13 || memcmp(d + pos + 4, "IHDR", 4) != 0)
                return false;
            uint32_t w = be32(d + pos + 8), h = be32(d + pos + 12);
            if (w == 0 || h == 0 || w > (1u << 15) || h > (1u << 15) || (uint64_t)w * h > PNG_MAX_PIXELS)
                return false;
            /* what the decoder needs for the inflated data, as stb computes it (filter byte per row included), twice
               over for the growth of its buffers; the colour type gives the components, the depth the bits */
            static const uint8_t comps[7] = {1, 0, 3, 1, 2, 0, 4};
            unsigned depth = d[pos + 8 + 8], ctype = d[pos + 8 + 9];
            if (ctype > 6 || comps[ctype] == 0 || depth == 0 || depth > 16)
                return false;
            uint64_t bpl = ((uint64_t)w * depth + 7) / 8;
            *cap = (size_t)(2 * (bpl * h * comps[ctype] + h) + 262144);
            raw_least = (uint64_t)w * h * depth * comps[ctype] / 8; /* the least the inflated data can be (Adam7 adds filter bytes) */
            header = true;
        }
        if (memcmp(d + pos + 4, "IDAT", 4) == 0)
        {
            /* stb reads an IDAT into a buffer it has not allocated yet when the first one is empty (a null pointer plus
               zero: undefined in C). An empty first IDAT is legal and absurd; not decoded. */
            if (!data && len == 0)
                return false;
            data = true;
            idat_total += len;
        }
        if (memcmp(d + pos + 4, "IEND", 4) == 0)
            break;
        pos += 12 + (size_t)len;
    }
    /* Deflate cannot expand more than 1032 to 1, so an image whose compressed data is that much smaller than the pixels
       it declares is not one: stb would feed its zlib reader zeros past the end of the input until the buffer the header
       allows is full (a 68-byte file declaring 12799 x 2305 took 20 s under ASan to fill it). */
    if (header && idat_total * 1032 + 4096 < raw_least)
        return false;
    return header;
}

bool png_decode(const void *data, size_t size, Framebuffer *out)
{
    if (!data || !out || size == 0 || size > 0x7FFFFFFFu)
        return false;
    size_t cap = 0;
    if (!png_plausible((const uint8_t *)data, size, &cap))
        return false;
    g_png_cap = cap;
    int w, h, comp;
    unsigned char *rgba = stbi_load_from_memory((const unsigned char *)data, (int)size, &w, &h, &comp, 4);
    g_png_cap = 0;
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
