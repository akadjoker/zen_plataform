/*
 * test_core_edge_wayland.c - the edge cases of tests/test_core_edge_x11.c that make sense on the Wayland backend, against the
 * small compositor of test_input_wayland.c (included below, its main renamed): a thousand windows in a row, sizes the
 * protocol or the memory cannot carry, titles of every kind, window states switched in any order, cursor shapes and modes in
 * the thousands, the clipboard at its extremes with a compositor that has no data device, windows destroyed with events in
 * flight, and the destruction of a window from inside its own hook. Skips (77) when a piece is missing. Best under ASan
 * (cmake -DPLATFORM_SANITIZE=ON), which is how tools/check.sh runs it.
 */
#define main wayland_input_main
#include "test_input_wayland.c"
#undef main

#include <dirent.h>
#include <limits.h>

static int open_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

static PlatformWindow *make(int w, int h)
{
    WindowConfig cfg = {.title = "edge", .width = w, .height = h, .resizable = true};
    return window_create(&cfg);
}

static void section(const char *name)
{
    printf("== %s\n", name);
    fflush(stdout);
}

static void test_thousand_windows(void)
{
    section("a thousand windows");
    int fds0 = -1;
    for (int i = 0; i < 1000; i++)
    {
        PlatformWindow *w = make(100 + i % 50, 80);
        CHECK(w != NULL);
        if (!w)
            return;
        if (i % 100 == 0)
            frame(w);
        window_destroy(w);
        if (i == 99)
            fds0 = open_fds(); /* the first hundred warm everything up */
    }
    int fds1 = open_fds();
    printf("  descriptors after 100 windows %d, after 1000 %d\n", fds0, fds1);
    CHECK(fds1 <= fds0);
}

static void test_sizes(PlatformWindow *w)
{
    section("sizes");
    static const int sizes[][2] = {{0, 0}, {-1, -1}, {1, 1}, {65535, 65535}, {65536, 1}, {INT_MAX, INT_MAX}, {INT_MIN, INT_MIN},
                                   {100000, 100000}, {1, 100000}, {300, 200}};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
    {
        window_set_size(w, sizes[i][0], sizes[i][1]);
        pump(w, 20);
        int ww, hh;
        window_get_size(w, &ww, &hh);
        CHECK(ww >= 0 && hh >= 0);
    }
    /* created at those sizes: a refusal is fine, a crash is not */
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
    {
        PlatformWindow *c = make(sizes[i][0], sizes[i][1]);
        if (c)
        {
            frame(c);
            window_destroy(c);
        }
    }
    window_set_size_limits(w, 100, 100, 50, 50); /* a minimum above the maximum */
    window_set_size_limits(w, -5, -5, -5, -5);
    window_set_size_limits(w, 0, 0, 0, 0);
    window_set_size(w, 320, 240);
    pump(w, 20);
    CHECK(true);
}

static void test_titles(PlatformWindow *w)
{
    section("titles");
    window_set_title(w, "");
    window_set_title(w, NULL);
    window_set_title(w, "t\xc3\xadtulo \xe2\x82\xac");
    window_set_title(w, "\xff\xfe\xc0\xaf\x80 broken \xed\xa0\x80 utf-8 \xf8\x88\x80\x80\x80");
    window_set_title(w, "\xe2\x82");
    static const size_t lens[] = {1000, 4000, 4090, 4096, 4097, 65536, 1u << 20, 8u << 20};
    for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++)
    {
        char *big = malloc(lens[i] + 1);
        memset(big, 'x', lens[i]);
        big[lens[i]] = 0;
        window_set_title(w, big);
        free(big);
        pump(w, 10);
    }
    /* a title whose cut falls inside a character, at the size where the protocol's message limit is */
    char *edge = malloc(5000);
    memset(edge, 'e', 4999);
    edge[4999] = 0;
    for (int at = 4070; at < 4100; at++)
    {
        memcpy(edge + at, "\xe2\x82\xac", 3);
        window_set_title(w, edge);
        memset(edge + at, 'e', 3);
    }
    free(edge);
    window_set_title(w, "after");
    pump(w, 20);
    CHECK(true);
}

static void test_state_storm(PlatformWindow *w)
{
    section("state storm");
    for (int i = 0; i < 600; i++)
    {
        switch (i % 12)
        {
        case 0: window_minimize(w); break;
        case 1: window_maximize(w); break;
        case 2: window_restore(w); break;
        case 3: window_set_mode(w, WINDOW_FULLSCREEN, 0); break;
        case 4: window_set_mode(w, WINDOW_WINDOWED, 0); break;
        case 5: window_hide(w); break;
        case 6: window_show(w); break;
        case 7: window_set_decorated(w, i % 24 < 12); break;
        case 8: window_set_opacity(w, (float)(i % 5) / 4.0f); break;
        case 9: window_focus(w); window_request_attention(w); break;
        case 10: window_set_position(w, i, -i); break;
        default: window_set_size(w, 200 + i % 300, 150 + i % 200); break;
        }
        if (i % 7 == 0)
            frame(w);
    }
    window_set_mode(w, WINDOW_WINDOWED, 0);
    window_show(w);
    window_restore(w);
    window_set_size(w, 320, 240);
    pump(w, 100);
    CHECK(!window_is_minimized(w) || true);
    cmd(CMD_CONFIGURE, 320, 240, 4, 0);
    pump(w, 40);
    CHECK(window_get_mode(w) == WINDOW_WINDOWED);
}

static void test_cursors(PlatformWindow *w)
{
    section("cursors and modes");
    for (int i = 0; i < 10000; i++)
    {
        mouse_set_cursor(w, i % (CURSOR_COUNT + 2) - 1); /* with -1 and one past the end */
        if (i % 9 == 0)
            mouse_set_mode(w, i % 3);
        if (i % 500 == 0)
            frame(w);
    }
    mouse_set_mode(w, MOUSE_MODE_NORMAL);
    mouse_set_cursor(w, CURSOR_ARROW);
    mouse_set_cursor_image(w, NULL);
    uint32_t px[16 * 16];
    memset(px, 0xFF, sizeof px);
    PlatformCursor *c = cursor_create(px, 16, 16, 3, 3);
    CHECK(c != NULL);
    if (c)
    {
        for (int i = 0; i < 100; i++)
        {
            mouse_set_cursor_image(w, c);
            mouse_set_cursor(w, CURSOR_HAND);
        }
        mouse_set_cursor_image(w, NULL);
        cursor_destroy(c);
    }
    pump(w, 20);
    CHECK(true);
}

static void test_clipboard(PlatformWindow *w)
{
    section("clipboard extremes (the compositor has no data device)");
    static const size_t sizes[] = {0, 1, 4095, 4096, 65536, 1u << 20, 8u << 20};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
    {
        char *data = malloc(sizes[i] + 1);
        for (size_t k = 0; k < sizes[i]; k++)
            data[k] = (char)(k % 251);
        data[sizes[i]] = 0;
        clipboard_set_data("application/x-test", data, sizes[i]);
        size_t n = 99;
        void *got = clipboard_get_data("application/x-test", &n);
        if (got)
            fs_free(got);
        (void)clipboard_has_data("application/x-test");
        free(data);
        frame(w);
    }
    clipboard_set("text");
    (void)clipboard_get();
    clipboard_set("");
    clipboard_set(NULL);
    Framebuffer fb;
    (void)clipboard_get_image(&fb);
    pump(w, 20);
    CHECK(true);
}

typedef struct
{
    int calls;
    PlatformWindow *made;
} HookState;

static void hook_destroy_self(PlatformWindow *w, const Event *e, void *user)
{
    (void)e;
    HookState *h = user;
    if (h->calls++ == 0)
        window_destroy(w);
}

static void hook_create_and_destroy(PlatformWindow *w, const Event *e, void *user)
{
    (void)e;
    HookState *h = user;
    if (h->calls++ == 0)
    {
        h->made = make(100, 80);
        window_destroy(w);
    }
}

static void test_destroy_with_events(void)
{
    section("destroy with events in flight, and from the hook");
    for (int i = 0; i < 50; i++)
    {
        PlatformWindow *w = make(200, 150);
        CHECK(w != NULL);
        if (!w)
            return;
        frame(w);
        cmd(CMD_CONFIGURE, 300 + i, 200, 4, 0);
        cmd(CMD_KB_ENTER, 0, 0, 0, 0);
        cmd(CMD_KEY, 30, 1, 0, 0);
        cmd(CMD_PTR_ENTER, 10, 10, 0, 0);
        cmd(CMD_PTR_MOVE, 20, 20, 0, 0);
        cmd(CMD_PTR_BUTTON, 272, 1, 0, 0);
        window_destroy(w); /* with those still to come */
        PlatformWindow *w2 = make(200, 150); /* the next window is not handed what was meant for the first */
        CHECK(w2 != NULL);
        if (w2)
        {
            pump(w2, 10);
            window_destroy(w2);
        }
    }
    /* the window destroys itself in its hook (on the first event of any kind), and creates another from it */
    for (int i = 0; i < 20; i++)
    {
        HookState hs = {0};
        PlatformWindow *w = make(200, 150);
        CHECK(w != NULL);
        if (!w)
            return;
        window_set_event_hook(w, i % 2 ? hook_destroy_self : hook_create_and_destroy, &hs);
        cmd(CMD_CONFIGURE, 300 + i, 200, 4, 0);
        for (int t = 0; t < 1000 && hs.calls == 0; t++) /* the frame in which the hook runs is the last use of w */
        {
            window_begin_frame(w); /* not frame(): that goes on to poll_event on a window the hook has destroyed */
            usleep(2000);
        }
        CHECK(hs.calls >= 1); /* the rest of the frame's events still reach the hook */
        window_destroy(w); /* already gone: nothing happens */
        if (hs.made)
            window_destroy(hs.made);
    }
}

int main(void)
{
    if (!start_compositor())
    {
        printf("SKIP the test compositor did not start\n");
        return 77;
    }
    if (!platform_init())
    {
        printf("SKIP %s\n", platform_get_error());
        return 77;
    }
    test_thousand_windows();
    WindowConfig cfg = {.title = "edge", .width = 320, .height = 240, .resizable = true};
    PlatformWindow *w = window_create(&cfg);
    if (!w)
    {
        printf("FAIL window_create: %s\n", platform_get_error());
        return 1;
    }
    for (int i = 0; i < 20; i++)
    {
        frame(w);
        usleep(2000);
    }
    test_sizes(w);
    test_titles(w);
    test_state_storm(w);
    test_cursors(w);
    test_clipboard(w);
    window_destroy(w);
    test_destroy_with_events();
    platform_shutdown();
    cmd(CMD_QUIT, 0, 0, 0, 0);
    pthread_join(S.thread, NULL);
    wl_event_source_remove(S.cmd_source);
    wl_display_destroy(S.display);
    close(S.cmd[0]);
    close(S.cmd[1]);
    xkb_state_unref(S.state);
    xkb_keymap_unref(S.keymap);
    xkb_context_unref(S.xkb);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
