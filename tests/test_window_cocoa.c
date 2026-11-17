/*
 * test_window_cocoa.c - window modes and looks on macOS (phase 7): fullscreen and borderless fullscreen (a borderless window the
 * size of the monitor, above the menu bar, with the Dock and the menu bar hidden), going back, opacity, always on top, window
 * decorations on and off, the application icon, the window kinds (dialog with a parent, utility, popup, tooltip), the hit test
 * (a custom title bar and edges: a drag on an edge resizes the window) and the trackpad gestures (pinch, rotate, swipe) as touch
 * points for the gesture module. AppKit is read through the runtime as the oracle (styleMask, level, alphaValue, presentation
 * options, parentWindow). A real trackpad, the system's drag of a window by a title bar, and what the Dock shows cannot be made
 * here; the gesture events are made-up objects with the methods the view reads (phase, magnification, rotation, deltaX, deltaY).
 *
 * WRITTEN, NEVER COMPILED OR RUN: made when no run of the macOS job was allowed (see docs/MAC-PORT.md); checked only for
 * syntax against stand-in headers. Skips (77) without a graphical session, unless ZP_REQUIRE_DISPLAY is set.
 */
#include "cocoa_test_util.h"

#define STYLE_TITLED 1UL
#define STYLE_UTILITY 16UL
#define LEVEL_FLOATING 3
#define LEVEL_ABOVE_MENU 25 /* NSMainMenuWindowLevel + 1 */
#define LEVEL_POPUP 101
#define PRESENT_HIDE_DOCK_AND_MENU 10UL /* NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar */

static unsigned long style_of(PlatformWindow *w)
{
    return msg_ulong(ns_window(w), "styleMask");
}
static long level_of(PlatformWindow *w)
{
    return msg_long(ns_window(w), "level");
}
static unsigned long presentation(void)
{
    return msg_ulong(ns_app(), "presentationOptions");
}

static PlatformWindow *make(WindowKind kind, PlatformWindow *parent, bool undecorated)
{
    WindowConfig cfg = {.title = "test_window_cocoa", .width = 300, .height = 200, .x = 100, .y = 100, .render = RENDER_VULKAN, .resizable = true,
                        .kind = kind, .parent = parent, .undecorated = undecorated};
    PlatformWindow *w = window_create(&cfg);
    if (w)
        pump_for(w, 200);
    return w;
}

static bool wait_size(PlatformWindow *w, int cw, int ch)
{
    int x, y;
    for (int i = 0; i < 100; i++)
    {
        pump_frame(w);
        window_get_size(w, &x, &y);
        if (x == cw && y == ch)
            return true;
        time_sleep(10);
    }
    return false;
}

static void test_modes(void)
{
    MonitorInfo mi;
    CHECK(monitor_get_info(0, &mi));
    PlatformWindow *w = make(WINDOW_KIND_NORMAL, NULL, false);
    CHECK(w != NULL);
    if (!w)
        return;
    CHECK(window_get_mode(w) == WINDOW_WINDOWED);
    CHECK(style_of(w) & STYLE_TITLED);
    for (int pass = 0; pass < 2; pass++)
    {
        WindowMode mode = pass == 0 ? WINDOW_FULLSCREEN : WINDOW_FULLSCREEN_BORDERLESS;
        seen_reset();
        window_set_mode(w, mode, 0);
        CHECK(window_get_mode(w) == mode);
        CHECK(wait_size(w, mi.width, mi.height));
        int px, py;
        window_get_position(w, &px, &py);
        printf("  mode %d: window %dx%d at %d,%d (monitor %dx%d at %d,%d)\n", (int)mode, mi.width, mi.height, px, py, mi.width, mi.height, mi.x, mi.y);
        CHECK(px == mi.x && py == mi.y);
        CHECK(!(style_of(w) & STYLE_TITLED));
        CHECK(level_of(w) == LEVEL_ABOVE_MENU);
        CHECK(presentation() == PRESENT_HIDE_DOCK_AND_MENU);
        CHECK(seen_count(EVENT_WINDOW_RESIZE) >= 1);

        window_set_mode(w, WINDOW_WINDOWED, 0);
        CHECK(window_get_mode(w) == WINDOW_WINDOWED);
        CHECK(wait_size(w, 300, 200));
        window_get_position(w, &px, &py);
        CHECK(px == 100 && py == 100);
        CHECK(style_of(w) & STYLE_TITLED);
        CHECK(level_of(w) == 0);
        CHECK(presentation() == 0);
    }
    /* a mode asked for twice is the same mode; asking for windowed when it is windowed does nothing */
    window_set_mode(w, WINDOW_FULLSCREEN, MONITOR_CURRENT);
    window_set_mode(w, WINDOW_FULLSCREEN, MONITOR_CURRENT);
    window_set_mode(w, WINDOW_WINDOWED, 0);
    window_set_mode(w, WINDOW_WINDOWED, 0);
    CHECK(presentation() == 0 && wait_size(w, 300, 200));

    /* the decoration that was asked for while it was fullscreen shows when it returns to a window */
    window_set_mode(w, WINDOW_FULLSCREEN, 0);
    window_set_decorated(w, false);
    CHECK(!(style_of(w) & STYLE_TITLED));
    window_set_mode(w, WINDOW_WINDOWED, 0);
    CHECK(!(style_of(w) & STYLE_TITLED));
    window_set_decorated(w, true);
    window_destroy(w);

    /* made fullscreen, and destroyed while it is: the menu bar and the Dock come back; with two, after the last one */
    WindowConfig cfg = {.title = "fs", .width = 200, .height = 150, .render = RENDER_VULKAN, .mode = WINDOW_FULLSCREEN_BORDERLESS, .monitor = 0};
    PlatformWindow *a = window_create(&cfg), *b = window_create(&cfg);
    CHECK(a && b);
    if (a && b)
    {
        pump_for(a, 150);
        CHECK(window_get_mode(a) == WINDOW_FULLSCREEN_BORDERLESS && presentation() == PRESENT_HIDE_DOCK_AND_MENU);
        window_destroy(a);
        CHECK(presentation() == PRESENT_HIDE_DOCK_AND_MENU);
        window_destroy(b);
        CHECK(presentation() == 0);
    }
}

static void test_looks(void)
{
    PlatformWindow *w = make(WINDOW_KIND_NORMAL, NULL, false);
    CHECK(w != NULL);
    if (!w)
        return;
    id win = ns_window(w);
    window_set_opacity(w, 0.5f);
    CHECK(fabs(msg_double(win, "alphaValue") - 0.5) < 0.01);
    window_set_opacity(w, 2.0f);
    CHECK(msg_double(win, "alphaValue") == 1.0);
    window_set_opacity(w, -1.0f);
    CHECK(msg_double(win, "alphaValue") == 0.0);
    window_set_opacity(w, 1.0f);

    window_set_always_on_top(w, true);
    CHECK(level_of(w) == LEVEL_FLOATING);
    window_set_mode(w, WINDOW_FULLSCREEN_BORDERLESS, 0);
    CHECK(level_of(w) == LEVEL_ABOVE_MENU); /* a fullscreen window is above everything */
    window_set_mode(w, WINDOW_WINDOWED, 0);
    CHECK(level_of(w) == LEVEL_FLOATING);
    window_set_always_on_top(w, false);
    CHECK(level_of(w) == 0);
    wait_size(w, 300, 200);

    int ww, wh, x0, y0, x1, y1;
    window_get_size(w, &ww, &wh);
    window_get_position(w, &x0, &y0);
    window_set_decorated(w, false);
    pump_for(w, 100);
    CHECK(!(style_of(w) & STYLE_TITLED));
    window_get_size(w, &x1, &y1);
    CHECK(x1 == ww && y1 == wh); /* the content stays where it was and as big */
    window_get_position(w, &x1, &y1);
    CHECK(x1 == x0 && y1 == y0);
    window_set_decorated(w, true);
    pump_for(w, 100);
    CHECK(style_of(w) & STYLE_TITLED);
    window_get_size(w, &x1, &y1);
    CHECK(x1 == ww && y1 == wh);

    /* the application icon: an image of the size given; no icon (NULL) gives the default back */
    uint8_t rgba[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; i++)
    {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = (uint8_t)(i % 16 * 16);
        rgba[i * 4 + 2] = 0;
        rgba[i * 4 + 3] = (uint8_t)(i / 16 * 16 + 15);
    }
    window_set_icon(w, 16, 16, rgba);
    id icon = msg_id(ns_app(), "applicationIconImage");
    CGSize size = MSG(CGSize, id, SEL)(icon, sel_registerName("size"));
    printf("  application icon image: %.0fx%.0f\n", size.width, size.height);
    CHECK(icon != NULL && size.width == 16.0 && size.height == 16.0);
    window_set_icon(w, 0, 0, NULL);
    window_set_icon(w, 16, 16, rgba);
    window_set_icon(w, 16, 16, NULL);
    window_destroy(w);
}

static void test_kinds(void)
{
    PlatformWindow *parent = make(WINDOW_KIND_NORMAL, NULL, false);
    CHECK(parent != NULL);
    if (!parent)
        return;
    PlatformWindow *dialog = make(WINDOW_KIND_DIALOG, parent, false);
    PlatformWindow *utility = make(WINDOW_KIND_UTILITY, parent, false);
    PlatformWindow *popup = make(WINDOW_KIND_POPUP, parent, false);
    PlatformWindow *tooltip = make(WINDOW_KIND_TOOLTIP, parent, false);
    PlatformWindow *bare = make(WINDOW_KIND_NORMAL, NULL, true);
    CHECK(dialog && utility && popup && tooltip && bare);
    if (dialog && utility && popup && tooltip && bare)
    {
        /* a dialog, utility, popup and tooltip belong to the parent and stay above it */
        CHECK(msg_id(ns_window(dialog), "parentWindow") == ns_window(parent));
        CHECK(msg_id(ns_window(utility), "parentWindow") == ns_window(parent));
        CHECK(msg_id(ns_window(popup), "parentWindow") == ns_window(parent));
        CHECK(msg_id(ns_window(tooltip), "parentWindow") == ns_window(parent));
        CHECK(style_of(dialog) & STYLE_TITLED);
        CHECK((style_of(utility) & STYLE_TITLED) && (style_of(utility) & STYLE_UTILITY));
        /* a popup and a tooltip have no title bar and are above the menus; an undecorated window is only without a title bar */
        CHECK(!(style_of(popup) & STYLE_TITLED) && level_of(popup) == LEVEL_POPUP);
        CHECK(!(style_of(tooltip) & STYLE_TITLED) && level_of(tooltip) == LEVEL_POPUP);
        CHECK(!(style_of(bare) & STYLE_TITLED) && level_of(bare) == 0);
        /* a borderless window can be the key window; a tooltip never is */
        CHECK(msg_bool(ns_window(bare), "canBecomeKeyWindow"));
        CHECK(msg_bool(ns_window(popup), "canBecomeKeyWindow"));
        CHECK(!msg_bool(ns_window(tooltip), "canBecomeKeyWindow"));
        window_focus(tooltip);
        pump_for(tooltip, 100);
        CHECK(!window_is_focused(tooltip));
    }
    /* the parent goes first: the others do not hold on to it */
    window_destroy(parent);
    if (dialog)
    {
        pump_for(dialog, 50);
        window_destroy(dialog);
    }
    if (utility)
        window_destroy(utility);
    if (popup)
        window_destroy(popup);
    if (tooltip)
        window_destroy(tooltip);
    if (bare)
        window_destroy(bare);

    /* a window that is hidden at the start joins its parent when it is shown */
    parent = make(WINDOW_KIND_NORMAL, NULL, false);
    if (parent)
    {
        WindowConfig cfg = {.title = "hidden child", .width = 100, .height = 80, .render = RENDER_VULKAN, .kind = WINDOW_KIND_DIALOG, .parent = parent, .hidden = true};
        PlatformWindow *child = window_create(&cfg);
        CHECK(child != NULL);
        if (child)
        {
            CHECK(msg_id(ns_window(child), "parentWindow") == NULL);
            window_show(child);
            CHECK(msg_id(ns_window(child), "parentWindow") == ns_window(parent));
            window_destroy(child);
        }
        window_destroy(parent);
    }
}

/* ---- the hit test ---- */

static int g_hit_calls;

static HitTestResult hit(PlatformWindow *w, int x, int y, void *user)
{
    (void)user;
    g_hit_calls++;
    int cw, ch;
    window_get_size(w, &cw, &ch);
    if (x >= cw - 8 && y < 8)
        return HIT_RESIZE_TOPRIGHT;
    if (x >= cw - 8 && y >= ch - 8)
        return HIT_RESIZE_BOTTOMRIGHT;
    if (y < 24)
        return HIT_DRAG;
    return HIT_NORMAL;
}

static void test_hit(void)
{
    PlatformWindow *w = make(WINDOW_KIND_NORMAL, NULL, true);
    CHECK(w != NULL);
    if (!w)
        return;
    int cw, ch;
    window_get_size(w, &cw, &ch);
    window_set_hit_test(w, hit, NULL);

    /* over an edge the pointer's moves are asked about, and the cursor is the edge's */
    seen_reset();
    g_hit_calls = 0;
    to_view(w, "mouseMoved:", event_mouse(w, NS_MOVED, cw - 3, 3, 0, 0));
    pump_frame(w);
    CHECK(g_hit_calls >= 1);
    id current = msg_id(ns_class("NSCursor"), "currentCursor");
    printf("  over the top right edge the system's cursor is %s\n", current ? object_getClassName(current) : "(none)");

    /* a press on an edge is not the program's, and the drag that follows resizes the window; the release is not either */
    seen_reset();
    to_view(w, "mouseDown:", event_mouse(w, NS_LEFT_DOWN, cw - 3, 3, 0, 1));
    pump_frame(w);
    CHECK(seen_count(EVENT_MOUSE_BUTTON) == 0 && !mouse_button_down(w, MOUSE_LEFT));
    to_view(w, "mouseDragged:", event_mouse(w, NS_LEFT_DRAGGED, cw + 37, -27, 0, 1)); /* 40 to the right, 30 up */
    pump_frame(w);
    CHECK(seen_count(EVENT_MOUSE_MOVE) == 0); /* the resize is not a motion of the pointer for the program */
    CHECK(wait_size(w, cw + 40, ch + 30));
    to_view(w, "mouseUp:", event_mouse(w, NS_LEFT_UP, cw + 37, -27, 0, 1));
    pump_frame(w);
    CHECK(seen_count(EVENT_MOUSE_BUTTON) == 0);
    int nw, nh;
    window_get_size(w, &nw, &nh);
    printf("  after a drag of the top right edge by 40,30: %dx%d (was %dx%d)\n", nw, nh, cw, ch);

    /* the size limits hold */
    window_set_size_limits(w, 0, 0, cw + 20, ch + 10);
    seen_reset();
    to_view(w, "mouseDown:", event_mouse(w, NS_LEFT_DOWN, nw - 3, 3, 0, 1));
    to_view(w, "mouseDragged:", event_mouse(w, NS_LEFT_DRAGGED, nw + 200, -200, 0, 1));
    to_view(w, "mouseUp:", event_mouse(w, NS_LEFT_UP, nw + 200, -200, 0, 1));
    pump_frame(w);
    window_get_size(w, &nw, &nh);
    CHECK(nw <= cw + 20 && nh <= ch + 10);
    window_set_size_limits(w, 0, 0, 0, 0);

    /* the middle of the window is the program's: the press comes as ever */
    seen_reset();
    to_view(w, "mouseDown:", event_mouse(w, NS_LEFT_DOWN, 100, 100, 0, 1));
    pump_frame(w);
    CHECK(seen_count(EVENT_MOUSE_BUTTON) == 1);
    to_view(w, "mouseUp:", event_mouse(w, NS_LEFT_UP, 100, 100, 0, 1));
    pump_frame(w);
    CHECK(seen_count(EVENT_MOUSE_BUTTON) == 2);

    /* with the hit test removed everything is the program's */
    window_set_hit_test(w, NULL, NULL);
    window_get_size(w, &cw, &ch);
    seen_reset();
    to_view(w, "mouseDown:", event_mouse(w, NS_LEFT_DOWN, cw - 3, 3, 0, 1));
    pump_frame(w);
    CHECK(seen_count(EVENT_MOUSE_BUTTON) == 1);
    to_view(w, "mouseUp:", event_mouse(w, NS_LEFT_UP, cw - 3, 3, 0, 1));
    pump_frame(w);
    window_destroy(w);
}

/* ---- trackpad gestures, with made-up events ---- */

static unsigned long g_phase;
static double g_magnification, g_rotation, g_dx, g_dy;

static unsigned long fake_phase(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return g_phase;
}
static double fake_magnification(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return g_magnification;
}
static float fake_rotation(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return (float)g_rotation;
}
static double fake_dx(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return g_dx;
}
static double fake_dy(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return g_dy;
}

static id make_gesture_event(void)
{
    static Class cls;
    if (!cls)
    {
        cls = objc_allocateClassPair((Class)ns_class("NSObject"), "ZenFakeGestureEvent", 0);
        class_addMethod(cls, sel_registerName("phase"), (IMP)fake_phase, "Q@:");
        class_addMethod(cls, sel_registerName("magnification"), (IMP)fake_magnification, "d@:");
        class_addMethod(cls, sel_registerName("rotation"), (IMP)fake_rotation, "f@:");
        class_addMethod(cls, sel_registerName("deltaX"), (IMP)fake_dx, "d@:");
        class_addMethod(cls, sel_registerName("deltaY"), (IMP)fake_dy, "d@:");
        objc_registerClassPair(cls);
    }
    return msg_id(msg_id((id)cls, "alloc"), "init");
}

#define NS_PHASE_BEGAN 1UL
#define NS_PHASE_CHANGED 4UL
#define NS_PHASE_ENDED 8UL

static void send_gesture(PlatformWindow *w, const char *selector, unsigned long phase, double magnification, double rotation, double dx, double dy)
{
    g_phase = phase;
    g_magnification = magnification;
    g_rotation = rotation;
    g_dx = dx;
    g_dy = dy;
    to_view(w, selector, make_gesture_event());
    pump_frame(w);
}

static void test_gestures(void)
{
    PlatformWindow *w = make(WINDOW_KIND_NORMAL, NULL, false);
    CHECK(w != NULL);
    if (!w)
        return;
    mouse_set_position(w, 150, 100);
    pump_frame(w);

    /* a pinch out: two fingers go down around the pointer, move apart, and come up; the left button is never pressed */
    seen_reset();
    send_gesture(w, "magnifyWithEvent:", NS_PHASE_BEGAN, 0.0, 0.0, 0, 0);
    CHECK(seen_count(EVENT_TOUCH) == 4 && touch_count(w) == 2); /* two DOWN, two MOVE (the first change is applied at once) */
    float x0a, y0a, x0b, y0b;
    touch_position(w, 0, &x0a, &y0a);
    touch_position(w, 1, &x0b, &y0b);
    double d0 = hypot((double)(x0a - x0b), (double)(y0a - y0b));
    int detected = 0;
    for (int i = 0; i < 5; i++)
    {
        send_gesture(w, "magnifyWithEvent:", NS_PHASE_CHANGED, 0.3, 0.0, 0, 0);
        detected |= gesture_is_detected(w, GESTURE_PINCH_OUT) ? 1 : 0;
    }
    float x1a, y1a, x1b, y1b;
    touch_position(w, 0, &x1a, &y1a);
    touch_position(w, 1, &x1b, &y1b);
    double d1 = hypot((double)(x1a - x1b), (double)(y1a - y1b));
    printf("  pinch out: the two fingers %.0f then %.0f points apart; the gesture module saw a pinch out: %s\n", d0, d1, detected ? "yes" : "no");
    CHECK(d1 > d0 * 2.0);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
    if (!detected)
        NOTE("the gesture module did not report GESTURE_PINCH_OUT for the made-up fingers (its own thresholds); the touch points above are what this test holds to");
    send_gesture(w, "magnifyWithEvent:", NS_PHASE_ENDED, 0.0, 0.0, 0, 0);
    CHECK(touch_count(w) == 0);
    CHECK(seen_count(EVENT_MOUSE_BUTTON) == 0);

    /* a rotation turns the line of the two fingers: 30 degrees counterclockwise is -30 with y down */
    seen_reset();
    send_gesture(w, "rotateWithEvent:", NS_PHASE_BEGAN, 0.0, 0.0, 0, 0);
    send_gesture(w, "rotateWithEvent:", NS_PHASE_CHANGED, 0.0, 30.0, 0, 0);
    CHECK(touch_count(w) == 2);
    touch_position(w, 0, &x1a, &y1a);
    touch_position(w, 1, &x1b, &y1b);
    double angle = atan2((double)(y1a - y1b), (double)(x1a - x1b)) * 180.0 / M_PI;
    printf("  rotation by 30 degrees: the line of the fingers is at %.1f degrees\n", angle);
    CHECK(fabs(angle + 30.0) < 1.0);
    send_gesture(w, "rotateWithEvent:", NS_PHASE_ENDED, 0.0, 0.0, 0, 0);
    CHECK(touch_count(w) == 0);

    /* a cancelled pinch lifts the fingers too */
    send_gesture(w, "magnifyWithEvent:", NS_PHASE_BEGAN, 0.0, 0.0, 0, 0);
    send_gesture(w, "magnifyWithEvent:", 16UL /* cancelled */, 0.0, 0.0, 0, 0);
    CHECK(touch_count(w) == 0);

    /* a swipe: AppKit's deltaX is 1 for a swipe to the left and deltaY 1 for one up; one finger goes down, a good way that way, and up */
    static const struct
    {
        double dx, dy;
        int sx, sy; /* the direction the finger goes, as the screen has it */
        unsigned want;
        const char *name;
    } swipes[] = {{1, 0, -1, 0, GESTURE_SWIPE_LEFT, "left"}, {-1, 0, 1, 0, GESTURE_SWIPE_RIGHT, "right"},
                  {0, 1, 0, -1, GESTURE_SWIPE_UP, "up"}, {0, -1, 0, 1, GESTURE_SWIPE_DOWN, "down"}};
    for (size_t i = 0; i < sizeof swipes / sizeof swipes[0]; i++)
    {
        seen_reset();
        send_gesture(w, "swipeWithEvent:", 0UL, 0.0, 0.0, swipes[i].dx, swipes[i].dy);
        CHECK(seen_count(EVENT_TOUCH) == 3 && seen_count(EVENT_MOUSE_BUTTON) == 0);
        const Event *down = seen_nth(EVENT_TOUCH, 0), *up = seen_nth(EVENT_TOUCH, 2);
        if (down && up)
        {
            CHECK(down->data.touch.phase == TOUCH_DOWN && up->data.touch.phase == TOUCH_UP);
            double mx = (double)(up->data.touch.x - down->data.touch.x), my = (double)(up->data.touch.y - down->data.touch.y);
            CHECK((swipes[i].sx == 0 || mx * swipes[i].sx > 100.0) && (swipes[i].sy == 0 || my * swipes[i].sy > 100.0));
        }
        bool seen_gesture = gesture_is_detected(w, swipes[i].want);
        printf("  swipe %s: the gesture module %s\n", swipes[i].name, seen_gesture ? "reports it" : "does not report it");
        if (!seen_gesture)
            NOTE("swipe %s was not reported by the gesture module for the made-up finger", swipes[i].name);
        pump_frame(w);
    }
    CHECK(touch_count(w) == 0);
    window_destroy(w);
}

int main(void)
{
    alarm(100);
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    printf("modes\n");
    test_modes();
    printf("looks\n");
    test_looks();
    printf("kinds\n");
    test_kinds();
    printf("hit test\n");
    test_hit();
    printf("gestures\n");
    test_gestures();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
