/*
 * test_time.c - the clock of time_seconds and time_nanos. It runs from zero at the first use, which is platform_init or,
 * for a program that reads the time before that, the first call: it used to be the machine's uptime (hours) until
 * platform_init, then jump back to zero, and start from zero again at every platform_init after a shutdown.
 */
#include "platform.h"

#include <stdio.h>

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

int main(void)
{
    /* before platform_init: small, and it goes forward */
    uint64_t t0 = time_nanos();
    printf("  time_nanos before platform_init: %llu ns\n", (unsigned long long)t0);
    CHECK(t0 < 1000000000ull);
    time_sleep(20);
    uint64_t t1 = time_nanos();
    CHECK(t1 >= t0 + 15000000ull);
    CHECK(time_seconds() >= (double)t1 / 1e9);

    /* platform_init does not take the clock back to zero */
    CHECK(platform_init());
    uint64_t t2 = time_nanos();
    printf("  time_nanos after platform_init: %llu ns (before it was %llu)\n", (unsigned long long)t2, (unsigned long long)t1);
    CHECK(t2 >= t1);

    /* nor does a shutdown and another init */
    platform_shutdown();
    uint64_t t3 = time_nanos();
    CHECK(t3 >= t2);
    CHECK(platform_init());
    uint64_t t4 = time_nanos();
    CHECK(t4 >= t3);
    platform_shutdown();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
