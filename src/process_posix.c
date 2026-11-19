#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "error_internal.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROCESS_CAP 128

typedef struct
{
    pid_t pid;
    bool running;
    int exit_code; /* -1 when the system no longer knew the child (somebody else waited for it) */
    unsigned long order; /* when the child was started: the oldest finished record is the one that makes room */
} ProcessInfo;

static ProcessInfo g_processes[PROCESS_CAP];
static unsigned long g_started;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int wait_for_pid(pid_t pid, int *status, int options, pid_t *out_pid)
{
    for (;;)
    {
        pid_t r = waitpid(pid, status, options);
        if (r != -1)
        {
            if (out_pid)
                *out_pid = r;
            return 0;
        }
        if (errno == EINTR)
            continue;
        return errno;
    }
}

static ProcessInfo *find_process(pid_t pid)
{
    for (int i = 0; i < PROCESS_CAP; i++)
    {
        if (g_processes[i].pid == pid)
            return &g_processes[i];
    }
    return NULL;
}

/* A slot nobody used, else the finished record that is the oldest. NULL when all PROCESS_CAP children still run. */
static ProcessInfo *free_slot(void)
{
    for (int i = 0; i < PROCESS_CAP; i++)
    {
        if (g_processes[i].pid == 0)
            return &g_processes[i];
    }
    ProcessInfo *oldest = NULL;
    for (int i = 0; i < PROCESS_CAP; i++)
    {
        if (!g_processes[i].running && (!oldest || g_processes[i].order < oldest->order))
            oldest = &g_processes[i];
    }
    return oldest;
}

static int status_to_code(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : status;
}

/* Looks at one child without waiting, and records its end. */
static void poll_slot(ProcessInfo *info)
{
    if (!info->running)
        return;
    int status = 0;
    pid_t done = -1;
    int result = wait_for_pid(info->pid, &status, WNOHANG, &done);
    if (result == 0 && done == 0)
        return; /* still running */
    info->running = false;
    info->exit_code = result == 0 ? status_to_code(status) : -1;
}

/* Every child that has ended is waited for, so one nobody asks about does not stay a zombie, and its slot can be
   given to a newer child. Called with the lock held, on every way into the table. */
static void reap_children(void)
{
    for (int i = 0; i < PROCESS_CAP; i++)
        if (g_processes[i].pid != 0 && g_processes[i].running)
            poll_slot(&g_processes[i]);
}

int64_t process_spawn(const char *path, const char *const *args, int argc, bool open_console)
{
    (void)open_console;
    if (!path || !path[0] || argc < 0 || (argc > 0 && !args))
    {
        error_set("invalid argument");
        return 0;
    }
    char **argv = (char **)calloc((size_t)argc + 2, sizeof *argv);
    if (!argv)
    {
        error_set("out of memory");
        return 0;
    }
    argv[0] = (char *)path;
    for (int i = 0; i < argc; i++)
        argv[i + 1] = (char *)args[i];

    pthread_mutex_lock(&g_lock);
    reap_children();
    ProcessInfo *slot = free_slot();
    if (!slot)
    {
        pthread_mutex_unlock(&g_lock);
        free(argv);
        error_set("too many child processes");
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        pthread_mutex_unlock(&g_lock);
        free(argv);
        error_set("cannot fork: %s", strerror(errno));
        return 0;
    }
    if (pid == 0)
    {
        setsid();
        execvp(path, argv);
        fprintf(stderr, "Could not create child process: %s\n", path);
        raise(SIGKILL);
        _exit(127);
    }

    /* The system can give a pid out again once its earlier owner is gone: a record that still carries it is that
       owner's, and would hide this child (find_process takes the first match). */
    for (int i = 0; i < PROCESS_CAP; i++)
        if (&g_processes[i] != slot && g_processes[i].pid == pid)
            memset(&g_processes[i], 0, sizeof g_processes[i]);
    slot->pid = pid;
    slot->running = true;
    slot->exit_code = 0;
    slot->order = ++g_started;
    pthread_mutex_unlock(&g_lock);
    free(argv);
    return (int64_t)pid;
}

bool process_is_running(int64_t id)
{
    if (id <= 0)
        return false;
    pthread_mutex_lock(&g_lock);
    reap_children();
    const ProcessInfo *info = find_process((pid_t)id);
    bool running = info && info->running;
    pthread_mutex_unlock(&g_lock);
    return running;
}

int process_exit_code(int64_t id)
{
    if (id <= 0)
        return -1;
    pthread_mutex_lock(&g_lock);
    reap_children();
    const ProcessInfo *info = find_process((pid_t)id);
    int result = (!info || info->running) ? -1 : info->exit_code;
    pthread_mutex_unlock(&g_lock);
    return result;
}

bool process_kill(int64_t id)
{
    if (id <= 0)
        return error_set("invalid process id");
    pthread_mutex_lock(&g_lock);
    reap_children();
    ProcessInfo *info = find_process((pid_t)id);
    if (!info)
    {
        /* Not a child process_spawn started (or one so old its record is gone, and the number may belong to
           somebody else now): killing it would be killing a stranger. */
        pthread_mutex_unlock(&g_lock);
        return error_set("process %lld was not started by process_spawn", (long long)id);
    }
    if (!info->running)
    {
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    int r = kill((pid_t)id, SIGKILL);
    if (r == 0)
    {
        int status = 0;
        wait_for_pid((pid_t)id, &status, 0, NULL);
        info->running = false;
        info->exit_code = status_to_code(status);
    }
    pthread_mutex_unlock(&g_lock);
    if (r != 0)
        return error_set("cannot kill process %lld: %s", (long long)id, strerror(errno));
    return true;
}

bool process_kill_tree(int64_t id)
{
    if (id <= 0)
        return error_set("invalid process id");
    pthread_mutex_lock(&g_lock);
    reap_children();
    ProcessInfo *info = find_process((pid_t)id);
    if (!info)
    {
        pthread_mutex_unlock(&g_lock);
        return error_set("process %lld was not started by process_spawn", (long long)id);
    }
    /* The group id stays taken while any member lives, so it cannot be a stranger's; with no member left the call
       fails with ESRCH, which is not an error here. */
    int r = kill(-(pid_t)id, SIGKILL);
    int err = errno;
    if (info->running)
    {
        if (r != 0)
            r = kill((pid_t)id, SIGKILL);
        if (r == 0)
        {
            int status = 0;
            wait_for_pid((pid_t)id, &status, 0, NULL);
            info->running = false;
            info->exit_code = status_to_code(status);
        }
        else
            err = errno;
    }
    else if (r != 0 && err == ESRCH)
        r = 0;
    pthread_mutex_unlock(&g_lock);
    if (r != 0)
        return error_set("cannot kill process %lld: %s", (long long)id, strerror(err));
    return true;
}
