/*
 * test_text_cocoa.c - text input on macOS (phase 5): the NSTextInputClient the view gives the input context. The input context
 * is driven the way it drives the view (insertText, setMarkedText, unmarkText, and the questions it asks: markedRange,
 * hasMarkedText, firstRectForCharacterRange), so what a real input method does to the view is what is done here; the input
 * method itself (a Japanese or Chinese keyboard, the dead keys of the layout) is not started. What the program sees is the
 * library's own: EVENT_CHAR, EVENT_TEXT_EDIT, char_get_pressed, window_text_composition.
 *
 * WRITTEN, NEVER COMPILED OR RUN: this file was made when no run of the macOS job was allowed (see docs/MAC-PORT.md); it was
 * checked only for syntax against stand-in headers. Skips (77) without a graphical session, unless ZP_REQUIRE_DISPLAY is set.
 */
#include "cocoa_test_util.h"

typedef struct
{
    unsigned long location, length;
} ZRange;
#define NOT_FOUND ((unsigned long)0x7fffffffffffffffUL)

static PlatformWindow *g_w;

static id nsstr(const char *utf8)
{
    return ns_string(utf8);
}

/* the client's methods, through the runtime */
static void insert_text(const char *utf8)
{
    MSG(void, id, SEL, id, ZRange)(ns_view(g_w), sel_registerName("insertText:replacementRange:"), nsstr(utf8), (ZRange){NOT_FOUND, 0});
    pump_frame(g_w);
}

static void set_marked(const char *utf8, unsigned long sel_at, unsigned long sel_len)
{
    MSG(void, id, SEL, id, ZRange, ZRange)(ns_view(g_w), sel_registerName("setMarkedText:selectedRange:replacementRange:"), nsstr(utf8),
                                           (ZRange){sel_at, sel_len}, (ZRange){NOT_FOUND, 0});
    pump_frame(g_w);
}

static void unmark(void)
{
    msg_void(ns_view(g_w), "unmarkText");
    pump_frame(g_w);
}

static ZRange marked_range(void)
{
    return MSG(ZRange, id, SEL)(ns_view(g_w), sel_registerName("markedRange"));
}

static int chars_seen(uint32_t *out, int cap)
{
    int n = 0;
    for (int i = 0; i < g_seen_count && n < cap; i++)
        if (g_seen[i].type == EVENT_CHAR)
            out[n++] = g_seen[i].data.codepoint;
    return n;
}

static void test_commit(void)
{
    uint32_t cp[8];
    static const struct
    {
        const char *utf8;
        uint32_t cp;
    } typed[] = {{"a", 'a'}, {"A", 'A'}, {" ", 0x20}, {"\xC3\xA9", 0xE9}, {"\xE2\x82\xAC", 0x20AC}, {"\xF0\x9F\x98\x80", 0x1F600}};
    for (size_t i = 0; i < sizeof typed / sizeof typed[0]; i++)
    {
        seen_reset();
        insert_text(typed[i].utf8);
        CHECK(chars_seen(cp, 8) == 1 && cp[0] == typed[i].cp);
        CHECK(seen_count(EVENT_TEXT_EDIT) == 0);
        CHECK(char_get_pressed(g_w) == typed[i].cp);
    }
    /* several characters in one commit, in order */
    seen_reset();
    insert_text("ab\xC3\xA7");
    CHECK(chars_seen(cp, 8) == 3 && cp[0] == 'a' && cp[1] == 'b' && cp[2] == 0xE7);
    /* what is not text is not delivered: control characters, delete, the C1 range, the private-use block of the arrow keys */
    static const char *not_text[] = {"\r", "\t", "\x1b", "\x7f", "\x03", "\xEF\x9C\x80", "\xEF\xA3\xBF", "\xC2\x85", ""};
    for (size_t i = 0; i < sizeof not_text / sizeof not_text[0]; i++)
    {
        seen_reset();
        insert_text(not_text[i]);
        CHECK(seen_count(EVENT_CHAR) == 0);
    }
    while (char_get_pressed(g_w))
        ;
}

static const Event *edit_nth(int i)
{
    return seen_nth(EVENT_TEXT_EDIT, i);
}

static void test_composition(void)
{
    uint32_t cp[8];
    /* a composition: each change is an edit event with the whole text and the caret in bytes */
    seen_reset();
    set_marked("n", 1, 0);
    CHECK(seen_count(EVENT_TEXT_EDIT) == 1 && strcmp(edit_nth(0)->data.edit.text, "n") == 0 && edit_nth(0)->data.edit.cursor == 1);
    CHECK(msg_bool(ns_view(g_w), "hasMarkedText"));
    ZRange m = marked_range();
    CHECK(m.location == 0 && m.length == 1);
    CHECK(strcmp(window_text_composition(g_w), "n") == 0);
    seen_reset();
    set_marked("ni", 2, 0);
    CHECK(seen_count(EVENT_TEXT_EDIT) == 1 && strcmp(edit_nth(0)->data.edit.text, "ni") == 0 && edit_nth(0)->data.edit.cursor == 2);
    m = marked_range();
    CHECK(m.location == 0 && m.length == 2);

    /* the caret inside the text, with characters that take several bytes: the offset is in bytes of UTF-8 */
    seen_reset();
    set_marked("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 1, 0); /* three characters of three bytes, the caret after the first */
    CHECK(seen_count(EVENT_TEXT_EDIT) == 1 && edit_nth(0)->data.edit.cursor == 3 && strlen(edit_nth(0)->data.edit.text) == 9);
    m = marked_range();
    CHECK(m.length == 3); /* UTF-16 units of the string */

    /* a committed text ends the composition (an empty edit first) and then arrives as characters */
    seen_reset();
    insert_text("\xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(seen_count(EVENT_TEXT_EDIT) == 1 && edit_nth(0)->data.edit.text[0] == '\0');
    CHECK(chars_seen(cp, 8) == 2 && cp[0] == 0x65E5 && cp[1] == 0x672C);
    CHECK(!msg_bool(ns_view(g_w), "hasMarkedText"));
    m = marked_range();
    CHECK(m.location == NOT_FOUND);
    CHECK(window_text_composition(g_w)[0] == '\0');
    /* the order in the stream: the end of the composition before the text */
    int edit_at = -1, char_at = -1;
    for (int i = 0; i < g_seen_count; i++)
    {
        if (g_seen[i].type == EVENT_TEXT_EDIT && edit_at < 0)
            edit_at = i;
        if (g_seen[i].type == EVENT_CHAR && char_at < 0)
            char_at = i;
    }
    CHECK(edit_at >= 0 && char_at > edit_at);

    /* cancelled: unmark ends it with an empty edit and no text */
    seen_reset();
    set_marked("abc", 3, 0);
    seen_reset();
    unmark();
    CHECK(seen_count(EVENT_TEXT_EDIT) == 1 && edit_nth(0)->data.edit.text[0] == '\0' && seen_count(EVENT_CHAR) == 0);
    CHECK(!msg_bool(ns_view(g_w), "hasMarkedText"));
    /* unmark with nothing marked says nothing */
    seen_reset();
    unmark();
    CHECK(seen_count(EVENT_TEXT_EDIT) == 0);

    /* an empty marked text is the end of a composition, not the start of one */
    set_marked("x", 1, 0);
    seen_reset();
    set_marked("", 0, 0);
    CHECK(seen_count(EVENT_TEXT_EDIT) == 1 && edit_nth(0)->data.edit.text[0] == '\0');
    CHECK(!msg_bool(ns_view(g_w), "hasMarkedText"));

    /* an edit event holds 63 bytes: a longer composition is cut on a character boundary (thirty characters of three bytes: twenty-one) */
    char longtext[128] = "";
    for (int i = 0; i < 30; i++)
        strcat(longtext, "\xE6\x97\xA5");
    seen_reset();
    set_marked(longtext, 30, 0);
    CHECK(seen_count(EVENT_TEXT_EDIT) == 1 && strlen(edit_nth(0)->data.edit.text) == 63 && edit_nth(0)->data.edit.cursor <= 63);
    unmark();
    seen_reset();
}

static void test_switch_off(void)
{
    uint32_t cp[8];
    CHECK(window_text_input_active(g_w));
    set_marked("kana", 4, 0);
    seen_reset();
    window_text_input_stop(g_w); /* a composition that is on is dropped */
    pump_frame(g_w);
    CHECK(!window_text_input_active(g_w));
    CHECK(seen_count(EVENT_TEXT_EDIT) == 1 && edit_nth(0)->data.edit.text[0] == '\0');
    CHECK(!msg_bool(ns_view(g_w), "hasMarkedText"));
    /* nothing is typed or composed while it is off */
    seen_reset();
    insert_text("a");
    set_marked("b", 1, 0);
    CHECK(seen_count(EVENT_CHAR) == 0 && seen_count(EVENT_TEXT_EDIT) == 0);
    /* and a key does not reach the input context: the key event is told, nothing is typed */
    seen_reset();
    to_view(g_w, "keyDown:", event_key(g_w, NS_KEY_DOWN, 0x00, 0, "a", false));
    pump_frame(g_w);
    CHECK(seen_count(EVENT_KEY) == 1 && seen_count(EVENT_CHAR) == 0 && seen_count(EVENT_TEXT_EDIT) == 0);
    to_view(g_w, "keyUp:", event_key(g_w, NS_KEY_UP, 0x00, 0, "a", false));
    pump_frame(g_w);
    window_text_input_start(g_w);
    seen_reset();
    insert_text("z");
    CHECK(chars_seen(cp, 8) == 1 && cp[0] == 'z');
    while (char_get_pressed(g_w))
        ;
}

static void test_focus_loss(void)
{
    set_marked("ka", 2, 0);
    seen_reset();
    msg_void_id(msg_id(ns_window(g_w), "delegate"), "windowDidResignKey:", NULL);
    pump_frame(g_w);
    CHECK(seen_count(EVENT_TEXT_EDIT) == 1 && edit_nth(0)->data.edit.text[0] == '\0');
    CHECK(!msg_bool(ns_view(g_w), "hasMarkedText"));
    msg_void_id(msg_id(ns_window(g_w), "delegate"), "windowDidBecomeKey:", NULL);
    pump_frame(g_w);
    seen_reset();
}

/* where the candidate window goes: the caret's rectangle of the program, in the screen's own (bottom left) coordinates */
static void test_rect(void)
{
    int wx, wy;
    window_get_position(g_w, &wx, &wy);
    MonitorInfo primary;
    CHECK(monitor_get_info(0, &primary));
    window_set_text_input_rect(g_w, 12, 34, 56, 18);
    typedef struct
    {
        double x, y, w, h;
    } R;
    R r;
    ZRange any = {0, 0};
    unsigned long actual[2] = {0, 0};
#if defined(__x86_64__)
    ((void (*)(R *, id, SEL, ZRange, void *))(void *)objc_msgSend_stret)(&r, ns_view(g_w), sel_registerName("firstRectForCharacterRange:actualRange:"), any, actual);
#else
    r = MSG(R, id, SEL, ZRange, void *)(ns_view(g_w), sel_registerName("firstRectForCharacterRange:actualRange:"), any, actual);
#endif
    printf("  caret rectangle (12,34 56x18 in the window) as the screen has it: %.1f,%.1f %.1fx%.1f; window at %d,%d, display %dx%d\n", r.x,
           r.y, r.w, r.h, wx, wy, primary.width, primary.height);
    CHECK(r.w == 56.0 && r.h == 18.0);
    CHECK(fabs(r.x - (wx + 12)) <= 1.0);
    CHECK(fabs(r.y - (primary.height - (wy + 34 + 18))) <= 1.0);
    /* a new rectangle is a new answer */
    window_set_text_input_rect(g_w, 0, 0, 10, 10);
}

/* a window that is destroyed in the middle of a composition takes it with it */
static void test_destroy_composing(void)
{
    WindowConfig cfg = {.title = "composing", .width = 100, .height = 80, .x = 300, .y = 300, .render = RENDER_VULKAN};
    for (int i = 0; i < 20; i++)
    {
        PlatformWindow *c = window_create(&cfg);
        CHECK(c != NULL);
        if (!c)
            break;
        MSG(void, id, SEL, id, ZRange, ZRange)(ns_view(c), sel_registerName("setMarkedText:selectedRange:replacementRange:"), nsstr("かな"),
                                               (ZRange){2, 0}, (ZRange){NOT_FOUND, 0});
        window_destroy(c);
    }
}

int main(void)
{
    alarm(100);
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    WindowConfig cfg = {.title = "cocoa text test", .width = 240, .height = 160, .x = 60, .y = 60, .render = RENDER_VULKAN};
    g_w = window_create(&cfg);
    if (!g_w)
    {
        printf("%s: %s\n", ZP_REQUIRE ? "FAIL" : "skip", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    pump_for(g_w, 250);
    seen_reset();
    printf("commit\n");
    test_commit();
    printf("composition\n");
    test_composition();
    printf("text input off\n");
    test_switch_off();
    printf("focus loss\n");
    test_focus_loss();
    printf("caret rectangle\n");
    test_rect();
    printf("destroyed while composing\n");
    test_destroy_composing();

    window_destroy(g_w);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
