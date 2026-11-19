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

#if !defined(_WIN32)
/* ---- process_kill_tree: the cases around the leader ---- */

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* the pid a script wrote to a file, within 30 s; 0 if it never did */
static long read_pid_file(const char *name)
{
    for (int i = 0; i < 1500; i++)
    {
        FILE *f = fopen(name, "r");
        if (f)
        {
            long pid = 0;
            int n = fscanf(f, "%ld", &pid);
            fclose(f);
            if (n == 1 && pid > 0)
                return pid;
        }
        SLEEP_MS(20);
    }
    return 0;
}

/* a process that has not ended: a zombie is an ended one that its parent has not waited for yet */
static bool process_alive(long pid)
{
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    FILE *f = fopen(path, "r");
    if (f)
    {
        char state = '?';
        if (fgets(line, sizeof line, f))
        {
            char *close = strrchr(line, ')');
            if (close && close[1] == ' ')
                state = close[2];
        }
        fclose(f);
        return state != 'Z' && state != 'X';
    }
    return kill((pid_t)pid, 0) == 0; /* no /proc here: the number answers while the process, zombie or not, exists */
}

static bool ends_soon(long pid)
{
    for (int i = 0; i < 1500; i++)
    {
        if (!process_alive(pid))
            return true;
        SLEEP_MS(20);
    }
    return false;
}

/* The leader ends, a grandchild does not: the group id stays taken while any member lives (POSIX: a process id is not
   reused while a process group with that id exists), so the number is still the group's, and process_kill_tree on the
   finished leader ends the grandchild. Nothing left after that: a second call is not an error. */
static void test_kill_tree_leader_gone(void)
{
    const char *file = "process_tree_leader_gone.pid";
    remove(file);
    char script[256];
    snprintf(script, sizeof script, "sleep 60 & echo $! > %s; exit 0", file);
    const char *args[] = {"-c", script};
    int64_t id = process_spawn(SHELL, args, 2, false);
    CHECK(id > 0);
    long child = read_pid_file(file);
    CHECK(child > 0);
    CHECK(wait_finished(id));
    CHECK(process_exit_code(id) == 0);
    CHECK(process_alive(child)); /* the grandchild outlived the leader, which was waited for */
    CHECK(process_kill(id));     /* a finished process: nothing to do, the grandchild is not touched */
    CHECK(process_alive(child));
    CHECK(process_kill_tree(id));
    CHECK(ends_soon(child));
    CHECK(process_kill_tree(id)); /* again: nothing left in the group */
    CHECK(process_exit_code(id) == 0);
    remove(file);
}

/* process_kill ends the child only; its grandchild stays, and process_kill_tree afterwards still reaches it */
static void test_kill_then_kill_tree(void)
{
    const char *file = "process_tree_kill_then.pid";
    remove(file);
    char script[256];
    snprintf(script, sizeof script, "sleep 60 & echo $! > %s; wait", file);
    const char *args[] = {"-c", script};
    int64_t id = process_spawn(SHELL, args, 2, false);
    CHECK(id > 0);
    long child = read_pid_file(file);
    CHECK(child > 0);
    CHECK(process_kill(id));
    CHECK(!process_is_running(id));
    SLEEP_MS(50);
    CHECK(process_alive(child));
    CHECK(process_kill_tree(id));
    CHECK(ends_soon(child));
    remove(file);
}

/* A child that starts its own session or group is out of reach: process_kill_tree ends the group process_spawn made and
   nothing else. (platform.h says so.) The test ends the escaped one itself. */
static void test_kill_tree_escaped(void)
{
    const char *probe[] = {"-c", "command -v setsid > /dev/null"};
    int64_t p = process_spawn(SHELL, probe, 2, false);
    CHECK(p > 0 && wait_finished(p));
    if (process_exit_code(p) != 0)
    {
        printf("  setsid is not installed: the escaped-child case is skipped\n");
        return;
    }
    const char *kept = "process_tree_kept.pid", *escaped = "process_tree_escaped.pid";
    remove(kept);
    remove(escaped);
    char script[512];
    snprintf(script, sizeof script, "setsid sh -c 'echo $$ > %s; exec sleep 60' & sleep 60 & echo $! > %s; wait", escaped, kept);
    const char *args[] = {"-c", script};
    int64_t id = process_spawn(SHELL, args, 2, false);
    CHECK(id > 0);
    long in_group = read_pid_file(kept), out_of_group = read_pid_file(escaped);
    CHECK(in_group > 0 && out_of_group > 0);
    CHECK(process_kill_tree(id));
    CHECK(!process_is_running(id));
    CHECK(ends_soon(in_group));
    SLEEP_MS(100);
    CHECK(process_alive(out_of_group)); /* it left the group: not reached */
    if (out_of_group > 0)
        kill((pid_t)out_of_group, SIGKILL);
    CHECK(out_of_group <= 0 || ends_soon(out_of_group));
    remove(kept);
    remove(escaped);
}

/* A leader that ends by itself while process_kill_tree is called: whichever comes first, the call succeeds, the leader is
   finished with a code that is its own or SIGKILL's, and the grandchild is gone. The leader waits on a pipe the test
   closes, and the call follows after a delay that walks from before the leader's exit to after it. */
static void test_kill_tree_while_leader_exits(void)
{
    int by_itself = 0, killed = 0, bad = 0;
    for (int i = 0; i < 40; i++)
    {
        char file[64], script[256];
        snprintf(file, sizeof file, "process_tree_race_%d.pid", i);
        remove(file);
        int gate[2];
        CHECK(pipe(gate) == 0);
        fcntl(gate[1], F_SETFD, FD_CLOEXEC);
        snprintf(script, sizeof script, "sleep 60 & echo $! > %s; read x; exit 7", file);
        const char *args[] = {"-c", script};
        int saved = dup(0);
        dup2(gate[0], 0);
        int64_t id = process_spawn(SHELL, args, 2, false);
        if (saved >= 0)
        {
            dup2(saved, 0);
            close(saved);
        }
        close(gate[0]);
        CHECK(id > 0);
        long child = read_pid_file(file);
        CHECK(child > 0);
        close(gate[1]); /* the leader is released: it ends in a moment */
        struct timespec ts = {0, (long)(i % 10) * 60000L};
        nanosleep(&ts, NULL);
        if (!process_kill_tree(id))
            bad++;
        int code = process_exit_code(id);
        if (code == 7)
            by_itself++;
        else if (code == 9)
            killed++;
        else
            bad++;
        if (!ends_soon(child))
            bad++;
        remove(file);
    }
    printf("  kill_tree while the leader ends: %d ended by themselves first, %d killed, %d wrong\n", by_itself, killed, bad);
    CHECK(bad == 0);
}

/* process_kill_tree on a number that is a real process but not a child of process_spawn is refused, and the ids that
   no process has are too */
static void test_kill_tree_refusals(void)
{
    CHECK(!process_kill_tree(0));
    CHECK(!process_kill_tree(-1));
    CHECK(!process_kill_tree(2147480000));
    CHECK(!process_kill_tree((int64_t)getpid()));
    CHECK(!process_kill_tree((int64_t)getppid()));
    CHECK(!process_kill_tree(1));
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
    test_kill_tree_leader_gone();
    test_kill_then_kill_tree();
    test_kill_tree_escaped();
    test_kill_tree_while_leader_exits();
    test_kill_tree_refusals();
#endif
    test_exit_code_259();
    test_failures();
    printf("test_process: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
