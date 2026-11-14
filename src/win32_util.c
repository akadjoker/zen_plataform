#include "win32_util.h"
#include "error_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

bool win32_widen(const char *utf8, wchar_t *out, size_t cap)
{
    if (!utf8 || cap == 0 || cap > INT_MAX)
        return error_set("invalid argument");
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, (int)cap);
    if (n <= 0)
    {
        out[0] = L'\0';
        return error_set(GetLastError() == ERROR_INSUFFICIENT_BUFFER ? "path too long" : "invalid UTF-8");
    }
    return true;
}

bool win32_widen_path(const char *utf8, wchar_t *out, size_t cap)
{
    if (!win32_widen(utf8, out, cap))
        return false;
    size_t n = wcslen(out);
    if (n < LONG_PATH_FROM)
        return true;
    if (out[0] == L'\\' && out[1] == L'\\' && (out[2] == L'?' || out[2] == L'.') && out[3] == L'\\')
        return true; /* already the raw form: the caller's own spelling */
    wchar_t *full = (wchar_t *)malloc(cap * sizeof *full);
    if (!full)
        return error_set("out of memory");
    DWORD got = GetFullPathNameW(out, (DWORD)cap, full, NULL);
    if (got == 0 || got >= cap)
    {
        free(full);
        out[0] = L'\0';
        return error_set("path too long");
    }
    /* "\\?\" turns off the clean-up Windows does on a name (slashes, "." and ".."): GetFullPathNameW did it */
    const wchar_t *prefix = L"\\\\?\\";
    const wchar_t *body = full;
    if (full[0] == L'\\' && full[1] == L'\\')
    {
        prefix = L"\\\\?\\UNC\\";
        body = full + 2;
    }
    size_t pn = wcslen(prefix), bn = wcslen(body);
    if (pn + bn + 1 > cap)
    {
        free(full);
        out[0] = L'\0';
        return error_set("path too long");
    }
    memcpy(out, prefix, pn * sizeof *out);
    memcpy(out + pn, body, (bn + 1) * sizeof *out);
    free(full);
    return true;
}

bool win32_narrow(const wchar_t *wide, char *out, size_t cap)
{
    if (!wide || cap == 0 || cap > INT_MAX)
        return error_set("invalid argument");
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL);
    if (n <= 0)
    {
        out[0] = '\0';
        return error_set(GetLastError() == ERROR_INSUFFICIENT_BUFFER ? "path too long" : "invalid UTF-16");
    }
    return true;
}

bool win32_error(const char *what, const char *path)
{
    DWORD code = GetLastError();
    wchar_t wide[256];
    char msg[512] = "";
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, wide, 256, NULL);
    if (n > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, msg, (int)sizeof msg, NULL, NULL);
        for (size_t i = strlen(msg); i > 0 && (msg[i - 1] == '\r' || msg[i - 1] == '\n' || msg[i - 1] == ' '); i--)
            msg[i - 1] = '\0';
    }
    if (!msg[0])
        snprintf(msg, sizeof msg, "error %lu", (unsigned long)code);
    if (path)
        return error_set("%s '%s': %s", what, path, msg);
    return error_set("%s: %s", what, msg);
}

void win32_slashes(char *path)
{
    for (; *path; path++)
    {
        if (*path == '\\')
            *path = '/';
    }
}
