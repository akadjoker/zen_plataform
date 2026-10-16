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
    int exit_code;
} ProcessInfo;

static ProcessInfo g_processes[PROCESS_CAP];
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

static ProcessInfo *free_slot(void)
{
    for (int i = 0; i < PROCESS_CAP; i++)
    {
        if (g_processes[i].pid == 0)
            return &g_processes[i];
    }
    for (int i = 0; i < PROCESS_CAP; i++)
    {
        if (!g_processes[i].running)
            return &g_processes[i];
    }
    return NULL;
}

static int status_to_code(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : status;
}

static bool check_running(pid_t pid, int *code)
{
    ProcessInfo *info = find_process(pid);
    if (info && !info->running)
    {
        if (code)
            *code = info->exit_code;
        return false;
    }
    int status = 0;
    pid_t done = -1;
    int result = wait_for_pid(pid, &status, WNOHANG, &done);
    if (result == 0 && done == 0)
        return true;
    if (result != 0)
        return false;
    status = status_to_code(status);
    if (info)
    {
        info->running = false;
        info->exit_code = status;
    }
    if (code)
        *code = status;
    return false;
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

    slot->pid = pid;
    slot->running = true;
    slot->exit_code = 0;
    pthread_mutex_unlock(&g_lock);
    free(argv);
    return (int64_t)pid;
}

bool process_is_running(int64_t id)
{
    if (id <= 0)
        return false;
    pthread_mutex_lock(&g_lock);
    bool running = check_running((pid_t)id, NULL);
    pthread_mutex_unlock(&g_lock);
    return running;
}

int process_exit_code(int64_t id)
{
    if (id <= 0)
        return -1;
    pthread_mutex_lock(&g_lock);
    int code = 0;
    int result = check_running((pid_t)id, &code) ? -1 : code;
    pthread_mutex_unlock(&g_lock);
    return result;
}

bool process_kill(int64_t id)
{
    if (id <= 0)
        return error_set("invalid process id");
    pthread_mutex_lock(&g_lock);
    const ProcessInfo *known = find_process((pid_t)id);
    if (known && !known->running)
    {
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    int r = kill((pid_t)id, SIGKILL);
    if (r == 0)
    {
        int status = 0;
        wait_for_pid((pid_t)id, &status, 0, NULL);
        ProcessInfo *info = find_process((pid_t)id);
        if (info)
        {
            info->running = false;
            info->exit_code = status_to_code(status);
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (r != 0)
        return error_set("cannot kill process %lld: %s", (long long)id, strerror(errno));
    return true;
}
