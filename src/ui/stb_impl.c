/*
 * stb_impl.c - the single translation unit that instantiates the vendored stb
 * single-header libraries. Everything else includes the headers declaration-only.
 *
 * stb_truetype: glyph rasterization for the TTF font path (ui_font.c).
 * stb_image:    decode the emoji PNG atlas (ui_emoji.c).
 * Both are public domain; see the headers for their license notice.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* The same hardening as src/png.c, which has its own static instance of stb_image for the clipboard (this one is the library's
   public stbi_* API, for whoever links zen_ui, and its input can come from outside):
   - stbi_load_from_memory and the other _from_memory calls refuse, before stb sees the bytes, a PNG whose chunks declare more
     than the buffer holds (a 100-byte file with an IDAT of 1.9 GB made stb ask for 1.9 GB), an empty first IDAT (stb then
     adds zero to a null pointer: undefined behaviour), and an image over 64 M pixels or 32768 on a side;
   - every allocation of stb goes through zen_stbi_realloc, which refuses to grow past what the image in the IHDR can need
     (stb's zlib reader goes on feeding zeros after the end of its input and expands its output without a limit: a 70-byte file
     ran for minutes and overflowed an int), and past 256 MB when the bytes are not in memory to be looked at (stbi_load of a
     file, callbacks), where the IHDR is not known beforehand: those paths are bounded, not checked.
   The wrappers are the library's stbi_load_from_memory, stbi_load_16_from_memory, stbi_loadf_from_memory, stbi_info_from_memory
   and stbi_is_16_bit_from_memory; stb's own definitions are renamed (zen_stbi_*) around the include. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STBI_MAX_DIMENSIONS (1 << 15)
#define ZEN_STBI_MAX_PIXELS ((uint64_t)1 << 26)
#define ZEN_STBI_MAX_BYTES ((size_t)256 << 20)

static _Thread_local size_t g_stbi_cap;
static void *zen_stbi_realloc(void *p, size_t oldsz, size_t newsz)
{
    (void)oldsz;
    if (newsz > (g_stbi_cap ? g_stbi_cap : ZEN_STBI_MAX_BYTES))
        return NULL;
    return realloc(p, newsz);
}
#define STBI_MALLOC(sz) zen_stbi_realloc(NULL, 0, (sz))
#define STBI_REALLOC_SIZED(p, oldsz, newsz) zen_stbi_realloc(p, oldsz, newsz)
#define STBI_FREE(p) free(p)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG /* the emoji atlas is PNG; keep the decoder small */
#define stbi_load_from_memory zen_stbi_load_from_memory
#define stbi_load_16_from_memory zen_stbi_load_16_from_memory
#define stbi_loadf_from_memory zen_stbi_loadf_from_memory
#define stbi_info_from_memory zen_stbi_info_from_memory
#define stbi_is_16_bit_from_memory zen_stbi_is_16_bit_from_memory
#include "stb_image.h"
#undef stbi_load_from_memory
#undef stbi_load_16_from_memory
#undef stbi_loadf_from_memory
#undef stbi_info_from_memory
#undef stbi_is_16_bit_from_memory

static uint32_t zen_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Whether the bytes can be a PNG worth decoding, and the most memory its image can need (the inflated data as stb computes
   it, filter byte per row included, twice over for the growth of its buffers). Not a PNG at all: true with no cap, stb
   refuses it by itself at the signature. */
static bool zen_png_check(const uint8_t *d, size_t n, size_t *cap)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    *cap = 0;
    if (n < 8 || memcmp(d, sig, 8) != 0)
        return true;
    if (n < 8 + 12 + 13)
        return false;
    size_t pos = 8;
    bool header = false, data = false;
    uint64_t idat_total = 0, raw_least = 0;
    while (pos + 8 <= n) /* the header of a chunk: stb reads an IDAT's data before its CRC, which a truncated file may lack */
    {
        uint32_t len = zen_be32(d + pos);
        if (len > n - pos - 8)
            return false; /* the chunk says it is longer than what is left of the buffer */
        if (!header)
        {
            if (len != 13 || memcmp(d + pos + 4, "IHDR", 4) != 0)
                return false;
            uint32_t w = zen_be32(d + pos + 8), h = zen_be32(d + pos + 12);
            if (w == 0 || h == 0 || w > (1u << 15) || h > (1u << 15) || (uint64_t)w * h > ZEN_STBI_MAX_PIXELS)
                return false;
            static const uint8_t comps[7] = {1, 0, 3, 1, 2, 0, 4};
            unsigned depth = d[pos + 8 + 8], ctype = d[pos + 8 + 9];
            if (ctype > 6 || comps[ctype] == 0 || depth == 0 || depth > 16)
                return false;
            /* the depths each colour type may have: an RGBA image of 4 bits trips an assert in stb (and decodes wrong without) */
            if ((ctype == 0 && depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) ||
                (ctype == 3 && depth > 8) || ((ctype == 2 || ctype == 4 || ctype == 6) && depth != 8 && depth != 16))
                return false;
            uint64_t bpl = ((uint64_t)w * depth + 7) / 8;
            uint64_t need = 2 * (bpl * h * comps[ctype] + h) + 262144;
            raw_least = (uint64_t)w * h * depth * comps[ctype] / 8;
            /* the image that comes back: 8 or 16 bits, or floats (stbi_loadf), with up to four components */
            uint64_t out = (uint64_t)w * h * 16 + 262144;
            *cap = (size_t)(need > out ? need : out);
            header = true;
        }
        if (memcmp(d + pos + 4, "IDAT", 4) == 0)
        {
            if (!data && len == 0)
                return false; /* stb would add zero to a null pointer */
            data = true;
            idat_total += len;
        }
        if (memcmp(d + pos + 4, "IEND", 4) == 0)
            break;
        pos += 12 + (size_t)len;
    }
    /* deflate cannot expand more than 1032 to 1: compressed data far smaller than the declared pixels is not an image (stb
       would feed zeros to its zlib reader until the buffer the header allows is full) */
    if (header && idat_total * 1032 + 4096 < raw_least)
        return false;
    return header;
}

#define ZEN_STBI_CHECKED(call, fail)                         \
    do                                                       \
    {                                                        \
        size_t cap;                                          \
        if (!buffer || len <= 0 || !zen_png_check(buffer, (size_t)len, &cap)) \
            return fail;                                     \
        g_stbi_cap = cap;                                    \
        __typeof__(call) result = (call);                    \
        g_stbi_cap = 0;                                      \
        return result;                                       \
    } while (0)

stbi_uc *stbi_load_from_memory(stbi_uc const *buffer, int len, int *x, int *y, int *comp, int req_comp)
{
    ZEN_STBI_CHECKED(zen_stbi_load_from_memory(buffer, len, x, y, comp, req_comp), NULL);
}

stbi_us *stbi_load_16_from_memory(stbi_uc const *buffer, int len, int *x, int *y, int *comp, int req_comp)
{
    ZEN_STBI_CHECKED(zen_stbi_load_16_from_memory(buffer, len, x, y, comp, req_comp), NULL);
}

float *stbi_loadf_from_memory(stbi_uc const *buffer, int len, int *x, int *y, int *comp, int req_comp)
{
    ZEN_STBI_CHECKED(zen_stbi_loadf_from_memory(buffer, len, x, y, comp, req_comp), NULL);
}

int stbi_info_from_memory(stbi_uc const *buffer, int len, int *x, int *y, int *comp)
{
    ZEN_STBI_CHECKED(zen_stbi_info_from_memory(buffer, len, x, y, comp), 0);
}

int stbi_is_16_bit_from_memory(stbi_uc const *buffer, int len)
{
    ZEN_STBI_CHECKED(zen_stbi_is_16_bit_from_memory(buffer, len), 0);
}
