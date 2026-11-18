/*
 * pasteboard_type.h - the pasteboard type of a MIME type, for the macOS clipboard. Plain C, no Apple header, so what it does can be
 * checked on any machine.
 *
 * NSPasteboard names what it holds by UTI (a uniform type identifier), and refuses a string that is not one: writing
 * "application/x-zen-test" gives "'application/x-zen-test' is not a valid UTI string. Cannot set data for an invalid UTI." and no
 * data (run 44285580324). The library's clipboard is named by MIME types, as on the other systems, so a MIME type is turned into a
 * UTI here, the same way for writing, asking and reading:
 *  - the ones the system has a type for are that type (text/html is public.html, image/jpeg is public.jpeg, application/pdf is
 *    com.adobe.pdf, ...), so that what the library puts there is what other programs read and the other way round;
 *    text/plain, image/png and text/uri-list have their own handling in backend_cocoa.m and are not here;
 *  - every other one is "com.zen-platform.mime." followed by the MIME type spelled with the characters a UTI has (letters, digits,
 *    hyphen, period): lower case (MIME types and UTIs are both case insensitive), '/' as "-s", '-' as "-d", a period that is first,
 *    last or next to another as "-p", any other byte as "-x" and its two hex digits. A hyphen always starts one of these, so two
 *    MIME types never share a type, and "application/x-zen-test" is "com.zen-platform.mime.application-sx-dzen-dtest".
 */
#ifndef PASTEBOARD_TYPE_H
#define PASTEBOARD_TYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define PASTEBOARD_TYPE_PREFIX "com.zen-platform.mime."
#define PASTEBOARD_TYPE_CAP 256 /* bytes with the terminating NUL; longer ones are refused */

static const struct
{
    const char *mime;
    const char *uti;
} k_pasteboard_known[] = {
    {"text/html", "public.html"},
    {"text/rtf", "public.rtf"},
    {"text/csv", "public.comma-separated-values-text"},
    {"text/xml", "public.xml"},
    {"application/xml", "public.xml"},
    {"application/json", "public.json"},
    {"application/pdf", "com.adobe.pdf"},
    {"application/octet-stream", "public.data"},
    {"image/jpeg", "public.jpeg"},
    {"image/tiff", "public.tiff"},
    {"image/gif", "com.compuserve.gif"},
    {"image/bmp", "com.microsoft.bmp"},
    {"image/svg+xml", "public.svg-image"},
};

static char pasteboard_lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool pasteboard_same_mime(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (pasteboard_lower(*a) != pasteboard_lower(*b))
            return false;
    return *a == *b;
}

/* The type for a MIME type into out (cap bytes): false when there is none to give (no MIME type, or one that does not fit). */
static bool pasteboard_type_for_mime(const char *mime, char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    if (!mime || !mime[0] || !out || cap == 0)
        return false;
    if (cap > PASTEBOARD_TYPE_CAP)
        cap = PASTEBOARD_TYPE_CAP;
    for (size_t i = 0; i < sizeof k_pasteboard_known / sizeof k_pasteboard_known[0]; i++)
        if (pasteboard_same_mime(mime, k_pasteboard_known[i].mime))
        {
            size_t n = strlen(k_pasteboard_known[i].uti);
            if (n + 1 > cap)
                return false;
            memcpy(out, k_pasteboard_known[i].uti, n + 1);
            return true;
        }
    size_t used = strlen(PASTEBOARD_TYPE_PREFIX);
    if (used + 1 > cap)
        return false;
    memcpy(out, PASTEBOARD_TYPE_PREFIX, used);
    size_t len = strlen(mime);
    for (size_t i = 0; i < len; i++)
    {
        char c = pasteboard_lower(mime[i]);
        char piece[4];
        size_t n = 1;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            piece[0] = c;
        else if (c == '.' && i > 0 && i + 1 < len && mime[i - 1] != '.' && mime[i + 1] != '.')
            piece[0] = '.';
        else if (c == '/' || c == '-' || c == '.')
        {
            piece[0] = '-';
            piece[1] = c == '/' ? 's' : c == '-' ? 'd' : 'p';
            n = 2;
        }
        else
        {
            piece[0] = '-';
            piece[1] = 'x';
            piece[2] = hex[((unsigned char)c >> 4) & 0xF];
            piece[3] = hex[(unsigned char)c & 0xF];
            n = 4;
        }
        if (used + n + 1 > cap)
            return false;
        memcpy(out + used, piece, n);
        used += n;
    }
    out[used] = '\0';
    return true;
}

#endif
