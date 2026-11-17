/*
 * test_drop_cocoa.c - files dropped on a window on macOS (phase 6): EVENT_WINDOW_DROP with the paths. The drag is a made-up
 * NSDraggingInfo (a class made at run time that answers draggingPasteboard with a pasteboard of file URLs) given to the view's
 * NSDraggingDestination methods, as AppKit gives it a real one; the dragging of a real file from the Finder cannot be made here.
 *
 * WRITTEN, NEVER COMPILED OR RUN: made when no run of the macOS job was allowed (see docs/MAC-PORT.md); checked only for
 * syntax against stand-in headers. Skips (77) without a graphical session, unless ZP_REQUIRE_DISPLAY is set.
 */
#include "cocoa_test_util.h"

static id g_pasteboard;

static id fake_dragging_pasteboard(id self, SEL cmd)
{
    (void)self;
    (void)cmd;
    return g_pasteboard;
}

static id make_dragging_info(void)
{
    static Class cls;
    if (!cls)
    {
        cls = objc_allocateClassPair((Class)ns_class("NSObject"), "ZenFakeDraggingInfo", 0);
        class_addMethod(cls, sel_registerName("draggingPasteboard"), (IMP)fake_dragging_pasteboard, "@@:");
        objc_registerClassPair(cls);
    }
    return msg_id(msg_id((id)cls, "alloc"), "init");
}

/* a pasteboard of its own with these paths as file URLs */
static void fill(const char *const *paths, int n)
{
    g_pasteboard = msg_id(ns_class("NSPasteboard"), "pasteboardWithUniqueName");
    msg_void(g_pasteboard, "clearContents");
    id urls = msg_id(ns_class("NSMutableArray"), "array");
    for (int i = 0; i < n; i++)
        msg_void_id(urls, "addObject:", msg_id_id(ns_class("NSURL"), "fileURLWithPath:", ns_string(paths[i])));
    MSG(unsigned char, id, SEL, id)(g_pasteboard, sel_registerName("writeObjects:"), urls);
}

static unsigned long enter(PlatformWindow *w, id info)
{
    return MSG(unsigned long, id, SEL, id)(ns_view(w), sel_registerName("draggingEntered:"), info);
}

static bool drop(PlatformWindow *w, id info)
{
    bool ok = MSG(unsigned char, id, SEL, id)(ns_view(w), sel_registerName("performDragOperation:"), info) != 0;
    pump_frame(w);
    return ok;
}

int main(void)
{
    alarm(60);
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    WindowConfig cfg = {.title = "cocoa drop test", .width = 200, .height = 150, .x = 60, .y = 60, .render = RENDER_VULKAN};
    PlatformWindow *w = window_create(&cfg);
    if (!w)
    {
        printf("%s: %s\n", ZP_REQUIRE ? "FAIL" : "skip", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    pump_for(w, 200);
    id info = make_dragging_info();

    /* two files: one event, with both paths, in order */
    static const char *two[] = {"/tmp/zen drop/a b.txt", "/tmp/zen-drop-b.png"};
    fill(two, 2);
    CHECK(enter(w, info) == 1); /* NSDragOperationCopy */
    seen_reset();
    CHECK(drop(w, info));
    const Event *e = seen_nth(EVENT_WINDOW_DROP, 0);
    CHECK(seen_count(EVENT_WINDOW_DROP) == 1 && e && e->data.drop.count == 2);
    if (e && e->data.drop.count == 2)
        CHECK(strcmp(e->data.drop.paths[0], two[0]) == 0 && strcmp(e->data.drop.paths[1], two[1]) == 0);

    /* a path with characters outside ASCII */
    static const char *odd[] = {"/tmp/zen-caf\xC3\xA9-\xE6\x97\xA5.txt"};
    fill(odd, 1);
    seen_reset();
    CHECK(drop(w, info));
    e = seen_nth(EVENT_WINDOW_DROP, 0);
    CHECK(e && e->data.drop.count == 1 && strcmp(e->data.drop.paths[0], odd[0]) == 0);

    /* a drag of something that is not files is refused: no operation, no event */
    g_pasteboard = msg_id(ns_class("NSPasteboard"), "pasteboardWithUniqueName");
    msg_void(g_pasteboard, "clearContents");
    MSG(unsigned char, id, SEL, id, id)(g_pasteboard, sel_registerName("setString:forType:"), ns_string("not a file"), ns_string("public.utf8-plain-text"));
    CHECK(enter(w, info) == 0);
    seen_reset();
    CHECK(!drop(w, info));
    CHECK(seen_count(EVENT_WINDOW_DROP) == 0);

    /* many drops in a row, and a window destroyed after one (the paths are the window's, and go with it) */
    for (int i = 0; i < 50; i++)
    {
        fill(two, 2);
        seen_reset();
        CHECK(drop(w, info));
        CHECK(seen_count(EVENT_WINDOW_DROP) == 1);
    }
    window_destroy(w);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
