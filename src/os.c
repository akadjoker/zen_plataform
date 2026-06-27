/*
 * os.c - portable OS/filesystem core. stdio for file I/O, string logic for paths,
 * POSIX for directories. The parts that cannot be portable (Android assets, web
 * MEMFS) route through os_backend hooks added in a later phase; until then asset_*
 * reads from a configured root on the real filesystem.
 */
#include "platform.h"
#include "os_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define getcwd _getcwd
#define chdir _chdir
#else
#include <dirent.h>
#include <unistd.h>
#endif

#ifndef PATH_CAP
#define PATH_CAP 4096
#endif

/* ---- whole-file read, the one place bytes come off disk ---- */

static uint8_t *read_whole(const char *path, size_t *out_size, bool text)
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
    if (len < 0)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)len + (text ? 1 : 0));
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len)
    {
        free(buf);
        return NULL;
    }
    if (text)
        buf[len] = '\0';
    if (out_size)
        *out_size = (size_t)len;
    return buf;
}

/* ---- asset root ---- */

static char g_asset_root[PATH_CAP];

void asset_set_root(const char *path)
{
    if (!path)
    {
        g_asset_root[0] = '\0';
        return;
    }
    snprintf(g_asset_root, sizeof g_asset_root, "%s", path);
}

static void asset_join(const char *path, char *out, size_t cap)
{
    const char *root = g_asset_root[0] ? g_asset_root : dir_app();
    if (root && root[0])
        snprintf(out, cap, "%s/%s", root, path);
    else
        snprintf(out, cap, "%s", path);
}

/* Backend-routed first (Android assets), then the asset root on disk. The backend
   buffer is always NUL-terminated, so it serves both the bytes and text paths. */
static uint8_t *asset_load(const char *path, size_t *out_size, bool text)
{
    size_t n = 0;
    uint8_t *routed = os_backend_asset_read(path, &n);
    if (routed)
    {
        if (out_size)
            *out_size = n;
        return routed;
    }
    char full[PATH_CAP];
    asset_join(path, full, sizeof full);
    return read_whole(full, out_size, text);
}

uint8_t *asset_read(const char *path, size_t *out_size)
{
    return asset_load(path, out_size, false);
}

char *asset_read_text(const char *path)
{
    return (char *)asset_load(path, NULL, true);
}

bool asset_exists(const char *path)
{
    int routed = os_backend_asset_exists(path);
    if (routed >= 0)
        return routed != 0;
    char full[PATH_CAP];
    asset_join(path, full, sizeof full);
    return file_exists(full);
}

/* ---- read-write files ---- */

uint8_t *file_read(const char *path, size_t *out_size)
{
    return read_whole(path, out_size, false);
}

char *file_read_text(const char *path)
{
    return (char *)read_whole(path, NULL, true);
}

bool file_write(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    size_t put = (size && data) ? fwrite(data, 1, size, f) : 0;
    fclose(f);
    return put == size;
}

bool file_write_text(const char *path, const char *text)
{
    return file_write(path, text, text ? strlen(text) : 0);
}

void fs_free(void *data)
{
    free(data);
}

/* ---- queries ---- */

bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

bool dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

int64_t file_mod_time(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_mtime : -1;
}

/* ---- path string helpers ---- */

static const char *last_sep(const char *path)
{
    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *back = strrchr(path, '\\');
    if (back > slash)
        slash = back;
#endif
    return slash;
}

const char *path_filename(const char *path)
{
    const char *sep = last_sep(path);
    return sep ? sep + 1 : path;
}

const char *path_extension(const char *path)
{
    const char *name = path_filename(path);
    const char *dot = strrchr(name, '.');
    return (dot && dot != name) ? dot : "";
}

void path_directory(const char *path, char *out, size_t cap)
{
    const char *sep = last_sep(path);
    if (!sep || cap == 0)
    {
        if (cap)
            out[0] = '\0';
        return;
    }
    size_t n = (size_t)(sep - path);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

bool path_has_extension(const char *path, const char *ext)
{
    const char *e = path_extension(path);
    if (!e[0])
        return false;
    if (e[0] == '.')
        e++;
    if (ext[0] == '.')
        ext++;
    return strcasecmp(e, ext) == 0;
}

/* ---- directories ---- */

static char g_path_buf[PATH_CAP];

const char *dir_current(void)
{
    if (!getcwd(g_path_buf, sizeof g_path_buf))
        g_path_buf[0] = '\0';
    return g_path_buf;
}

const char *dir_app(void)
{
#if defined(_WIN32)
    /* Filled in by the Win32 backend later; cwd is the desktop fallback. */
    return dir_current();
#else
    ssize_t n = readlink("/proc/self/exe", g_path_buf, sizeof g_path_buf - 1);
    if (n <= 0)
        return dir_current();
    g_path_buf[n] = '\0';
    char *sep = strrchr(g_path_buf, '/');
    if (sep)
        *sep = '\0';
    return g_path_buf;
#endif
}

const char *dir_data(void)
{
    const char *routed = os_backend_data_dir();
    return routed ? routed : dir_app();
}

bool dir_change(const char *path)
{
    return chdir(path) == 0;
}

static bool make_one(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0777) == 0 || dir_exists(path);
#endif
}

bool dir_make(const char *path)
{
    char tmp[PATH_CAP];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (tmp[0] && !make_one(tmp))
                return false;
            *p = '/';
        }
    }
    return make_one(tmp);
}

#if !defined(_WIN32)
bool dir_list(const char *path, DirList *out)
{
    if (!out)
        return false;
    out->paths = NULL;
    out->count = 0;

    DIR *d = opendir(path);
    if (!d)
        return false;

    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (out->count == cap)
        {
            cap = cap ? cap * 2 : 16;
            char **grown = realloc(out->paths, (size_t)cap * sizeof *grown);
            if (!grown)
            {
                closedir(d);
                dir_list_free(out);
                return false;
            }
            out->paths = grown;
        }
        char full[PATH_CAP];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        out->paths[out->count++] = strdup(full);
    }
    closedir(d);
    return true;
}
#else
bool dir_list(const char *path, DirList *out)
{
    (void)path;
    if (out)
    {
        out->paths = NULL;
        out->count = 0;
    }
    return false; /* Win32 listing wired up with the Win32 backend phase */
}
#endif

void dir_list_free(DirList *list)
{
    if (!list || !list->paths)
        return;
    for (int i = 0; i < list->count; i++)
        free(list->paths[i]);
    free(list->paths);
    list->paths = NULL;
    list->count = 0;
}

/* Default OS-backend hooks: no routing. The Android backend overrides these. */
#if !defined(__ANDROID__)
uint8_t *os_backend_asset_read(const char *path, size_t *out_size)
{
    (void)path;
    (void)out_size;
    return NULL;
}
int os_backend_asset_exists(const char *path)
{
    (void)path;
    return -1;
}
const char *os_backend_data_dir(void)
{
    return NULL;
}
#endif
