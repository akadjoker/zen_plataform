#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "gamepad_xinput_map.h"

#define XINPUT_USERS 4
#define RESCAN_NANOS 1000000000ull

typedef struct
{
    DWORD packet;
    XInputPadState pad;
} XInputRawState;

typedef DWORD(WINAPI *PFN_XInputGetState)(DWORD, void *);

static HMODULE g_dll;
static PFN_XInputGetState g_get_state;
static int g_slot[XINPUT_USERS];
static uint64_t g_next_scan;

static bool read_user(int user, XInputPadState *out)
{
    XInputRawState raw;
    if (g_get_state(user, &raw) != ERROR_SUCCESS)
        return false;
    *out = raw.pad;
    return true;
}

static void scan_disconnected(void)
{
    for (int i = 0; i < XINPUT_USERS; i++)
    {
        XInputPadState state;
        if (g_slot[i] >= 0 || !read_user(i, &state))
            continue;
        g_slot[i] = gamepad_internal_connect("Xbox Controller");
        if (g_slot[i] >= 0)
            xinput_apply(g_slot[i], &state);
    }
}

void gamepad_backend_init(void)
{
    static const wchar_t *k_names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (int i = 0; i < XINPUT_USERS; i++)
        g_slot[i] = -1;
    for (unsigned i = 0; i < sizeof k_names / sizeof k_names[0] && !g_dll; i++)
        g_dll = LoadLibraryW(k_names[i]);
    if (!g_dll)
        return;
    g_get_state = (PFN_XInputGetState)(void *)GetProcAddress(g_dll, "XInputGetState");
    if (!g_get_state)
    {
        FreeLibrary(g_dll);
        g_dll = NULL;
        return;
    }
    scan_disconnected();
    g_next_scan = time_nanos() + RESCAN_NANOS;
}

void gamepad_backend_poll(void)
{
    if (!g_get_state)
        return;
    for (int i = 0; i < XINPUT_USERS; i++)
    {
        XInputPadState state;
        if (g_slot[i] < 0)
            continue;
        if (read_user(i, &state))
        {
            xinput_apply(g_slot[i], &state);
        }
        else
        {
            gamepad_internal_disconnect(g_slot[i]);
            g_slot[i] = -1;
        }
    }
    uint64_t now = time_nanos();
    if (now >= g_next_scan)
    {
        scan_disconnected();
        g_next_scan = now + RESCAN_NANOS;
    }
}

void gamepad_backend_shutdown(void)
{
    for (int i = 0; i < XINPUT_USERS; i++)
    {
        if (g_slot[i] >= 0)
            gamepad_internal_disconnect(g_slot[i]);
        g_slot[i] = -1;
    }
    if (g_dll)
        FreeLibrary(g_dll);
    g_dll = NULL;
    g_get_state = NULL;
}
