/*
 * test_crash.c - the crash report: a child process installs the handler and dies from a signal; the parent
 * checks how it died and what the report says. Elsewhere than POSIX the install reports that it is not supported.
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
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

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)

static void extra_section(int fd, void *user)
{
    crash_write(fd, "extra section: ");
    crash_write(fd, (const char *)user);
    crash_write(fd, "\n");
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *text = (char *)calloc(1, 65536);
    size_t n = fread(text, 1, 65535, f);
    text[n] = 0;
    fclose(f);
    return text;
}

static void overflow(volatile int depth)
{
    volatile char block[4096];
    block[0] = (char)depth;
    overflow(depth + 1);
    block[1] = block[0];
}

// kind: 0 invalid memory access, 1 abort, 2 stack overflow
static void run_child(const char *path, int kind, int *signal_seen, char **report)
{
    remove(path);
    pid_t pid = fork();
    if (pid == 0)
    {
        if (!crash_handler_install(path, "ZenTest 1.0 report", extra_section, (void *)"hello from the callback"))
            _exit(3);
        if (kind == 0)
            *(volatile int *)0 = 1;
        else if (kind == 1)
            abort();
        else
            overflow(0);
        _exit(4);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    *signal_seen = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
    *report = read_all(path);
}

static void test_crash_reports(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/zen_crash_test_%d.txt", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    int sig = 0;
    char *report = NULL;

    run_child(path, 0, &sig, &report);
    CHECK(sig == SIGSEGV); /* it still dies of the signal */
    CHECK(report != NULL);
    if (report)
    {
        CHECK(strstr(report, "ZenTest 1.0 report") != NULL);
        CHECK(strstr(report, "signal 11 (SIGSEGV") != NULL);
        CHECK(strstr(report, "at address 0x0") != NULL);
        CHECK(strstr(report, "extra section: hello from the callback") != NULL);
        CHECK(strstr(report, "backtrace") != NULL);
        free(report);
    }

    run_child(path, 1, &sig, &report);
    CHECK(sig == SIGABRT);
    CHECK(report != NULL && strstr(report, "signal 6 (SIGABRT") != NULL);
    free(report);

    run_child(path, 2, &sig, &report);
    CHECK(sig == SIGSEGV);
    CHECK(report != NULL && strstr(report, "signal 11 (SIGSEGV") != NULL); /* written from the alternate stack */
    free(report);
    remove(path);
}

static void test_arguments(void)
{
    CHECK(!crash_handler_install(NULL, "x", NULL, NULL));
    CHECK(!crash_handler_install("", "x", NULL, NULL));
    /* installing and removing leaves the program alive */
    CHECK(crash_handler_install("zen_crash_unused.txt", NULL, NULL, NULL));
    crash_handler_remove();
    crash_handler_remove();
}

#endif

int main(void)
{
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    test_arguments();
    test_crash_reports();
#else
    CHECK(!crash_handler_install("crash.txt", "x", NULL, NULL));
    crash_handler_remove();
#endif
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
