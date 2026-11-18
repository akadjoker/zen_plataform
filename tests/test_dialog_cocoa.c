/*
 * test_dialog_cocoa.c - the native dialogs of macOS (phase 6): NSAlert, NSOpenPanel and NSSavePanel behind message_box,
 * confirm_box and dialog_*. Each dialog blocks until it is answered and nobody is there to answer, so a timer on the run loop
 * ends the modal session (abortModal) a moment after it starts: the dialog is shown, then taken as cancelled. That checks that each
 * one opens, returns and leaves the program as it was, and what a cancel returns; a path chosen by a person cannot be made.
 * open_url is tried only with what it refuses (a real URL would start a browser).
 *
 * WRITTEN, NEVER COMPILED OR RUN: made when no run of the macOS job was allowed (see docs/MAC-PORT.md); checked only for
 * syntax against stand-in headers. Skips (77) without a graphical session, unless ZP_REQUIRE_DISPLAY is set.
 */
#include "cocoa_test_util.h"

/* The modal session is ended every half second from 0.5 s after this call, until the dialog is back: the dialogs of macOS take a
   moment to come up (the open and save panels are made by a service of their own), and one abort that comes before the dialog
   is in its modal loop would end nothing and leave it waiting for a person for the rest of the test. The timer is dropped when the
   dialog is back, and no run loop turns between the two, so it cannot fire outside a dialog. */
static id arm_abort(void)
{
    id timer = MSG(id, id, SEL, double, id, SEL, id, unsigned char)(ns_class("NSTimer"),
                                                                    sel_registerName("timerWithTimeInterval:target:selector:userInfo:repeats:"), 0.5,
                                                                    ns_app(), sel_registerName("abortModal"), NULL, 1);
    MSG(void, id, SEL, id, CFStringRef)(msg_id(ns_class("NSRunLoop"), "currentRunLoop"), sel_registerName("addTimer:forMode:"), timer, kCFRunLoopCommonModes);
    return timer;
}

static void disarm(id timer)
{
    msg_void(timer, "invalidate");
}

int main(void)
{
    alarm(90);
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    WindowConfig cfg = {.title = "cocoa dialog test", .width = 200, .height = 150, .x = 60, .y = 60, .render = RENDER_VULKAN};
    PlatformWindow *w = window_create(&cfg);
    CHECK(w != NULL);
    if (!w)
        return 1;
    pump_for(w, 200);
    char out[512] = "unchanged";
    FileFilter filters[] = {{"Images", "*.png;*.jpg"}, {"Text", "*.txt"}};

    id t = arm_abort();
    CHECK(message_box(w, MESSAGE_INFO, "Title", "A message"));
    disarm(t);
    CHECK(platform_get_error()[0] == '\0');

    t = arm_abort();
    CHECK(message_box(NULL, MESSAGE_ERROR, "Error", "Something failed"));
    disarm(t);

    t = arm_abort();
    int answer = confirm_box(w, "Sure?", "Really?");
    disarm(t);
    printf("  confirm_box ended by the abort: %d\n", answer);
    CHECK(answer == 0); /* not Yes */

    t = arm_abort();
    CHECK(!dialog_open_file(w, "Open", NULL, filters, 2, out, sizeof out));
    disarm(t);
    CHECK(platform_get_error()[0] == '\0' && strcmp(out, "unchanged") == 0); /* a cancel is no error and writes nothing */

    t = arm_abort();
    CHECK(!dialog_save_file(w, "Save", "/tmp/zen-dialog-test.png", filters, 2, out, sizeof out));
    disarm(t);
    CHECK(platform_get_error()[0] == '\0');

    t = arm_abort();
    CHECK(!dialog_pick_folder(w, "Folder", "/tmp", out, sizeof out));
    disarm(t);
    CHECK(platform_get_error()[0] == '\0');

    t = arm_abort();
    CHECK(dialog_open_files(w, "Open several", "/tmp", NULL, 0, out, sizeof out) == 0);
    disarm(t);
    CHECK(platform_get_error()[0] == '\0');

    /* a filter that allows everything, and a start that is a file that does not exist */
    FileFilter all[] = {{"All", "*"}};
    t = arm_abort();
    CHECK(!dialog_open_file(NULL, NULL, "/tmp/zen-no-such-file.txt", all, 1, out, sizeof out));
    disarm(t);

    /* the window is still there and works */
    pump_for(w, 100);
    CHECK(!window_should_close(w));

    /* open_url refuses what it must, without starting anything */
    CHECK(!open_url(NULL));
    CHECK(!open_url(""));
    CHECK(!open_url("-x"));
    CHECK(strstr(platform_get_error(), "invalid URL") != NULL);
    CHECK(!open_url("https://example.com/\x01"));

    window_destroy(w);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
