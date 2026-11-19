/*
 * test_process_slots.c - the table behind process_spawn (POSIX): a pid the system gives out again, children nobody
 * polls, more children than the table holds, and ids that were never handed out. The test includes the library file
 * itself, to put a record in the table by hand where a real pid reuse cannot be arranged.
 *
 * Nothing here assumes how fast a child ends: a machine with a load of 50 starts a shell in 200 ms. A child that must
 * outlive some step waits on a pipe the test closes when it wants it gone (gate_*), and a step that needs children to
 * have ended waits for that, up to 30 s, instead of sleeping a fixed time. (The first version slept 30 ms per 16 spawns
 * and 300 ms at the end: on a loaded machine one run in three had all 128 slots held by children that had not ended
 * yet, the library refused the spawn as it must, and the test blamed the library.)
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

/* the child has ended, within 30 s */
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

/* ---- children that live as long as the test wants ---- */

static int g_gate_r = -1, g_gate_w = -1;

/* A pipe the children read their standard input from: `read x` returns when the test closes the write end, so a child
   lives exactly as long as the test says, whatever the speed of the machine. */
static void gate_open(void)
{
    int p[2];
    if (pipe(p) != 0)
    {
        perror("pipe");
        exit(2);
    }
    fcntl(p[1], F_SETFD, FD_CLOEXEC); /* a child must not hold the end that releases it */
    g_gate_r = p[0];
    g_gate_w = p[1];
}

static int64_t gate_spawn(const char *script)
{
    int saved = dup(0);
    dup2(g_gate_r, 0);
    const char *args[] = {"-c", script};
    int64_t id = process_spawn("/bin/sh", args, 2, false);
    if (saved >= 0)
    {
        dup2(saved, 0);
        close(saved);
    }
    return id;
}

static void gate_release(void)
{
    close(g_gate_w);
    close(g_gate_r);
    g_gate_w = g_gate_r = -1;
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

/* at least n children have ended and nobody has waited for them, within 30 s (nothing here calls into the library) */
static bool wait_zombies(int n)
{
    for (int i = 0; i < 1500; i++)
    {
        if (zombies() >= n)
            return true;
        sleep_ms(20);
    }
    return false;
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

static int finished_records(void)
{
    int n = 0;
    for (int i = 0; i < PROCESS_CAP; i++)
        if (g_processes[i].pid != 0 && !g_processes[i].running)
            n++;
    return n;
}

static void test_never_polled(void)
{
    /* children that end while nobody asks about them are waited for by the next call into the library */
    const char *quick[] = {"-c", "exit 0"};
    /* twelve children that stay until the test lets them go, then end together */
    gate_open();
    for (int i = 0; i < 12; i++)
        CHECK(gate_spawn("read x; exit 0") > 0);
    int reaped_before = finished_records(); /* what earlier tests left, plus whatever the spawns above waited for */
    gate_release();
    /* all twelve end, and nobody calls into the library until they have */
    bool all_ended = false;
    for (int i = 0; i < 1500 && !all_ended; i++)
    {
        all_ended = zombies() + (finished_records() - reaped_before) >= 12;
        if (!all_ended)
            sleep_ms(20);
    }
    CHECK(all_ended);
    CHECK(zombies() >= 1); /* they are there, in the state of waiting to be reaped */
    int64_t last = process_spawn("/bin/sh", quick, 2, false);
    CHECK(last > 0);
    int left = zombies();
    printf("  never polled: zombies left after the next spawn: %d\n", left);
    CHECK(left <= 1); /* only the one just started may still be unreaped */
    CHECK(finished(last));
    CHECK(zombies() == 0);
}

/* process_spawn, again and again until the table has room: it is refused only while every slot holds a child that
   still runs (a slow machine can have 128 of them, children that end at once included), never while a finished record
   or an unreaped child could make room. */
static int g_refusals, g_bad_refusals;

static int64_t spawn_exit(int code)
{
    char script[32];
    snprintf(script, sizeof script, "exit %d", code);
    const char *args[] = {"-c", script};
    for (int tries = 0; tries < 6000; tries++)
    {
        int64_t id = process_spawn("/bin/sh", args, 2, false);
        if (id > 0)
            return id;
        g_refusals++;
        if (strstr(platform_get_error(), "too many") != NULL)
        {
            int running = 0;
            for (int k = 0; k < PROCESS_CAP; k++)
                if (g_processes[k].pid != 0 && g_processes[k].running)
                    running++;
            if (running != PROCESS_CAP)
                g_bad_refusals++; /* a record that is not running was there to give its slot */
        }
        sleep_ms(5);
    }
    return 0;
}

static void test_more_children_than_slots(void)
{
    /* children that end at once, nobody polls, more of them than the table holds: no spawn fails for good, and a
       record that is still in the table says what its own child exited with. How many are still there depends on how
       many children were running when a spawn needed a slot (a running child's record cannot be dropped, a finished
       one's can), so the count is printed, not checked: test_batches fixes it. */
    const int total = 3 * PROCESS_CAP / 2 + 20;
    int64_t ids[400];
    int failed = 0;
    g_refusals = g_bad_refusals = 0;
    for (int i = 0; i < total; i++)
    {
        ids[i] = spawn_exit(i % 200); /* nobody polls any of them */
        if (ids[i] <= 0)
            failed++;
    }
    CHECK(failed == 0);
    CHECK(g_bad_refusals == 0);
    for (int i = total - PROCESS_CAP; i < total; i++) /* the children that can still have a record */
        if (ids[i] > 0)
            finished(ids[i]);
    int wrong = 0, remembered = 0;
    for (int i = 0; i < total; i++)
    {
        if (ids[i] <= 0 || process_is_running(ids[i]))
        {
            wrong += ids[i] > 0;
            continue;
        }
        int code = process_exit_code(ids[i]);
        if (code == i % 200)
            remembered++;
        else if (code != -1)
            wrong++; /* somebody else's code */
    }
    printf("  %d children, %d refusals while all %d slots held running children, %d records left, %d wrong\n", total,
           g_refusals, PROCESS_CAP, remembered, wrong);
    CHECK(wrong == 0);
    CHECK(remembered >= 1 && process_exit_code(ids[total - 1]) == (total - 1) % 200); /* the newest is always there */
    /* every record of the table has ended by now, so no child of the program can be left unreaped */
    CHECK(zombies() == 0);
}

static void test_batches(void)
{
    /* three batches of PROCESS_CAP / 2 children, each batch ends (and nobody asks) before the next starts: the table
       never holds more running children than it has room for, so the last PROCESS_CAP are remembered with their codes
       and the first batch is forgotten */
    const int batch = PROCESS_CAP / 2;
    int64_t ids[3 * 64];
    int started = 0;
    for (int b = 0; b < 3; b++)
    {
        gate_open();
        for (int i = 0; i < batch; i++)
        {
            char script[48];
            int n = b * batch + i;
            snprintf(script, sizeof script, "read x; exit %d", n % 200);
            ids[n] = gate_spawn(script);
            started += ids[n] > 0;
        }
        gate_release();
        CHECK(wait_zombies(batch)); /* the previous batch was waited for by this batch's first spawn */
    }
    CHECK(started == 3 * batch);
    int forgotten = 0, right = 0, wrong = 0;
    for (int n = 0; n < 3 * batch; n++)
    {
        int code = process_exit_code(ids[n]);
        if (code == -1)
            forgotten++;
        else if (code == n % 200)
            right++;
        else
            wrong++;
    }
    printf("  3 batches of %d: remembered %d, forgotten %d, wrong %d\n", batch, right, forgotten, wrong);
    CHECK(right == PROCESS_CAP && forgotten == batch && wrong == 0);
    CHECK(process_exit_code(ids[0]) == -1 && process_exit_code(ids[batch - 1]) == -1);
    CHECK(process_exit_code(ids[batch]) == batch % 200);
    CHECK(zombies() == 0);
}

static void test_full_table(void)
{
    /* PROCESS_CAP children that all run: the next spawn is refused, and nothing it did is left behind */
    gate_open();
    int64_t ids[PROCESS_CAP];
    int started = 0;
    for (int i = 0; i < PROCESS_CAP; i++)
    {
        ids[i] = gate_spawn("read x; exit 11");
        if (ids[i] > 0)
            started++;
    }
    CHECK(started == PROCESS_CAP);
    const char *quick[] = {"-c", "exit 0"};
    CHECK(process_spawn("/bin/sh", quick, 2, false) == 0);
    CHECK(strstr(platform_get_error(), "too many") != NULL);
    CHECK(process_is_running(ids[0]) && process_is_running(ids[PROCESS_CAP - 1]));

    /* one that is killed makes room, and it is the killed one's record that goes */
    CHECK(process_kill(ids[0]));
    int64_t extra = process_spawn("/bin/sh", quick, 2, false);
    CHECK(extra > 0);
    CHECK(process_exit_code(ids[0]) == -1);
    CHECK(process_is_running(ids[1]));

    /* the rest end by themselves and nobody asks: the records say running, the children are zombies. The next spawn
       finds room all the same, by waiting for them, and the oldest record is the one forgotten. */
    gate_release();
    CHECK(wait_zombies(PROCESS_CAP)); /* the PROCESS_CAP - 1 gated children and the quick one */
    int64_t after = process_spawn("/bin/sh", quick, 2, false);
    CHECK(after > 0);
    CHECK(zombies() <= 1);
    CHECK(process_exit_code(ids[1]) == -1);
    CHECK(process_exit_code(ids[2]) == 11);
    CHECK(process_exit_code(ids[PROCESS_CAP - 1]) == 11);
    CHECK(process_exit_code(extra) == 0);
    CHECK(finished(after));
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
    test_full_table();
    test_more_children_than_slots();
    test_batches();
    test_unknown_ids();
    printf("test_process_slots: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
