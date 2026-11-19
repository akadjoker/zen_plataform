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
    /* The group process_spawn made (the child's pid is its id) was last seen with a member in it. While one lives the
       system does not give the number out again (POSIX, "Process ID Reuse": a process id is not reused while a process
       group with that id exists), so the number is still ours. Once the last member is gone it can be anybody's, and
       process_kill_tree must not signal it. Set at the spawn, so it is true for every child that runs (reap_children looks
       at the records that have it, and at no other); cleared by the first look that finds the group empty. */
    bool group;
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

/* An id as a pid, or false for one that is no pid (0, negative, wider than pid_t: a number with a bit set above the
   pid's would otherwise be read as the pid that is left of it, and end a child nobody named). */
static bool id_to_pid(int64_t id, pid_t *pid)
{
    pid_t p = (pid_t)id;
    if (id <= 0 || (int64_t)p != id)
        return false;
    *pid = p;
    return true;
}

/* A process of the group is left (a zombie counts: it still has its group). EPERM is an answer too: it is there. */
static bool group_has_members(pid_t pgid)
{
    return kill(-pgid, 0) == 0 || errno == EPERM;
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
    info->group = group_has_members(info->pid);
}

/* Every child that has ended is waited for, so one nobody asks about does not stay a zombie, and its slot can be
   given to a newer child. A finished child whose group still had members when it was waited for is looked at again, so
   the record learns when the last of them is gone. Called with the lock held, on every way into the table. */
static void reap_children(void)
{
    for (int i = 0; i < PROCESS_CAP; i++)
    {
        ProcessInfo *info = &g_processes[i];
        if (!info->group)
            continue; /* a slot nobody used, or a finished child with nobody left in its group: nothing to look at */
        if (info->running)
            poll_slot(info);
        else if (!group_has_members(info->pid))
            info->group = false;
    }
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
    slot->group = true;
    slot->exit_code = 0;
    slot->order = ++g_started;
    pthread_mutex_unlock(&g_lock);
    free(argv);
    return (int64_t)pid;
}

bool process_is_running(int64_t id)
{
    pid_t pid;
    if (!id_to_pid(id, &pid))
        return false;
    pthread_mutex_lock(&g_lock);
    reap_children();
    const ProcessInfo *info = find_process(pid);
    bool running = info && info->running;
    pthread_mutex_unlock(&g_lock);
    return running;
}

int process_exit_code(int64_t id)
{
    pid_t pid;
    if (!id_to_pid(id, &pid))
        return -1;
    pthread_mutex_lock(&g_lock);
    reap_children();
    const ProcessInfo *info = find_process(pid);
    int result = (!info || info->running) ? -1 : info->exit_code;
    pthread_mutex_unlock(&g_lock);
    return result;
}

bool process_kill(int64_t id)
{
    pid_t pid;
    if (!id_to_pid(id, &pid))
        return error_set("invalid process id");
    pthread_mutex_lock(&g_lock);
    reap_children();
    ProcessInfo *info = find_process(pid);
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
    int r = kill(pid, SIGKILL);
    if (r == 0)
    {
        int status = 0;
        wait_for_pid(pid, &status, 0, NULL);
        info->running = false;
        info->exit_code = status_to_code(status);
        info->group = group_has_members(pid);
    }
    pthread_mutex_unlock(&g_lock);
    if (r != 0)
        return error_set("cannot kill process %lld: %s", (long long)id, strerror(errno));
    return true;
}

bool process_kill_tree(int64_t id)
{
    pid_t pid;
    if (!id_to_pid(id, &pid))
        return error_set("invalid process id");
    pthread_mutex_lock(&g_lock);
    reap_children();
    ProcessInfo *info = find_process(pid);
    if (!info)
    {
        pthread_mutex_unlock(&g_lock);
        return error_set("process %lld was not started by process_spawn", (long long)id);
    }
    int r = 0, err = 0;
    if (info->running)
    {
        /* The child is ours and not waited for yet (a zombie keeps its pid): the number is not a stranger's. The group
           is signalled first; it does not exist before the child's setsid has run (ESRCH), and then the child, which
           has started nothing yet, is the whole tree. */
        r = kill(-pid, SIGKILL);
        if (r != 0)
            r = kill(pid, SIGKILL);
        err = errno;
        if (r == 0)
        {
            int status = 0;
            wait_for_pid(pid, &status, 0, NULL);
            info->running = false;
            info->exit_code = status_to_code(status);
            info->group = group_has_members(pid); /* the members just signalled may take a moment to go */
        }
    }
    else if (info->group)
    {
        /* The leader is gone and waited for; members were left in the group when it was looked at last, so the number
           was still the group's then. Only an empty group (ESRCH) is not an error: it has just emptied. */
        r = kill(-pid, SIGKILL);
        err = errno;
        if (r != 0 && err == ESRCH)
        {
            r = 0;
            info->group = false;
        }
    }
    /* else: the group is empty and the number may be anybody's by now: there is nothing of ours to end, and what
       carries the number is not ours to signal. */
    pthread_mutex_unlock(&g_lock);
    if (r != 0)
        return error_set("cannot kill process %lld: %s", (long long)id, strerror(err));
    return true;
}
