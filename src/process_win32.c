#include "platform.h"
#include "win32_util.h"
#include "error_internal.h"

#include <stdlib.h>
#include <string.h>

#define PROCESS_CAP 128

typedef struct
{
    DWORD pid;
    HANDLE process;
    HANDLE job; /* NULL when the job could not be made or joined */
    bool running;
    int exit_code;
    unsigned long order; /* when it was started: the oldest finished record makes room first */
} ProcessInfo;

static ProcessInfo g_processes[PROCESS_CAP];
static unsigned long g_started;
static SRWLOCK g_lock = SRWLOCK_INIT;

/* An id as a process id (a DWORD), or false for one that is no process id: 0, negative, or wider than 32 bits (a number
   with a bit set above them would otherwise be read as the process id that is left of it). */
static bool id_to_pid(int64_t id, DWORD *pid)
{
    if (id <= 0 || id > 0xFFFFFFFFLL)
        return false;
    *pid = (DWORD)id;
    return true;
}

static ProcessInfo *find_process(DWORD pid)
{
    for (int i = 0; i < PROCESS_CAP; i++)
    {
        if (g_processes[i].pid == pid)
            return &g_processes[i];
    }
    return NULL;
}

/* A slot nobody used, else the finished record that is the oldest (still intact: release_slot takes it over once the
   new process exists, so a spawn that fails does not make the table forget a record). NULL when all PROCESS_CAP
   children still run. */
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

static void release_slot(ProcessInfo *slot)
{
    if (slot->process)
        CloseHandle(slot->process);
    if (slot->job)
        CloseHandle(slot->job);
    memset(slot, 0, sizeof *slot);
}

static bool needs_quotes(const char *text)
{
    if (!text[0])
        return true;
    for (const char *p = text; *p; p++)
    {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '|' || c == '<' || c == '>' || c == '%' || c == '&' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
            c == '^' || c == '=' || c == ';' || c == '!' || c == '\'' || c == '+' || c == ',' || c == '`' || c == '~')
            return true;
    }
    return false;
}

static size_t append_argument(char *out, size_t at, const char *text)
{
    if (!needs_quotes(text))
    {
        size_t n = strlen(text);
        if (out)
            memcpy(out + at, text, n);
        return n;
    }
    size_t n = 0;
    if (out)
        out[at + n] = '"';
    n++;
    size_t slashes = 0;
    for (const char *p = text; *p; p++)
    {
        if (*p == '\\')
        {
            slashes++;
            if (out)
                out[at + n] = '\\';
            n++;
            continue;
        }
        if (*p == '"')
        {
            for (size_t i = 0; i < slashes; i++)
            {
                if (out)
                    out[at + n] = '\\';
                n++;
            }
            if (out)
                out[at + n] = '\\';
            n++;
        }
        slashes = 0;
        if (out)
            out[at + n] = *p;
        n++;
    }
    for (size_t i = 0; i < slashes; i++)
    {
        if (out)
            out[at + n] = '\\';
        n++;
    }
    if (out)
        out[at + n] = '"';
    n++;
    return n;
}

static char *build_command(const char *path, const char *const *args, int argc)
{
    size_t total = append_argument(NULL, 0, path);
    for (int i = 0; i < argc; i++)
        total += 1 + append_argument(NULL, 0, args[i]);
    char *command = (char *)malloc(total + 1);
    if (!command)
        return NULL;
    size_t at = append_argument(command, 0, path);
    for (int i = 0; i < argc; i++)
    {
        command[at++] = ' ';
        at += append_argument(command, at, args[i]);
    }
    command[at] = '\0';
    return command;
}

static bool refresh(ProcessInfo *info)
{
    if (!info->running)
        return false;
    /* The exit code alone cannot tell: a process that ends with 259 (STILL_ACTIVE) would read as running for ever. */
    if (WaitForSingleObject(info->process, 0) == WAIT_TIMEOUT)
        return true;
    DWORD code = 0;
    info->running = false;
    info->exit_code = GetExitCodeProcess(info->process, &code) ? (int)code : -1;
    return false;
}

int64_t process_spawn(const char *path, const char *const *args, int argc, bool open_console)
{
    if (!path || !path[0] || argc < 0 || (argc > 0 && !args))
    {
        error_set("invalid argument");
        return 0;
    }
    char *command = build_command(path, args, argc);
    if (!command)
    {
        error_set("out of memory");
        return 0;
    }
    int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command, -1, NULL, 0);
    wchar_t *wide = wide_count > 0 ? (wchar_t *)malloc((size_t)wide_count * sizeof *wide) : NULL;
    if (!wide || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command, -1, wide, wide_count) <= 0)
    {
        free(wide);
        free(command);
        error_set("invalid UTF-8");
        return 0;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    DWORD flags = NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED | (open_console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW);

    AcquireSRWLockExclusive(&g_lock);
    ProcessInfo *slot = free_slot();
    if (!slot)
    {
        ReleaseSRWLockExclusive(&g_lock);
        free(wide);
        free(command);
        error_set("too many child processes");
        return 0;
    }
    BOOL ok = CreateProcessW(NULL, wide, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi);
    if (!ok)
    {
        ReleaseSRWLockExclusive(&g_lock);
        win32_error("Could not create child process", command);
        free(wide);
        free(command);
        return 0;
    }
    /* Suspended until it is in the job, so nothing it starts can escape. Without a job (the process is in one that
       does not nest, as before Windows 8, or the job could not be made) the process still runs, and process_kill_tree
       ends only the process. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job && !AssignProcessToJobObject(job, pi.hProcess))
    {
        CloseHandle(job);
        job = NULL;
    }
    if (ResumeThread(pi.hThread) == (DWORD)-1)
    {
        /* it would stay suspended for ever, and read as running */
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job)
            CloseHandle(job);
        SetLastError(err);
        ReleaseSRWLockExclusive(&g_lock);
        win32_error("Could not start child process", command);
        free(wide);
        free(command);
        return 0;
    }
    CloseHandle(pi.hThread);
    release_slot(slot);
    slot->job = job;
    slot->pid = pi.dwProcessId;
    slot->process = pi.hProcess;
    slot->running = true;
    slot->exit_code = 0;
    slot->order = ++g_started;
    ReleaseSRWLockExclusive(&g_lock);
    free(wide);
    free(command);
    return (int64_t)pi.dwProcessId;
}

bool process_is_running(int64_t id)
{
    DWORD pid;
    if (!id_to_pid(id, &pid))
        return false;
    AcquireSRWLockExclusive(&g_lock);
    ProcessInfo *info = find_process(pid);
    bool running = info && refresh(info);
    ReleaseSRWLockExclusive(&g_lock);
    return running;
}

int process_exit_code(int64_t id)
{
    DWORD pid;
    if (!id_to_pid(id, &pid))
        return -1;
    AcquireSRWLockExclusive(&g_lock);
    ProcessInfo *info = find_process(pid);
    int result = -1;
    if (info && !refresh(info))
        result = info->running ? -1 : info->exit_code;
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

/* The process has been told to end (or has ended by itself just now): waits for it and records the code it really ended
   with, which is the one TerminateProcess gave it or the one it ended with first. */
static void wait_and_record(ProcessInfo *info)
{
    WaitForSingleObject(info->process, INFINITE);
    refresh(info);
}

bool process_kill(int64_t id)
{
    DWORD pid;
    if (!id_to_pid(id, &pid))
        return error_set("invalid process id");
    bool ok = false;
    DWORD err = 0;
    AcquireSRWLockExclusive(&g_lock);
    ProcessInfo *info = find_process(pid);
    if (info)
    {
        if (refresh(info)) /* still running: one that already ended cannot be terminated, and needs no killing */
        {
            ok = TerminateProcess(info->process, 0) != 0;
            err = GetLastError();
            /* it may have ended by itself between refresh and TerminateProcess, which then fails with access denied */
            if (!ok && WaitForSingleObject(info->process, 0) == WAIT_OBJECT_0)
                ok = true;
            if (ok)
                wait_and_record(info);
        }
        else
        {
            ok = true;
        }
    }
    else
    {
        /* Not a child process_spawn started, or one whose record is gone (the number may be somebody else's now):
           ending it would be ending a stranger. */
        ReleaseSRWLockExclusive(&g_lock);
        return error_set("process %lld was not started by process_spawn", (long long)id);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!ok)
    {
        SetLastError(err);
        return win32_error("cannot kill process", NULL);
    }
    return true;
}

bool process_kill_tree(int64_t id)
{
    DWORD pid;
    if (!id_to_pid(id, &pid))
        return error_set("invalid process id");
    bool ok = false;
    DWORD err = 0;
    AcquireSRWLockExclusive(&g_lock);
    ProcessInfo *info = find_process(pid);
    if (!info)
    {
        ReleaseSRWLockExclusive(&g_lock);
        return error_set("process %lld was not started by process_spawn", (long long)id);
    }
    /* Everything in the job, the process included: this reaches the members a finished leader left, too. A job that is
       empty or was never made leaves nothing to do here. */
    bool job_ended = info->job && TerminateJobObject(info->job, 0) != 0;
    if (refresh(info)) /* the leader still runs: ending a job is not instant, and a missing job ends nobody */
    {
        ok = job_ended || TerminateProcess(info->process, 0) != 0;
        err = GetLastError();
        if (!ok && WaitForSingleObject(info->process, 0) == WAIT_OBJECT_0)
            ok = true; /* it ended by itself in the meantime */
        if (ok)
            wait_and_record(info);
    }
    else
    {
        ok = true; /* it is finished, and keeps the code it ended with */
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!ok)
    {
        SetLastError(err);
        return win32_error("cannot kill process", NULL);
    }
    return true;
}
