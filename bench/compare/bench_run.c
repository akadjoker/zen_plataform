/* bench_run.c - runs one bench program, and for "events" is the generator of the events (the same for every library).
     bench_run once <program> <args...>
       sets BENCH_T0 to the CLOCK_MONOTONIC time just before the exec and runs the program, which prints its line
     bench_run events motion|key N <program> <args...>
       starts the program (args: events motion|key N [gl]), waits for it to be ready, moves the pointer into its
       window with XTest, sends N motion or N key events (alternating press and release of "a"), syncs, and only then lets the program read them, so what is measured is the program taking the events
       from the X connection. The program may be a wrapper (valgrind ...) as long as it ends in the bench program. */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void say(int fd)
{
    if (write(fd, "go\n", 3) != 3)
        exit(1);
}

static void key(Display *d, KeySym ks, Bool down)
{
    XTestFakeKeyEvent(d, XKeysymToKeycode(d, ks), down, CurrentTime);
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    if (strcmp(argv[1], "events") != 0)
    {
        char buf[32];
        snprintf(buf, sizeof buf, "%lld", now_ns());
        setenv("BENCH_T0", buf, 1);
        execvp(argv[2], argv + 2);
        perror("execv");
        return 1;
    }
    if (argc < 5)
        return 2;
    int in[2], out[2];
    if (pipe(in) || pipe(out))
        return 1;
    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[1]);
        close(out[0]);
        execvp(argv[4], argv + 4);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    FILE *fr = fdopen(out[0], "r");
    char line[512];
    long n = atol(argv[3]);
    int motion = strcmp(argv[2], "motion") == 0;
    Display *d = XOpenDisplay(NULL);
    if (!d || !fr)
        return 1;
    int ok = 0;
    if (fgets(line, sizeof line, fr) && strncmp(line, "READY1", 6) == 0)
    {
        XTestFakeMotionEvent(d, -1, 200, 150, CurrentTime);
        XSync(d, False);
        say(in[1]);
        if (fgets(line, sizeof line, fr) && strncmp(line, "READY2", 6) == 0)
        {
            for (long i = 0; i < n; i++)
            {
                if (motion)
                    XTestFakeMotionEvent(d, -1, 200 + (int)((i & 1) ^ 1) * 40, 150, CurrentTime);
                else
                    key(d, XK_a, (i & 1) == 0);
                if ((i & 1023) == 1023)
                    XSync(d, False); /* keeps the generator from running a long way ahead of the server */
            }
            XSync(d, False);
            say(in[1]);
            ok = 1;
        }
    }
    if (ok && fgets(line, sizeof line, fr))
        fputs(line, stdout);
    int st = 0;
    waitpid(pid, &st, 0);
    XCloseDisplay(d);
    return ok && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 1;
}
