/*
 * test_boot.h - what every macOS test does before main: the output goes out a line at a time and the test's own alarm says so.
 * A test that is stopped (its alarm(), ctest's timeout, a crash) used to leave nothing of what it had printed, because stdout is
 * a pipe under ctest and a pipe is block buffered: the log of the one run of the macOS job would have had a red test and no
 * line of why. Plain C, no Apple header, so test_gl_worker_cocoa.c can have it too.
 */
#ifndef TEST_BOOT_H
#define TEST_BOOT_H

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static void zp_test_on_alarm(int sig)
{
    static const char msg[] = "TIMEOUT: the test's own alarm() went off, it did not finish in time\n";
    (void)sig;
    if (write(2, msg, sizeof msg - 1) < 0)
    {
    }
    _exit(124);
}

__attribute__((constructor)) static void zp_test_boot(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGALRM, zp_test_on_alarm);
}

#endif
