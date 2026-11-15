#include "io_internal.h"
#include "win32_util.h"
#include "error_internal.h"

#include <stdio.h>

#ifndef PATH_CAP
#define PATH_CAP 4096
#endif

#define IO_CHUNK_MAX ((DWORD)0x40000000)

static HANDLE handle_of(IoStream *s)
{
    return s->u.handle;
}

static int64_t h_size(IoStream *s)
{
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle_of(s), &size))
    {
        win32_error("cannot get the file size", NULL);
        return -1;
    }
    return (int64_t)size.QuadPart;
}

static int64_t h_seek(IoStream *s, int64_t offset, IoWhence whence)
{
    static const DWORD k_method[] = {FILE_BEGIN, FILE_CURRENT, FILE_END};
    LARGE_INTEGER to, now;
    to.QuadPart = offset;
    if (!SetFilePointerEx(handle_of(s), to, &now, k_method[whence]))
    {
        win32_error("seek failed", NULL);
        return -1;
    }
    return (int64_t)now.QuadPart;
}

static size_t h_read(IoStream *s, void *dst, size_t n)
{
    char *p = dst;
    size_t done = 0;
    while (done < n)
    {
        DWORD want = n - done < IO_CHUNK_MAX ? (DWORD)(n - done) : IO_CHUNK_MAX;
        DWORD got = 0;
        if (!ReadFile(handle_of(s), p + done, want, &got, NULL))
        {
            if (GetLastError() == ERROR_BROKEN_PIPE) /* the other end of a pipe closed: that is the end of the data */
            {
                s->eof = true;
                break;
            }
            win32_error("read failed", NULL);
            break;
        }
        if (got == 0)
        {
            s->eof = true;
            break;
        }
        done += got;
    }
    return done;
}

static size_t h_write(IoStream *s, const void *src, size_t n)
{
    const char *p = src;
    size_t done = 0;
    while (done < n)
    {
        DWORD want = n - done < IO_CHUNK_MAX ? (DWORD)(n - done) : IO_CHUNK_MAX;
        DWORD put = 0;
        if (!WriteFile(handle_of(s), p + done, want, &put, NULL) || put == 0)
        {
            win32_error("write failed", NULL);
            break;
        }
        done += put;
    }
    return done;
}

static bool h_flush(IoStream *s)
{
    /* FlushFileBuffers wants a handle with write access (GENERIC_WRITE) and fails with "access denied" on one opened to read;
       there is nothing of ours to flush then, and fsync on a descriptor opened to read says the same: success */
    if (s->read_only)
        return true;
    if (FlushFileBuffers(handle_of(s)))
        return true;
    return win32_error("flush failed", NULL);
}

static bool h_close(IoStream *s)
{
    if (CloseHandle(handle_of(s)))
        return true;
    return win32_error("close failed", NULL);
}

static const IoVTable k_handle_vt = {h_size, h_seek, h_read, h_write, h_flush, h_close};

IoStream *io_platform_open_file(const char *path, int flags)
{
    wchar_t wide[PATH_CAP];
    if (!win32_widen_path(path, wide, PATH_CAP))
        return NULL;

    DWORD access = 0;
    if (flags & IO_MODE_READ)
        access |= GENERIC_READ;
    if (flags & IO_MODE_APPEND)
        access |= FILE_APPEND_DATA;
    else if (flags & IO_MODE_WRITE)
        access |= GENERIC_WRITE;

    DWORD creation = OPEN_EXISTING;
    if (flags & IO_MODE_TRUNCATE)
        creation = CREATE_ALWAYS;
    else if (flags & IO_MODE_CREATE)
        creation = OPEN_ALWAYS;

    HANDLE h = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, creation,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        win32_error("cannot open", path);
        return NULL;
    }
    /* INVALID_FILE_ATTRIBUTES is 0xFFFFFFFF, which has the directory bit: it says "no attributes" (a pipe or a device opened
       through CreateFileW, a file gone since), not "a directory" */
    DWORD attributes = GetFileAttributesW(wide);
    if (GetFileType(h) == FILE_TYPE_UNKNOWN || (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)))
    {
        CloseHandle(h);
        error_set("cannot open '%s': is a directory", path);
        return NULL;
    }

    IoStream *s = io_stream_alloc(&k_handle_vt);
    if (!s)
    {
        CloseHandle(h);
        return NULL;
    }
    s->u.handle = h;
    s->read_only = !(flags & (IO_MODE_WRITE | IO_MODE_APPEND));
    return s;
}

const char *io_platform_save_target(const char *path, char *buf, size_t cap)
{
    (void)buf;
    (void)cap;
    return path;
}

void io_platform_copy_mode(IoStream *s, const char *like)
{
    (void)s;
    (void)like;
}

unsigned long long io_platform_unique(void)
{
    static volatile LONG64 counter;
    unsigned long long n = (unsigned long long)InterlockedIncrement64(&counter);
    return ((unsigned long long)GetCurrentProcessId() << 32) ^ n;
}

void io_platform_clean_stale_temps(const char *path)
{
    (void)path; /* not done here: a temporary left by a save that died stays (docs/SUSPEITAS.md) */
}

bool io_platform_replace_file(const char *from, const char *to)
{
    wchar_t wfrom[PATH_CAP], wto[PATH_CAP];
    if (!win32_widen_path(from, wfrom, PATH_CAP) || !win32_widen_path(to, wto, PATH_CAP))
        return false;
    if (!MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return win32_error("cannot rename", from);
    return true;
}

bool io_platform_remove_file(const char *path)
{
    wchar_t wide[PATH_CAP];
    return win32_widen_path(path, wide, PATH_CAP) && DeleteFileW(wide);
}
