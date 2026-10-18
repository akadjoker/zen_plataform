#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE "test_win32_input"

static int g_pass, g_fail;
static HWND g_hwnd, g_other;

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

static bool extended_vk(WORD vk)
{
    switch (vk)
    {
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_DIVIDE:
    case VK_SNAPSHOT:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
        return true;
    default:
        return false;
    }
}

static void key(WORD vk, bool down)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (extended_vk(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof in);
}

static void key_enter_numpad(bool down)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_RETURN;
    in.ki.wScan = 0x1C;
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof in);
}

static void mouse(DWORD flags, int data)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = (DWORD)data;
    SendInput(1, &in, sizeof in);
}

static void mouse_move_by(int dx, int dy)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof in);
}

static void cursor_to(int x, int y)
{
    POINT p = {x, y};
    ClientToScreen(g_hwnd, &p);
    SetCursorPos(p.x, p.y);
}

static void frames(PlatformWindow *w, int n)
{
    Event ev;
    for (int i = 0; i < n; i++)
    {
        Sleep(5);
        window_begin_frame(w);
        while (poll_event(w, &ev))
        {
        }
    }
}

#define WAIT_N(w, cond, n)           \
    do                               \
    {                                \
        (n) = 0;                     \
        while (!(cond) && (n) < 100) \
        {                            \
            Sleep(5);                \
            window_begin_frame(w);   \
            (n)++;                   \
        }                            \
    } while (0)

#define WAIT(w, cond)        \
    do                       \
    {                        \
        int n_;              \
        WAIT_N(w, cond, n_); \
        (void)n_;            \
    } while (0)

static bool foreground(PlatformWindow *w, HWND h)
{
    for (int i = 0; i < 20 && GetForegroundWindow() != h; i++)
    {
        HWND fg = GetForegroundWindow();
        DWORD fg_thread = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
        DWORD me = GetCurrentThreadId();
        if (fg_thread && fg_thread != me)
            AttachThreadInput(fg_thread, me, TRUE);
        ShowWindow(h, SW_SHOW);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        SetFocus(h);
        if (fg_thread && fg_thread != me)
            AttachThreadInput(fg_thread, me, FALSE);
        frames(w, 4);
    }
    frames(w, 4);
    return GetForegroundWindow() == h;
}

static int keys_down(PlatformWindow *w)
{
    int n = 0;
    for (int k = 0; k < KEY_MAX; k++)
        n += key_down(w, k);
    return n;
}

static void test_key_sweep(PlatformWindow *w)
{
    static const struct
    {
        WORD vk;
        int key;
    } keys[] = {
        {VK_SPACE, KEY_SPACE},
        {VK_OEM_7, KEY_APOSTROPHE},
        {VK_OEM_COMMA, KEY_COMMA},
        {VK_OEM_MINUS, KEY_MINUS},
        {VK_OEM_PERIOD, KEY_PERIOD},
        {VK_OEM_2, KEY_SLASH},
        {VK_OEM_1, KEY_SEMICOLON},
        {VK_OEM_PLUS, KEY_EQUAL},
        {VK_OEM_4, KEY_LEFT_BRACKET},
        {VK_OEM_5, KEY_BACKSLASH},
        {VK_OEM_6, KEY_RIGHT_BRACKET},
        {VK_OEM_3, KEY_GRAVE},
        {VK_ESCAPE, KEY_ESCAPE},
        {VK_RETURN, KEY_ENTER},
        {VK_TAB, KEY_TAB},
        {VK_BACK, KEY_BACKSPACE},
        {VK_INSERT, KEY_INSERT},
        {VK_DELETE, KEY_DELETE},
        {VK_RIGHT, KEY_RIGHT},
        {VK_LEFT, KEY_LEFT},
        {VK_DOWN, KEY_DOWN},
        {VK_UP, KEY_UP},
        {VK_PRIOR, KEY_PAGE_UP},
        {VK_NEXT, KEY_PAGE_DOWN},
        {VK_HOME, KEY_HOME},
        {VK_END, KEY_END},
        {VK_PAUSE, KEY_PAUSE},
        {VK_SCROLL, KEY_SCROLL_LOCK},
        {VK_DECIMAL, KEY_KP_DECIMAL},
        {VK_DIVIDE, KEY_KP_DIVIDE},
        {VK_MULTIPLY, KEY_KP_MULTIPLY},
        {VK_SUBTRACT, KEY_KP_SUBTRACT},
        {VK_ADD, KEY_KP_ADD},
        {VK_LSHIFT, KEY_LEFT_SHIFT},
        {VK_LCONTROL, KEY_LEFT_CONTROL},
        {VK_LMENU, KEY_LEFT_ALT},
        {VK_RSHIFT, KEY_RIGHT_SHIFT},
        {VK_RCONTROL, KEY_RIGHT_CONTROL},
        {VK_RMENU, KEY_RIGHT_ALT},
        {VK_APPS, KEY_MENU},
    };
    WORD vk[128];
    int want[128];
    int n = 0;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++, n++)
    {
        vk[n] = keys[i].vk;
        want[n] = keys[i].key;
    }
    for (int i = 0; i < 26; i++, n++)
    {
        vk[n] = (WORD)('A' + i);
        want[n] = KEY_A + i;
    }
    for (int i = 0; i < 10; i++, n++)
    {
        vk[n] = (WORD)('0' + i);
        want[n] = KEY_ZERO + i;
        vk[++n] = (WORD)(VK_NUMPAD0 + i);
        want[n] = KEY_KP_0 + i;
    }
    for (int i = 0; i < 12; i++, n++)
    {
        vk[n] = (WORD)(VK_F1 + i);
        want[n] = KEY_F1 + i;
    }
    frames(w, 5);
    int wrong = 0, slow = 0;
    for (int i = 0; i < n; i++)
    {
        key(vk[i], true);
        int f, g;
        WAIT_N(w, key_down(w, want[i]), f);
        int others = keys_down(w) - key_down(w, want[i]);
        bool ok = key_down(w, want[i]) && others == 0;
        key(vk[i], false);
        WAIT_N(w, !key_down(w, want[i]), g);
        ok = ok && !key_down(w, want[i]) && keys_down(w) == 0;
        if (f > 2 || g > 2)
            slow++;
        if (!ok)
        {
            printf("  vk 0x%02X is not key %d alone (frames %d/%d):", vk[i], want[i], f, g);
            for (int k = 0; k < KEY_MAX; k++)
                if (key_down(w, k))
                    printf(" %d down", k);
            printf("\n");
            wrong++;
            for (int k = 0; k < 4 && keys_down(w); k++)
                frames(w, 10);
        }
    }
    CHECK(wrong == 0);
    if (slow)
        printf("  %d keys took more than two frames\n", slow);

    key_enter_numpad(true);
    WAIT(w, key_down(w, KEY_KP_ENTER));
    CHECK(key_down(w, KEY_KP_ENTER) && !key_down(w, KEY_ENTER));
    key_enter_numpad(false);
    WAIT(w, !key_down(w, KEY_KP_ENTER));
    CHECK(keys_down(w) == 0);
}

static void test_both_shifts(PlatformWindow *w)
{
    for (int order = 0; order < 2; order++)
    {
        frames(w, 5);
        key(VK_LSHIFT, true);
        key(VK_RSHIFT, true);
        WAIT(w, key_down(w, KEY_LEFT_SHIFT) && key_down(w, KEY_RIGHT_SHIFT));
        CHECK(key_down(w, KEY_LEFT_SHIFT) && key_down(w, KEY_RIGHT_SHIFT));
        key(order ? VK_RSHIFT : VK_LSHIFT, false);
        frames(w, 10);
        bool first_up = !key_down(w, order ? KEY_RIGHT_SHIFT : KEY_LEFT_SHIFT);
        bool other_down = key_down(w, order ? KEY_LEFT_SHIFT : KEY_RIGHT_SHIFT);
        if (!first_up || !other_down)
            printf("  both shifts, the %s let go first: left %d right %d\n", order ? "right" : "left",
                   key_down(w, KEY_LEFT_SHIFT), key_down(w, KEY_RIGHT_SHIFT));
        CHECK(first_up && other_down);
        key(order ? VK_LSHIFT : VK_RSHIFT, false);
        frames(w, 10);
        if (key_down(w, KEY_LEFT_SHIFT) || key_down(w, KEY_RIGHT_SHIFT))
            printf("  both shifts let go: left %d right %d still down\n", key_down(w, KEY_LEFT_SHIFT),
                   key_down(w, KEY_RIGHT_SHIFT));
        CHECK(!key_down(w, KEY_LEFT_SHIFT) && !key_down(w, KEY_RIGHT_SHIFT));
        CHECK((key_mods(w) & KEYMOD_SHIFT) == 0);
    }
}

static void test_print_screen(PlatformWindow *w)
{
    frames(w, 5);
    Event ev;
    int downs = 0, ups = 0;
    key(VK_SNAPSHOT, true);
    key(VK_SNAPSHOT, false);
    for (int i = 0; i < 20; i++)
    {
        Sleep(5);
        window_begin_frame(w);
        while (poll_event(w, &ev))
        {
            if (ev.type == EVENT_KEY && ev.data.key.key == KEY_PRINT_SCREEN)
                (void)(ev.data.key.down ? downs++ : ups++);
        }
    }
    if (downs != 1 || ups != 1)
        printf("  print screen: %d presses, %d releases\n", downs, ups);
    CHECK(downs == 1 && ups == 1);
    CHECK(!key_down(w, KEY_PRINT_SCREEN));
}

static void test_altgr(PlatformWindow *w)
{
    HKL before = GetKeyboardLayout(0);
    HKL pt = LoadKeyboardLayoutW(L"00000816", KLF_ACTIVATE);
    if (!pt)
    {
        printf("SKIP altgr: no Portuguese layout\n");
        return;
    }
    frames(w, 10);
    Event ev;
    while (poll_event(w, &ev))
    {
    }
    key(VK_RMENU, true);
    WAIT(w, key_down(w, KEY_RIGHT_ALT));
    bool ctrl = key_down(w, KEY_LEFT_CONTROL);
    CHECK(key_down(w, KEY_RIGHT_ALT));
    key(VK_RMENU, false);
    frames(w, 10);
    if (ctrl || keys_down(w))
        printf("  AltGr on the Portuguese layout: left control %s, %d keys down after\n", ctrl ? "down" : "up",
               keys_down(w));
    CHECK(!ctrl);
    CHECK(keys_down(w) == 0);
    ActivateKeyboardLayout(before, 0);
    UnloadKeyboardLayout(pt);
    frames(w, 5);
}

static void test_long_hold(PlatformWindow *w)
{
    frames(w, 5);
    key('W', true);
    WAIT(w, key_down(w, KEY_W));
    int frames_n = 0, held = 0, repeats = 0, presses = 0;
    Event ev;
    DWORD end = GetTickCount() + 2000;
    while (GetTickCount() < end)
    {
        Sleep(16);
        window_begin_frame(w);
        frames_n++;
        held += key_down(w, KEY_W);
        presses += key_pressed(w, KEY_W);
        while (poll_event(w, &ev))
            repeats += ev.type == EVENT_KEY && ev.data.key.key == KEY_W && ev.data.key.repeat;
    }
    if (held != frames_n)
        printf("  W held 2 s: down in %d of %d frames\n", held, frames_n);
    CHECK(held == frames_n);
    CHECK(presses == 0);
    key('W', false);
    WAIT(w, !key_down(w, KEY_W));
    CHECK(!key_down(w, KEY_W));
    printf("  2 s hold: %d frames, %d repeats\n", frames_n, repeats);
}

static void test_keys_together(PlatformWindow *w)
{
    static const WORD vks[] = {'W', 'A', VK_SPACE, VK_LSHIFT, VK_UP};
    static const int keys[] = {KEY_W, KEY_A, KEY_SPACE, KEY_LEFT_SHIFT, KEY_UP};
    frames(w, 5);
    for (int i = 0; i < 5; i++)
        key(vks[i], true);
    WAIT(w, keys_down(w) == 5);
    CHECK(keys_down(w) == 5);
    static const int order[] = {2, 0, 4, 1, 3};
    for (int i = 0; i < 5; i++)
    {
        key(vks[order[i]], false);
        WAIT(w, !key_down(w, keys[order[i]]));
        CHECK(!key_down(w, keys[order[i]]));
        for (int j = i + 1; j < 5; j++)
            CHECK(key_down(w, keys[order[j]]));
    }
}

static void test_focus_away_held(PlatformWindow *w)
{
    frames(w, 5);
    key('W', true);
    WAIT(w, key_down(w, KEY_W));
    CHECK(key_down(w, KEY_W));
    if (!foreground(w, g_other))
    {
        printf("SKIP focus away: the other window never came to the front\n");
        key('W', false);
        frames(w, 5);
        return;
    }
    CHECK(!window_is_focused(w));
    CHECK(!key_down(w, KEY_W));
    foreground(w, g_hwnd);
    CHECK(window_is_focused(w));
    WAIT(w, key_down(w, KEY_W));
    if (!key_down(w, KEY_W))
        printf("  W held across the focus trip is up on return\n");
    CHECK(key_down(w, KEY_W));

    foreground(w, g_other);
    key('W', false);
    key('S', true);
    frames(w, 5);
    foreground(w, g_hwnd);
    frames(w, 10);
    CHECK(!key_down(w, KEY_W));
    if (!key_down(w, KEY_S))
        printf("  S pressed while away is up on return\n");
    CHECK(key_down(w, KEY_S));
    key('S', false);
    WAIT(w, !key_down(w, KEY_S));
    CHECK(keys_down(w) == 0);
}

static void test_mouse_buttons(PlatformWindow *w)
{
    static const struct
    {
        DWORD down, up;
        int data, button;
    } map[] = {
        {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0, MOUSE_LEFT},
        {MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0, MOUSE_RIGHT},
        {MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0, MOUSE_MIDDLE},
        {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1, MOUSE_X1},
        {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2, MOUSE_X2},
    };
    cursor_to(30, 40);
    frames(w, 5);
    CHECK(mouse_x(w) == 30 && mouse_y(w) == 40);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
    {
        Event ev;
        while (poll_event(w, &ev))
        {
        }
        mouse(map[i].down, map[i].data);
        int at = 0, f = 0;
        while (!mouse_button_down(w, map[i].button) && f++ < 100)
        {
            Sleep(5);
            window_begin_frame(w);
            while (poll_event(w, &ev))
                at += ev.type == EVENT_MOUSE_BUTTON && ev.data.mouse.x == 30 && ev.data.mouse.y == 40;
        }
        if (!mouse_button_down(w, map[i].button))
            printf("  mouse button %d never came down\n", map[i].button);
        CHECK(mouse_button_down(w, map[i].button));
        CHECK(at == 1);
        mouse(map[i].up, map[i].data);
        WAIT(w, !mouse_button_down(w, map[i].button));
        CHECK(!mouse_button_down(w, map[i].button));
    }
    CHECK(GetCapture() != g_hwnd);
}

static void test_mouse_wheel(PlatformWindow *w)
{
    static const struct
    {
        DWORD flag;
        int data;
        float x, y;
    } notch[] = {{MOUSEEVENTF_WHEEL, WHEEL_DELTA, 0, 1},
                 {MOUSEEVENTF_WHEEL, -WHEEL_DELTA, 0, -1},
                 {MOUSEEVENTF_HWHEEL, WHEEL_DELTA, 1, 0},
                 {MOUSEEVENTF_HWHEEL, -WHEEL_DELTA, -1, 0}};
    cursor_to(50, 50);
    frames(w, 5);
    for (size_t i = 0; i < sizeof notch / sizeof notch[0]; i++)
    {
        mouse(notch[i].flag, notch[i].data);
        float x = 0, y = 0;
        for (int f = 0; f < 20; f++)
        {
            Sleep(5);
            window_begin_frame(w);
            float fx, fy;
            mouse_wheel_v(w, &fx, &fy);
            x += fx;
            y += fy;
        }
        if (x != notch[i].x || y != notch[i].y)
            printf("  wheel notch %zu gave %g,%g\n", i, x, y);
        CHECK(x == notch[i].x && y == notch[i].y);
    }
    for (int i = 0; i < 5; i++)
        mouse(MOUSEEVENTF_WHEEL, -WHEEL_DELTA);
    float sum = 0;
    for (int f = 0; f < 20; f++)
    {
        Sleep(5);
        window_begin_frame(w);
        sum += mouse_wheel(w);
    }
    CHECK(sum == -5);
}

static void test_mouse_drag(PlatformWindow *w)
{
    cursor_to(20, 20);
    frames(w, 5);
    mouse(MOUSEEVENTF_LEFTDOWN, 0);
    WAIT(w, mouse_button_down(w, MOUSE_LEFT));
    CHECK(mouse_button_down(w, MOUSE_LEFT));
    cursor_to(400, 300);
    WAIT(w, mouse_x(w) == 400);
    CHECK(mouse_x(w) == 400 && mouse_y(w) == 300);
    cursor_to(-40, -30);
    WAIT(w, mouse_x(w) == -40);
    CHECK(mouse_x(w) == -40 && mouse_y(w) == -30);
    mouse(MOUSEEVENTF_LEFTUP, 0);
    WAIT(w, !mouse_button_down(w, MOUSE_LEFT));
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
    cursor_to(60, 60);
    frames(w, 5);
    CHECK(mouse_x(w) == 60 && mouse_y(w) == 60);
}

static int sum_dx, sum_dy, worst;

static void count_frames(PlatformWindow *w, int n)
{
    for (int i = 0; i < n; i++)
    {
        Sleep(5);
        window_begin_frame(w);
        int dx, dy;
        mouse_delta(w, &dx, &dy);
        sum_dx += dx;
        sum_dy += dy;
        if (abs(dx) > worst)
            worst = abs(dx);
        if (abs(dy) > worst)
            worst = abs(dy);
    }
}

static void test_mouse_captured(PlatformWindow *w)
{
    int saved[3], flat[3] = {0, 0, 0}, speed = 10, saved_speed = 10;
    SystemParametersInfoW(SPI_GETMOUSE, 0, saved, 0);
    SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &saved_speed, 0);
    SystemParametersInfoW(SPI_SETMOUSE, 0, flat, 0);
    SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(INT_PTR)speed, 0);

    cursor_to(40, 40);
    frames(w, 5);
    mouse_set_mode(w, MOUSE_MODE_CAPTURED);
    frames(w, 10);
    sum_dx = sum_dy = worst = 0;
    for (int i = 0; i < 40; i++)
    {
        mouse_move_by(5, -2);
        count_frames(w, 1);
    }
    count_frames(w, 10);
    if (sum_dx != 200 || sum_dy != -80)
        printf("  captured: moved 200,-80, mouse_delta summed %d,%d\n", sum_dx, sum_dy);
    CHECK(sum_dx == 200 && sum_dy == -80);

    sum_dx = sum_dy = 0;
    for (int i = 0; i < 4; i++)
        mouse_move_by(30, 0);
    count_frames(w, 10);
    mouse_move_by(250, 0);
    count_frames(w, 10);
    if (sum_dx != 370)
        printf("  captured: fast moves 120 then 250 in one go, summed %d\n", sum_dx);
    CHECK(sum_dx == 370);

    sum_dx = sum_dy = worst = 0;
    mouse_set_position(w, 5, 5);
    count_frames(w, 5);
    CHECK(sum_dx == 0 && sum_dy == 0);

    sum_dx = sum_dy = worst = 0;
    if (foreground(w, g_other))
    {
        SetCursorPos(10, 10);
        count_frames(w, 5);
        mouse_move_by(7, 7);
        count_frames(w, 5);
        foreground(w, g_hwnd);
        count_frames(w, 5);
        cursor_to(150, 20);
        count_frames(w, 5);
        for (int i = 0; i < 10; i++)
        {
            mouse_move_by(3, 0);
            count_frames(w, 1);
        }
        count_frames(w, 5);
        if (worst > 3)
            printf("  captured across a focus change: largest step %d, summed %d,%d\n", worst, sum_dx, sum_dy);
        CHECK(worst <= 3);
    }

    for (int i = 0; i < 20; i++)
    {
        mouse_set_mode(w, i % 2 ? MOUSE_MODE_NORMAL : MOUSE_MODE_CAPTURED);
        frames(w, 1);
    }
    mouse_set_mode(w, MOUSE_MODE_NORMAL);
    frames(w, 5);
    RECT clip, screen = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                         GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                         GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    GetClipCursor(&clip);
    CHECK(EqualRect(&clip, &screen));

    SystemParametersInfoW(SPI_SETMOUSE, 0, saved, 0);
    SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(INT_PTR)saved_speed, 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!platform_init())
    {
        printf("FAIL platform_init: %s\n", platform_get_error());
        return 1;
    }
    WindowConfig cfg = {.title = TITLE, .width = 200, .height = 150, .render = RENDER_PIXELS, .x = 200, .y = 200};
    PlatformWindow *w = window_create(&cfg);
    if (!w)
    {
        printf("FAIL window_create: %s\n", platform_get_error());
        return 1;
    }
    STICKYKEYS sticky = {sizeof sticky, 0}, sticky_off;
    FILTERKEYS filter = {sizeof filter, 0, 0, 0, 0, 0}, filter_off;
    TOGGLEKEYS toggle = {sizeof toggle, 0}, toggle_off;
    SystemParametersInfoW(SPI_GETSTICKYKEYS, sizeof sticky, &sticky, 0);
    SystemParametersInfoW(SPI_GETFILTERKEYS, sizeof filter, &filter, 0);
    SystemParametersInfoW(SPI_GETTOGGLEKEYS, sizeof toggle, &toggle, 0);
    sticky_off = sticky;
    filter_off = filter;
    toggle_off = toggle;
    sticky_off.dwFlags &= ~(DWORD)(SKF_HOTKEYACTIVE | SKF_CONFIRMHOTKEY);
    filter_off.dwFlags &= ~(DWORD)(FKF_HOTKEYACTIVE | FKF_CONFIRMHOTKEY);
    toggle_off.dwFlags &= ~(DWORD)(TKF_HOTKEYACTIVE | TKF_CONFIRMHOTKEY);
    SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof sticky_off, &sticky_off, 0);
    SystemParametersInfoW(SPI_SETFILTERKEYS, sizeof filter_off, &filter_off, 0);
    SystemParametersInfoW(SPI_SETTOGGLEKEYS, sizeof toggle_off, &toggle_off, 0);

    g_hwnd = FindWindowW(L"zen_platform_window", L"" TITLE);
    g_other = CreateWindowExW(0, L"STATIC", L"other", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 600, 200, 120, 80, NULL, NULL,
                              GetModuleHandleW(NULL), NULL);
    CHECK(g_hwnd != NULL && g_other != NULL);
    frames(w, 10);
    if (!foreground(w, g_hwnd) || !window_is_focused(w))
    {
        printf("SKIP the window never came to the front (foreground %p, ours %p)\n", (void *)GetForegroundWindow(),
               (void *)g_hwnd);
        window_destroy(w);
        platform_shutdown();
        return 77;
    }

    static void (*const tests[])(PlatformWindow *) = {
        test_key_sweep,
        test_both_shifts,
        test_long_hold,
        test_keys_together,
        test_focus_away_held,
        test_mouse_buttons,
        test_mouse_wheel,
        test_mouse_drag,
        test_mouse_captured,
        test_altgr,
        test_print_screen,
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
    {
        if (!foreground(w, g_hwnd))
            printf("  test %zu: the window is not in front (foreground %p)\n", i, (void *)GetForegroundWindow());
        cursor_to(100, 75);
        frames(w, 5);
        tests[i](w);
    }

    SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof sticky, &sticky, 0);
    SystemParametersInfoW(SPI_SETFILTERKEYS, sizeof filter, &filter, 0);
    SystemParametersInfoW(SPI_SETTOGGLEKEYS, sizeof toggle, &toggle, 0);
    DestroyWindow(g_other);
    window_destroy(w);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
