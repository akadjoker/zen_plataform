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

static void test_failures(void)
{
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
    test_failures();
    printf("test_process: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
