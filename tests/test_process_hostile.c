/*
 * test_process_hostile.c - process_spawn on arguments and environments nobody would write by hand: a single argument of
 * 1 MB, 50 000 arguments, an environment variable of 1 MB (the exec of the child fails with E2BIG), a path that is a
 * directory, argument lists with a NULL in the middle, children that die by a signal at once, and a child whose exec fails
 * while another thread holds the lock of stderr (the child used to print its message with fprintf and wait for ever for a
 * lock whose owner does not exist in it). POSIX only. Every child must end within a few seconds and the parent go on.
 */
#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* the child is gone within about 5 s */
static bool ends(int64_t id)
{
    for (int i = 0; i < 500; i++)
    {
        if (!process_is_running(id))
            return true;
        sleep_ms(10);
    }
    process_kill(id);
    return false;
}

static void test_huge_argument(void)
{
    char *big = malloc(1 << 20);
    memset(big, 'x', (1 << 20) - 1);
    big[(1 << 20) - 1] = 0;
    const char *args[] = {big};
    int64_t id = process_spawn("/bin/true", args, 1, false);
    CHECK(id > 0); /* the fork worked, the exec of the child did not (E2BIG): it ends, and says so */
    CHECK(ends(id));
    CHECK(process_exit_code(id) != 0);
    free(big);
}

static void test_many_arguments(void)
{
    enum { N = 50000 };
    const char **args = malloc(N * sizeof *args);
    for (int i = 0; i < N; i++)
        args[i] = "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789";
    int64_t id = process_spawn("/bin/true", args, N, false);
    CHECK(id > 0);
    CHECK(ends(id));
    CHECK(process_exit_code(id) != 0);
    free(args);
}

static void test_huge_environment(void)
{
    char *big = malloc(1 << 20);
    memset(big, 'e', (1 << 20) - 1);
    big[(1 << 20) - 1] = 0;
    setenv("ZP_HUGE_ENV", big, 1);
    int64_t id = process_spawn("/bin/true", NULL, 0, false);
    CHECK(id > 0);
    CHECK(ends(id));
    CHECK(process_exit_code(id) != 0);
    unsetenv("ZP_HUGE_ENV");
    free(big);
    id = process_spawn("/bin/true", NULL, 0, false); /* and without it the same program runs */
    CHECK(id > 0);
    CHECK(ends(id));
    CHECK(process_exit_code(id) == 0);
}

static void test_odd_paths_and_lists(void)
{
    int64_t id = process_spawn("/tmp", NULL, 0, false); /* a directory */
    CHECK(id > 0);
    CHECK(ends(id));
    CHECK(process_exit_code(id) != 0);
    CHECK(process_spawn("", NULL, 0, false) == 0);
    CHECK(process_spawn("/bin/true", NULL, -1, false) == 0);
    CHECK(process_spawn("/bin/true", NULL, 3, false) == 0); /* argc without args */
    const char *holes[] = {"a", NULL, "b"};                 /* a NULL in the middle ends the list there */
    id = process_spawn("/bin/true", holes, 3, false);
    CHECK(id > 0);
    CHECK(ends(id));
    CHECK(process_exit_code(id) == 0);
}

static void test_children_that_die(void)
{
    static const char *const scripts[] = {"kill -9 $$", "kill -SEGV $$", "kill -TERM $$", "exit 255", "exec 1>&- 2>&-; exit 7"};
    for (unsigned i = 0; i < sizeof scripts / sizeof scripts[0]; i++)
    {
        const char *args[] = {"-c", scripts[i]};
        int64_t id = process_spawn("/bin/sh", args, 2, false);
        CHECK(id > 0);
        CHECK(ends(id));
        CHECK(process_exit_code(id) != 0);
        CHECK(process_kill(id)); /* killing what is already gone is not an error */
        CHECK(process_kill(id));
    }
}

static void *hold_stderr(void *arg)
{
    int *go = arg;
    flockfile(stderr);
    __atomic_store_n(go, 1, __ATOMIC_SEQ_CST);
    sleep_ms(2500);
    funlockfile(stderr);
    return NULL;
}

/* fork copies the lock of stderr as it is: held by a thread the child does not have */
static void test_exec_failure_with_stderr_locked(void)
{
    int go = 0;
    pthread_t t;
    pthread_create(&t, NULL, hold_stderr, &go);
    while (!__atomic_load_n(&go, __ATOMIC_SEQ_CST))
        sleep_ms(1);
    int64_t id = process_spawn("/nonexistent/program", NULL, 0, false);
    CHECK(id > 0);
    int gone = 0;
    for (int i = 0; i < 100 && !gone; i++) /* a second: the lock is held for 2.5 s */
    {
        gone = !process_is_running(id);
        sleep_ms(10);
    }
    CHECK(gone);
    if (!gone)
        process_kill(id);
    pthread_join(t, NULL);
}

int main(void)
{
    test_huge_argument();
    test_many_arguments();
    test_huge_environment();
    test_odd_paths_and_lists();
    test_children_that_die();
    test_exec_failure_with_stderr_locked();
    printf("test_process_hostile: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
