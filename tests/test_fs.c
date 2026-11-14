/*
 * test_fs.c - PHASE 1 (fs). Exercises os.c headless: file round-trips, queries,
 * path-string helpers, recursive dir make/list, and the asset_* read path against
 * a configured root. Runs in the test's working directory, cleaning up after.
 */
#include "platform.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
        {                                                          \
            g_pass++;                                              \
        }                                                          \
        else                                                       \
        {                                                          \
            g_fail++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

#define ROOT "test_fs_tmp"

static void test_file_roundtrip(void)
{
    const char bytes[] = {0x00, 0x01, 0x02, 'z', (char)0xFF};
    CHECK(file_write(ROOT "/a.bin", bytes, sizeof bytes));
    CHECK(file_exists(ROOT "/a.bin"));
    CHECK(!file_exists(ROOT "/missing.bin"));
    CHECK(file_size(ROOT "/a.bin") == (int64_t)sizeof bytes);

    size_t n = 0;
    uint8_t *back = file_read(ROOT "/a.bin", &n);
    CHECK(back != NULL);
    CHECK(n == sizeof bytes);
    CHECK(back && memcmp(back, bytes, sizeof bytes) == 0);
    fs_free(back);

    CHECK(file_write_text(ROOT "/note.txt", "hello"));
    char *txt = file_read_text(ROOT "/note.txt");
    CHECK(txt && strcmp(txt, "hello") == 0); /* NUL-terminated */
    fs_free(txt);

    CHECK(file_mod_time(ROOT "/a.bin") > 0);
}

static void test_path_helpers(void)
{
    CHECK(strcmp(path_filename("a/b/c.txt"), "c.txt") == 0);
    CHECK(strcmp(path_filename("noslash"), "noslash") == 0);
    CHECK(strcmp(path_extension("a/b/c.txt"), ".txt") == 0);
    CHECK(strcmp(path_extension("a/b/c"), "") == 0);
    CHECK(strcmp(path_extension(".hidden"), "") == 0); /* leading dot is not an extension */

    char dir[256];
    path_directory("a/b/c.txt", dir, sizeof dir);
    CHECK(strcmp(dir, "a/b") == 0);
    path_directory("noslash", dir, sizeof dir);
    CHECK(dir[0] == '\0');

    /* The folder of something directly under the root is the root, not "" (which means the working directory),
       and a doubled separator before the name does not stay in the result. */
#if !defined(_WIN32)
    path_directory("/file.txt", dir, sizeof dir);
    CHECK(strcmp(dir, "/") == 0);
    path_directory("/", dir, sizeof dir);
    CHECK(strcmp(dir, "/") == 0);
    path_directory("//file.txt", dir, sizeof dir);
    CHECK(strcmp(dir, "/") == 0);
    path_directory("/home/M\xc3\xbasica/v.mp3", dir, sizeof dir);
    CHECK(strcmp(dir, "/home/M\xc3\xbasica") == 0);
    path_directory("/home", dir, sizeof dir);
    CHECK(strcmp(dir, "/") == 0);
    path_directory("a//b", dir, sizeof dir);
    CHECK(strcmp(dir, "a") == 0);
    path_directory("/a//b", dir, sizeof dir);
    CHECK(strcmp(dir, "/a") == 0);
    path_directory("a/b/", dir, sizeof dir);
    CHECK(strcmp(dir, "a/b") == 0);
#else
    path_directory("C:\\file.txt", dir, sizeof dir);
    CHECK(strcmp(dir, "C:/") == 0 || strcmp(dir, "C:\\") == 0);
#endif

    CHECK(path_has_extension("image.PNG", "png")); /* case-insensitive */
    CHECK(path_has_extension("image.png", ".png"));
    CHECK(!path_has_extension("image.png", "jpg"));
}

static void test_directories(void)
{
    CHECK(dir_make(ROOT "/sub/deep"));
    CHECK(dir_exists(ROOT "/sub/deep"));
    CHECK(!dir_exists(ROOT "/sub/none"));
    CHECK(dir_make(ROOT "/sub/deep")); /* idempotent */

    CHECK(file_write_text(ROOT "/sub/one.txt", "1"));
    CHECK(file_write_text(ROOT "/sub/two.txt", "2"));

    DirList list;
    CHECK(dir_list(ROOT "/sub", &list));
    int files = 0;
    bool saw_deep = false;
    for (int i = 0; i < list.count; i++)
    {
        const char *name = path_filename(list.paths[i]);
        if (strcmp(name, "one.txt") == 0 || strcmp(name, "two.txt") == 0)
            files++;
        if (strcmp(name, "deep") == 0)
            saw_deep = true;
    }
    CHECK(files == 2);
    CHECK(saw_deep);
    dir_list_free(&list);

    const char *cwd = dir_current();
    CHECK(cwd && cwd[0] != '\0');
    const char *app = dir_app();
    CHECK(app && app[0] != '\0');
}

static void test_assets(void)
{
    CHECK(file_write_text(ROOT "/hello.txt", "from asset root"));
    asset_set_root(ROOT);
    CHECK(asset_exists("hello.txt"));
    CHECK(!asset_exists("nope.txt"));
    char *txt = asset_read_text("hello.txt");
    CHECK(txt && strcmp(txt, "from asset root") == 0);
    fs_free(txt);
    asset_set_root(NULL);
}

static void cleanup(void)
{
    remove(ROOT "/a.bin");
    remove(ROOT "/note.txt");
    remove(ROOT "/hello.txt");
    remove(ROOT "/sub/one.txt");
    remove(ROOT "/sub/two.txt");
    remove(ROOT "/sub/deep");
    remove(ROOT "/sub");
    remove(ROOT);
}

/* Paths of more than 260 characters, which the Windows file functions only take with the \\?\ prefix (the library
   adds it): create the folders, write, read, list, rename and remove, at the bottom of a path of ~380 characters. */
static bool count_entries(const char *path, PathType type, void *user)
{
    (void)path;
    (void)type;
    (*(int *)user)++;
    return true;
}

static void test_long_paths(void)
{
    char dir[1024] = ROOT;
    size_t n = strlen(dir);
    for (int i = 0; i < 14; i++)
        n += (size_t)snprintf(dir + n, sizeof dir - n, "/level_%02d_with_a_longish_name", i);
    CHECK(n > 380);
    CHECK(fs_create_directory(dir));
    CHECK(dir_exists(dir));
    CHECK(fs_create_directory(dir)); /* again: nothing to do */

    char file[1100], moved[1100];
    snprintf(file, sizeof file, "%s/deep.txt", dir);
    snprintf(moved, sizeof moved, "%s/moved.txt", dir);
    CHECK(file_write_text(file, "deep down"));
    CHECK(file_exists(file));
    char *back = file_read_text(file);
    CHECK(back && strcmp(back, "deep down") == 0);
    fs_free(back);
    CHECK(file_size(file) == 9);

    CHECK(fs_rename_path(file, moved));
    CHECK(!file_exists(file) && file_exists(moved));

    int seen = 0;
    bool ok = fs_enumerate_directory(dir, false, count_entries, &seen);
    CHECK(ok);
    CHECK(seen == 1); /* the moved file */

    CHECK(fs_remove_path(moved));
    CHECK(!file_exists(moved));
    /* the folders go from the bottom up */
    for (int i = 13; i >= 0; i--)
    {
        CHECK(fs_remove_path(dir));
        char *cut = strrchr(dir, '/');
        if (cut)
            *cut = 0;
    }
}

int main(void)
{
    dir_make(ROOT);

    test_file_roundtrip();
    test_path_helpers();
    test_directories();
    test_assets();
    test_long_paths();

    cleanup();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
