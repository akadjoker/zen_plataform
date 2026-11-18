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
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
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

/* The children die on purpose: no core dump, which on some systems means a crash reporter run for each of them (a
   second or more, with the process waiting for it). A process that is not dumpable gets none, whatever the limit. */
static void no_core_dump(void)
{
    struct rlimit none = {0, 0};
    setrlimit(RLIMIT_CORE, &none);
#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
}

/* The recursion is the point: it runs out of stack on purpose. A compiler that sees it never returns says so. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winfinite-recursion"
#endif
static void overflow(volatile int depth)
{
    volatile char block[4096];
    block[0] = (char)depth;
    overflow(depth + 1);
    block[1] = block[0];
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static int overflow_in_thread(void *user)
{
    (void)user;
    overflow(0);
    return 0;
}

// kind: 0 invalid memory access, 1 abort, 2 stack overflow, 3 stack overflow in a thread made by create_thread
static void run_child(const char *path, int kind, int *signal_seen, char **report)
{
    remove(path);
    pid_t pid = fork();
    if (pid == 0)
    {
        no_core_dump();
        if (!crash_handler_install(path, "ZenTest 1.0 report", extra_section, (void *)"hello from the callback"))
            _exit(3);
        if (kind == 0)
            *(volatile int *)0 = 1;
        else if (kind == 1)
            abort();
        else if (kind == 2)
            overflow(0);
        else
            join_thread(create_thread(overflow_in_thread, NULL, "overflow"));
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

    /* sigaltstack is per thread: a thread of the library's own that runs out of stack needs a stack for the handler
       too, or the kernel cannot run it and the process dies with no report */
    run_child(path, 3, &sig, &report);
#if defined(__APPLE__)
    /* On macOS the guard page of a thread's stack is reported as SIGBUS (the main thread's as SIGSEGV, above): the check is the
       same, for the signal the system really gives (run 44125308694: the child died of signal 10). */
    CHECK(sig == SIGBUS);
    CHECK(report != NULL && strstr(report, "signal 10 (SIGBUS") != NULL);
#else
    CHECK(sig == SIGSEGV);
    CHECK(report != NULL && strstr(report, "signal 11 (SIGSEGV") != NULL);
#endif
    free(report);
    remove(path);
}

/* Several threads fault at the same instant. Only one of them may write the report: the others wait for it
   to end the process. The callback stays in the handler for a while, so a second writer is not missed. */
#define STORM_THREADS 6
#define STORM_ROUNDS 150

static volatile int g_storm_arrived;

static void slow_extra_section(int fd, void *user)
{
    (void)user;
    crash_write(fd, "extra section: once\n");
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do
        clock_gettime(CLOCK_MONOTONIC, &t);
    while ((t.tv_sec - t0.tv_sec) * 1000000000L + (t.tv_nsec - t0.tv_nsec) < 3000000L);
}

static void *storm_thread(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&g_storm_arrived, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&g_storm_arrived, __ATOMIC_SEQ_CST) < STORM_THREADS)
    {
    }
    *(volatile int *)0 = 1;
    return NULL;
}

static int count_of(const char *text, const char *needle)
{
    int n = 0;
    for (const char *p = text; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

static void test_crash_in_many_threads(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/zen_crash_storm_%d.txt", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    int doubled = 0, missing = 0, wrong_death = 0;
    for (int round = 0; round < STORM_ROUNDS; round++)
    {
        remove(path);
        pid_t pid = fork();
        if (pid == 0)
        {
            no_core_dump();
            if (!crash_handler_install(path, "storm", slow_extra_section, NULL))
                _exit(3);
            pthread_t threads[STORM_THREADS];
            g_storm_arrived = 0; /* this thread faults too, so STORM_THREADS in all */
            for (int i = 0; i < STORM_THREADS - 1; i++)
                pthread_create(&threads[i], NULL, storm_thread, NULL);
            storm_thread(NULL);
            _exit(4);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV)
            wrong_death++;
        char *report = read_all(path);
        if (!report)
            missing++;
        else
        {
            if (count_of(report, "extra section: once") != 1 || count_of(report, "signal 11 (SIGSEGV") != 1)
                doubled++;
            free(report);
        }
    }
    remove(path);
    printf("threads crashing together: %d rounds, %d wrote the report twice, %d without report, %d died other\n",
           STORM_ROUNDS, doubled, missing, wrong_death);
    CHECK(doubled == 0);
    CHECK(missing == 0);
    CHECK(wrong_death == 0);
}

/* A report callback that itself crashes: the process still dies of the signal, at once (not by the alarm 10 s
   later), and what was written before is in the file. */
static void faulty_extra_section(int fd, void *user)
{
    (void)user;
    crash_write(fd, "before the second fault\n");
    *(volatile int *)0 = 2;
}

static void test_fault_inside_the_handler(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/zen_crash_nested_%d.txt", tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    remove(path);
    time_t start = time(NULL);
    pid_t pid = fork();
    if (pid == 0)
    {
        no_core_dump();
        if (!crash_handler_install(path, "nested", faulty_extra_section, NULL))
            _exit(3);
        *(volatile int *)0 = 1;
        _exit(4);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    CHECK(time(NULL) - start < 5);
    char *report = read_all(path);
    CHECK(report != NULL && strstr(report, "before the second fault") != NULL);
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
    test_crash_in_many_threads();
    test_fault_inside_the_handler();
#else
    CHECK(!crash_handler_install("crash.txt", "x", NULL, NULL));
    crash_handler_remove();
#endif
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
