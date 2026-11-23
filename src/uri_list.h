#ifndef URI_LIST_H
#define URI_LIST_H

/* text/uri-list, the form files are dragged in under X11 (XDND) and Wayland (wl_data_offer): one URI per line, '#' starts a
   comment. Shared by the two backends. */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static inline int uri_list_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* A URI path with its %XX escapes decoded; NULL when out of memory. */
static inline char *uri_list_unescape(const char *s)
{
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++)
    {
        int hi, lo;
        if (s[i] == '%' && i + 2 < len && (hi = uri_list_hex(s[i + 1])) >= 0 && (lo = uri_list_hex(s[i + 2])) >= 0)
        {
            out[o++] = (char)(hi * 16 + lo);
            i += 2;
        }
        else
            out[o++] = s[i];
    }
    out[o] = 0;
    return out;
}

/* Only local file: URIs are kept. `text` is cut up in place. Returns how many paths were stored in out; the strings are
   the caller's. */
static inline int uri_list_parse(char *text, char **out, int max)
{
    int n = 0;
    char *line = text;
    while (*line && n < max)
    {
        char *end = line + strcspn(line, "\r\n");
        char *next = end + strspn(end, "\r\n");
        *end = 0;
        char *p = line;
        line = next;

        if (!*p || *p == '#' || strncasecmp(p, "file:", 5) != 0)
            continue;
        p += 5;
        if (strncmp(p, "//", 2) == 0)
        {
            /* file://host/path: the host has to be this machine */
            p += 2;
            char *slash = strchr(p, '/');
            if (!slash)
                continue;
            size_t host = (size_t)(slash - p);
            if (host != 0 && !(host == 9 && strncasecmp(p, "localhost", 9) == 0))
                continue;
            p = slash;
        }
        if (*p != '/')
            continue;
        char *path = uri_list_unescape(p);
        if (path)
            out[n++] = path;
    }
    return n;
}

#endif /* URI_LIST_H */
