/* bench_once.c - Wayland builds: sets BENCH_T0 (CLOCK_MONOTONIC, ns) to now and runs the program given, so that the
   start-up mode of bench_main.c measures from the exec like it does under bench_run (X11). usage: bench_once cmd args... */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char t0[32];
    snprintf(t0, sizeof t0, "%lld", (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec);
    setenv("BENCH_T0", t0, 1);
    execvp(argv[1], argv + 1);
    perror("bench_once");
    return 127;
}
