/*
 * test_process_slots.c - the table behind process_spawn (POSIX): a pid the system gives out again, children nobody
 * polls, more children than the table holds, and ids that were never handed out. The test includes the library file
 * itself, to put a record in the table by hand where a real pid reuse cannot be arranged.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t test_fork(void);
#define fork() test_fork()
#undef _DEFAULT_SOURCE
#undef _POSIX_C_SOURCE
#include "../src/process_posix.c"
#undef fork

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

/* ---- a fork that can leave an old finished record in the table under the new child's pid ---- */

static int g_stale_slot = -1;
static int g_stale_code;

static pid_t test_fork(void)
{
    pid_t pid = (fork)();
    if (pid > 0 && g_stale_slot >= 0)
    {
        /* what the table holds after the system gave this pid out again: the earlier owner's finished record */
        g_processes[g_stale_slot].pid = pid;
        g_processes[g_stale_slot].running = false;
        g_processes[g_stale_slot].exit_code = g_stale_code;
        g_stale_slot = -1;
    }
    return pid;
}

static bool finished(int64_t id)
{
    for (int i = 0; i < 500; i++)
    {
        if (!process_is_running(id))
            return true;
        sleep_ms(5);
    }
    return false;
}

/* children of this process that have ended and not been waited for */
static int zombies(void)
{
    int n = 0;
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char path[300], line[512];
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(line, sizeof line, f))
        {
            char *close = strrchr(line, ')');
            char state = 0;
            int ppid = 0;
            if (close && sscanf(close + 1, " %c %d", &state, &ppid) == 2 && ppid == (int)getpid() && state == 'Z')
                n++;
        }
        fclose(f);
    }
    closedir(d);
    return n;
}

static void test_pid_reuse(void)
{
    /* an earlier child ends and leaves its finished record in slot 0 */
    const char *quick[] = {"-c", "exit 3"};
    int64_t first = process_spawn("/bin/sh", quick, 2, false);
    CHECK(first > 0);
    CHECK(finished(first));
    CHECK(g_processes[0].pid == (pid_t)first);

    /* the next child gets a pid that record already carries */
    const char *slow[] = {"-c", "exec sleep 30"};
    g_stale_slot = 0;
    g_stale_code = 42;
    int64_t id = process_spawn("/bin/sh", slow, 2, false);
    CHECK(id > 0);
    sleep_ms(30);
    CHECK(process_is_running(id));        /* the old record did not answer for the new process */
    CHECK(process_exit_code(id) == -1);   /* ... nor does it give its exit code while it runs */
    CHECK(process_kill(id));
    CHECK(!process_is_running(id));
    CHECK(process_exit_code(id) == 9);    /* SIGKILL, not the old 42 */
}

static void test_never_polled(void)
{
    /* children that end while nobody asks about them are waited for by the next call into the library */
    const char *quick[] = {"-c", "exit 0"};
    for (int i = 0; i < 12; i++)
        CHECK(process_spawn("/bin/sh", quick, 2, false) > 0);
    sleep_ms(300);
    CHECK(zombies() >= 8); /* they are there, in the state of waiting to be reaped */
    int64_t last = process_spawn("/bin/sh", quick, 2, false);
    CHECK(last > 0);
    int left = zombies();
    printf("  never polled: zombies left after the next spawn: %d\n", left);
    CHECK(left <= 1); /* only the one just started may still be unreaped */
    CHECK(finished(last));
    CHECK(zombies() == 0);
}

static void test_more_children_than_slots(void)
{
    /* the table remembers the last PROCESS_CAP children; more than that never makes a spawn fail, and the ones
       remembered are the newest, with their own codes */
    const int total = 3 * PROCESS_CAP / 2 + 20;
    int64_t ids[400];
    int failed = 0;
    for (int i = 0; i < total; i++)
    {
        char script[32];
        snprintf(script, sizeof script, "exit %d", i % 200);
        const char *args[] = {"-c", script};
        ids[i] = process_spawn("/bin/sh", args, 2, false); /* nobody polls any of them */
        if (ids[i] <= 0)
            failed++;
        if (i % 16 == 15)
            sleep_ms(30);
    }
    CHECK(failed == 0);
    sleep_ms(300);
    int wrong = 0, remembered = 0;
    for (int i = total - PROCESS_CAP / 2; i < total; i++) /* the newest half of the table */
    {
        if (ids[i] <= 0)
            continue;
        if (process_is_running(ids[i]))
        {
            wrong++;
            continue;
        }
        int code = process_exit_code(ids[i]);
        if (code == i % 200)
            remembered++;
        else
            wrong++;
    }
    printf("  %d children, newest %d: remembered %d, wrong %d\n", total, PROCESS_CAP / 2, remembered, wrong);
    CHECK(remembered == PROCESS_CAP / 2 && wrong == 0);
    CHECK(zombies() == 0);
}

static void test_unknown_ids(void)
{
    /* an id nobody got from process_spawn: no exit code, and no kill of whatever process has that number */
    CHECK(process_exit_code(2147480000) == -1);
    CHECK(!process_is_running(2147480000));

    pid_t stranger = (fork)(); /* a process of the program's own that process_spawn knows nothing about */
    if (stranger == 0)
    {
        sleep(30);
        _exit(0);
    }
    CHECK(stranger > 0);
    CHECK(process_exit_code(stranger) == -1);
    CHECK(!process_kill(stranger));
    sleep_ms(30);
    CHECK(kill(stranger, 0) == 0); /* still alive */
    int status = 0;
    CHECK(waitpid(stranger, &status, WNOHANG) == 0);
    kill(stranger, SIGKILL);
    waitpid(stranger, &status, 0);
}

int main(void)
{
    test_pid_reuse();
    test_never_polled();
    test_more_children_than_slots();
    test_unknown_ids();
    printf("test_process_slots: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
