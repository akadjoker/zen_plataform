/*
 * test_win32_review.c - what reading the Windows backend for the defects found on X11 turned up, one test for each fix. Written
 * on a machine with no Windows (compiled with MinGW by tools/check.sh, NEVER RUN): run it on Windows before trusting it.
 * Every window is hidden. The clipboard is the user's: the test puts data there and leaves it empty.
 *
 *   hit test and live callback destroying the window     the destroy waits for the callback to return
 *   sizes and positions                                   INT_MAX, 0 and negative values neither overflow nor fail
 *   vsync on a window of pixels                           another window's swap interval is left alone
 *   WM_DROPFILES                                          the paths outlive the message; a handle that is not a drop is not read nor freed
 *   the clipboard                                         an unterminated text, a DIB larger than its block, an HDROP whose list does not end
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <GL/gl.h>

/* the DROPFILES of <shlobj.h> */
typedef struct
{
    DWORD pFiles;
    POINT pt;
    BOOL fNC;
    BOOL fWide;
} DROPFILES;

#include "platform.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int core_window_count(void); /* the windows still allocated (src/core.c; internal, linked in statically) */

static int g_pass, g_fail;

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
            g_pass++;                                              \
        else                                                       \
        {                                                          \
            g_fail++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

static PlatformWindow *make(RenderMode render, int w, int h)
{
    WindowConfig cfg = {.title = "review", .width = w, .height = h, .render = render, .hidden = true, .resizable = true};
    return window_create(&cfg);
}

static HWND hwnd_of(PlatformWindow *w)
{
    return (HWND)window_native_handle(w, NATIVE_WINDOW);
}

/* ---- the application's code in a window procedure ---- */

static int g_calls, g_count_after;

static HitTestResult hit_destroys(PlatformWindow *w, int x, int y, void *user)
{
    (void)x, (void)y, (void)user;
    g_calls++;
    window_destroy(w);
    g_count_after = core_window_count(); /* deferred: the window is still allocated, the code that called us is still using it */
    return HIT_NORMAL;
}

static void live_destroys(PlatformWindow *w, void *user)
{
    (void)user;
    g_calls++;
    window_destroy(w);
    g_count_after = core_window_count();
}

/* WM_NCHITTEST comes with every mouse move, also while the program is in a dialog or any call that pumps messages: no scope of
   the platform is open then. Sent here from outside window_begin_frame, as such a message is. */
static void test_callbacks_that_destroy(void)
{
    PlatformWindow *w = make(RENDER_PIXELS, 200, 150);
    CHECK(w != NULL);
    if (!w)
        return;
    int before = core_window_count();
    window_set_hit_test(w, hit_destroys, NULL);
    g_calls = 0;
    HWND h = hwnd_of(w);
    SendMessageW(h, WM_NCHITTEST, 0, MAKELPARAM(10, 10));
    CHECK(g_calls == 1);
    CHECK(g_count_after == before); /* it was freed after the hit test returned, not inside it (it used to be: use after free) */
    CHECK(core_window_count() == before - 1);

    w = make(RENDER_PIXELS, 200, 150);
    CHECK(w != NULL);
    if (!w)
        return;
    before = core_window_count();
    window_set_live_callback(w, live_destroys, NULL);
    g_calls = 0;
    h = hwnd_of(w);
    SendMessageW(h, WM_ENTERSIZEMOVE, 0, 0); /* the OS's own loop for dragging begins: the live callback is due */
    SendMessageW(h, WM_TIMER, 1 /* the live timer */, 0);
    CHECK(g_calls == 1);
    CHECK(g_count_after == before);
    CHECK(core_window_count() == before - 1);
}

/* ---- sizes ---- */

static void test_sizes(void)
{
    PlatformWindow *w = make(RENDER_PIXELS, INT_MAX, INT_MAX);
    CHECK(w != NULL); /* not refused: clamped */
    if (!w)
        return;
    int cw, ch;
    window_get_size(w, &cw, &ch);
    CHECK(cw >= 1 && cw <= 32767 + 64 && ch >= 1 && ch <= 32767 + 64);
    static const int values[][2] = {{INT_MAX, INT_MAX}, {INT_MIN, INT_MIN}, {0, 0}, {-5, 100}, {100, -5}, {1, 1}, {100000, 100000}, {200, 150}};
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++)
    {
        window_set_size(w, values[i][0], values[i][1]);
        window_get_size(w, &cw, &ch);
        CHECK(cw >= 0 && cw <= 32767 + 64 && ch >= 0 && ch <= 32767 + 64);
    }
    window_set_position(w, INT_MAX, INT_MAX);
    window_set_position(w, INT_MIN, INT_MIN);
    window_set_position(w, 0, 0);
    window_set_icon(w, 100000, 100000, (const uint8_t *)"x"); /* the size is not believed (nothing is read from the pixels) */
    window_begin_frame(w);
    window_destroy(w);
    WindowConfig cfg = {.title = "at the edge", .width = 100, .height = 100, .x = INT_MAX, .y = INT_MIN, .hidden = true, .render = RENDER_PIXELS};
    w = window_create(&cfg);
    CHECK(w != NULL);
    window_destroy(w);
}

/* ---- vsync ---- */

typedef int(WINAPI *PFN_GetSwapInterval)(void);

static void test_vsync_of_pixel_window(void)
{
    PlatformWindow *gl = make(RENDER_GL, 200, 150);
    if (!gl)
    {
        printf("  (no OpenGL here: the vsync check is skipped)\n");
        return;
    }
    PFN_GetSwapInterval get = (PFN_GetSwapInterval)(void *)wglGetProcAddress("wglGetSwapIntervalEXT");
    if (!get)
    {
        printf("  (no wglGetSwapIntervalEXT: the vsync check is skipped)\n");
        window_destroy(gl);
        return;
    }
    PlatformWindow *px = make(RENDER_PIXELS, 100, 100);
    CHECK(px != NULL);
    window_make_current(gl);
    window_set_vsync(gl, true);
    CHECK(get() == 1);
    window_set_vsync(px, false); /* a window with no context of its own: it must not touch the one that is current */
    CHECK(get() == 1);
    window_set_vsync(gl, false);
    CHECK(get() == 0);
    window_set_vsync(px, true);
    CHECK(get() == 0);
    window_destroy(px);
    window_destroy(gl);
}

/* ---- WM_DROPFILES ---- */

static HGLOBAL make_drop(const wchar_t *list, size_t list_chars, DWORD pfiles, BOOL wide)
{
    SIZE_T bytes = sizeof(DROPFILES) + list_chars * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GHND, bytes);
    DROPFILES *df = g ? GlobalLock(g) : NULL;
    if (!df)
        return NULL;
    df->pFiles = pfiles;
    df->fWide = wide;
    memcpy((char *)df + sizeof *df, list, list_chars * sizeof(wchar_t));
    GlobalUnlock(g);
    return g;
}

static bool drop_event(PlatformWindow *w, char out[2][260], int *count)
{
    window_begin_frame(w);
    Event e;
    bool got = false;
    while (poll_event(w, &e))
        if (e.type == EVENT_WINDOW_DROP && !got)
        {
            got = true;
            *count = e.data.drop.count;
            /* the frame has been taken: the paths must still be there (they were freed as the message ended) */
            for (int i = 0; i < e.data.drop.count && i < 2; i++)
                snprintf(out[i], 260, "%s", e.data.drop.paths[i]);
        }
    return got;
}

static void test_drop(void)
{
    PlatformWindow *w = make(RENDER_PIXELS, 200, 150);
    CHECK(w != NULL);
    if (!w)
        return;
    HWND h = hwnd_of(w);
    static const wchar_t list[] = L"C:\\one.txt\0C:\\two words.png\0";
    HGLOBAL good = make_drop(list, sizeof list / sizeof list[0], sizeof(DROPFILES), TRUE);
    CHECK(good != NULL);
    PostMessageW(h, WM_DROPFILES, (WPARAM)good, 0);
    char paths[2][260];
    int count = 0;
    CHECK(drop_event(w, paths, &count));
    CHECK(count == 2);
    CHECK(strcmp(paths[0], "C:/one.txt") == 0);
    CHECK(strcmp(paths[1], "C:/two words.png") == 0);

    /* handles that are not drops: not read, no event, and not freed (DragFinish frees whatever the handle is) */
    static const wchar_t unended[] = L"C:\\x.txt"; /* no empty name after the last: the list does not end */
    HGLOBAL bad[4];
    bad[0] = make_drop(unended, 8, sizeof(DROPFILES), TRUE);                  /* but the block has no double NUL: ends mid-block */
    bad[1] = make_drop(list, sizeof list / sizeof list[0], 0x7FFFFFF0, TRUE); /* the list starts far past the block */
    bad[2] = make_drop(list, sizeof list / sizeof list[0], 3, TRUE);           /* ... inside the header */
    bad[3] = GlobalAlloc(GHND, 4);                                             /* too small to be one */
    for (int i = 0; i < 4; i++)
    {
        CHECK(bad[i] != NULL);
        PostMessageW(h, WM_DROPFILES, (WPARAM)bad[i], 0);
    }
    PostMessageW(h, WM_DROPFILES, (WPARAM)0x1234, 0); /* not a handle at all */
    PostMessageW(h, WM_DROPFILES, 0, 0);
    CHECK(!drop_event(w, paths, &count));
    for (int i = 0; i < 4; i++)
        CHECK(bad[i] && GlobalSize(bad[i]) > 0); /* still ours: nothing freed them */
    for (int i = 0; i < 4; i++)
        if (bad[i])
            GlobalFree(bad[i]);
    window_destroy(w);
}

/* ---- the clipboard ---- */

static bool clip_put(UINT format, const void *data, SIZE_T bytes, bool fill)
{
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    void *p = g ? GlobalLock(g) : NULL;
    if (!p)
        return false;
    SIZE_T real = GlobalSize(g);
    if (fill)
        memset(p, 'x', real); /* every byte of the block, whatever it was rounded up to: nothing in it is a terminator */
    else
        memcpy(p, data, bytes);
    GlobalUnlock(g);
    if (!OpenClipboard(NULL))
    {
        GlobalFree(g);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(format, g) != NULL;
    CloseClipboard();
    if (!ok)
        GlobalFree(g);
    return ok;
}

static void test_clipboard(void)
{
    PlatformWindow *w = make(RENDER_PIXELS, 100, 100); /* a window of this program owns the clipboard calls */
    CHECK(w != NULL);

    /* text with no NUL anywhere in its block: the text ends where the block does */
    if (clip_put(CF_UNICODETEXT, NULL, 64, true))
    {
        const char *t = clipboard_get();
        size_t n = t ? strlen(t) : 0;
        CHECK(t != NULL);
        CHECK(n <= 64); /* 32 wide characters at the most (the block may have been rounded up: not by much) */
        CHECK(n >= 30);
    }
    else
        printf("  (the clipboard is not available: the text check is skipped)\n");

    /* a DIB whose header says 30000 x 30000 over a block of 64 bytes: refused, not read for a gigabyte */
    BITMAPINFOHEADER bi;
    memset(&bi, 0, sizeof bi);
    bi.biSize = sizeof bi;
    bi.biWidth = 30000;
    bi.biHeight = 30000;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    unsigned char block[64];
    memset(block, 0, sizeof block);
    memcpy(block, &bi, sizeof bi);
    if (clip_put(CF_DIB, block, sizeof block, false))
    {
        size_t n = 0;
        void *png = clipboard_get_data(CLIPBOARD_PNG, &n);
        CHECK(png == NULL);
        if (png)
            fs_free(png);
        /* a header with a huge size and a palette count of 4 billion is refused too */
        bi.biWidth = 2;
        bi.biHeight = 2;
        bi.biSize = 0xFFFFFFF0u;
        bi.biClrUsed = 0xFFFFFFFFu;
        memcpy(block, &bi, sizeof bi);
        if (clip_put(CF_DIB, block, sizeof block, false))
        {
            png = clipboard_get_data(CLIPBOARD_PNG, &n);
            CHECK(png == NULL);
            if (png)
                fs_free(png);
        }
        /* and a small one that does fit is turned into a PNG */
        bi.biSize = sizeof bi;
        bi.biClrUsed = 0;
        memcpy(block, &bi, sizeof bi);
        memset(block + sizeof bi, 0x80, 16); /* 2 x 2 pixels of 32 bits */
        if (clip_put(CF_DIB, block, sizeof bi + 16, false))
        {
            png = clipboard_get_data(CLIPBOARD_PNG, &n);
            CHECK(png != NULL && n > 8 && memcmp(png, "\x89PNG", 4) == 0);
            if (png)
                fs_free(png);
        }
    }

    /* an HDROP whose list starts past its block, and one whose list does not end: no uris, no crash */
    static const wchar_t list[] = L"C:\\a.txt\0\0";
    HGLOBAL far_start = make_drop(list, sizeof list / sizeof list[0], 0x7FFFFFF0, TRUE);
    if (far_start)
    {
        DROPFILES *df = GlobalLock(far_start);
        SIZE_T sz = GlobalSize(far_start);
        (void)df;
        GlobalUnlock(far_start);
        if (OpenClipboard(NULL))
        {
            EmptyClipboard();
            SetClipboardData(CF_HDROP, far_start);
            CloseClipboard();
            size_t n = 0;
            void *uris = clipboard_get_data(CLIPBOARD_URIS, &n);
            CHECK(uris == NULL);
            if (uris)
                fs_free(uris);
        }
        (void)sz;
    }
    if (OpenClipboard(NULL))
    {
        EmptyClipboard();
        CloseClipboard();
    }
    window_destroy(w);
}

int main(void)
{
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return 77;
    }
    test_callbacks_that_destroy();
    test_sizes();
    test_vsync_of_pixel_window();
    test_drop();
    test_clipboard();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
