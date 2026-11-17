#define _DEFAULT_SOURCE /* syscall() for the folder read below */
#define _FILE_OFFSET_BITS 64

#include "io_internal.h"
#include "error_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define IO_CHUNK_MAX ((size_t)0x7ffff000)

static int64_t fd_size(IoStream *s)
{
    struct stat st;
    if (fstat(s->u.fd, &st) != 0)
    {
        error_set("fstat failed: %s", strerror(errno));
        return -1;
    }
    return (int64_t)st.st_size;
}

static int64_t fd_seek(IoStream *s, int64_t offset, IoWhence whence)
{
    static const int k_whence[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    off_t pos = lseek(s->u.fd, (off_t)offset, k_whence[whence]);
    if (pos < 0)
    {
        error_set("seek failed: %s", strerror(errno));
        return -1;
    }
    return (int64_t)pos;
}

static size_t fd_read(IoStream *s, void *dst, size_t n)
{
    uint8_t *p = dst;
    size_t done = 0;
    while (done < n)
    {
        size_t chunk = n - done < IO_CHUNK_MAX ? n - done : IO_CHUNK_MAX;
        ssize_t r = read(s->u.fd, p + done, chunk);
        if (r > 0)
        {
            done += (size_t)r;
            continue;
        }
        if (r == 0)
        {
            s->eof = true;
            break;
        }
        if (errno == EINTR)
            continue;
        error_set("read failed: %s", strerror(errno));
        break;
    }
    return done;
}

static size_t fd_write(IoStream *s, const void *src, size_t n)
{
    const uint8_t *p = src;
    size_t done = 0;
    while (done < n)
    {
        size_t chunk = n - done < IO_CHUNK_MAX ? n - done : IO_CHUNK_MAX;
        ssize_t r = write(s->u.fd, p + done, chunk);
        if (r > 0)
        {
            done += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        error_set("write failed: %s", r < 0 ? strerror(errno) : "no progress");
        break;
    }
    return done;
}

/* fsync on macOS hands the data to the drive and does not wait for the drive to write it from its cache, so a power cut can
   still lose it, and lose the order of writes with it (fsync(2) says so, and names F_FULLFSYNC for programs that need more).
   A file system that has no F_FULLFSYNC (a network volume, FAT) says no, and fsync is what is left. */
static int sync_fd(int fd)
{
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
#endif
    return fsync(fd);
}

static bool fd_flush(IoStream *s)
{
    if (sync_fd(s->u.fd) == 0)
        return true;
    return error_set("fsync failed: %s", strerror(errno));
}

static bool fd_close(IoStream *s)
{
    if (close(s->u.fd) == 0 || errno == EINTR)
        return true;
    return error_set("close failed: %s", strerror(errno));
}

static const IoVTable k_fd_vt = {fd_size, fd_seek, fd_read, fd_write, fd_flush, fd_close};

IoStream *io_platform_open_file(const char *path, int flags)
{
    int oflags = O_CLOEXEC;
    if ((flags & IO_MODE_READ) && (flags & IO_MODE_WRITE))
        oflags |= O_RDWR;
    else if (flags & IO_MODE_WRITE)
        oflags |= O_WRONLY;
    else
        oflags |= O_RDONLY;
    if (flags & IO_MODE_CREATE)
        oflags |= O_CREAT;
    if (flags & IO_MODE_TRUNCATE)
        oflags |= O_TRUNC;
    if (flags & IO_MODE_APPEND)
        oflags |= O_APPEND;

    int fd;
    do
    {
        fd = open(path, oflags, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
    {
        error_set("cannot open '%s': %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))
    {
        close(fd);
        error_set("cannot open '%s': is a directory", path);
        return NULL;
    }

    IoStream *s = io_stream_alloc(&k_fd_vt);
    if (!s)
    {
        close(fd);
        return NULL;
    }
    s->u.fd = fd;
    return s;
}

#define LINK_DEPTH_MAX 40 /* the kernel's own limit on a chain of links */

const char *io_platform_save_target(const char *path, char *buf, size_t cap)
{
    const char *cur = path;
    for (int depth = 0; depth < LINK_DEPTH_MAX; depth++)
    {
        struct stat st;
        if (lstat(cur, &st) != 0 || !S_ISLNK(st.st_mode))
            return cur;
        char link[4096], next[4096];
        ssize_t n = readlink(cur, link, sizeof link - 1);
        if (n <= 0)
            return cur;
        link[n] = '\0';
        if (link[0] == '/')
            snprintf(next, sizeof next, "%s", link);
        else
        {
            char dir[4096];
            path_directory(cur, dir, sizeof dir);
            if (!path_join(next, sizeof next, dir, link))
                return cur;
        }
        if (strlen(next) >= cap)
            return cur;
        memcpy(buf, next, strlen(next) + 1);
        cur = buf;
    }
    error_set("cannot save '%s': %s", path, strerror(ELOOP)); /* a loop of links */
    return NULL;
}

void io_platform_copy_mode(IoStream *s, const char *like)
{
    struct stat st;
    if (stat(like, &st) == 0 && S_ISREG(st.st_mode))
        fchmod(s->u.fd, st.st_mode & 07777); /* a failure leaves the mode the umask gave: nothing to report */
}

unsigned long long io_platform_unique(void)
{
    static unsigned long long counter;
    unsigned long long n = __atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST);
    return ((unsigned long long)getpid() << 32) ^ n;
}

/* The folders looked at already (a 64-bit hash of each, FNV-1a), in a table that never evicts: a folder is looked at
   once in the life of the process, and when 1024 folders have been seen it stops looking at new ones (a program that
   saves in more than a thousand folders is not the one this is for, and a bounded cost is worth more than the leftovers
   of the later ones). Every access is atomic: two threads saving in a new folder at once do not both look at it, and
   nothing is torn. A hash that collides with another folder's means that folder is not looked at. */
#define SEEN_DIRS 1024 /* a power of two */

static bool first_save_in(const char *dir)
{
    static unsigned long long seen[SEEN_DIRS];
    unsigned long long h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)dir; *p; p++)
        h = (h ^ *p) * 1099511628211ull;
    if (h == 0)
        h = 1; /* 0 is the empty slot */
    unsigned slot = (unsigned)h & (SEEN_DIRS - 1);
    for (int i = 0; i < SEEN_DIRS; i++, slot = (slot + 1) & (SEEN_DIRS - 1))
    {
        unsigned long long cur = __atomic_load_n(&seen[slot], __ATOMIC_RELAXED);
        if (cur == h)
            return false;
        if (cur == 0)
        {
            if (__atomic_compare_exchange_n(&seen[slot], &cur, h, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                return true;
            if (cur == h) /* the same folder, taken by another thread a moment ago */
                return false;
        }
    }
    return false;
}

/* Is `name` "<anything>" TEMP_MARK "<hex>.tmp", the name io_save_file gives its temporary, with the process id of the
   save in the upper 32 bits of the number? The number is lower-case hex without leading zeros, as "%llx" writes it, and has
   at least nine digits (a process id of 1 or more); anything else is somebody else's file. The mark is what tells the
   file of a save from a file that merely ends in ".<hex>.tmp". */
static bool temp_owner(const char *name, pid_t *pid)
{
    const char *at = NULL; /* the last mark: the file that was saved may have the mark in its own name */
    for (const char *q = name; (q = strstr(q, IO_TEMP_MARK)) != NULL; q++)
        at = q;
    if (!at || at == name)
        return false;
    const char *p = at + strlen(IO_TEMP_MARK);
    const char *first = p;
    unsigned long long v = 0;
    for (; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'); p++)
        v = (v << 4) | (unsigned long long)(*p <= '9' ? *p - '0' : *p - 'a' + 10);
    long digits = (long)(p - first);
    if (digits < 9 || digits > 16 || *first == '0' || strcmp(p, ".tmp") != 0)
        return false;
    unsigned long long owner = v >> 32;
    if (owner == 0 || owner > (unsigned long long)INT_MAX)
        return false;
    *pid = (pid_t)owner;
    return true;
}

/* What the clean-up does with one name in the folder: a temporary of a save whose process is gone is removed. */
static void stale_check(const char *dir, const char *name)
{
    pid_t owner;
    if (!temp_owner(name, &owner))
        return;
    /* ESRCH is "no such process"; EPERM is a process of somebody else, which is alive. A process id that was given to
       another program since is alive too, and its leftover stays: the safe side. */
    if (kill(owner, 0) == 0 || errno != ESRCH)
        return;
    char file[4096];
    if (snprintf(file, sizeof file, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "", name) < (int)sizeof file)
        unlink(file);
}

/* A save must not read a folder of any size: at most this many directory entries are looked at (the "." and ".." count),
   so the extra time of the first save in a folder has a bound that does not depend on the folder (about 0.04 ms at it on
   Linux, see the commit that set it). In a folder with more entries than that the leftovers are removed only if they come
   within the first STALE_SCAN_MAX entries in the order the file system lists them, which in a hashed folder is not the
   order they were made in; the others stay (docs/SUSPEITAS.md). */
#define STALE_SCAN_MAX 256

static int g_last_scan; /* entries looked at by the latest scan, for the test (a relaxed atomic: any thread may scan) */

int io_platform_last_stale_scan(void)
{
    return __atomic_load_n(&g_last_scan, __ATOMIC_RELAXED);
}

#if defined(__linux__)
/* readdir asks the kernel for 32 KiB of entries at a time (about a thousand names), so counting entries after the fact
   still costs the kernel the whole first buffer: 0.3 ms in a big folder, and several disk blocks when it is cold. The
   kernel's own call is asked for 4 KiB, a hundred names or so, and stops as soon as STALE_SCAN_MAX are in. */
#include <sys/syscall.h>

struct zs_dirent64
{
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

void io_platform_clean_stale_temps(const char *path)
{
    char dir[4096];
    path_directory(path, dir, sizeof dir);
    if (!first_save_in(dir))
        return;
    int fd = open(dir[0] ? dir : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return;
    char buf[4096] __attribute__((aligned(8)));
    int looked = 0;
    while (looked < STALE_SCAN_MAX)
    {
        long got = syscall(SYS_getdents64, fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (long pos = 0; pos < got && looked < STALE_SCAN_MAX;)
        {
            const struct zs_dirent64 *e = (const struct zs_dirent64 *)(buf + pos);
            looked++;
            stale_check(dir, e->d_name);
            pos += e->d_reclen;
        }
    }
    __atomic_store_n(&g_last_scan, looked, __ATOMIC_RELAXED);
    close(fd);
}
#else
void io_platform_clean_stale_temps(const char *path)
{
    char dir[4096];
    path_directory(path, dir, sizeof dir);
    if (!first_save_in(dir))
        return;
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d)
        return;
    struct dirent *e;
    int looked = 0;
    while (looked < STALE_SCAN_MAX && (e = readdir(d)))
    {
        looked++;
        stale_check(dir, e->d_name);
    }
    __atomic_store_n(&g_last_scan, looked, __ATOMIC_RELAXED);
    closedir(d);
}
#endif

static void sync_parent_dir(const char *path)
{
    char dir[4096];
    path_directory(path, dir, sizeof dir);
    int fd = open(dir[0] ? dir : (path[0] == '/' ? "/" : "."), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    sync_fd(fd);
    close(fd);
}

bool io_platform_replace_file(const char *from, const char *to)
{
    if (rename(from, to) != 0)
        return error_set("cannot rename '%s' to '%s': %s", from, to, strerror(errno));
    sync_parent_dir(to);
    return true;
}

bool io_platform_remove_file(const char *path)
{
    return unlink(path) == 0;
}
