/*
 * test_process_pidreuse.c - process_kill_tree against a process id the system gives out again, with the real thing: a
 * pid namespace of the test's own (unshare(2): user + pid + mount namespaces, no privilege needed where the system lets
 * a user make them), in which the next pid is set by hand (/proc/sys/kernel/ns_last_pid, which root of the namespace may
 * write). Linux only; it skips (77) when the namespaces cannot be made.
 *
 * What it settles (POSIX, "Process ID Reuse": a process id is not reused while a process group with that id exists):
 *   1. A leader ends and is waited for, a grandchild stays: the number stays taken (a fork that is offered it gets another)
 *      and process_kill_tree on the finished leader ends the grandchild.
 *   2. The group is empty, the number is free and goes to somebody else, who leads a group of its own (setsid): a call to
 *      process_kill_tree on the old record must not signal it. (The code before this test signalled -pid whenever the
 *      record was finished.)
 */
#define _GNU_SOURCE
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_pass, g_fail;

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
            g_pass++;                                              \
        else                                                       \
        {                                                          \
            g_fail++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static bool write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return false;
    size_t n = strlen(text);
    bool ok = write(fd, text, n) == (ssize_t)n;
    close(fd);
    return ok;
}

static bool set_last_pid(pid_t pid)
{
    char text[16];
    snprintf(text, sizeof text, "%d", (int)pid);
    return write_file("/proc/sys/kernel/ns_last_pid", text);
}

/* a child that gets the pid `want` if the system is free to give it (the next pid is set just before the fork), and
   optionally leads a group of its own, as a shell or a service does */
static pid_t fork_pid(pid_t want, bool own_group)
{
    if (!set_last_pid(want - 1))
        return -1;
    pid_t pid = fork();
    if (pid == 0)
    {
        if (own_group)
            setsid();
        for (;;)
            pause();
    }
    return pid;
}

static bool finished(int64_t id)
{
    for (int i = 0; i < 6000; i++)
    {
        if (!process_is_running(id))
            return true;
        sleep_ms(5);
    }
    return false;
}

static long read_pid(const char *file)
{
    for (int i = 0; i < 1500; i++)
    {
        FILE *f = fopen(file, "r");
        if (f)
        {
            long pid = 0;
            int n = fscanf(f, "%ld", &pid);
            fclose(f);
            if (n == 1 && pid > 0)
                return pid;
        }
        sleep_ms(20);
    }
    return 0;
}

static char g_file[96];

/* runs as pid 1 of the new namespace, with /proc of it */
static int in_namespace(void)
{
    if (!set_last_pid(1000))
        return 77; /* not root of the namespace, or no /proc of it */
    char script[200];
    snprintf(script, sizeof script, "sleep 60 & echo $! > %s; exit 0", g_file);
    const char *args[] = {"-c", script};
    int64_t leader = process_spawn("/bin/sh", args, 2, false);
    CHECK(leader > 0);
    long member = read_pid(g_file);
    CHECK(member > 0);
    CHECK(finished(leader)); /* the leader is gone and waited for; its grandchild is not */
    printf("  leader %lld ended, member %ld left in its group\n", (long long)leader, member);

    /* 1. the number stays taken: a fork offered it gets another one */
    pid_t probe = fork_pid((pid_t)leader, false);
    CHECK(probe > 0 && probe != (pid_t)leader);
    if (probe > 0)
    {
        kill(probe, SIGKILL);
        waitpid(probe, NULL, 0);
    }
    CHECK(process_kill_tree(leader)); /* ends the member through the group of the finished leader */
    if (member > 0)
    {
        int status = 0;
        CHECK(waitpid((pid_t)member, &status, 0) == (pid_t)member && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    }

    /* 2. nothing is left in the group: the number is free, and goes to a stranger who leads a group of its own */
    CHECK(!process_is_running(leader)); /* a call into the library: the record sees that the group is empty */
    pid_t stranger = fork_pid((pid_t)leader, true);
    CHECK(stranger == (pid_t)leader); /* the same number, given out again */
    for (int i = 0; i < 1000 && stranger > 0 && getpgid(stranger) != stranger; i++)
        sleep_ms(5);
    CHECK(stranger > 0 && getpgid(stranger) == stranger);
    CHECK(process_kill_tree(leader)); /* the record is the old one's: nothing of ours is left to end */
    sleep_ms(100);
    int status = 0;
    bool alive = stranger > 0 && waitpid(stranger, &status, WNOHANG) == 0 && kill(stranger, 0) == 0;
    if (!alive)
        printf("  the stranger that got the number was signalled\n");
    CHECK(alive);
    if (stranger > 0)
    {
        kill(stranger, SIGKILL);
        waitpid(stranger, &status, 0);
    }
    remove(g_file);
    printf("test_process_pidreuse: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* the namespace's processes leave with _exit */
    snprintf(g_file, sizeof g_file, "process_pidreuse_%d.pid", (int)getpid());
    remove(g_file);
    fflush(stdout);
    pid_t outer = fork();
    if (outer == 0)
    {
        char map[64];
        uid_t uid = getuid();
        gid_t gid = getgid();
        if (unshare(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS) != 0)
            _exit(77);
        snprintf(map, sizeof map, "0 %u 1", (unsigned)uid);
        if (!write_file("/proc/self/setgroups", "deny") || !write_file("/proc/self/uid_map", map))
            _exit(77);
        snprintf(map, sizeof map, "0 %u 1", (unsigned)gid);
        if (!write_file("/proc/self/gid_map", map))
            _exit(77);
        pid_t inner = fork(); /* pid 1 of the new pid namespace */
        if (inner == 0)
        {
            if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0 || mount("proc", "/proc", "proc", 0, NULL) != 0)
                _exit(77);
            _exit(in_namespace());
        }
        int status = 0;
        waitpid(inner, &status, 0);
        _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    }
    int status = 0;
    waitpid(outer, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (code == 77)
        printf("test_process_pidreuse: skipped (no user + pid namespaces here)\n");
    remove(g_file);
    return code;
}
