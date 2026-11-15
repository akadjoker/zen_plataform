#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#if !defined(_WIN32)
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "io_internal.h" /* io_platform_last_stale_scan */
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

/* Saving to a symbolic link writes the file it leads to and leaves the link a link (a settings file kept in a synced
   folder, or in a dotfiles repository, and linked from where the program looks): it used to be replaced by a regular
   file, and the real one was never written again. */
static bool count_entry(const char *path, PathType type, void *user);

static bool is_link(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

static void test_save_through_link(void)
{
    CHECK(dir_make(ROOT "/lk/real"));
    CHECK(dir_make(ROOT "/lk/other"));
    CHECK(io_save_file(ROOT "/lk/real/target.txt", "old", 3));
    /* a link in the same folder, one with a relative path through "..", a link to a link, and one to a file that is
       not there yet */
    CHECK(symlink("real/target.txt", ROOT "/lk/direct") == 0);
    CHECK(symlink("../real/target.txt", ROOT "/lk/other/up") == 0);
    CHECK(symlink("direct", ROOT "/lk/chain") == 0);
    CHECK(symlink("real/new.txt", ROOT "/lk/dangling") == 0);

    CHECK(io_save_file(ROOT "/lk/direct", "via direct", 10));
    CHECK(is_link(ROOT "/lk/direct"));
    size_t n = 0;
    char *t = io_load_file(ROOT "/lk/real/target.txt", &n);
    CHECK(t && n == 10 && strcmp(t, "via direct") == 0);
    fs_free(t);

    CHECK(io_save_file(ROOT "/lk/other/up", "via up", 6));
    CHECK(is_link(ROOT "/lk/other/up"));
    t = io_load_file(ROOT "/lk/real/target.txt", &n);
    CHECK(t && n == 6 && strcmp(t, "via up") == 0);
    fs_free(t);

    CHECK(io_save_file(ROOT "/lk/chain", "via chain", 9));
    CHECK(is_link(ROOT "/lk/chain") && is_link(ROOT "/lk/direct"));
    t = io_load_file(ROOT "/lk/real/target.txt", &n);
    CHECK(t && n == 9 && strcmp(t, "via chain") == 0);
    fs_free(t);

    CHECK(io_save_file(ROOT "/lk/dangling", "made", 4));
    CHECK(is_link(ROOT "/lk/dangling"));
    t = io_load_file(ROOT "/lk/real/new.txt", &n);
    CHECK(t && n == 4 && strcmp(t, "made") == 0);
    fs_free(t);

    /* nothing is left in either folder but what belongs there */
    int entries = 0;
    CHECK(fs_enumerate_directory(ROOT "/lk/real", false, count_entry, &entries));
    CHECK(entries == 2); /* target.txt and new.txt */

    CHECK(symlink("loop2", ROOT "/lk/loop1") == 0);
    CHECK(symlink("loop1", ROOT "/lk/loop2") == 0);
    CHECK(!io_save_file(ROOT "/lk/loop1", "x", 1)); /* a loop of links: an error, not a hang */

    remove(ROOT "/lk/loop1");
    remove(ROOT "/lk/loop2");
    remove(ROOT "/lk/dangling");
    remove(ROOT "/lk/chain");
    remove(ROOT "/lk/direct");
    remove(ROOT "/lk/other/up");
    remove(ROOT "/lk/real/target.txt");
    remove(ROOT "/lk/real/new.txt");
    remove(ROOT "/lk/real");
    remove(ROOT "/lk/other");
    remove(ROOT "/lk");
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

/* A program that dies in the middle of a save leaves "<path>.zsave-<number>.tmp" for ever (the number of
   io_platform_unique, the process id in its upper half). The first save in a folder removes those of processes that
   no longer exist, from the whole folder, and nothing else. The child is killed in the middle of a save by the kernel
   and not by a timed kill: a limit of 64 KiB on the size of a file it writes ends it with SIGXFSZ at the first byte
   past it. */
typedef struct
{
    int count;      /* files whose name ends in ".tmp" and contains ".zsave-" */
    int64_t size;   /* the size of the last one found */
    char last[512]; /* and its path */
} TempScan;

static bool scan_temps(const char *path, PathType type, void *user)
{
    TempScan *t = user;
    const char *name = path_filename(path);
    size_t n = strlen(name);
    if (type == PATH_TYPE_FILE && strstr(name, ".zsave-") != NULL && n > 4 && strcmp(name + n - 4, ".tmp") == 0)
    {
        PathInfo info;
        t->count++;
        t->size = fs_get_path_info(path, &info) ? info.size : -1;
        snprintf(t->last, sizeof t->last, "%s", path);
    }
    return true;
}

static void make_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f)
    {
        fputs(text, f);
        fclose(f);
    }
}

/* the temporary of a save of `base` by process `pid`, as io_save_file names it, in `dir` */
static char *temp_path(const char *dir, const char *base, pid_t pid, unsigned n)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s/%s.zsave-%llx.tmp", dir, base, ((unsigned long long)pid << 32) ^ n);
    return strdup(buf);
}

static bool dir_exists_at(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

#define STALE ROOT "/stale"
#define MAX_WIPE 1024

static char g_wipe[MAX_WIPE][256];
static int g_wipe_n;

static bool collect_name(const char *path, PathType type, void *user)
{
    (void)type;
    (void)user;
    if (g_wipe_n < MAX_WIPE)
        snprintf(g_wipe[g_wipe_n++], sizeof g_wipe[0], "%s", path);
    return true;
}

static void wipe_directory(const char *dir)
{
    g_wipe_n = 0;
    fs_enumerate_directory(dir, false, collect_name, NULL);
    for (int i = 0; i < g_wipe_n; i++)
        fs_remove_path(g_wipe[i]);
    fs_remove_path(dir);
}

static void test_stale_temps(void)
{
    wipe_directory(STALE);
    wipe_directory(STALE "_other");
    CHECK(dir_make(STALE));
    CHECK(dir_make(STALE "_other"));

    /* the process that dies in the middle of a save */
    pid_t dead = fork();
    CHECK(dead >= 0);
    if (dead == 0)
    {
        struct rlimit none = {0, 0}, small = {64 * 1024, 64 * 1024};
        setrlimit(RLIMIT_CORE, &none); /* no core file of a death that is meant to happen */
        setrlimit(RLIMIT_FSIZE, &small);
        signal(SIGXFSZ, SIG_DFL);
        unsigned char *big = malloc(1 << 20);
        memset(big, 'x', 1 << 20);
        io_save_file(STALE "/a.dat", big, 1 << 20); /* dies inside the write */
        _exit(0);                                   /* only if it did not */
    }
    int status = 0;
    CHECK(waitpid(dead, &status, 0) == dead);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGXFSZ);
    TempScan left = {0, 0, ""};
    CHECK(fs_enumerate_directory(STALE, false, scan_temps, &left));
    CHECK(left.count == 1 && left.size == 64 * 1024); /* the half-written temporary of the save that died */
    CHECK(!file_exists(STALE "/a.dat"));              /* and the file itself was never made */
    char *litter = strdup(left.last);

    /* a process that lives, and names that look nearly like a temporary but are not */
    pid_t alive = fork();
    CHECK(alive >= 0);
    if (alive == 0)
    {
        pause();
        _exit(0);
    }
    char *keep[16];
    int nkeep = 0;
    char *name;
#define KEEP(expr)                       \
    do                                   \
    {                                    \
        name = (expr);                   \
        CHECK(name != NULL);             \
        make_file(name, "not yours");    \
        keep[nkeep++] = name;            \
    } while (0)
    KEEP(strdup(STALE "/a.dat.tmp"));                          /* the fixed name an old version used */
    KEEP(strdup(STALE "/a.dat.zsave-1.tmp"));                  /* a short number */
    KEEP(strdup(STALE "/a.dat.zsave-1234abcd.tmp"));           /* eight digits: process 0 */
    KEEP(strdup(STALE "/a.dat.zsave-0123456789a.tmp"));        /* a leading zero */
    KEEP(strdup(STALE "/.zsave-0123456789a.tmp"));             /* no name before the mark */
    KEEP(temp_path(STALE, "a.dat", getpid(), 6));              /* of this process: another thread's save */
    KEEP(temp_path(STALE, "a.dat", alive, 7));                 /* of a process that lives */
    KEEP(strdup(STALE "/a.dat.9999999999.tmp"));               /* without the mark: the name of round 3, it may be anyone's */
    char *bak = temp_path(STALE, "a.dat", dead, 8);
    bak = realloc(bak, strlen(bak) + 5);
    strcat(bak, ".bak");                                       /* something is added after the .tmp */
    KEEP(bak);
    char *upper = temp_path(STALE, "a.dat", dead, 0xAB);
    for (char *c = strstr(upper, ".zsave-") + 7; *c != '.'; c++)
        if (*c >= 'a' && *c <= 'f')
            *c = (char)(*c - 'a' + 'A');
    KEEP(upper);                                               /* upper-case hex is not what "%llx" writes */
    char *dir_keep = temp_path(STALE, "a.dat", dead, 9);
    CHECK(dir_make(dir_keep));                                 /* a folder with the name of a temporary is left alone */
    char *orphan = temp_path(STALE, "a.dat", dead, 10);        /* a temporary of a dead process, made by hand */
    make_file(orphan, "orphan");
    char *orphan_b = temp_path(STALE, "b.dat", dead, 11);      /* of another file in the same folder: the folder is cleaned, not the path */
    make_file(orphan_b, "orphan of b");
    char *elsewhere = temp_path(STALE "_other", "a.dat", dead, 12); /* in a folder where nothing is saved yet */
    make_file(elsewhere, "other folder");

    CHECK(file_exists(litter));
    CHECK(io_save_file(STALE "/a.dat", "fresh", 5));
    CHECK(!file_exists(litter)); /* what the dead save left is gone, ... */
    CHECK(!file_exists(orphan) && !file_exists(orphan_b)); /* ... and so are the others of dead processes in the folder */
    CHECK(file_exists(elsewhere));
    CHECK(dir_exists_at(dir_keep));
    for (int i = 0; i < nkeep; i++)
        CHECK(file_exists(keep[i]));
    size_t n = 0;
    char *back = io_load_file(STALE "/a.dat", &n);
    CHECK(back && n == 5 && memcmp(back, "fresh", 5) == 0);
    fs_free(back);

    /* a folder is looked at once: what a dead save leaves in it later waits for the next run of the program, and
       a folder not seen yet is looked at when the first save in it works */
    char *later = temp_path(STALE, "a.dat", dead, 13);
    make_file(later, "later");
    CHECK(io_save_file(STALE "/b.dat", "again", 5));
    CHECK(file_exists(later));
    CHECK(io_save_file(STALE "_other/c.dat", "c", 1));
    CHECK(!file_exists(elsewhere));

    /* The look at a folder is bounded: a small folder is read whole (its entries, "." and ".." among them), a big one for
       256 entries and no more, whatever it holds: a save never reads a folder of any size */
    char fpath[256];
    wipe_directory(STALE "_small");
    CHECK(dir_make(STALE "_small"));
    for (int i = 0; i < 5; i++)
    {
        snprintf(fpath, sizeof fpath, STALE "_small/f%d.dat", i);
        make_file(fpath, "x");
    }
    char *small_orphan = temp_path(STALE "_small", "f0.dat", dead, 21);
    make_file(small_orphan, "orphan");
    CHECK(io_save_file(STALE "_small/new.dat", "n", 1));
    CHECK(io_platform_last_stale_scan() == 5 + 1 + 1 + 2 + 0); /* five files, the orphan, the saved one, "." and ".." */
    CHECK(!file_exists(small_orphan));

    wipe_directory(STALE "_big");
    CHECK(dir_make(STALE "_big"));
    for (int i = 0; i < 900; i++)
    {
        snprintf(fpath, sizeof fpath, STALE "_big/file%04d.dat", i);
        make_file(fpath, "x");
    }
    CHECK(io_save_file(STALE "_big/new.dat", "n", 1));
    CHECK(io_platform_last_stale_scan() == 256);

    kill(alive, SIGKILL);
    waitpid(alive, &status, 0);
    free(small_orphan);
    wipe_directory(STALE "_small");
    wipe_directory(STALE "_big");
    for (int i = 0; i < nkeep; i++)
        free(keep[i]);
    free(litter);
    free(dir_keep);
    free(orphan);
    free(orphan_b);
    free(elsewhere);
    free(later);
    wipe_directory(STALE);
    wipe_directory(STALE "_other");
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
    test_save_through_link();
    test_stale_temps();
#endif
    test_unsized_files();
    test_assets();

    cleanup();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
