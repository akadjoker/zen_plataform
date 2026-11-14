/*
 * crash.c - a report written when the program dies from a fatal signal.
 *
 * On POSIX systems the handlers for SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT write a text report to a file
 * (the text the program gave, the signal and the address that faulted, whatever the program's callback adds,
 * and a backtrace where the C library has one), then run the signal's default action again so the program still
 * dies the way it would have (core dump, exit status of the shell, debugger). What runs inside the handler uses
 * only open, write, close, sigaction, raise and backtrace (its first call, made when the handlers are installed,
 * loads what it needs): no malloc, no stdio, no locks. Elsewhere there is nothing to install.
 */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include "platform.h"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define CRASH_HAS_BACKTRACE 1
#endif

#define CRASH_PATH_MAX 1024
#define CRASH_HEADER_MAX 1024
#define CRASH_STACK_BYTES (64 * 1024)
#define CRASH_FRAMES 64

static const int g_signals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
#define CRASH_SIGNALS ((int)(sizeof(g_signals) / sizeof(g_signals[0])))

static char g_path[CRASH_PATH_MAX];
static char g_header[CRASH_HEADER_MAX];
static CrashCallback g_extra;
static void *g_user;
static bool g_installed;
static struct sigaction g_previous[CRASH_SIGNALS];
static char g_stack[CRASH_STACK_BYTES];
static volatile sig_atomic_t g_busy;

void crash_write(int fd, const char *text)
{
    if (fd < 0 || !text)
        return;
    size_t left = 0;
    while (text[left])
        left++;
    while (left > 0)
    {
        ssize_t n = write(fd, text, left);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return;
        }
        text += n;
        left -= (size_t)n;
    }
}

static void put_unsigned(int fd, unsigned long long value, unsigned base)
{
    char digits[2 * sizeof(unsigned long long) + 1];
    int at = (int)sizeof(digits) - 1;
    digits[at] = 0;
    do
    {
        digits[--at] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value && at > 0);
    crash_write(fd, digits + at);
}

static const char *signal_name(int sig)
{
    switch (sig)
    {
    case SIGSEGV:
        return "SIGSEGV, invalid memory access";
    case SIGBUS:
        return "SIGBUS, bad memory access";
    case SIGFPE:
        return "SIGFPE, arithmetic error";
    case SIGILL:
        return "SIGILL, illegal instruction";
    case SIGABRT:
        return "SIGABRT, abort";
    default:
        return "signal";
    }
}

static void write_report(int fd, int sig, const siginfo_t *info)
{
    crash_write(fd, g_header);
    crash_write(fd, "\nsignal ");
    put_unsigned(fd, (unsigned long long)sig, 10);
    crash_write(fd, " (");
    crash_write(fd, signal_name(sig));
    crash_write(fd, ")");
    if (info && sig != SIGABRT)
    {
        crash_write(fd, " at address 0x");
        put_unsigned(fd, (unsigned long long)(size_t)info->si_addr, 16);
    }
    crash_write(fd, "\nprocess ");
    put_unsigned(fd, (unsigned long long)getpid(), 10);
    crash_write(fd, ", unix time ");
    put_unsigned(fd, (unsigned long long)time(NULL), 10);
    crash_write(fd, "\n\n");
    if (g_extra)
        g_extra(fd, g_user);
#ifdef CRASH_HAS_BACKTRACE
    void *frames[CRASH_FRAMES];
    int count = backtrace(frames, CRASH_FRAMES);
    crash_write(fd, "\nbacktrace of the thread that crashed (");
    put_unsigned(fd, (unsigned long long)count, 10);
    crash_write(fd, " frames, newest first):\n");
    backtrace_symbols_fd(frames, count, fd);
#else
    crash_write(fd, "\n(no backtrace on this system)\n");
#endif
}

static void on_signal(int sig, siginfo_t *info, void *context)
{
    (void)context;
    // Whatever happens next, the process ends: a second crash while writing, or another thread stuck behind
    // this one, is stopped by the alarm.
    signal(SIGALRM, SIG_DFL);
    alarm(10);
    if (g_busy)
        for (;;)
            pause(); // another thread is writing the report; it ends the process when it is done
    g_busy = 1;
    int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        write_report(fd, sig, info);
        close(fd);
    }
    // Back to the default action (SA_RESETHAND did it already), and take it.
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigaction(sig, &action, NULL);
    raise(sig);
}

bool crash_handler_install(const char *path, const char *header, CrashCallback extra, void *user)
{
    if (!path || !path[0] || strlen(path) >= CRASH_PATH_MAX)
        return false;
    if (g_installed)
        crash_handler_remove();
    strncpy(g_path, path, CRASH_PATH_MAX - 1);
    g_path[CRASH_PATH_MAX - 1] = 0;
    strncpy(g_header, header ? header : "", CRASH_HEADER_MAX - 1);
    g_header[CRASH_HEADER_MAX - 1] = 0;
    g_extra = extra;
    g_user = user;
    g_busy = 0;
#ifdef CRASH_HAS_BACKTRACE
    void *warm[2];
    backtrace(warm, 2); // the first call loads the unwinder: not in the handler
#endif
    // A stack of its own for this thread, so that a crash by stack overflow can still be reported.
    stack_t stack;
    stack.ss_sp = g_stack;
    stack.ss_size = sizeof(g_stack);
    stack.ss_flags = 0;
    sigaltstack(&stack, NULL);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND | SA_NODEFER;
    for (int i = 0; i < CRASH_SIGNALS; i++)
        sigaction(g_signals[i], &action, &g_previous[i]);
    g_installed = true;
    return true;
}

void crash_handler_remove(void)
{
    if (!g_installed)
        return;
    for (int i = 0; i < CRASH_SIGNALS; i++)
        sigaction(g_signals[i], &g_previous[i], NULL);
    g_installed = false;
}

#else /* Windows and the web: nothing is installed */

void crash_write(int fd, const char *text)
{
    (void)fd;
    (void)text;
}

bool crash_handler_install(const char *path, const char *header, CrashCallback extra, void *user)
{
    (void)path;
    (void)header;
    (void)extra;
    (void)user;
    return false;
}

void crash_handler_remove(void)
{
}

#endif
