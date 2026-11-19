/*
 * fuzz_paths.c - the path string helpers and the file system calls on names that come from outside the program (a dropped
 * file, a command line, a settings file): empty, very long, with NULs (the input is cut at its NULs into up to three
 * strings), invalid UTF-8, "..", names that are symlink loops. Input: byte 0 picks the operation, byte 1 the size of the
 * output buffer (allocated at exactly that size, so a write past it is an ASan report), the rest is the strings.
 * The file system calls work in a directory of their own under /dev/shm with four symlinks in it (a loop of two, a link to
 * itself, a link to the directory) and only on names without "..", so nothing can leave it.
 */
#define _GNU_SOURCE 1
#include "platform.h"

#include <dirent.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_sandbox[128];

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)flag, (void)ftw;
    return remove(path); /* a symlink is removed, not followed */
}

static void sandbox_reset(void)
{
    nftw(g_sandbox, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    mkdir(g_sandbox, 0700);
    char p[256];
    snprintf(p, sizeof p, "%s/loop_b", g_sandbox);
    (void)!symlink("loop_a", p);
    snprintf(p, sizeof p, "%s/loop_a", g_sandbox);
    (void)!symlink("loop_b", p);
    snprintf(p, sizeof p, "%s/selfloop", g_sandbox);
    (void)!symlink("selfloop", p);
    snprintf(p, sizeof p, "%s/updir", g_sandbox);
    (void)!symlink(".", p);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc, (void)argv;
    snprintf(g_sandbox, sizeof g_sandbox, "/dev/shm/zp-fuzz-paths-%d", (int)getpid());
    return 0;
}

static bool enum_cb(const char *path, PathType type, void *user)
{
    (void)type;
    int *n = user;
    return path && ++*n < 2000;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;
    unsigned op = data[0] % 24;
    size_t cap = data[1];
    /* the strings: cut at NULs, each a malloc of exactly its size */
    char *str[3] = {NULL, NULL, NULL};
    const uint8_t *p = data + 2, *end = data + size;
    for (int i = 0; i < 3; i++)
    {
        const uint8_t *q = p;
        while (q < end && *q)
            q++;
        str[i] = malloc((size_t)(q - p) + 1);
        memcpy(str[i], p, (size_t)(q - p));
        str[i][q - p] = 0;
        p = q < end ? q + 1 : end;
    }
    char *out = malloc(cap ? cap : 1);
    bool fs = op >= 12;
    char *full[2] = {NULL, NULL};
    if (fs)
    {
        for (int i = 0; i < 2; i++)
        {
            if (strstr(str[i], "..") || str[i][0] == '/')
                continue; /* nothing leaves the sandbox */
            size_t n = strlen(g_sandbox) + 1 + strlen(str[i]) + 1;
            full[i] = malloc(n);
            snprintf(full[i], n, "%s/%s", g_sandbox, str[i]);
        }
        if (!full[0])
            goto done;
        sandbox_reset();
    }
    PathInfo pi;
    int count = 0;
    switch (op)
    {
    case 0:
        (void)path_filename(str[0]);
        (void)path_extension(str[0]);
        break;
    case 1:
        path_directory(str[0], out, cap);
        break;
    case 2:
        (void)path_has_extension(str[0], str[1]);
        break;
    case 3:
        (void)path_is_absolute(str[0]);
        break;
    case 4:
    case 5:
        (void)path_join(out, cap, str[0], str[1]);
        break;
    case 6:
    case 7:
        (void)path_normalize(out, cap, str[0]);
        break;
    case 8:
        (void)path_absolute(out, cap, str[0]);
        break;
    case 9:
    case 10:
        (void)path_relative(out, cap, str[0], str[1]);
        break;
    case 11:
        (void)fs_get_path_info(str[0], &pi);
        (void)file_exists(str[0]);
        (void)dir_exists(str[0]);
        (void)file_size(str[0]);
        (void)file_mod_time(str[0]);
        break;
    case 12:
        (void)fs_create_directory(full[0]);
        (void)dir_exists(full[0]);
        break;
    case 13:
        (void)fs_get_path_info(full[0], &pi);
        (void)file_size(full[0]);
        break;
    case 14:
        (void)io_save_file(full[0], "x", 1);
        (void)file_exists(full[0]);
        break;
    case 15:
    {
        size_t n = 0;
        uint8_t *d = file_read(full[0], &n);
        fs_free(d);
        break;
    }
    case 16:
        (void)fs_create_directory(full[0]);
        (void)fs_enumerate_directory(g_sandbox, true, enum_cb, &count);
        break;
    case 17:
    {
        DirList dl = {0};
        if (dir_list(full[0], &dl))
            dir_list_free(&dl);
        break;
    }
    case 18:
        if (full[1])
            (void)fs_rename_path(full[0], full[1]);
        break;
    case 19:
        (void)fs_remove_path(full[0]);
        break;
    case 20:
        (void)fs_create_directory(full[0]);
        (void)fs_enumerate_directory(full[0], false, enum_cb, &count);
        (void)fs_remove_path(full[0]);
        break;
    case 21:
        (void)io_save_file(full[0], "abc", 3);
        (void)io_save_file(full[0], "defgh", 5); /* the second one replaces the first through the temporary */
        break;
    case 22:
        (void)fs_make_executable(full[0]);
        break;
    default:
    {
        IoStream *s = io_open_file(full[0], str[2][0] ? str[2] : "rb");
        if (s)
            io_close(s);
        break;
    }
    }
done:
    free(full[0]);
    free(full[1]);
    free(out);
    for (int i = 0; i < 3; i++)
        free(str[i]);
    return 0;
}
