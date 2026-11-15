#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#if !defined(_WIN32)
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

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

#define ROOT "test_io_tmp"

static void test_memory(void)
{
    const char data[] = "0123456789";
    char buf[16];
    IoStream *s = io_open_memory(data, 10);
    CHECK(s != NULL);
    CHECK(io_size(s) == 10);
    CHECK(io_read(s, buf, 4) == 4 && memcmp(buf, "0123", 4) == 0);
    CHECK(io_tell(s) == 4);
    CHECK(!io_eof(s));
    CHECK(io_seek(s, -2, IO_SEEK_END) == 8);
    CHECK(io_read(s, buf, 8) == 2 && memcmp(buf, "89", 2) == 0);
    CHECK(io_eof(s));
    CHECK(io_seek(s, 1, IO_SEEK_SET) == 1);
    CHECK(!io_eof(s));
    CHECK(io_seek(s, 2, IO_SEEK_CUR) == 3);
    CHECK(io_seek(s, 11, IO_SEEK_SET) == -1);
    CHECK(io_seek(s, -1, IO_SEEK_SET) == -1);
    CHECK(io_tell(s) == 3);
    CHECK(io_write(s, "x", 1) == 0);
    CHECK(platform_get_error()[0] != '\0');
    CHECK(io_close(s));

    size_t n = 0;
    s = io_open_memory(data, 10);
    io_seek(s, 6, IO_SEEK_SET);
    char *rest = io_load(s, &n, true);
    CHECK(rest && n == 4 && strcmp(rest, "6789") == 0);
    fs_free(rest);

    CHECK(io_open_memory(NULL, 4) == NULL);
    s = io_open_memory(NULL, 0);
    CHECK(s && io_size(s) == 0);
    io_close(s);
}

static int released_count;
static void *released_mem;

static void count_release(void *mem)
{
    released_count++;
    released_mem = mem;
    free(mem);
}

/* Seeking by an offset that does not fit once added to the base is an error, not an overflow of the sum (undefined
   behaviour; with UBSan halting, a failure of the test). */
static void test_memory_seek_extremes(void)
{
    static const char data[] = "0123456789";
    IoStream *s = io_open_memory(data, 10);
    CHECK(s != NULL);
    if (!s)
        return;
    CHECK(io_seek(s, 4, IO_SEEK_SET) == 4);
    CHECK(io_seek(s, INT64_MAX, IO_SEEK_END) == -1);
    CHECK(io_seek(s, INT64_MAX, IO_SEEK_CUR) == -1);
    CHECK(io_seek(s, INT64_MAX, IO_SEEK_SET) == -1);
    CHECK(io_seek(s, INT64_MIN, IO_SEEK_END) == -1);
    CHECK(io_seek(s, INT64_MIN, IO_SEEK_CUR) == -1);
    CHECK(io_seek(s, INT64_MIN, IO_SEEK_SET) == -1);
    CHECK(io_tell(s) == 4); /* a refused seek leaves the position alone */
    CHECK(io_seek(s, 0, IO_SEEK_END) == 10);
    io_close(s);
}

static void test_memory_owned(void)
{
    char buf[16];
    char *mem = malloc(10);
    memcpy(mem, "0123456789", 10);
    released_count = 0;
    released_mem = NULL;
    IoStream *s = io_open_memory_owned(mem, 10, count_release);
    CHECK(s != NULL);
    CHECK(io_size(s) == 10);
    CHECK(io_seek(s, 6, IO_SEEK_SET) == 6);
    CHECK(io_read(s, buf, 8) == 4 && memcmp(buf, "6789", 4) == 0);
    CHECK(io_eof(s));
    CHECK(io_write(s, "x", 1) == 0);
    CHECK(released_count == 0);
    CHECK(io_close(s));
    CHECK(released_count == 1 && released_mem == mem);

    mem = malloc(4);
    memcpy(mem, "abcd", 4);
    s = io_open_memory_owned(mem, 4, NULL);
    CHECK(s != NULL);
    CHECK(io_read(s, buf, 4) == 4 && memcmp(buf, "abcd", 4) == 0);
    CHECK(io_close(s));

    size_t n = 0;
    mem = malloc(6);
    memcpy(mem, "owned!", 6);
    released_count = 0;
    char *all = io_load(io_open_memory_owned(mem, 6, count_release), &n, true);
    CHECK(all && n == 6 && strcmp(all, "owned!") == 0);
    CHECK(released_count == 1);
    fs_free(all);

    released_count = 0;
    CHECK(io_open_memory_owned(NULL, 4, count_release) == NULL);
    CHECK(released_count == 0);

    s = io_open_memory_owned(NULL, 0, count_release);
    CHECK(s && io_size(s) == 0);
    CHECK(io_close(s));
    CHECK(released_count == 1 && released_mem == NULL);
}

static void test_file_modes(void)
{
    char buf[32];
    IoStream *s = io_open_file(ROOT "/m.bin", "wb");
    CHECK(s != NULL);
    CHECK(io_write(s, "hello", 5) == 5);
    CHECK(io_size(s) == 5);
    CHECK(io_close(s));

    s = io_open_file(ROOT "/m.bin", "ab");
    CHECK(io_write(s, " world", 6) == 6);
    io_close(s);

    s = io_open_file(ROOT "/m.bin", "rb");
    CHECK(io_size(s) == 11);
    CHECK(io_read(s, buf, sizeof buf) == 11 && memcmp(buf, "hello world", 11) == 0);
    CHECK(io_eof(s));
    CHECK(io_write(s, "x", 1) == 0);
    CHECK(io_seek(s, 6, IO_SEEK_SET) == 6);
    CHECK(!io_eof(s));
    CHECK(io_read(s, buf, 5) == 5 && memcmp(buf, "world", 5) == 0);
    io_close(s);

    s = io_open_file(ROOT "/m.bin", "r+b");
    io_seek(s, 0, IO_SEEK_SET);
    CHECK(io_write(s, "J", 1) == 1);
    CHECK(io_flush(s));
    io_close(s);

    s = io_open_file(ROOT "/m.bin", "w+");
    CHECK(io_size(s) == 0);
    CHECK(io_write(s, "abc", 3) == 3);
    CHECK(io_seek(s, 0, IO_SEEK_SET) == 0);
    CHECK(io_read(s, buf, 3) == 3 && memcmp(buf, "abc", 3) == 0);
    io_close(s);
}

static void test_open_errors(void)
{
    platform_clear_error();
    CHECK(io_open_file(ROOT "/missing.bin", "rb") == NULL);
    CHECK(strstr(platform_get_error(), "missing.bin") != NULL);
    CHECK(io_open_file(ROOT "/m.bin", "x") == NULL);
    CHECK(io_open_file(ROOT "/m.bin", "rw") == NULL);
    CHECK(io_open_file(ROOT "/m.bin", NULL) == NULL);
    CHECK(io_open_file("", "rb") == NULL);
    CHECK(io_open_file(NULL, "rb") == NULL);
    CHECK(io_open_file(ROOT, "rb") == NULL);
    CHECK(io_close(NULL) == false);
}

static void test_load_save(void)
{
    size_t n = 99;
    CHECK(io_save_file(ROOT "/empty.bin", NULL, 0));
    char *e = io_load_file(ROOT "/empty.bin", &n);
    CHECK(e && n == 0 && e[0] == '\0');
    fs_free(e);

    CHECK(io_save_file(ROOT "/s.txt", "first", 5));
    CHECK(io_save_file(ROOT "/s.txt", "second", 6));
    char *t = io_load_file(ROOT "/s.txt", &n);
    CHECK(t && n == 6 && strcmp(t, "second") == 0);
    fs_free(t);
    CHECK(!file_exists(ROOT "/s.txt.tmp"));

    size_t big = (1u << 20) + 123;
    unsigned char *src = malloc(big);
    for (size_t i = 0; i < big; i++)
        src[i] = (unsigned char)(i * 31u + 7u);
    CHECK(io_save_file(ROOT "/big.bin", src, big));
    unsigned char *back = io_load_file(ROOT "/big.bin", &n);
    CHECK(back && n == big && memcmp(back, src, big) == 0);
    fs_free(back);
    free(src);

    CHECK(!io_save_file(ROOT "/nodir/x.bin", "x", 1));
    CHECK(!file_exists(ROOT "/nodir/x.bin.tmp"));
    CHECK(io_load_file(ROOT "/nodir/x.bin", &n) == NULL);
    CHECK(!io_save_file(NULL, "x", 1));
    CHECK(!io_save_file(ROOT "/n.bin", NULL, 3));
}

#if !defined(_WIN32)
/* Replacing a file keeps its permissions: a file that was private (0600) must not become readable by others because
   the program saved it again. A file that is new gets what the umask says, as before. */
static unsigned file_mode(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (unsigned)(st.st_mode & 07777) : 0xFFFFu;
}

static void test_save_keeps_mode(void)
{
    umask(022);
    CHECK(io_save_file(ROOT "/mode.dat", "one", 3));
    CHECK(file_mode(ROOT "/mode.dat") == 0644);
    chmod(ROOT "/mode.dat", 0600);
    CHECK(io_save_file(ROOT "/mode.dat", "two", 3));
    CHECK(file_mode(ROOT "/mode.dat") == 0600);
    chmod(ROOT "/mode.dat", 0640);
    CHECK(io_save_file(ROOT "/mode.dat", "three", 5));
    CHECK(file_mode(ROOT "/mode.dat") == 0640);
    size_t n = 0;
    char *back = io_load_file(ROOT "/mode.dat", &n);
    CHECK(back && n == 5 && strcmp(back, "three") == 0);
    fs_free(back);
    remove(ROOT "/mode.dat");
}

/* Saves of one path from several threads at once (two instances of a program do the same across processes): each
   save has a temporary file of its own, so none fails for want of the file another one renamed, the result is one
   whole payload and not a mix, and nothing is left behind. */
#define RACE_THREADS 4
#define RACE_ROUNDS 40
#define RACE_SIZE (512 * 1024)

static int g_race_failed[RACE_THREADS];
static volatile int g_race_go;

static void *race_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    unsigned char *payload = malloc(RACE_SIZE);
    memset(payload, 'A' + id, RACE_SIZE);
    while (!__atomic_load_n(&g_race_go, __ATOMIC_SEQ_CST))
    {
    }
    for (int i = 0; i < RACE_ROUNDS; i++)
        if (!io_save_file(ROOT "/race/shared.dat", payload, RACE_SIZE))
            g_race_failed[id]++;
    free(payload);
    return NULL;
}

static bool count_entry(const char *path, PathType type, void *user)
{
    (void)path;
    (void)type;
    (*(int *)user)++;
    return true;
}

static void test_concurrent_save(void)
{
    CHECK(dir_make(ROOT "/race"));
    pthread_t threads[RACE_THREADS];
    for (intptr_t i = 0; i < RACE_THREADS; i++)
        pthread_create(&threads[i], NULL, race_worker, (void *)i);
    __atomic_store_n(&g_race_go, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < RACE_THREADS; i++)
        pthread_join(threads[i], NULL);
    int failed = 0;
    for (int i = 0; i < RACE_THREADS; i++)
        failed += g_race_failed[i];
    printf("  concurrent saves: %d of %d failed\n", failed, RACE_THREADS * RACE_ROUNDS);
    CHECK(failed == 0);

    size_t n = 0;
    unsigned char *data = io_load_file(ROOT "/race/shared.dat", &n);
    CHECK(data != NULL && n == RACE_SIZE);
    bool whole = data != NULL && n == RACE_SIZE && data[0] >= 'A' && data[0] < 'A' + RACE_THREADS;
    for (size_t i = 1; whole && i < n; i++)
        whole = data[i] == data[0];
    CHECK(whole);
    fs_free(data);

    int entries = 0;
    CHECK(fs_enumerate_directory(ROOT "/race", false, count_entry, &entries));
    CHECK(entries == 1); /* shared.dat, and no temporary file left */
    remove(ROOT "/race/shared.dat");
    remove(ROOT "/race");
}
#endif

/* procfs reports a size of 0 for files that do hold text: the size alone cannot say a file is empty. */
static void test_unsized_files(void)
{
#if defined(__linux__)
    size_t n = 0;
    char *status = io_load_file("/proc/self/status", &n);
    CHECK(status != NULL);
    CHECK(n > 0 && status && strlen(status) == n);
    CHECK(status && strstr(status, "Name:") != NULL);
    fs_free(status);

    char *cpuinfo = file_read_text("/proc/cpuinfo");
    CHECK(cpuinfo && cpuinfo[0] != '\0');
    fs_free(cpuinfo);

    uint8_t *raw = file_read("/proc/self/stat", &n);
    CHECK(raw && n > 0);
    fs_free(raw);
#endif
}

static void test_assets(void)
{
    CHECK(io_save_file(ROOT "/asset.txt", "packed", 6));
    asset_set_root(ROOT);
    IoStream *s = io_open_asset("asset.txt");
    CHECK(s != NULL);
    CHECK(io_size(s) == 6);
    CHECK(io_write(s, "x", 1) == 0);
    io_close(s);
    size_t n = 0;
    uint8_t *a = asset_read("asset.txt", &n);
    CHECK(a && n == 6 && memcmp(a, "packed", 6) == 0);
    fs_free(a);
    CHECK(io_open_asset("none.txt") == NULL);
    CHECK(io_open_asset("") == NULL);
    asset_set_root(NULL);
}

static void cleanup(void)
{
    remove(ROOT "/m.bin");
    remove(ROOT "/empty.bin");
    remove(ROOT "/s.txt");
    remove(ROOT "/big.bin");
    remove(ROOT "/asset.txt");
    remove(ROOT);
}

int main(void)
{
    dir_make(ROOT);

    test_memory();
    test_memory_seek_extremes();
    test_memory_owned();
    test_file_modes();
    test_open_errors();
    test_load_save();
#if !defined(_WIN32)
    test_concurrent_save();
    test_save_keeps_mode();
#endif
    test_unsized_files();
    test_assets();

    cleanup();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
