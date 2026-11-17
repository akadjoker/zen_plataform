/*
 * test_input_cocoa.c - keyboard, mouse and cursors on macOS, modelled on test_input_x11.c and test_mouse_x11.c. The input is made
 * as NSEvents (see cocoa_test_util.h) and either posted to the application's own queue (the real path through NSApplication,
 * the window and the first responder) or given straight to the view; neither needs the permission that posting to the system
 * does. What the program sees (the events of window_begin_frame and the polled state) is checked against what was sent; the
 * parts the system itself does (the pointer, the display) are checked through CoreGraphics and AppKit read-only.
 *
 * What cannot be made without permission, or without a person, is said when it is skipped: a real keyboard and mouse (the
 * routing is the application's, but the device is not), a scroll that comes from a trackpad, the pointer leaving the window
 * with a button held, an application that is not the active one. Skips (77) without a graphical session, unless
 * ZP_REQUIRE_DISPLAY is set.
 */
#include "cocoa_test_util.h"

static PlatformWindow *g_w;
static bool g_routed; /* events go through the application's queue; else straight to the view */

static const char *selector_for(unsigned long type)
{
    switch (type)
    {
    case NS_KEY_DOWN:
        return "keyDown:";
    case NS_KEY_UP:
        return "keyUp:";
    case NS_FLAGS_CHANGED:
        return "flagsChanged:";
    case NS_LEFT_DOWN:
        return "mouseDown:";
    case NS_LEFT_UP:
        return "mouseUp:";
    case NS_RIGHT_DOWN:
        return "rightMouseDown:";
    case NS_RIGHT_UP:
        return "rightMouseUp:";
    case NS_OTHER_DOWN:
        return "otherMouseDown:";
    case NS_OTHER_UP:
        return "otherMouseUp:";
    case NS_MOVED:
        return "mouseMoved:";
    case NS_LEFT_DRAGGED:
        return "mouseDragged:";
    case NS_RIGHT_DRAGGED:
        return "rightMouseDragged:";
    case NS_OTHER_DRAGGED:
        return "otherMouseDragged:";
    case NS_SCROLL:
        return "scrollWheel:";
    default:
        return "noSuchEvent:";
    }
}

static void deliver(id event)
{
    if (!event)
        return;
    if (g_routed)
        post(event);
    else
        to_view(g_w, selector_for(msg_ulong(event, "type")), event);
}

static void send_key(bool down, unsigned short code, unsigned long flags, const char *chars, bool repeat)
{
    deliver(event_key(g_w, down ? NS_KEY_DOWN : NS_KEY_UP, code, flags, chars, repeat));
    pump_frame(g_w);
}

static void send_flags(unsigned short code, unsigned long flags)
{
    deliver(event_key(g_w, NS_FLAGS_CHANGED, code, flags, "", false));
    pump_frame(g_w);
}

static const Event *only_key(void)
{
    return seen_count(EVENT_KEY) == 1 ? seen_nth(EVENT_KEY, 0) : NULL;
}

/* ========================================================================== */
/*  keys                                                                      */
/* ========================================================================== */

/* Every key of platform.h with the virtual key code the keyboard sends for it, written out by number (Apple's list, which
   the backend reads by name, so a slip in either shows). The modifiers are in test_modifiers. */
static const struct
{
    int key;
    unsigned short code;
    const char *name;
} k_table[] = {
    {KEY_A, 0x00, "A"}, {KEY_B, 0x0B, "B"}, {KEY_C, 0x08, "C"}, {KEY_D, 0x02, "D"}, {KEY_E, 0x0E, "E"}, {KEY_F, 0x03, "F"},
    {KEY_G, 0x05, "G"}, {KEY_H, 0x04, "H"}, {KEY_I, 0x22, "I"}, {KEY_J, 0x26, "J"}, {KEY_K, 0x28, "K"}, {KEY_L, 0x25, "L"},
    {KEY_M, 0x2E, "M"}, {KEY_N, 0x2D, "N"}, {KEY_O, 0x1F, "O"}, {KEY_P, 0x23, "P"}, {KEY_Q, 0x0C, "Q"}, {KEY_R, 0x0F, "R"},
    {KEY_S, 0x01, "S"}, {KEY_T, 0x11, "T"}, {KEY_U, 0x20, "U"}, {KEY_V, 0x09, "V"}, {KEY_W, 0x0D, "W"}, {KEY_X, 0x07, "X"},
    {KEY_Y, 0x10, "Y"}, {KEY_Z, 0x06, "Z"},
    {KEY_ZERO, 0x1D, "0"}, {KEY_ONE, 0x12, "1"}, {KEY_TWO, 0x13, "2"}, {KEY_THREE, 0x14, "3"}, {KEY_FOUR, 0x15, "4"},
    {KEY_FIVE, 0x17, "5"}, {KEY_SIX, 0x16, "6"}, {KEY_SEVEN, 0x1A, "7"}, {KEY_EIGHT, 0x1C, "8"}, {KEY_NINE, 0x19, "9"},
    {KEY_APOSTROPHE, 0x27, "'"}, {KEY_COMMA, 0x2B, ","}, {KEY_MINUS, 0x1B, "-"}, {KEY_PERIOD, 0x2F, "."}, {KEY_SLASH, 0x2C, "/"},
    {KEY_SEMICOLON, 0x29, ";"}, {KEY_EQUAL, 0x18, "="}, {KEY_LEFT_BRACKET, 0x21, "["}, {KEY_BACKSLASH, 0x2A, "\\"},
    {KEY_RIGHT_BRACKET, 0x1E, "]"}, {KEY_GRAVE, 0x32, "`"},
    {KEY_SPACE, 0x31, "space"}, {KEY_ESCAPE, 0x35, "escape"}, {KEY_ENTER, 0x24, "return"}, {KEY_TAB, 0x30, "tab"},
    {KEY_BACKSPACE, 0x33, "delete (backspace)"}, {KEY_INSERT, 0x72, "help (insert)"}, {KEY_DELETE, 0x75, "forward delete"},
    {KEY_RIGHT, 0x7C, "right"}, {KEY_LEFT, 0x7B, "left"}, {KEY_DOWN, 0x7D, "down"}, {KEY_UP, 0x7E, "up"},
    {KEY_PAGE_UP, 0x74, "page up"}, {KEY_PAGE_DOWN, 0x79, "page down"}, {KEY_HOME, 0x73, "home"}, {KEY_END, 0x77, "end"},
    {KEY_NUM_LOCK, 0x47, "keypad clear (num lock)"}, {KEY_PRINT_SCREEN, 0x69, "F13 (print screen)"},
    {KEY_SCROLL_LOCK, 0x6B, "F14 (scroll lock)"}, {KEY_PAUSE, 0x71, "F15 (pause)"},
    {KEY_F1, 0x7A, "F1"}, {KEY_F2, 0x78, "F2"}, {KEY_F3, 0x63, "F3"}, {KEY_F4, 0x76, "F4"}, {KEY_F5, 0x60, "F5"},
    {KEY_F6, 0x61, "F6"}, {KEY_F7, 0x62, "F7"}, {KEY_F8, 0x64, "F8"}, {KEY_F9, 0x65, "F9"}, {KEY_F10, 0x6D, "F10"},
    {KEY_F11, 0x67, "F11"}, {KEY_F12, 0x6F, "F12"},
    {KEY_KP_0, 0x52, "keypad 0"}, {KEY_KP_1, 0x53, "keypad 1"}, {KEY_KP_2, 0x54, "keypad 2"}, {KEY_KP_3, 0x55, "keypad 3"},
    {KEY_KP_4, 0x56, "keypad 4"}, {KEY_KP_5, 0x57, "keypad 5"}, {KEY_KP_6, 0x58, "keypad 6"}, {KEY_KP_7, 0x59, "keypad 7"},
    {KEY_KP_8, 0x5B, "keypad 8"}, {KEY_KP_9, 0x5C, "keypad 9"}, {KEY_KP_DECIMAL, 0x41, "keypad ."},
    {KEY_KP_DIVIDE, 0x4B, "keypad /"}, {KEY_KP_MULTIPLY, 0x43, "keypad *"}, {KEY_KP_SUBTRACT, 0x4E, "keypad -"},
    {KEY_KP_ADD, 0x45, "keypad +"}, {KEY_KP_ENTER, 0x4C, "keypad enter"}, {KEY_KP_EQUAL, 0x51, "keypad ="},
    {KEY_MENU, 0x6E, "contextual menu"},
};
#define TABLE_COUNT ((int)(sizeof k_table / sizeof k_table[0]))

static void test_key_table(void)
{
    int bad = 0;
    for (int i = 0; i < TABLE_COUNT; i++)
    {
        send_key(true, k_table[i].code, 0, NULL, false);
        const Event *down = only_key();
        bool ok = down && down->data.key.key == k_table[i].key && down->data.key.scancode == k_table[i].code && down->data.key.down &&
                  !down->data.key.repeat && down->data.key.mods == 0;
        ok = ok && key_down(g_w, k_table[i].key) && key_pressed(g_w, k_table[i].key) && seen_count(EVENT_CHAR) == 0;
        seen_reset();
        send_key(false, k_table[i].code, 0, NULL, false);
        const Event *up = only_key();
        ok = ok && up && up->data.key.key == k_table[i].key && up->data.key.scancode == k_table[i].code && !up->data.key.down &&
             !key_down(g_w, k_table[i].key) && key_released(g_w, k_table[i].key);
        seen_reset();
        if (!ok)
        {
            bad++;
            printf("  key %s (code 0x%02X): down/up did not come as KEY %d\n", k_table[i].name, k_table[i].code, k_table[i].key);
        }
        CHECK(ok);
    }
    printf("  %d keys, each pressed and released: %d wrong\n", TABLE_COUNT, bad);

    /* a code with no key of the library's (the ISO extra key, fn, F17, JIS yen, a code past the table) still goes out */
    static const unsigned short unmapped[] = {0x0A, 0x3F, 0x40, 0x5D, 0x90};
    for (size_t i = 0; i < sizeof unmapped / sizeof unmapped[0]; i++)
    {
        send_key(true, unmapped[i], 0, NULL, false);
        const Event *e = only_key();
        CHECK(e && e->data.key.key == KEY_NULL && e->data.key.scancode == unmapped[i] && e->data.key.down);
        seen_reset();
        send_key(false, unmapped[i], 0, NULL, false);
        e = only_key();
        CHECK(e && e->data.key.key == KEY_NULL && !e->data.key.down);
        seen_reset();
    }
}

static void test_repeat(void)
{
    send_key(true, 0x00, 0, "a", false);
    const Event *e = only_key();
    CHECK(e && !e->data.key.repeat);
    CHECK(seen_count(EVENT_CHAR) == 1);
    seen_reset();
    for (int i = 0; i < 3; i++)
    {
        send_key(true, 0x00, 0, "a", true);
        e = only_key();
        CHECK(e && e->data.key.down && e->data.key.repeat);
        CHECK(key_down(g_w, KEY_A));
        CHECK(seen_count(EVENT_CHAR) == 1); /* a repeat types again */
        seen_reset();
    }
    /* the press queue has the key once, whatever the repeats */
    CHECK(key_get_pressed(g_w) == KEY_A);
    CHECK(key_get_pressed(g_w) == 0);
    send_key(false, 0x00, 0, "a", false);
    CHECK(!key_down(g_w, KEY_A));
    seen_reset();
}

static struct
{
    unsigned short code;
    int key;
    unsigned long flag;
    int mod;
    const char *name;
} k_mods[] = {
    {0x38, KEY_LEFT_SHIFT, NS_FLAG_SHIFT, KEYMOD_SHIFT, "left shift"},   {0x3C, KEY_RIGHT_SHIFT, NS_FLAG_SHIFT, KEYMOD_SHIFT, "right shift"},
    {0x3B, KEY_LEFT_CONTROL, NS_FLAG_CTRL, KEYMOD_CTRL, "left control"}, {0x3E, KEY_RIGHT_CONTROL, NS_FLAG_CTRL, KEYMOD_CTRL, "right control"},
    {0x3A, KEY_LEFT_ALT, NS_FLAG_OPT, KEYMOD_ALT, "left option"},        {0x3D, KEY_RIGHT_ALT, NS_FLAG_OPT, KEYMOD_ALT, "right option"},
    {0x37, KEY_LEFT_SUPER, NS_FLAG_CMD, KEYMOD_SUPER, "left command"},   {0x36, KEY_RIGHT_SUPER, NS_FLAG_CMD, KEYMOD_SUPER, "right command"},
};

static void test_modifiers(void)
{
    /* each modifier key alone: down, with its mod in key_mods and the right side's key down; and up */
    for (size_t i = 0; i < sizeof k_mods / sizeof k_mods[0]; i++)
    {
        send_flags(k_mods[i].code, k_mods[i].flag);
        const Event *e = only_key();
        bool ok = e && e->data.key.key == k_mods[i].key && e->data.key.scancode == k_mods[i].code && e->data.key.down &&
                  !e->data.key.repeat && e->data.key.mods == k_mods[i].mod;
        ok = ok && key_down(g_w, k_mods[i].key) && (key_mods(g_w) & ~KEYMOD_CAPS_LOCK) == k_mods[i].mod;
        for (size_t j = 0; j < sizeof k_mods / sizeof k_mods[0]; j++)
            ok = ok && (j == i || !key_down(g_w, k_mods[j].key)); /* only this side */
        seen_reset();
        send_flags(k_mods[i].code, 0);
        e = only_key();
        ok = ok && e && e->data.key.key == k_mods[i].key && !e->data.key.down && e->data.key.mods == 0 && !key_down(g_w, k_mods[i].key) &&
             (key_mods(g_w) & ~KEYMOD_CAPS_LOCK) == 0;
        seen_reset();
        if (!ok)
            printf("  %s: down or up did not come right\n", k_mods[i].name);
        CHECK(ok);
    }

    /* both shifts: the flag stays while one is down, the sides are told apart */
    send_flags(0x38, NS_FLAG_SHIFT);
    send_flags(0x3C, NS_FLAG_SHIFT);
    CHECK(key_down(g_w, KEY_LEFT_SHIFT) && key_down(g_w, KEY_RIGHT_SHIFT));
    seen_reset();
    send_flags(0x38, NS_FLAG_SHIFT); /* left lets go, right is still down */
    CHECK(!key_down(g_w, KEY_LEFT_SHIFT) && key_down(g_w, KEY_RIGHT_SHIFT));
    CHECK((key_mods(g_w) & KEYMOD_SHIFT) != 0);
    CHECK(seen_count(EVENT_KEY) == 1 && !seen_nth(EVENT_KEY, 0)->data.key.down && seen_nth(EVENT_KEY, 0)->data.key.key == KEY_LEFT_SHIFT);
    seen_reset();
    send_flags(0x3C, 0);
    CHECK(!key_down(g_w, KEY_RIGHT_SHIFT) && (key_mods(g_w) & KEYMOD_SHIFT) == 0);
    seen_reset();

    /* a release that was never seen heals at the next change: the shift is gone from the flags, so it is let go */
    send_flags(0x38, NS_FLAG_SHIFT);
    CHECK(key_down(g_w, KEY_LEFT_SHIFT));
    seen_reset();
    send_flags(0x3A, NS_FLAG_OPT); /* option goes down, and the flags no longer have shift */
    CHECK(!key_down(g_w, KEY_LEFT_SHIFT) && key_down(g_w, KEY_LEFT_ALT));
    CHECK(seen_count(EVENT_KEY) == 2);
    seen_reset();
    send_flags(0x3A, 0);
    seen_reset();

    /* the mods of a key's own events: each, and together */
    static const struct
    {
        unsigned long flags;
        int mods;
    } combos[] = {{NS_FLAG_SHIFT, KEYMOD_SHIFT}, {NS_FLAG_CTRL, KEYMOD_CTRL}, {NS_FLAG_OPT, KEYMOD_ALT}, {NS_FLAG_CMD, KEYMOD_SUPER},
                  {NS_FLAG_SHIFT | NS_FLAG_CMD, KEYMOD_SHIFT | KEYMOD_SUPER},
                  {NS_FLAG_SHIFT | NS_FLAG_CTRL | NS_FLAG_OPT | NS_FLAG_CMD, KEYMOD_SHIFT | KEYMOD_CTRL | KEYMOD_ALT | KEYMOD_SUPER},
                  {NS_FLAG_CAPS, 0}}; /* the lock is not a modifier of the key */
    for (size_t i = 0; i < sizeof combos / sizeof combos[0]; i++)
    {
        send_key(true, 0x06, combos[i].flags, NULL, false); /* Z */
        const Event *e = only_key();
        CHECK(e && e->data.key.mods == combos[i].mods);
        seen_reset();
        send_key(false, 0x06, combos[i].flags, NULL, false);
        seen_reset();
    }

    /* Caps Lock: one event for each way the lock turns, told as a tap; and the lock's state is the system's own */
    send_flags(0x39, NS_FLAG_CAPS);
    CHECK(seen_count(EVENT_KEY) == 2);
    const Event *c0 = seen_nth(EVENT_KEY, 0), *c1 = seen_nth(EVENT_KEY, 1);
    CHECK(c0 && c1 && c0->data.key.key == KEY_CAPS_LOCK && c0->data.key.down && c1->data.key.key == KEY_CAPS_LOCK && !c1->data.key.down);
    CHECK(!key_down(g_w, KEY_CAPS_LOCK));
    seen_reset();
    send_flags(0x39, 0);
    CHECK(seen_count(EVENT_KEY) == 2);
    seen_reset();
    unsigned long system_flags = msg_ulong(ns_class("NSEvent"), "modifierFlags");
    CHECK(((key_mods(g_w) & KEYMOD_CAPS_LOCK) != 0) == ((system_flags & NS_FLAG_CAPS) != 0));
    CHECK((key_mods(g_w) & KEYMOD_NUM_LOCK) == 0); /* a Mac has no Num Lock */
    printf("  Caps Lock is %s in the system, key_mods says %s\n", (system_flags & NS_FLAG_CAPS) ? "on" : "off",
           (key_mods(g_w) & KEYMOD_CAPS_LOCK) ? "on" : "off");
}

static int chars_seen(uint32_t *out, int cap)
{
    int n = 0;
    for (int i = 0; i < g_seen_count && n < cap; i++)
        if (g_seen[i].type == EVENT_CHAR)
            out[n++] = g_seen[i].data.codepoint;
    return n;
}

static void test_text(void)
{
    uint32_t cp[8];
    /* plain typed characters */
    send_key(true, 0x00, 0, "a", false);
    CHECK(chars_seen(cp, 8) == 1 && cp[0] == 'a');
    seen_reset();
    send_key(false, 0x00, 0, "a", false);
    seen_reset();
    send_key(true, 0x00, NS_FLAG_SHIFT, "A", false);
    CHECK(chars_seen(cp, 8) == 1 && cp[0] == 'A');
    seen_reset();
    send_key(false, 0x00, NS_FLAG_SHIFT, "A", false);
    seen_reset();
    /* what the layout makes of a key: any code point, two bytes, three, four (outside the BMP, a surrogate pair in AppKit's string) */
    static const struct
    {
        const char *utf8;
        uint32_t cp;
    } typed[] = {{"\xC3\xA9", 0xE9}, {"\xE2\x82\xAC", 0x20AC}, {"\xF0\x9F\x98\x80", 0x1F600}, {" ", 0x20}, {"\xC3\xA5", 0xE5}};
    for (size_t i = 0; i < sizeof typed / sizeof typed[0]; i++)
    {
        send_key(true, 0x0E, NS_FLAG_OPT, typed[i].utf8, false);
        CHECK(chars_seen(cp, 8) == 1 && cp[0] == typed[i].cp);
        seen_reset();
        send_key(false, 0x0E, NS_FLAG_OPT, typed[i].utf8, false);
        seen_reset();
    }
    /* a key that makes several */
    send_key(true, 0x00, 0, "ab", false);
    CHECK(chars_seen(cp, 8) == 2 && cp[0] == 'a' && cp[1] == 'b');
    seen_reset();
    send_key(false, 0x00, 0, "ab", false);
    seen_reset();

    /* what is not text: control characters, delete, the private-use block of the arrow and function keys, the C1 range */
    static const char *not_text[] = {"\r", "\t", "\x1b", "\x7f", "\x03", "\xEF\x9C\x80" /* U+F700 */, "\xEF\xA3\xBF" /* U+F8FF */, "\xC2\x85", ""};
    for (size_t i = 0; i < sizeof not_text / sizeof not_text[0]; i++)
    {
        send_key(true, 0x7E, 0, not_text[i], false);
        CHECK(seen_count(EVENT_CHAR) == 0);
        seen_reset();
        send_key(false, 0x7E, 0, not_text[i], false);
        seen_reset();
    }

    /* with Command held a key is a shortcut, not text; with Control the character is a control one, which is not text either */
    send_key(true, 0x08, NS_FLAG_CMD, "c", false);
    CHECK(seen_count(EVENT_CHAR) == 0);
    seen_reset();
    send_key(false, 0x08, NS_FLAG_CMD, "c", false);
    seen_reset();
    send_key(true, 0x08, NS_FLAG_CTRL, "\x03", false);
    CHECK(seen_count(EVENT_CHAR) == 0);
    seen_reset();
    send_key(false, 0x08, NS_FLAG_CTRL, "\x03", false);
    seen_reset();

    /* the program can turn text off for a game that reads keys, and on again for a text field */
    window_text_input_stop(g_w);
    send_key(true, 0x00, 0, "a", false);
    CHECK(seen_count(EVENT_CHAR) == 0 && seen_count(EVENT_KEY) == 1);
    seen_reset();
    send_key(false, 0x00, 0, "a", false);
    seen_reset();
    window_text_input_start(g_w);
    send_key(true, 0x00, 0, "a", false);
    CHECK(seen_count(EVENT_CHAR) == 1);
    seen_reset();
    send_key(false, 0x00, 0, "a", false);
    seen_reset();
    CHECK(char_get_pressed(g_w) != 0);
}

/* ========================================================================== */
/*  key equivalents and the application's own keys                            */
/* ========================================================================== */

/* Command+C, V, S, Z and the rest are not in the menu: the program gets them as keys, both ways (the release of a key pressed with
   Command held is not delivered by NSApplication, and the pump hands it to the window). Command+Q is the menu's. */
static void test_shortcuts(void)
{
    if (!g_routed)
        return;
    static const struct
    {
        unsigned short code;
        int key;
        const char *chars;
    } shortcuts[] = {{0x08, KEY_C, "c"}, {0x09, KEY_V, "v"}, {0x01, KEY_S, "s"}, {0x06, KEY_Z, "z"}, {0x00, KEY_A, "a"},
                     {0x07, KEY_X, "x"}, {0x0D, KEY_W, "w"}, {0x04, KEY_H, "h"}, {0x2E, KEY_M, "m"}};
    int bad = 0;
    for (size_t i = 0; i < sizeof shortcuts / sizeof shortcuts[0]; i++)
    {
        send_flags(0x37, NS_FLAG_CMD);
        seen_reset();
        send_key(true, shortcuts[i].code, NS_FLAG_CMD, shortcuts[i].chars, false);
        const Event *down = only_key();
        bool ok = down && down->data.key.key == shortcuts[i].key && down->data.key.down && (down->data.key.mods & KEYMOD_SUPER) &&
                  seen_count(EVENT_CHAR) == 0 && key_down(g_w, shortcuts[i].key);
        seen_reset();
        send_key(false, shortcuts[i].code, NS_FLAG_CMD, shortcuts[i].chars, false);
        const Event *up = only_key();
        ok = ok && up && up->data.key.key == shortcuts[i].key && !up->data.key.down && !key_down(g_w, shortcuts[i].key);
        seen_reset();
        send_flags(0x37, 0);
        seen_reset();
        if (!ok)
        {
            bad++;
            printf("  Command+%s did not come as a key (down %s, up %s)\n", shortcuts[i].chars, down ? "seen" : "missing", up ? "seen" : "missing");
        }
        CHECK(ok);
    }
    printf("  %zu Command shortcuts as keys, both ways: %d wrong\n", sizeof shortcuts / sizeof shortcuts[0], bad);

    /* Command+Q: the menu has it. The program is told its windows were asked to close, and never sees the key. */
    seen_reset();
    send_key(true, 0x0C, NS_FLAG_CMD, "q", false);
    send_key(false, 0x0C, NS_FLAG_CMD, "q", false);
    pump_frames(g_w, 3);
    printf("  Command+Q: %d close requests, %d key events\n", seen_count(EVENT_WINDOW_CLOSE), seen_count(EVENT_KEY));
    CHECK(seen_count(EVENT_WINDOW_CLOSE) == 1);
    CHECK(seen_count(EVENT_KEY) == 0 && !key_down(g_w, KEY_Q));
    CHECK(window_should_close(g_w));
    window_set_should_close(g_w, false);
    seen_reset();
}

/* ========================================================================== */
/*  focus                                                                     */
/* ========================================================================== */

static id window_delegate(void)
{
    return msg_id(ns_window(g_w), "delegate");
}

/* Keys and buttons down when the focus goes are let go, told in the stream before the loss, and the polled state is clear. */
static void test_focus_loss(void)
{
    send_key(true, 0x0D, 0, NULL, false); /* W */
    send_flags(0x38, NS_FLAG_SHIFT);
    deliver(event_mouse(g_w, NS_LEFT_DOWN, 20, 20, 0, 1));
    pump_frame(g_w);
    CHECK(key_down(g_w, KEY_W) && key_down(g_w, KEY_LEFT_SHIFT) && mouse_button_down(g_w, MOUSE_LEFT));
    seen_reset();

    /* the delegate's own notification, as AppKit sends it when another window becomes the key one */
    msg_void_id(window_delegate(), "windowDidResignKey:", NULL);
    pump_frame(g_w);
    int key_ups = 0, order_ok = 0;
    int focus_at = -1, last_up_at = -1;
    for (int i = 0; i < g_seen_count; i++)
    {
        if (g_seen[i].type == EVENT_KEY && !g_seen[i].data.key.down)
        {
            key_ups++;
            last_up_at = i;
        }
        if (g_seen[i].type == EVENT_WINDOW_FOCUS && !g_seen[i].data.focus.gained)
            focus_at = i;
    }
    order_ok = focus_at > last_up_at && last_up_at >= 0;
    printf("  focus lost: %d key releases told, all before the loss: %s; W %s, shift %s, left button %s\n", key_ups, order_ok ? "yes" : "NO",
           key_down(g_w, KEY_W) ? "down" : "up", key_down(g_w, KEY_LEFT_SHIFT) ? "down" : "up", mouse_button_down(g_w, MOUSE_LEFT) ? "down" : "up");
    CHECK(key_ups == 2 && order_ok);
    CHECK(!key_down(g_w, KEY_W) && !key_down(g_w, KEY_LEFT_SHIFT) && !mouse_button_down(g_w, MOUSE_LEFT));
    CHECK(seen_count(EVENT_MOUSE_BUTTON) == 1 && !seen_nth(EVENT_MOUSE_BUTTON, 0)->data.mouse.down);
    seen_reset();
    /* the release that comes after is not a second one */
    send_key(false, 0x0D, 0, NULL, false);
    send_flags(0x38, 0);
    deliver(event_mouse(g_w, NS_LEFT_UP, 20, 20, 0, 1));
    pump_frame(g_w);
    CHECK(seen_count(EVENT_MOUSE_BUTTON) == 0);
    seen_reset();
    msg_void_id(window_delegate(), "windowDidBecomeKey:", NULL);
    pump_frame(g_w);
    CHECK(seen_count(EVENT_WINDOW_FOCUS) == 1 && seen_nth(EVENT_WINDOW_FOCUS, 0)->data.focus.gained);
    seen_reset();

    /* the same with a real change of the key window, where the session lets it happen */
    window_focus(g_w);
    pump_for(g_w, 150);
    WindowConfig cfg = {.title = "other", .width = 100, .height = 80, .x = 400, .y = 60, .render = RENDER_VULKAN};
    PlatformWindow *other = window_create(&cfg);
    if (!other)
        return;
    window_focus(other);
    bool took = false;
    for (int i = 0; i < 100 && !took; i++)
    {
        pump_frame(other);
        pump_frame(g_w);
        took = window_is_focused(other);
        time_sleep(10);
    }
    if (took)
    {
        seen_reset();
        pump_for(g_w, 100);
        CHECK(!window_is_focused(g_w));
        window_focus(g_w);
        for (int i = 0; i < 100 && !window_is_focused(g_w); i++)
        {
            pump_frame(other);
            pump_frame(g_w);
            time_sleep(10);
        }
        CHECK(window_is_focused(g_w));
    }
    else
        NOTE("another window could not be made the key window here, the real focus change was not tried");
    window_destroy(other);
    window_focus(g_w);
    pump_for(g_w, 150);
    seen_reset();
}

/* ========================================================================== */
/*  mouse                                                                     */
/* ========================================================================== */

static void test_moves(void)
{
    int cw, ch;
    window_get_size(g_w, &cw, &ch);
    /* the four corners, the middle, and out of the window on every side: positions in points from the top left, as
       window_get_size has them */
    const int pts[][2] = {{0, 0},      {cw - 1, 0},    {0, ch - 1}, {cw - 1, ch - 1}, {cw / 2, ch / 2},
                          {-5, -7},    {cw + 12, 3},   {4, ch + 20}, {-30, ch / 2}};
    for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++)
    {
        seen_reset();
        deliver(event_mouse(g_w, NS_MOVED, pts[i][0], pts[i][1], 0, 0));
        pump_frame(g_w);
        const Event *e = seen_nth(EVENT_MOUSE_MOVE, 0);
        bool ok = e && seen_count(EVENT_MOUSE_MOVE) == 1 && e->data.mouse.x == pts[i][0] && e->data.mouse.y == pts[i][1] &&
                  e->data.mouse.dx == 0 && e->data.mouse.dy == 0 && mouse_x(g_w) == pts[i][0] && mouse_y(g_w) == pts[i][1];
        if (!ok)
            printf("  move to %d,%d: got %d,%d\n", pts[i][0], pts[i][1], e ? e->data.mouse.x : -999, e ? e->data.mouse.y : -999);
        CHECK(ok);
    }
    /* the delta since the previous frame is the core's */
    deliver(event_mouse(g_w, NS_MOVED, 10, 10, 0, 0));
    pump_frame(g_w);
    deliver(event_mouse(g_w, NS_MOVED, 25, 22, 0, 0));
    pump_frame(g_w);
    int dx, dy;
    mouse_delta(g_w, &dx, &dy);
    CHECK(dx == 15 && dy == 12);
    seen_reset();
}

static const Event *button_event(int n)
{
    return seen_nth(EVENT_MOUSE_BUTTON, n);
}

static void test_buttons(void)
{
    static const struct
    {
        int button;
        unsigned long down, up;
    } basic[] = {{MOUSE_LEFT, NS_LEFT_DOWN, NS_LEFT_UP}, {MOUSE_RIGHT, NS_RIGHT_DOWN, NS_RIGHT_UP}};
    for (size_t i = 0; i < 2; i++)
    {
        seen_reset();
        deliver(event_mouse(g_w, basic[i].down, 33, 44, 0, 1));
        pump_frame(g_w);
        const Event *e = button_event(0);
        CHECK(e && seen_count(EVENT_MOUSE_BUTTON) == 1 && e->data.mouse.button == basic[i].button && e->data.mouse.down &&
              e->data.mouse.x == 33 && e->data.mouse.y == 44);
        CHECK(mouse_button_down(g_w, basic[i].button) && mouse_button_pressed(g_w, basic[i].button));
        seen_reset();
        deliver(event_mouse(g_w, basic[i].up, 33, 44, 0, 1));
        pump_frame(g_w);
        e = button_event(0);
        CHECK(e && e->data.mouse.button == basic[i].button && !e->data.mouse.down && e->data.mouse.x == 33 && e->data.mouse.y == 44);
        CHECK(!mouse_button_down(g_w, basic[i].button) && mouse_button_released(g_w, basic[i].button));
    }

    /* the middle button and the two side ones are the "other" buttons of AppKit, numbered 2, 3 and 4; a made-up event of
       CoreGraphics gives them their number (a button 5 has no name here and is dropped) */
    static const struct
    {
        int number, button;
    } others[] = {{2, MOUSE_MIDDLE}, {3, MOUSE_X1}, {4, MOUSE_X2}, {5, -1}};
    for (size_t i = 0; i < 4; i++)
    {
        id d = event_other_button(true, others[i].number, 0, 0);
        id u = event_other_button(false, others[i].number, 0, 0);
        if (!d || !u)
        {
            NOTE("an event of button %d could not be made from CoreGraphics here, skipped", others[i].number);
            continue;
        }
        seen_reset();
        to_view(g_w, "otherMouseDown:", d); /* straight to the view: the made-up event has no window */
        pump_frame(g_w);
        if (others[i].button < 0)
        {
            CHECK(seen_count(EVENT_MOUSE_BUTTON) == 0);
            continue;
        }
        const Event *e = button_event(0);
        CHECK(e && e->data.mouse.button == others[i].button && e->data.mouse.down);
        CHECK(mouse_button_down(g_w, others[i].button));
        seen_reset();
        to_view(g_w, "otherMouseUp:", u);
        pump_frame(g_w);
        e = button_event(0);
        CHECK(e && e->data.mouse.button == others[i].button && !e->data.mouse.down);
        CHECK(!mouse_button_down(g_w, others[i].button));
    }
    seen_reset();
}

/* A drag is moves with a button down, and goes on outside the window: positions past its edges are told as they are. */
static void test_drags(void)
{
    deliver(event_mouse(g_w, NS_LEFT_DOWN, 50, 50, 0, 1));
    pump_frame(g_w);
    seen_reset();
    static const int path[][2] = {{60, 55}, {120, 70}, {-20, 40}, {10, 400}, {250, -50}};
    for (size_t i = 0; i < sizeof path / sizeof path[0]; i++)
    {
        deliver(event_mouse(g_w, NS_LEFT_DRAGGED, path[i][0], path[i][1], 0, 1));
        pump_frame(g_w);
    }
    CHECK(seen_count(EVENT_MOUSE_MOVE) == 5);
    for (int i = 0; i < 5; i++)
    {
        const Event *e = seen_nth(EVENT_MOUSE_MOVE, i);
        CHECK(e && e->data.mouse.x == path[i][0] && e->data.mouse.y == path[i][1]);
    }
    CHECK(mouse_button_down(g_w, MOUSE_LEFT));
    deliver(event_mouse(g_w, NS_LEFT_UP, 300, -60, 0, 1)); /* let go outside */
    pump_frame(g_w);
    CHECK(!mouse_button_down(g_w, MOUSE_LEFT) && seen_count(EVENT_MOUSE_BUTTON) == 1);
    CHECK(button_event(0)->data.mouse.x == 300 && button_event(0)->data.mouse.y == -60);
    seen_reset();

    /* the right button's drag and the other buttons' */
    deliver(event_mouse(g_w, NS_RIGHT_DOWN, 5, 5, 0, 1));
    deliver(event_mouse(g_w, NS_RIGHT_DRAGGED, 77, 66, 0, 1));
    deliver(event_mouse(g_w, NS_RIGHT_UP, 77, 66, 0, 1));
    pump_frame(g_w);
    CHECK(seen_count(EVENT_MOUSE_MOVE) == 1 && seen_nth(EVENT_MOUSE_MOVE, 0)->data.mouse.x == 77);
    seen_reset();
    id od = event_moved_by(1, 1, true);
    if (od)
    {
        /* made from a left drag, delivered as the other drag: the handler only looks at the position */
        to_view(g_w, "otherMouseDragged:", od);
        pump_frame(g_w);
        CHECK(seen_count(EVENT_MOUSE_MOVE) == 1);
        seen_reset();
    }
}

static void test_scroll(void)
{
    struct
    {
        bool precise;
        int dx, dy;
        const char *what;
    } cases[] = {{false, 0, 3, "3 lines up"}, {false, 0, -2, "2 lines down"}, {false, 2, 0, "2 lines to the right (AppKit's x is positive to the left)"},
                 {false, -4, 0, "4 lines to the left"}, {true, 0, 40, "40 points up"}, {true, 0, -25, "25 points down"},
                 {true, 30, 0, "30 points one way"}, {true, 0, 0, "nothing"}, {false, 1, -1, "both"}};
    id probe = event_scroll(false, 0, 1);
    if (!probe)
    {
        NOTE("a scroll event could not be made from CoreGraphics here, scroll was not tried");
        return;
    }
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        id e = event_scroll(cases[i].precise, cases[i].dx, cases[i].dy);
        double sdx = msg_double(e, "scrollingDeltaX"), sdy = msg_double(e, "scrollingDeltaY");
        bool precise_seen = msg_bool(e, "hasPreciseScrollingDeltas");
        seen_reset();
        to_view(g_w, "scrollWheel:", e);
        pump_frame(g_w);
        float wx = 0, wy = 0;
        mouse_wheel_v(g_w, &wx, &wy);
        printf("  scroll %s: AppKit says %s deltas %.2f,%.2f; the library reports %.2f,%.2f\n", cases[i].what,
               precise_seen ? "precise" : "line", sdx, sdy, (double)wx, (double)wy);
        if (cases[i].dx == 0 && cases[i].dy == 0)
        {
            CHECK(seen_count(EVENT_MOUSE_WHEEL) == 0);
            continue;
        }
        /* what AppKit gives is mapped, not made up: ours is the same direction in y, the opposite one in x, and a notch is
           one while a trackpad's points are a tenth */
        CHECK(seen_count(EVENT_MOUSE_WHEEL) == 1);
        const double want_y = precise_seen ? sdy / 10.0 : sdy, want_x = precise_seen ? -sdx / 10.0 : -sdx;
        /* a 'precise' event that AppKit calls a line one would show as a mismatch here, not hide it */
        CHECK(fabs((double)wy - want_y) < 0.001 && fabs((double)wx - want_x) < 0.001);
        /* the sign of what AppKit makes of a CoreGraphics event is AppKit's (printed above): up is positive in y in
           both, and x is the opposite in ours */
    }
    /* the events of one frame add up in the polled wheel */
    seen_reset();
    to_view(g_w, "scrollWheel:", event_scroll(false, 0, 1));
    to_view(g_w, "scrollWheel:", event_scroll(false, 0, 1));
    pump_frame(g_w);
    float wx, wy;
    mouse_wheel_v(g_w, &wx, &wy);
    CHECK(seen_count(EVENT_MOUSE_WHEEL) == 2 && wy > 0.0f && fabs((double)mouse_wheel(g_w) - (double)wy) < 0.0001);
    seen_reset();
}

static void test_first_mouse(void)
{
    /* the click that activates a window is a click for the program too */
    id e = event_mouse(g_w, NS_LEFT_DOWN, 5, 5, 0, 1);
    CHECK(MSG(unsigned char, id, SEL, id)(ns_view(g_w), sel_registerName("acceptsFirstMouse:"), e) != 0);
}

static void test_set_position(void)
{
    int wx, wy;
    window_get_position(g_w, &wx, &wy);
    static const int spots[][2] = {{30, 40}, {1, 1}, {100, 60}};
    for (size_t i = 0; i < sizeof spots / sizeof spots[0]; i++)
    {
        seen_reset();
        mouse_set_position(g_w, spots[i][0], spots[i][1]);
        pump_frame(g_w);
        int gx = -1, gy = -1;
        for (int k = 0; k < 50; k++)
        {
            mouse_global_position(&gx, &gy);
            if (abs(gx - (wx + spots[i][0])) <= 1 && abs(gy - (wy + spots[i][1])) <= 1)
                break;
            pump_for(g_w, 10);
        }
        printf("  mouse_set_position %d,%d: the pointer is at %d,%d (window at %d,%d)\n", spots[i][0], spots[i][1], gx, gy, wx, wy);
        CHECK(abs(gx - (wx + spots[i][0])) <= 1 && abs(gy - (wy + spots[i][1])) <= 1);
        /* the program is told, as it is by the other systems, with a move to the place */
        const Event *e = seen_nth(EVENT_MOUSE_MOVE, 0);
        CHECK(e && e->data.mouse.x == spots[i][0] && e->data.mouse.y == spots[i][1]);
        CHECK(mouse_x(g_w) == spots[i][0] && mouse_y(g_w) == spots[i][1]);
    }
    seen_reset();
}

/* The explicit capture: the call says whether it is in effect, can be repeated and undone, and goes away with the window. */
static void test_capture(void)
{
    CHECK(mouse_capture(g_w, true));
    CHECK(mouse_capture(g_w, true)); /* again: still in effect */
    CHECK(!mouse_capture(g_w, false));
    CHECK(!mouse_capture(g_w, false)); /* off when it is off is fine */
    for (int i = 0; i < 200; i++)
        CHECK(mouse_capture(g_w, i % 2 == 0) == (i % 2 == 0));
    mouse_capture(g_w, false);
    /* a window destroyed while it holds the capture takes it with it */
    WindowConfig cfg = {.title = "capture", .width = 80, .height = 60, .x = 300, .y = 300, .render = RENDER_VULKAN};
    for (int i = 0; i < 20; i++)
    {
        PlatformWindow *c = window_create(&cfg);
        CHECK(c != NULL);
        if (!c)
            break;
        CHECK(mouse_capture(c, true));
        window_destroy(c);
    }
    /* what the monitor turns the pointer's events outside the application into cannot be made here: nothing outside this
       process can be given a mouse event without the permission to post to the system */
    NOTE("mouse_capture: the call, its repeats and the cleanup are tested; a pointer that moves outside the application with no button down cannot be made without permission");
}

static void test_modes(void)
{
    /* hidden: the pointer is free, moves and clicks come as ever */
    mouse_set_mode(g_w, MOUSE_MODE_HIDDEN);
    seen_reset();
    deliver(event_mouse(g_w, NS_MOVED, 40, 50, 0, 0));
    pump_frame(g_w);
    CHECK(seen_count(EVENT_MOUSE_MOVE) == 1 && seen_nth(EVENT_MOUSE_MOVE, 0)->data.mouse.x == 40);
    mouse_set_mode(g_w, MOUSE_MODE_NORMAL);

    /* captured: only while the window is the key one is the pointer let loose from the mouse, so that is made so */
    window_focus(g_w);
    bool key = false;
    for (int i = 0; i < 100 && !key; i++)
    {
        pump_frame(g_w);
        key = window_is_focused(g_w);
        time_sleep(10);
    }
    if (!key)
    {
        NOTE("the test window is not the key window here: the captured mode's motion was not tried");
        mouse_set_mode(g_w, MOUSE_MODE_CAPTURED);
        mouse_set_mode(g_w, MOUSE_MODE_NORMAL);
        return;
    }
    mouse_set_mode(g_w, MOUSE_MODE_NORMAL);
    mouse_set_position(g_w, 40, 30);
    pump_frame(g_w);
    seen_reset();
    mouse_set_mode(g_w, MOUSE_MODE_CAPTURED);
    pump_frame(g_w);
    seen_reset();
    id m1 = event_moved_by(5, -3, false), m2 = event_moved_by(7, 2, false), m3 = event_moved_by(-2, 4, true);
    if (!m1 || !m2 || !m3)
    {
        NOTE("a motion event could not be made from CoreGraphics here, the captured mode's motion was not tried");
        mouse_set_mode(g_w, MOUSE_MODE_NORMAL);
        return;
    }
    to_view(g_w, "mouseMoved:", m1);
    to_view(g_w, "mouseMoved:", m2);
    to_view(g_w, "mouseDragged:", m3);
    pump_frame(g_w);
    int n = seen_count(EVENT_MOUSE_MOVE);
    printf("  captured: %d moves; first %d,%d (%d,%d), last %d,%d (%d,%d)\n", n, n ? seen_nth(EVENT_MOUSE_MOVE, 0)->data.mouse.x : 0,
           n ? seen_nth(EVENT_MOUSE_MOVE, 0)->data.mouse.y : 0, n ? seen_nth(EVENT_MOUSE_MOVE, 0)->data.mouse.dx : 0,
           n ? seen_nth(EVENT_MOUSE_MOVE, 0)->data.mouse.dy : 0, n ? seen_nth(EVENT_MOUSE_MOVE, n - 1)->data.mouse.x : 0,
           n ? seen_nth(EVENT_MOUSE_MOVE, n - 1)->data.mouse.y : 0, n ? seen_nth(EVENT_MOUSE_MOVE, n - 1)->data.mouse.dx : 0,
           n ? seen_nth(EVENT_MOUSE_MOVE, n - 1)->data.mouse.dy : 0);
    CHECK(n == 3);
    if (n == 3)
    {
        /* the motion, and the position the program follows, which is the sum of it from where the pointer was */
        const Event *a = seen_nth(EVENT_MOUSE_MOVE, 0), *c = seen_nth(EVENT_MOUSE_MOVE, 2);
        CHECK(a->data.mouse.dx == 5 && a->data.mouse.dy == -3 && a->data.mouse.x == 45 && a->data.mouse.y == 27);
        CHECK(c->data.mouse.dx == -2 && c->data.mouse.dy == 4 && c->data.mouse.x == 50 && c->data.mouse.y == 33);
    }
    int dx, dy;
    mouse_delta(g_w, &dx, &dy);
    CHECK(mouse_x(g_w) == 50 && mouse_y(g_w) == 33);

    /* a button in this mode is where the program's position is, not where the held pointer is */
    seen_reset();
    deliver(event_mouse(g_w, NS_LEFT_DOWN, 5, 5, 0, 1));
    pump_frame(g_w);
    CHECK(seen_count(EVENT_MOUSE_BUTTON) == 1 && button_event(0)->data.mouse.x == 50 && button_event(0)->data.mouse.y == 33);
    deliver(event_mouse(g_w, NS_LEFT_UP, 5, 5, 0, 1));
    pump_frame(g_w);
    seen_reset();

    /* set position moves the followed position only */
    mouse_set_position(g_w, 100, 90);
    to_view(g_w, "mouseMoved:", event_moved_by(1, 1, false));
    pump_frame(g_w);
    CHECK(seen_count(EVENT_MOUSE_MOVE) == 1 && seen_nth(EVENT_MOUSE_MOVE, 0)->data.mouse.x == 101 && seen_nth(EVENT_MOUSE_MOVE, 0)->data.mouse.y == 91);
    seen_reset();

    /* the focus going gives the pointer back: motion is no longer taken as the program's, and comes back with the focus */
    msg_void_id(window_delegate(), "windowDidResignKey:", NULL);
    pump_frame(g_w);
    seen_reset();
    to_view(g_w, "mouseMoved:", event_moved_by(9, 9, false));
    pump_frame(g_w);
    CHECK(seen_count(EVENT_MOUSE_MOVE) == 0);
    msg_void_id(window_delegate(), "windowDidBecomeKey:", NULL);
    pump_frame(g_w);
    seen_reset();
    to_view(g_w, "mouseMoved:", event_moved_by(2, 3, false));
    pump_frame(g_w);
    CHECK(seen_count(EVENT_MOUSE_MOVE) == 1 && seen_nth(EVENT_MOUSE_MOVE, 0)->data.mouse.dx == 2);
    seen_reset();

    /* switching it on and off, over and over, and destroying a window that is in the mode */
    for (int i = 0; i < 50; i++)
        mouse_set_mode(g_w, i % 2 ? MOUSE_MODE_NORMAL : MOUSE_MODE_CAPTURED);
    mouse_set_mode(g_w, MOUSE_MODE_NORMAL);
    WindowConfig cfg = {.title = "mode", .width = 80, .height = 60, .x = 300, .y = 300, .render = RENDER_VULKAN};
    PlatformWindow *c = window_create(&cfg);
    CHECK(c != NULL);
    if (c)
    {
        window_focus(c);
        pump_for(c, 150);
        mouse_set_mode(c, MOUSE_MODE_CAPTURED);
        window_destroy(c); /* gives the pointer back */
    }
    window_focus(g_w);
    pump_for(g_w, 150);
    /* the pointer is associated with the mouse again: a warp takes effect at once */
    int wx, wy, gx, gy;
    window_get_position(g_w, &wx, &wy);
    mouse_set_position(g_w, 20, 20);
    mouse_global_position(&gx, &gy);
    CHECK(abs(gx - (wx + 20)) <= 1 && abs(gy - (wy + 20)) <= 1);
    seen_reset();
}

/* ========================================================================== */
/*  cursors                                                                   */
/* ========================================================================== */

static void test_cursors(void)
{
    /* the pointer inside the window, so that the cursor set is the one in effect */
    int wx, wy;
    window_get_position(g_w, &wx, &wy);
    mouse_set_position(g_w, 60, 40);
    pump_for(g_w, 50);
    id ns_cursor = ns_class("NSCursor");
    static const char *names[] = {"default", "arrow", "ibeam", "crosshair", "hand", "resize ew", "resize ns", "not allowed",
                                  "resize nwse", "resize nesw", "resize all"};
    static const char *expect_class[] = {"arrowCursor", "arrowCursor", "IBeamCursor", "crosshairCursor", "pointingHandCursor",
                                         "resizeLeftRightCursor", "resizeUpDownCursor", "operationNotAllowedCursor", NULL, NULL, "openHandCursor"};
    platform_clear_error();
    int matched = 0;
    for (int c = 0; c < CURSOR_COUNT; c++)
    {
        mouse_set_cursor(g_w, c);
        pump_for(g_w, 20);
        id current = msg_id(ns_cursor, "currentCursor");
        id wanted = expect_class[c] ? msg_id(ns_cursor, expect_class[c]) : NULL;
        bool is = current && wanted && current == wanted;
        matched += is;
        printf("  cursor %-12s: %s\n", names[c], !wanted ? "(a public cursor of the system, or the nearest)" : is ? "AppKit's is the one expected" : "AppKit's current cursor is another");
    }
    CHECK(platform_get_error()[0] == '\0'); /* every cursor of the enum is made, none is refused */
    if (matched < 8)
        NOTE("AppKit reported the cursor in effect for %d of the 8 shapes with a public cursor (the pointer's own cursor may not be readable in this session)", matched);
    /* a value outside the enum is the default, as on the other backends */
    mouse_set_cursor(g_w, -1);
    mouse_set_cursor(g_w, CURSOR_COUNT + 5);
    mouse_set_cursor(g_w, CURSOR_DEFAULT);
}

static uint32_t *make_argb(int w, int h)
{
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; px && y < h; y++)
        for (int x = 0; x < w; x++)
            px[y * w + x] = ((uint32_t)(255 * x / (w > 1 ? w - 1 : 1)) << 24) | 0x00FF0000u | (uint32_t)(255 * y / (h > 1 ? h - 1 : 1)) << 8;
    return px;
}

static void test_cursor_images(void)
{
    uint32_t *img = make_argb(16, 16);
    PlatformCursor *c = cursor_create(img, 16, 16, 3, 4);
    CHECK(c != NULL);
    if (c)
    {
        mouse_set_cursor_image(g_w, c);
        pump_for(g_w, 30);
        mouse_set_cursor_image(g_w, NULL); /* back to the shape */
        pump_for(g_w, 30);
        mouse_set_cursor_image(g_w, c);
        mouse_set_cursor(g_w, CURSOR_HAND); /* a shape replaces the image */
        pump_for(g_w, 30);
        /* a cursor that is destroyed while a window shows it: the window goes back to its shape and nothing dangles */
        mouse_set_cursor_image(g_w, c);
        cursor_destroy(c);
        pump_for(g_w, 30);
        mouse_set_cursor(g_w, CURSOR_DEFAULT);
    }
    free(img);

    /* the sizes at the ends, and what the library refuses before the backend is asked */
    struct
    {
        int w, h, hx, hy;
        bool valid;
    } cases[] = {{1, 1, 0, 0, true},     {256, 256, 255, 255, true}, {17, 31, 16, 30, true}, {64, 1, 63, 0, true},
                 {0, 5, 0, 0, false},    {5, 0, 0, 0, false},        {257, 8, 0, 0, false},  {8, 8, 8, 0, false},
                 {8, 8, 0, -1, false}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        int w = cases[i].w > 0 ? cases[i].w : 1, h = cases[i].h > 0 ? cases[i].h : 1;
        uint32_t *px = make_argb(w > 256 ? 256 : w, h);
        platform_clear_error();
        PlatformCursor *k = cursor_create(px, cases[i].w, cases[i].h, cases[i].hx, cases[i].hy);
        CHECK((k != NULL) == cases[i].valid);
        if (!cases[i].valid)
            CHECK(strstr(platform_get_error(), "invalid cursor image") != NULL);
        if (k)
        {
            mouse_set_cursor_image(g_w, k);
            cursor_destroy(k);
        }
        free(px);
    }
    platform_clear_error();
    CHECK(cursor_create(NULL, 8, 8, 0, 0) == NULL);
    cursor_destroy(NULL);
    mouse_set_cursor(g_w, CURSOR_DEFAULT);

    /* created and destroyed 200 times, set on the window now and then: nothing is left */
    uint32_t *big = make_argb(32, 32);
    unsigned b0 = 0, b1 = 0;
    size_t h0 = 0, h1 = 0;
    int made = 0;
    for (int i = 0; i < 230; i++)
    {
        if (i == 30)
            h0 = heap_in_use(&b0);
        PlatformCursor *k = cursor_create(big, 32, 32, i % 32, (i * 7) % 32);
        if (!k)
            continue;
        made++;
        if (i % 5 == 0)
            mouse_set_cursor_image(g_w, k);
        if (i % 5 == 0)
            mouse_set_cursor_image(g_w, NULL);
        cursor_destroy(k);
    }
    h1 = heap_in_use(&b1);
    printf("  %d custom cursors made and destroyed: heap %zu -> %zu bytes (%+ld over 200), blocks %u -> %u\n", made, h0, h1, (long)h1 - (long)h0, b0, b1);
    CHECK(made == 230);
#if defined(ZP_ASAN)
    NOTE("AddressSanitizer is on: the heap check is skipped (its quarantine counts as in use)");
#else
    CHECK((long)h1 - (long)h0 < 512 * 1024);
    CHECK((long)b1 - (long)b0 < 4000);
#endif
    free(big);
}

/* ========================================================================== */

static bool make_key_window(void)
{
    window_focus(g_w);
    for (int i = 0; i < 100; i++)
    {
        pump_frame(g_w);
        if (window_is_focused(g_w))
            return true;
        time_sleep(10);
    }
    return false;
}

int main(void)
{
    alarm(110);
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    WindowConfig cfg = {.title = "cocoa input test", .width = 200, .height = 150, .x = 60, .y = 60, .render = RENDER_VULKAN, .resizable = true};
    g_w = window_create(&cfg);
    if (!g_w)
    {
        printf("%s: %s\n", ZP_REQUIRE ? "FAIL" : "skip", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    bool key = make_key_window();
    printf("the test window %s the key window\n", key ? "is" : "is NOT");
    pump_for(g_w, 200);
    seen_reset();

    /* keys and modifiers go through the application's queue when the window is the key one (that is the real path); and also
       straight to the view, which is what is left when it is not */
    for (int pass = 0; pass < 2; pass++)
    {
        g_routed = pass == 0;
        if (g_routed && !key)
        {
            NOTE("not the key window: the events were not posted through the application's queue");
            continue;
        }
        printf("%s\n", g_routed ? "== events posted to the application's queue" : "== events given to the view");
        printf("key table\n");
        test_key_table();
        printf("repeat\n");
        test_repeat();
        printf("modifiers\n");
        test_modifiers();
        printf("text\n");
        test_text();
        printf("shortcuts\n");
        test_shortcuts();
        printf("moves\n");
        test_moves();
        printf("buttons\n");
        test_buttons();
        printf("drags\n");
        test_drags();
    }
    g_routed = false;
    printf("== the rest\n");
    printf("focus loss\n");
    test_focus_loss();
    printf("scroll\n");
    test_scroll();
    printf("first mouse\n");
    test_first_mouse();
    printf("mouse_set_position\n");
    test_set_position();
    printf("mouse_capture\n");
    test_capture();
    printf("mouse modes\n");
    test_modes();
    printf("cursors\n");
    test_cursors();
    printf("cursor images\n");
    test_cursor_images();

    window_destroy(g_w);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
