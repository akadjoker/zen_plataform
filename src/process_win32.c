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
    bool running;
    int exit_code;
} ProcessInfo;

static ProcessInfo g_processes[PROCESS_CAP];
static SRWLOCK g_lock = SRWLOCK_INIT;

static ProcessInfo *find_process(DWORD pid)
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
        {
            if (g_processes[i].process)
                CloseHandle(g_processes[i].process);
            memset(&g_processes[i], 0, sizeof g_processes[i]);
            return &g_processes[i];
        }
    }
    return NULL;
}

static bool needs_quotes(const char *text)
{
    if (!text[0])
        return true;
    for (const char *p = text; *p; p++)
    {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '"' || c == '&' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
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
    DWORD code = 0;
    if (!GetExitCodeProcess(info->process, &code))
        return false;
    if (code == STILL_ACTIVE)
        return true;
    info->running = false;
    info->exit_code = (int)code;
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
    DWORD flags = NORMAL_PRIORITY_CLASS | (open_console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW);

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
    CloseHandle(pi.hThread);
    slot->pid = pi.dwProcessId;
    slot->process = pi.hProcess;
    slot->running = true;
    slot->exit_code = 0;
    ReleaseSRWLockExclusive(&g_lock);
    free(wide);
    free(command);
    return (int64_t)pi.dwProcessId;
}

bool process_is_running(int64_t id)
{
    if (id <= 0)
        return false;
    AcquireSRWLockExclusive(&g_lock);
    ProcessInfo *info = find_process((DWORD)id);
    bool running = info && refresh(info);
    ReleaseSRWLockExclusive(&g_lock);
    return running;
}

int process_exit_code(int64_t id)
{
    if (id <= 0)
        return -1;
    AcquireSRWLockExclusive(&g_lock);
    ProcessInfo *info = find_process((DWORD)id);
    int result = -1;
    if (info && !refresh(info))
        result = info->running ? -1 : info->exit_code;
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

bool process_kill(int64_t id)
{
    if (id <= 0)
        return error_set("invalid process id");
    bool ok = false;
    AcquireSRWLockExclusive(&g_lock);
    ProcessInfo *info = find_process((DWORD)id);
    if (info)
    {
        if (info->running)
        {
            ok = TerminateProcess(info->process, 0) != 0;
            if (ok)
            {
                WaitForSingleObject(info->process, INFINITE);
                info->running = false;
                info->exit_code = 0;
            }
        }
        else
        {
            ok = true;
        }
    }
    else
    {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)id);
        if (h)
        {
            ok = TerminateProcess(h, 0) != 0;
            CloseHandle(h);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!ok)
        return win32_error("cannot kill process", NULL);
    return true;
}
