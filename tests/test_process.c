/*
 * test_process.c - child processes: spawn with arguments, poll, read the exit
 * code once the child is gone, kill a child that never ends.
 */
#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define SHELL "cmd.exe"
#define SHELL_FLAG "/c"
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>
#define SHELL "/bin/sh"
#define SHELL_FLAG "-c"
static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
#define SLEEP_MS(ms) sleep_ms(ms)
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

static bool wait_finished(int64_t id)
{
    for (int i = 0; i < 500; i++)
    {
        if (!process_is_running(id))
            return true;
        SLEEP_MS(10);
    }
    return false;
}

static void test_exit_code(void)
{
    const char *ok[] = {SHELL_FLAG, "exit 0"};
    int64_t a = process_spawn(SHELL, ok, 2, false);
    CHECK(a > 0);
    CHECK(wait_finished(a));
    CHECK(process_exit_code(a) == 0);

    const char *seven[] = {SHELL_FLAG, "exit 7"};
    int64_t b = process_spawn(SHELL, seven, 2, false);
    CHECK(b > 0);
    CHECK(wait_finished(b));
    CHECK(process_exit_code(b) == 7);
    CHECK(process_exit_code(b) == 7);
    CHECK(!process_is_running(b));
    CHECK(process_kill(b));
    CHECK(process_exit_code(b) == 7);
}

static void test_arguments(void)
{
#if !defined(_WIN32)
    const char *args[] = {SHELL_FLAG, "test \"$1\" = \"two words\" && test \"$2\" = x", "sh", "two words", "x"};
    int64_t id = process_spawn(SHELL, args, 5, false);
    CHECK(id > 0);
    CHECK(wait_finished(id));
    CHECK(process_exit_code(id) == 0);
#endif
}

static void test_kill(void)
{
#if defined(_WIN32)
    const char *args[] = {"/c", "ping -n 30 127.0.0.1 > nul"};
#else
    const char *args[] = {"-c", "exec sleep 30"};
#endif
    int64_t id = process_spawn(SHELL, args, 2, false);
    CHECK(id > 0);
    SLEEP_MS(50);
    CHECK(process_is_running(id));
    CHECK(process_exit_code(id) == -1);
    CHECK(process_kill(id));
    CHECK(!process_is_running(id));
    CHECK(process_exit_code(id) != -1);
}

#if !defined(_WIN32)
/* process_kill sends SIGKILL, which a child can neither ignore nor catch, and which also ends a child that is stopped:
   one that ignores every other signal, and one that stopped itself, are both gone when the call returns. The call
   waits for the child with the table's lock held, so these must not leave it hung. */
static void test_kill_stubborn(void)
{
    const char *deaf[] = {"-c", "trap '' TERM INT HUP QUIT USR1 USR2 PIPE ALRM; while :; do sleep 1; done"};
    const char *stopped[] = {"-c", "kill -STOP $$; sleep 30"};
    int64_t a = process_spawn(SHELL, deaf, 2, false);
    int64_t b = process_spawn(SHELL, stopped, 2, false);
    CHECK(a > 0 && b > 0);
    SLEEP_MS(200); /* the shell has set its traps and stopped itself */
    CHECK(process_is_running(a) && process_is_running(b));
    CHECK(process_kill(a));
    CHECK(!process_is_running(a));
    CHECK(process_kill(b));
    CHECK(!process_is_running(b));
    CHECK(process_exit_code(a) != -1 && process_exit_code(b) != -1);
}

/* A shell with a background child: process_kill_tree ends both, process_kill alone would leave the child. */
static void test_kill_tree(void)
{
    const char *pid_file = "process_tree_child.pid";
    remove(pid_file);
    char script[256];
    snprintf(script, sizeof script, "sleep 60 & echo $! > %s; wait", pid_file);
    const char *args[] = {"-c", script};
    int64_t id = process_spawn(SHELL, args, 2, false);
    CHECK(id > 0);
    long child = 0;
    for (int i = 0; i < 100 && !child; i++)
    {
        SLEEP_MS(20);
        FILE *f = fopen(pid_file, "r");
        if (f)
        {
            if (fscanf(f, "%ld", &child) != 1)
                child = 0;
            fclose(f);
        }
    }
    CHECK(child > 0);
    CHECK(kill((pid_t)child, 0) == 0);
    CHECK(process_kill_tree(id));
    CHECK(!process_is_running(id));
    int gone = 0;
    for (int i = 0; i < 100 && !gone; i++)
    {
        gone = kill((pid_t)child, 0) != 0 && errno == ESRCH;
        if (!gone)
            SLEEP_MS(20);
    }
    CHECK(gone);
    CHECK(process_kill_tree(id));
    CHECK(!process_kill_tree(0));
    remove(pid_file);
}
#endif

/* An exit code of 259 is STILL_ACTIVE to GetExitCodeProcess: it must not read as a process that never ends. The
   same child is a plain exit code elsewhere. */
static void test_exit_code_259(void)
{
#if defined(_WIN32)
    const char *args[] = {"/c", "exit 259"};
    int64_t id = process_spawn("cmd.exe", args, 2, false);
#else
    const char *args[] = {"-c", "exit 259"}; /* 259 & 0xff = 3 */
    int64_t id = process_spawn("/bin/sh", args, 2, false);
#endif
    CHECK(id > 0);
    CHECK(wait_finished(id));
    CHECK(!process_is_running(id));
#if defined(_WIN32)
    CHECK(process_exit_code(id) == 259);
#else
    CHECK(process_exit_code(id) == 3);
#endif
}

static void test_failures(void)
{
    CHECK(process_exit_code(2147480000) == -1); /* an id nobody got from process_spawn */
    CHECK(!process_is_running(2147480000));
    CHECK(process_spawn(NULL, NULL, 0, false) == 0);
    CHECK(!process_is_running(0));
    CHECK(process_exit_code(-5) == -1);
    CHECK(!process_kill(0));

    int64_t id = process_spawn("/nonexistent/program", NULL, 0, false);
#if defined(_WIN32)
    CHECK(id == 0);
#else
    CHECK(id > 0);
    CHECK(wait_finished(id));
    CHECK(process_exit_code(id) != 0);
#endif
}

int main(void)
{
    test_exit_code();
    test_arguments();
    test_kill();
#if !defined(_WIN32)
    test_kill_stubborn();
    test_kill_tree();
#endif
    test_exit_code_259();
    test_failures();
    printf("test_process: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
