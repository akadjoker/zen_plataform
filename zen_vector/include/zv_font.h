#ifndef ZV_FONT_H
#define ZV_FONT_H

/*
 * TrueType fonts: glyph outlines as paths, drawn by the same rasterizer as
 * everything else. Reads the tables head, hhea, hmtx, maxp, cmap (formats 4
 * and 12), loca and glyf, including composite glyphs. No hinting, no
 * shaping: one glyph per code point, with the kern table (format 0) when
 * the font has one. The font keeps a pointer to the bytes; the caller keeps
 * them alive.
 */

#include "zv_geom.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ZV_GLYPH_CACHE 256

    typedef struct
    {
        int glyph;
        ZvPath path; /* in font units, y up */
        bool valid;
    } ZvGlyphEntry;

    typedef struct
    {
        const uint8_t *data;
        size_t size;
        const ZvAllocator *allocator;
        uint32_t loca, glyf, cmap, hmtx, kern;
        uint32_t loca_count;
        int index_to_loc; /* 0: short offsets, 1: long */
        int units_per_em;
        int ascent, descent, line_gap; /* hhea, font units */
        int glyph_count;
        int hmetric_count;
        uint32_t cmap_sub; /* offset of the chosen subtable */
        int cmap_format;
        ZvGlyphEntry cache[ZV_GLYPH_CACHE];
    } ZvFont;

    /* Returns false when the bytes are not a TrueType font with outlines. */
    bool zv_font_init(ZvFont *font, const uint8_t *data, size_t size, const ZvAllocator *allocator);
    void zv_font_release(ZvFont *font);

    int zv_font_glyph_index(const ZvFont *font, uint32_t codepoint); /* 0 when missing */
    int zv_font_advance(const ZvFont *font, int glyph);             /* font units */
    int zv_font_kerning(const ZvFont *font, int left, int right);   /* font units, often 0 */
    /* The outline of a glyph (cached), in font units with y up. NULL when it
       cannot be read. */
    const ZvPath *zv_font_glyph_path(ZvFont *font, int glyph);

    /* Appends the outline of utf8 text to path, in user units: pen at (x, y)
       on the baseline, size in user units per em, y down. Returns the width
       advanced. */
    float zv_font_text_path(ZvFont *font, const char *utf8, float x, float y, float size, ZvPath *path);
    float zv_font_text_width(ZvFont *font, const char *utf8, float size);

    /* Decodes one UTF-8 code point; returns the number of bytes used (1 for
       a bad byte, which becomes U+FFFD). */
    int zv_utf8_decode(const char *s, uint32_t *codepoint);

#ifdef __cplusplus
}
#endif

#endif /* ZV_FONT_H */
