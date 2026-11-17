/*
 * backend_cocoa.m - macOS backend, over AppKit. The one Objective-C file of the library; the rest stays C.
 *
 * Built by hand with manual retain/release (-fno-objc-arc, see CMakeLists.txt): a BackendWindow is a C struct
 * made with calloc like in every other backend, and what it holds (window, view, delegate, timer) is released in
 * backend_destroy in one place, in the order that stops AppKit calling back into a struct that is going away.
 *
 * Threading: AppKit belongs to the main thread, so does every call into this file (backend_init and backend_create
 * refuse another thread). The application is created in code, without a nib, as a regular application with a
 * menu bar (Quit, Cmd+Q) and is activated so its windows come to the front.
 *
 * Events: like the Win32 backend, one process-wide queue (NSApp's) feeds every window. backend_pump_events drains it
 * without blocking and sends each event on; the window delegate and the view are called from inside that, and push
 * normalized events into the core. What happens outside a pump (an API call that resizes the window answers
 * synchronously) waits in the window's own list and goes to the core at the start of the next pump.
 *
 * Coordinates: AppKit puts the origin at the bottom left of the primary screen and the y axis up; the library's
 * positions (windows and monitors) have the origin at the top left of the primary screen and y down, as on the
 * other backends, and a window's position is the top left corner of its content area. All sizes are in points;
 * the framebuffer size, in pixels, is the points times the backing scale factor.
 *
 * OpenGL: an NSOpenGLContext on the window's view (core profile 3.2 or 4.1, or the 2.1 legacy one), a second context made
 * with CGL for gl_worker_* (CGL has no view in it, so another thread can use it without touching AppKit), and the
 * software surface drawn from the view's drawRect. Keyboard and mouse: the view turns NSEvents into the library's events;
 * the key table is built from Apple's virtual key codes (kVK_*).
 *
 * Phase markers: what is not here yet is a stub that says so, marked TODO(mac phase N). 5 = text input (the input method,
 * dead keys), 6 = clipboard and file drop, 7 = fullscreen, icon, opacity, always on top, undecorated windows and hit test,
 * 8 = Vulkan (Metal) surface.
 */
/* Apple has deprecated OpenGL since macOS 10.14 and says so for every call; this file is the only one that uses it. */
#define GL_SILENCE_DEPRECATION 1
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h> /* CGSessionCopyCurrentDictionary, CGDisplay*, CGEventSourceKeyState */
/* Carbon's header is only for kVK_*, the virtual key codes (nothing of Carbon is linked). It has a typedef called EventType
   (a UInt32, in its event manager), and platform.h an enum of that name: Carbon's gets another name for the length of its
   header, which the corrida 44156573685 showed is needed. */
#define EventType zen_carbon_EventType
#import <Carbon/Carbon.h>
#undef EventType
#import <OpenGL/OpenGL.h>                          /* CGL: the context of gl_worker_* */

#include <dlfcn.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cocoa pulls in <mach/task.h>, which declares a thread_create of its own, and platform.h declares the library's
   (thread_create(ThreadFunc, void *, const char *)): in one file the two conflict. This file calls neither, so the
   library's goes by another name here, for the length of the library headers. */
#define thread_create zen_platform_thread_create
#include "core_internal.h"
#include "backend.h"
#include "error_internal.h"
#include "preedit.h"
#undef thread_create

#define PENDING_MAX 256
#define MAX_MONITORS 16
#define MONITOR_NAME_CAP 64
#define LIVE_INTERVAL 0.016 /* seconds between live callbacks while AppKit runs its own loop */
#define KEYMAP_SIZE 128     /* virtual key codes are below 0x7F */
#define WHEEL_PIXELS_PER_NOTCH 10.0 /* a touchpad's scroll in points, per wheel notch (what the Wayland backend takes too) */

@class ZenAppDelegate;

/* The window's content view: every event of the keyboard and the mouse arrives here, and the software surface is drawn
   by it. Declared here, above the window's struct, so the helpers below can message it. */
@interface ZenView : NSView <NSTextInputClient>
{
    BackendWindow *owner;
    NSTrackingArea *area;
}
- (id)initWithFrame:(NSRect)frame owner:(BackendWindow *)b;
- (void)detach;
@end

@interface ZenWindowDelegate : NSObject <NSWindowDelegate>
{
    BackendWindow *owner;
}
- (id)initWithOwner:(BackendWindow *)b;
- (void)detach;
- (void)liveTick:(NSTimer *)timer;
@end

struct BackendWindow
{
    Event pending[PENDING_MAX];
    int pending_count;
    bool pumping;
    Core *core;
    BackendWindow *next; /* every live window, for what the application as a whole asks (Cmd+Q) */

    NSWindow *window;
    ZenView *view;
    ZenWindowDelegate *delegate;
    RenderMode render;

    /* RENDER_GL. The window's own context draws to its own view, until window_make_current_on points it at another
       window's: gl_target is the window whose view this context draws to (this one, mostly) and gl_drawer the window
       whose context draws to this view, so window_swap(b) knows whose buffer to flush. */
    NSOpenGLPixelFormat *gl_format;
    NSOpenGLContext *gl_ctx;
    BackendWindow *gl_target;
    BackendWindow *gl_drawer;
    GLConfig gl_cfg;

    /* RENDER_PIXELS: the back buffer, in pixels (the window's framebuffer size), 0xAARRGGBB */
    uint32_t *pixels;
    int px_w, px_h;

    /* keyboard: which virtual key codes this window has told the program are down */
    bool held[KEYMAP_SIZE];

    /* a drop (EVENT_WINDOW_DROP): the paths of the last one, which the event points to until the next drop or the window's end */
    char **drop_paths;
    int drop_count;

    /* text input (NSTextInputClient): on by default; the text being composed, as UTF-16 units, and where the program's caret is */
    bool text_input;
    bool composing;
    int marked_len;
    int ti_x, ti_y, ti_w, ti_h;

    /* mouse */
    int mouse_mode;       /* MOUSE_MODE_* */
    bool mouse_locked;    /* the pointer is cut loose from the mouse (CGAssociateMouseAndMouseCursorPosition(false)) */
    int virt_x, virt_y;   /* the position the program is told in MOUSE_MODE_CAPTURED: the sum of the motion */
    int last_x, last_y;   /* the last position of the pointer in the window, points */
    double rel_x, rel_y;  /* motion in MOUSE_MODE_CAPTURED that is less than a point so far, carried to the next event */
    int cursor_shape;     /* CURSOR_*, the one mouse_set_cursor chose */
    PlatformCursor *cursor_image; /* the one mouse_set_cursor_image chose, NULL for the shape; not owned */
    NSCursor *cursor_now; /* what the view's cursor rect uses: retained */
    id capture_monitor;   /* mouse_capture(true): the global monitor that follows the pointer out of the application */

    bool hovered;
    bool zoomed;   /* isZoomed at the last size change, to tell a maximize from a plain resize */
    float scale;   /* backingScaleFactor at the last look, to tell a change from a repeat */
    int min_w, min_h, max_w, max_h;

    bool in_modal, in_live; /* inside AppKit's own loop (a drag, a menu); inside the live callback */
    bool modal_in_send;     /* that loop was started by an event this pump sent, so it is over when sendEvent returns */
    NSTimer *live_timer;
    PlatformWindow *live_w;
    FrameCallback live_cb;
    void *live_user;
};

static struct
{
    bool ready;
    bool launched; /* finishLaunching runs once per process, not once per platform_init */
    bool in_send;  /* inside [NSApp sendEvent:] of backend_pump_events */
    ZenAppDelegate *app_delegate;
    BackendWindow *windows;
    NSPoint cascade;
    bool cascaded;
    char monitor_names[MAX_MONITORS][MONITOR_NAME_CAP];
    NSCursor *blank_cursor;     /* a cursor with nothing to show, made once: what a hidden pointer is */
    NSCursor *shape_cursors[CURSOR_COUNT]; /* the system cursors, looked up once */
    void *gl_framework;         /* dlopen of OpenGL.framework, for gl_proc_address */
} g;

struct PlatformCursor
{
    uint32_t *argb; /* the image as it was given: 0xAARRGGBB, w * h pixels (not premultiplied) */
    int w, h, hot_x, hot_y;
    NSCursor *cursor; /* built for `scale` pixels per point */
    float scale;
};

/* ========================================================================== */
/*  helpers                                                                   */
/* ========================================================================== */

/* Raised while the window is being driven by an API call (setFrame answers synchronously) the events arrive outside the
   pump; they wait here and are delivered at the start of the next pump, so begin_frame never discards them. */
static void push(BackendWindow *b, const Event *e)
{
    if (b->pumping && b->core)
        core_push_event(b->core, e);
    else if (b->pending_count < PENDING_MAX)
        b->pending[b->pending_count++] = *e;
}

static bool on_main_thread(void)
{
    return pthread_main_np() != 0;
}

/* The primary screen is the first of NSScreen's list, the one with the menu bar; its height is what turns AppKit's
   y (up, from the bottom) into the library's (down, from the top). */
static CGFloat primary_height(void)
{
    NSArray *screens = [NSScreen screens];
    return [screens count] ? [[screens objectAtIndex:0] frame].size.height : 0;
}

static int round_int(CGFloat v)
{
    return (int)lround((double)v);
}

/* A rectangle of the screen space, from AppKit's to the library's convention. */
static void top_left(NSRect r, int *x, int *y)
{
    *x = round_int(r.origin.x);
    *y = round_int(primary_height() - r.origin.y - r.size.height);
}

/* The content area of the window, in points. */
static NSRect content_rect(BackendWindow *b)
{
    return [b->window contentRectForFrameRect:[b->window frame]];
}

static void fb_size(BackendWindow *b, int *w, int *h)
{
    NSRect px = [b->view convertRectToBacking:[b->view bounds]];
    *w = round_int(px.size.width);
    *h = round_int(px.size.height);
}

static void push_fb_size(BackendWindow *b)
{
    Event fb = {.type = EVENT_WINDOW_FB_RESIZE};
    fb_size(b, &fb.data.resize.w, &fb.data.resize.h);
    push(b, &fb);
}

static void push_size(BackendWindow *b)
{
    NSRect c = content_rect(b);
    Event r = {.type = EVENT_WINDOW_RESIZE};
    r.data.resize.w = round_int(c.size.width);
    r.data.resize.h = round_int(c.size.height);
    push(b, &r);
    push_fb_size(b);
}

static void push_move(BackendWindow *b)
{
    Event m = {.type = EVENT_WINDOW_MOVE};
    top_left(content_rect(b), &m.data.move.x, &m.data.move.y);
    push(b, &m);
}

/* ========================================================================== */
/*  keyboard                                                                  */
/* ========================================================================== */

/* The library's key for each of Apple's virtual key codes (kVK_*, from Carbon/HIToolbox/Events.h). A code names the place
   of a key on the keyboard, not the letter the layout in use prints on it, which is what KEY_* means too. A code with no
   entry (the extra keys of ISO and JIS keyboards, the function key, the media keys) gives KEY_NULL: the event still goes
   out, with the code as its scancode. Where a PC key has no Mac twin the key in its place is used: Help is where Insert
   is, Clear is where Num Lock is, F13 to F15 are where Print Screen, Scroll Lock and Pause are, Command is the Super key. */
static const unsigned short k_keys[KEYMAP_SIZE] = {
    [kVK_ANSI_A] = KEY_A, [kVK_ANSI_B] = KEY_B, [kVK_ANSI_C] = KEY_C, [kVK_ANSI_D] = KEY_D,
    [kVK_ANSI_E] = KEY_E, [kVK_ANSI_F] = KEY_F, [kVK_ANSI_G] = KEY_G, [kVK_ANSI_H] = KEY_H,
    [kVK_ANSI_I] = KEY_I, [kVK_ANSI_J] = KEY_J, [kVK_ANSI_K] = KEY_K, [kVK_ANSI_L] = KEY_L,
    [kVK_ANSI_M] = KEY_M, [kVK_ANSI_N] = KEY_N, [kVK_ANSI_O] = KEY_O, [kVK_ANSI_P] = KEY_P,
    [kVK_ANSI_Q] = KEY_Q, [kVK_ANSI_R] = KEY_R, [kVK_ANSI_S] = KEY_S, [kVK_ANSI_T] = KEY_T,
    [kVK_ANSI_U] = KEY_U, [kVK_ANSI_V] = KEY_V, [kVK_ANSI_W] = KEY_W, [kVK_ANSI_X] = KEY_X,
    [kVK_ANSI_Y] = KEY_Y, [kVK_ANSI_Z] = KEY_Z,

    [kVK_ANSI_0] = KEY_ZERO, [kVK_ANSI_1] = KEY_ONE, [kVK_ANSI_2] = KEY_TWO, [kVK_ANSI_3] = KEY_THREE,
    [kVK_ANSI_4] = KEY_FOUR, [kVK_ANSI_5] = KEY_FIVE, [kVK_ANSI_6] = KEY_SIX, [kVK_ANSI_7] = KEY_SEVEN,
    [kVK_ANSI_8] = KEY_EIGHT, [kVK_ANSI_9] = KEY_NINE,

    [kVK_ANSI_Quote] = KEY_APOSTROPHE, [kVK_ANSI_Comma] = KEY_COMMA, [kVK_ANSI_Minus] = KEY_MINUS,
    [kVK_ANSI_Period] = KEY_PERIOD, [kVK_ANSI_Slash] = KEY_SLASH, [kVK_ANSI_Semicolon] = KEY_SEMICOLON,
    [kVK_ANSI_Equal] = KEY_EQUAL, [kVK_ANSI_LeftBracket] = KEY_LEFT_BRACKET,
    [kVK_ANSI_Backslash] = KEY_BACKSLASH, [kVK_ANSI_RightBracket] = KEY_RIGHT_BRACKET,
    [kVK_ANSI_Grave] = KEY_GRAVE,

    [kVK_Space] = KEY_SPACE, [kVK_Return] = KEY_ENTER, [kVK_Tab] = KEY_TAB, [kVK_Escape] = KEY_ESCAPE,
    [kVK_Delete] = KEY_BACKSPACE, [kVK_ForwardDelete] = KEY_DELETE, [kVK_Help] = KEY_INSERT,
    [kVK_ContextualMenu] = KEY_MENU,

    [kVK_LeftArrow] = KEY_LEFT, [kVK_RightArrow] = KEY_RIGHT, [kVK_UpArrow] = KEY_UP, [kVK_DownArrow] = KEY_DOWN,
    [kVK_PageUp] = KEY_PAGE_UP, [kVK_PageDown] = KEY_PAGE_DOWN, [kVK_Home] = KEY_HOME, [kVK_End] = KEY_END,

    [kVK_F1] = KEY_F1, [kVK_F2] = KEY_F2, [kVK_F3] = KEY_F3, [kVK_F4] = KEY_F4, [kVK_F5] = KEY_F5,
    [kVK_F6] = KEY_F6, [kVK_F7] = KEY_F7, [kVK_F8] = KEY_F8, [kVK_F9] = KEY_F9, [kVK_F10] = KEY_F10,
    [kVK_F11] = KEY_F11, [kVK_F12] = KEY_F12,
    [kVK_F13] = KEY_PRINT_SCREEN, [kVK_F14] = KEY_SCROLL_LOCK, [kVK_F15] = KEY_PAUSE,

    [kVK_ANSI_Keypad0] = KEY_KP_0, [kVK_ANSI_Keypad1] = KEY_KP_1, [kVK_ANSI_Keypad2] = KEY_KP_2,
    [kVK_ANSI_Keypad3] = KEY_KP_3, [kVK_ANSI_Keypad4] = KEY_KP_4, [kVK_ANSI_Keypad5] = KEY_KP_5,
    [kVK_ANSI_Keypad6] = KEY_KP_6, [kVK_ANSI_Keypad7] = KEY_KP_7, [kVK_ANSI_Keypad8] = KEY_KP_8,
    [kVK_ANSI_Keypad9] = KEY_KP_9, [kVK_ANSI_KeypadDecimal] = KEY_KP_DECIMAL,
    [kVK_ANSI_KeypadDivide] = KEY_KP_DIVIDE, [kVK_ANSI_KeypadMultiply] = KEY_KP_MULTIPLY,
    [kVK_ANSI_KeypadMinus] = KEY_KP_SUBTRACT, [kVK_ANSI_KeypadPlus] = KEY_KP_ADD,
    [kVK_ANSI_KeypadEnter] = KEY_KP_ENTER, [kVK_ANSI_KeypadEquals] = KEY_KP_EQUAL,
    [kVK_ANSI_KeypadClear] = KEY_NUM_LOCK,

    [kVK_Shift] = KEY_LEFT_SHIFT, [kVK_RightShift] = KEY_RIGHT_SHIFT,
    [kVK_Control] = KEY_LEFT_CONTROL, [kVK_RightControl] = KEY_RIGHT_CONTROL,
    [kVK_Option] = KEY_LEFT_ALT, [kVK_RightOption] = KEY_RIGHT_ALT,
    [kVK_Command] = KEY_LEFT_SUPER, [kVK_RightCommand] = KEY_RIGHT_SUPER,
    [kVK_CapsLock] = KEY_CAPS_LOCK,
};

/* The modifier keys in pairs, left then right, each with the flag that stays set while a key of its kind is down. */
static const struct
{
    unsigned short code;
    NSEventModifierFlags kind;
} k_modifiers[] = {
    {kVK_Shift, NSEventModifierFlagShift},     {kVK_RightShift, NSEventModifierFlagShift},
    {kVK_Control, NSEventModifierFlagControl}, {kVK_RightControl, NSEventModifierFlagControl},
    {kVK_Option, NSEventModifierFlagOption},   {kVK_RightOption, NSEventModifierFlagOption},
    {kVK_Command, NSEventModifierFlagCommand}, {kVK_RightCommand, NSEventModifierFlagCommand},
};
#define MODIFIER_COUNT ((int)(sizeof k_modifiers / sizeof k_modifiers[0]))

static int translate_key(unsigned short code)
{
    return code < KEYMAP_SIZE ? k_keys[code] : KEY_NULL;
}

/* The modifiers held, as KEYMOD_*. Command is the Super key. Caps Lock is a lock, not a held key: key_mods adds it. */
static int translate_mods(NSEventModifierFlags flags)
{
    int mods = 0;
    if (flags & NSEventModifierFlagShift)
        mods |= KEYMOD_SHIFT;
    if (flags & NSEventModifierFlagControl)
        mods |= KEYMOD_CTRL;
    if (flags & NSEventModifierFlagOption)
        mods |= KEYMOD_ALT;
    if (flags & NSEventModifierFlagCommand)
        mods |= KEYMOD_SUPER;
    return mods;
}

static void push_key(BackendWindow *b, unsigned short code, bool down, bool repeat, int mods)
{
    if (code < KEYMAP_SIZE)
        b->held[code] = down;
    Event e = {.type = EVENT_KEY};
    e.data.key.key = translate_key(code);
    e.data.key.scancode = code;
    e.data.key.down = down;
    e.data.key.repeat = repeat;
    e.data.key.mods = mods;
    push(b, &e);
}

/* Typed text reaches the core as characters, so its own switch for text input (window_text_input_stop) is honoured. */
static void push_char(BackendWindow *b, uint32_t cp)
{
    if (b->pumping && b->core)
    {
        core_push_char(b->core, cp);
        return;
    }
    if (b->core && b->core->in.text_input_off)
        return;
    Event e = {.type = EVENT_CHAR};
    e.data.codepoint = cp;
    push(b, &e);
}

/* The next code point of a UTF-8 string, and the string after it; a byte that does not belong is skipped. */
static const unsigned char *utf8_next(const unsigned char *p, uint32_t *cp)
{
    unsigned char c = *p;
    int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
    if (extra < 0)
    {
        *cp = 0xFFFD;
        return p + 1;
    }
    uint32_t v = extra == 0 ? c : c & (0x3Fu >> extra);
    int i = 1;
    for (; i <= extra && (p[i] & 0xC0) == 0x80; i++)
        v = (v << 6) | (p[i] & 0x3Fu);
    *cp = i <= extra ? 0xFFFD : v;
    return p + i;
}

/* Text the input context hands over, as plain characters: what the layout types (dead keys already composed by it), what an
   input method committed. The control characters, Delete, the C1 range and the private-use block where AppKit puts the arrow
   and function keys are not text. Commit ends the composition that was on, as the other backends do (an empty edit first). */
static void text_edit_end(BackendWindow *b);

static void text_commit(BackendWindow *b, NSString *text)
{
    if (!b->text_input)
        return;
    text_edit_end(b);
    const char *s = text ? [text UTF8String] : NULL;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p;)
    {
        uint32_t cp;
        p = utf8_next(p, &cp);
        if (cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp < 0xA0) && !(cp >= 0xF700 && cp <= 0xF8FF) && cp != 0xFFFD)
            push_char(b, cp);
    }
}

/* The composition as EVENT_TEXT_EDIT: UTF-8 cut to what the event holds (63 bytes) on a character boundary, the caret as a byte
   offset in it. The selected range is in UTF-16 units of the string, so the caret is the length of what comes before it. An empty
   text ends the composition. */
static void text_edit(BackendWindow *b, NSString *text, NSRange selected)
{
    if (!b->text_input)
        return;
    const char *u = text ? [text UTF8String] : NULL;
    uint32_t cps[PREEDIT_CAP], before[PREEDIT_CAP];
    int count = preedit_from_utf8(u ? u : "", u ? (int)strlen(u) : 0, cps, PREEDIT_CAP);
    int caret = count;
    if (selected.location != NSNotFound && text)
    {
        NSUInteger at = selected.location < [text length] ? selected.location : [text length];
        const char *head = [[text substringToIndex:at] UTF8String];
        caret = preedit_from_utf8(head ? head : "", head ? (int)strlen(head) : 0, before, PREEDIT_CAP);
    }
    Preedit pre;
    preedit_clear(&pre);
    preedit_draw(&pre, caret, 0, 0, cps, count);
    Event e = {.type = EVENT_TEXT_EDIT};
    preedit_to_utf8(&pre, e.data.edit.text, (int)sizeof e.data.edit.text, &e.data.edit.cursor);
    b->composing = count > 0;
    b->marked_len = text ? (int)[text length] : 0;
    push(b, &e);
}

static void text_edit_end(BackendWindow *b)
{
    if (!b->composing)
        return;
    b->composing = false;
    b->marked_len = 0;
    Event e = {.type = EVENT_TEXT_EDIT}; /* an empty text */
    push(b, &e);
}

/* A window that loses the focus gives up the composition: the input method is told to drop it and the program is told it ended. */
static void text_abandon(BackendWindow *b)
{
    if (b->composing)
        [[b->view inputContext] discardMarkedText];
    text_edit_end(b);
}

/* A modifier key changed (flagsChanged). The event names the key, and its flags say which kinds of modifier are held after
   the change, but not on which side, so the key's own state is flipped; and when a kind is no longer held at all, both of
   its keys are let go, so a missed event heals at the next one. Caps Lock sends one event for each way the lock turns, not
   one for the press and one for the release: it is told as a tap. */
static void modifiers_changed(BackendWindow *b, NSEvent *event)
{
    unsigned short code = [event keyCode];
    NSEventModifierFlags flags = [event modifierFlags];
    int mods = translate_mods(flags);

    if (code == kVK_CapsLock)
    {
        push_key(b, code, true, false, mods);
        push_key(b, code, false, false, mods);
        return;
    }
    for (int i = 0; i < MODIFIER_COUNT; i++)
    {
        unsigned short c = k_modifiers[i].code;
        bool kind_down = (flags & k_modifiers[i].kind) != 0;
        if (c == code)
        {
            bool down = kind_down && !b->held[c];
            if (down != b->held[c])
                push_key(b, c, down, false, mods);
        }
        else if (!kind_down && b->held[c])
            push_key(b, c, false, false, mods);
    }
}

/* A window that loses the focus lets go of what it held: the program is told, key by key, before it is told about the
   focus (the core drops the polled state on the focus event; this is for whoever follows the event stream). */
static void release_held_keys(BackendWindow *b)
{
    int mods = translate_mods([NSEvent modifierFlags]);
    for (int code = 0; code < KEYMAP_SIZE; code++)
        if (b->held[code])
            push_key(b, (unsigned short)code, false, false, mods);
}

/* Keys that are already down when the window gets the focus send no press of their own: the session's own state says
   which, as far as it will say (a session may not answer for key states, so the modifiers are also taken from the flags,
   as the left key of their kind), and a key held across a focus change goes on working. */
static void press_held_keys(BackendWindow *b)
{
    NSEventModifierFlags flags = [NSEvent modifierFlags];
    int mods = translate_mods(flags);
    for (int code = 0; code < KEYMAP_SIZE; code++)
    {
        if (!k_keys[code] || k_keys[code] == KEY_CAPS_LOCK || b->held[code])
            continue;
        if (CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, (CGKeyCode)code))
            push_key(b, (unsigned short)code, true, false, mods);
    }
    for (int i = 0; i < MODIFIER_COUNT; i += 2)
        if ((flags & k_modifiers[i].kind) && !b->held[k_modifiers[i].code] && !b->held[k_modifiers[i + 1].code])
            push_key(b, k_modifiers[i].code, true, false, mods);
}

/* ========================================================================== */
/*  mouse                                                                     */
/* ========================================================================== */

/* A point of the window's own coordinates (AppKit: points, origin at the bottom left) as the library has it: points from
   the top left corner of the content area, y down. The same space as backend_get_size, at any scale. */
static void view_point(BackendWindow *b, NSPoint p, int *x, int *y)
{
    NSPoint v = [b->view convertPoint:p fromView:nil];
    *x = (int)floor(v.x);
    *y = (int)floor([b->view bounds].size.height - v.y);
}

/* Where the pointer is now, in those coordinates, whether or not the window has been told of it. */
static void pointer_in_window(BackendWindow *b, int *x, int *y)
{
    NSPoint m = [NSEvent mouseLocation];
    NSRect c = content_rect(b);
    *x = (int)floor(m.x - c.origin.x + 0.5);
    *y = (int)floor(c.origin.y + c.size.height - m.y + 0.5);
}

static void push_mouse_move(BackendWindow *b, int x, int y, int dx, int dy)
{
    Event e = {.type = EVENT_MOUSE_MOVE};
    e.data.mouse.x = x;
    e.data.mouse.y = y;
    e.data.mouse.dx = dx;
    e.data.mouse.dy = dy;
    push(b, &e);
}

/* Buttons are told where they happened, which in MOUSE_MODE_CAPTURED is the position the program is following (the real
   pointer is held still then). */
static void push_mouse_button(BackendWindow *b, int button, bool down, int x, int y)
{
    Event e = {.type = EVENT_MOUSE_BUTTON};
    e.data.mouse.button = button;
    e.data.mouse.down = down;
    e.data.mouse.x = b->mouse_mode == MOUSE_MODE_CAPTURED ? b->virt_x : x;
    e.data.mouse.y = b->mouse_mode == MOUSE_MODE_CAPTURED ? b->virt_y : y;
    push(b, &e);
}

/* The library's button for AppKit's numbering of the "other" buttons: 2 is the middle one, 3 and 4 the side ones. */
static int other_button(NSInteger number)
{
    switch (number)
    {
    case 2:
        return MOUSE_MIDDLE;
    case 3:
        return MOUSE_X1;
    case 4:
        return MOUSE_X2;
    default:
        return -1;
    }
}

static void mouse_button_event(BackendWindow *b, NSEvent *event, int button, bool down)
{
    if (button < 0)
        return;
    int x, y;
    view_point(b, [event locationInWindow], &x, &y);
    b->last_x = x;
    b->last_y = y;
    push_mouse_button(b, button, down, x, y);
}

/* Moves and drags are the same to the program. With the pointer cut loose from the mouse the event carries the motion and
   not a position: the positions the program sees are the sum of it, and fractions of a point are carried over. */
static void mouse_motion_event(BackendWindow *b, NSEvent *event)
{
    if (b->mouse_locked)
    {
        b->rel_x += [event deltaX];
        b->rel_y += [event deltaY];
        int dx = (int)b->rel_x, dy = (int)b->rel_y;
        b->rel_x -= dx;
        b->rel_y -= dy;
        if (dx == 0 && dy == 0)
            return;
        b->virt_x += dx;
        b->virt_y += dy;
        push_mouse_move(b, b->virt_x, b->virt_y, dx, dy);
        return;
    }
    int x, y;
    view_point(b, [event locationInWindow], &x, &y);
    b->last_x = x;
    b->last_y = y;
    if (b->mouse_mode == MOUSE_MODE_CAPTURED)
        return; /* waiting for the window to be the key one again, when the pointer is let loose from the mouse */
    push_mouse_move(b, x, y, 0, 0);
}

/* The wheel as the other backends tell it: one notch of a wheel is 1.0, up and to the right are positive. AppKit gives a
   notch as about one "line" (more when the wheel is turned fast) and a trackpad as points, ten of which count as a notch
   here, as in the Wayland backend; its y grows upward as ours does and its x grows to the left, which ours does not. The
   system's setting of natural scrolling is already in what AppKit says, and is followed. */
static void scroll_event(BackendWindow *b, NSEvent *event)
{
    double dx = [event scrollingDeltaX], dy = [event scrollingDeltaY];
    if ([event hasPreciseScrollingDeltas])
    {
        dx /= WHEEL_PIXELS_PER_NOTCH;
        dy /= WHEEL_PIXELS_PER_NOTCH;
    }
    if (dx == 0.0 && dy == 0.0)
        return;
    Event e = {.type = EVENT_MOUSE_WHEEL};
    e.data.wheel.x = (float)-dx;
    e.data.wheel.y = (float)dy;
    push(b, &e);
}

/* The pointer is held still and the program follows the motion of the mouse: the pointer goes to the middle of the window
   first, so it is inside, away from the edges, and stays where the events go. */
static void mouse_lock(BackendWindow *b)
{
    if (b->mouse_locked)
        return;
    NSRect c = content_rect(b);
    int cx, cy;
    top_left(c, &cx, &cy);
    CGWarpMouseCursorPosition(CGPointMake(cx + c.size.width / 2, cy + c.size.height / 2));
    CGAssociateMouseAndMouseCursorPosition(false);
    b->rel_x = b->rel_y = 0;
    b->mouse_locked = true;
}

static void mouse_unlock(BackendWindow *b)
{
    if (!b->mouse_locked)
        return;
    CGAssociateMouseAndMouseCursorPosition(true);
    b->mouse_locked = false;
}

/* mouse_capture(true): AppKit keeps a drag going outside the window by itself, but not a pointer that moves with no button
   down. A global monitor is told of the mouse events that go to other applications (it needs no permission for the mouse,
   only for the keyboard) and these are turned into the window's own. Events that go to this application's other windows
   are not seen by it. */
static void capture_event(BackendWindow *b, NSEvent *event)
{
    if (b->mouse_locked)
        return;
    int x, y;
    pointer_in_window(b, &x, &y);
    switch ([event type])
    {
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
        b->last_x = x;
        b->last_y = y;
        if (b->mouse_mode != MOUSE_MODE_CAPTURED)
            push_mouse_move(b, x, y, 0, 0);
        break;
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
        push_mouse_button(b, MOUSE_LEFT, [event type] == NSEventTypeLeftMouseDown, x, y);
        break;
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp:
        push_mouse_button(b, MOUSE_RIGHT, [event type] == NSEventTypeRightMouseDown, x, y);
        break;
    case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseUp:
        push_mouse_button(b, other_button([event buttonNumber]), [event type] == NSEventTypeOtherMouseDown, x, y);
        break;
    default:
        break;
    }
}

static void capture_stop(BackendWindow *b)
{
    if (!b->capture_monitor)
        return;
    [NSEvent removeMonitor:b->capture_monitor];
    [b->capture_monitor release];
    b->capture_monitor = nil;
}

/* ========================================================================== */
/*  file drop                                                                 */
/* ========================================================================== */

static void drop_free(BackendWindow *b)
{
    for (int i = 0; i < b->drop_count; i++)
        free(b->drop_paths[i]);
    free(b->drop_paths);
    b->drop_paths = NULL;
    b->drop_count = 0;
}

/* The files on a dragging pasteboard, as file URLs (not other kinds of URL). */
static NSArray *pasteboard_files(NSPasteboard *pb)
{
    NSDictionary *only_files = [NSDictionary dictionaryWithObject:[NSNumber numberWithBool:YES]
                                                           forKey:NSPasteboardURLReadingFileURLsOnlyKey];
    return [pb readObjectsForClasses:[NSArray arrayWithObject:[NSURL class]] options:only_files];
}

/* A drop of files raises EVENT_WINDOW_DROP with their paths (UTF-8), which stay valid until the next drop. */
static bool drop_files(BackendWindow *b, NSPasteboard *pb)
{
    NSArray *urls = pasteboard_files(pb);
    NSUInteger n = [urls count];
    if (n == 0)
        return false;
    drop_free(b);
    b->drop_paths = calloc(n, sizeof *b->drop_paths);
    if (!b->drop_paths)
        return false;
    for (NSUInteger i = 0; i < n; i++)
    {
        const char *path = [[urls objectAtIndex:i] fileSystemRepresentation];
        char *copy = path ? strdup(path) : NULL;
        if (copy)
            b->drop_paths[b->drop_count++] = copy;
    }
    if (b->drop_count == 0)
        return false;
    Event e = {.type = EVENT_WINDOW_DROP};
    e.data.drop.count = b->drop_count;
    e.data.drop.paths = (const char **)b->drop_paths;
    push(b, &e);
    return true;
}

/* ========================================================================== */
/*  cursors                                                                   */
/* ========================================================================== */

/* The system cursor for a CURSOR_*. AppKit has public cursors for most; for the rest the nearest one is used: the two
   diagonal resize cursors are public from macOS 15 (before it, the crosshair stands in) and the four-way move cursor has
   no public counterpart, so the open hand stands in. */
static NSCursor *make_shape_cursor(int cursor)
{
    switch (cursor)
    {
    case CURSOR_IBEAM:
        return [NSCursor IBeamCursor];
    case CURSOR_CROSSHAIR:
        return [NSCursor crosshairCursor];
    case CURSOR_HAND:
        return [NSCursor pointingHandCursor];
    case CURSOR_RESIZE_EW:
        return [NSCursor resizeLeftRightCursor];
    case CURSOR_RESIZE_NS:
        return [NSCursor resizeUpDownCursor];
    case CURSOR_NOT_ALLOWED:
        return [NSCursor operationNotAllowedCursor];
    case CURSOR_RESIZE_NWSE:
    case CURSOR_RESIZE_NESW:
        if (@available(macOS 15.0, *))
        {
            NSCursorFrameResizePosition at =
                cursor == CURSOR_RESIZE_NWSE ? NSCursorFrameResizePositionTopLeft : NSCursorFrameResizePositionTopRight;
            return [NSCursor frameResizeCursorFromPosition:at inDirections:NSCursorFrameResizeDirectionsAll];
        }
        return [NSCursor crosshairCursor];
    case CURSOR_RESIZE_ALL:
        return [NSCursor openHandCursor];
    default:
        return [NSCursor arrowCursor];
    }
}

static NSCursor *shape_cursor(int cursor)
{
    if (cursor < 0 || cursor >= CURSOR_COUNT)
        cursor = CURSOR_DEFAULT;
    if (!g.shape_cursors[cursor])
        g.shape_cursors[cursor] = [make_shape_cursor(cursor) retain];
    return g.shape_cursors[cursor];
}

/* A pointer that is not there: a cursor whose picture is one transparent pixel. */
static NSCursor *blank_cursor(void)
{
    if (!g.blank_cursor)
    {
        NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
        g.blank_cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
        [image release];
    }
    return g.blank_cursor;
}

/* The cursor picture of a PlatformCursor for a display of `scale` pixels per point: the image is in pixels, and is shown
   pixel for pixel, as on the other platforms, so on a scale 2 display it is half its size in points. AppKit wants the
   alpha premultiplied, the library's pixels are not. */
static NSCursor *image_cursor(PlatformCursor *pc, float scale)
{
    if (pc->cursor && pc->scale == scale)
        return pc->cursor;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                    pixelsWide:pc->w
                                                                    pixelsHigh:pc->h
                                                                 bitsPerSample:8
                                                               samplesPerPixel:4
                                                                      hasAlpha:YES
                                                                      isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace
                                                                   bytesPerRow:pc->w * 4
                                                                  bitsPerPixel:32];
    if (!rep)
        return nil;
    unsigned char *out = [rep bitmapData];
    for (int i = 0; i < pc->w * pc->h; i++)
    {
        uint32_t p = pc->argb[i];
        uint32_t a = p >> 24;
        out[i * 4 + 0] = (unsigned char)(((p >> 16) & 0xFF) * a / 255);
        out[i * 4 + 1] = (unsigned char)(((p >> 8) & 0xFF) * a / 255);
        out[i * 4 + 2] = (unsigned char)((p & 0xFF) * a / 255);
        out[i * 4 + 3] = (unsigned char)a;
    }
    NSSize size = NSMakeSize(pc->w / scale, pc->h / scale);
    [rep setSize:size];
    NSImage *image = [[NSImage alloc] initWithSize:size];
    [image addRepresentation:rep];
    NSCursor *cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(pc->hot_x / scale, pc->hot_y / scale)];
    [image release];
    [rep release];
    [pc->cursor release];
    pc->cursor = cursor;
    pc->scale = scale;
    return cursor;
}

/* The cursor a window should show now: nothing while the pointer is hidden or captured, else the image, else the shape. */
static NSCursor *cursor_wanted(BackendWindow *b)
{
    if (b->mouse_mode != MOUSE_MODE_NORMAL)
        return blank_cursor();
    if (b->cursor_image)
    {
        NSCursor *c = image_cursor(b->cursor_image, (float)[b->window backingScaleFactor]);
        if (c)
            return c;
    }
    return shape_cursor(b->cursor_shape);
}

/* The view lays the cursor over its whole area as a cursor rect, which AppKit keeps applying as the pointer moves (a
   cursor set by hand would be put back to the arrow with the next move); changing it means asking for the rects again and,
   with the pointer inside, putting the new one on at once. */
static void cursor_apply(BackendWindow *b)
{
    NSCursor *c = cursor_wanted(b);
    [c retain];
    [b->cursor_now release];
    b->cursor_now = c;
    [b->window invalidateCursorRectsForView:b->view];
    if (NSPointInRect([NSEvent mouseLocation], content_rect(b)))
        [c set];
}

/* ========================================================================== */
/*  OpenGL                                                                    */
/* ========================================================================== */

#define ZGL_MAJOR_VERSION 0x821B
#define ZGL_MINOR_VERSION 0x821C
#define ZGL_FRAMEWORK "/System/Library/Frameworks/OpenGL.framework/OpenGL"

typedef void (*GlGetIntegerv)(unsigned name, int *out);

static void gl_destroy(BackendWindow *b)
{
    if (b->gl_target && b->gl_target != b)
        b->gl_target->gl_drawer = NULL;
    if (b->gl_drawer && b->gl_drawer != b)
    {
        [b->gl_drawer->gl_ctx clearDrawable]; /* another window's context was drawing to this view */
        b->gl_drawer->gl_target = NULL;
    }
    b->gl_target = b->gl_drawer = NULL;
    if (b->gl_ctx)
    {
        if ([NSOpenGLContext currentContext] == b->gl_ctx)
            [NSOpenGLContext clearCurrentContext];
        [b->gl_ctx clearDrawable];
        [b->gl_ctx release];
        b->gl_ctx = nil;
    }
    [b->gl_format release];
    b->gl_format = nil;
}

/* The context a GLConfig asks for: made, current, and checked to be what was asked, or NO with the error set (never a
   context of another version or profile than the request, as on the other backends). What macOS has: the core profile as
   3.2 or 4.1 (a context made for either is the highest the hardware has, which is 4.1 on every GPU of the last decade),
   and the compatibility profile as 2.1 only. No OpenGL ES (it needs EGL, which macOS does not have) and no debug context. */
static bool gl_create(BackendWindow *b, const GLConfig *gl)
{
    GLProfile profile = gl->profile == GL_PROFILE_DEFAULT ? GL_PROFILE_CORE : gl->profile;
    int major = gl->major, minor = gl->minor;
    if (major == 0)
    {
        major = 3;
        minor = profile == GL_PROFILE_ES ? 0 : 3;
    }
    const char *name = profile == GL_PROFILE_COMPAT ? "compatibility" : profile == GL_PROFILE_ES ? "ES" : "core";

    if (profile == GL_PROFILE_ES)
        return error_set("cannot create an OpenGL ES %d.%d context: OpenGL ES is not available on macOS", major, minor);
    if (gl->debug)
        return error_set("cannot create an OpenGL %s %d.%d context with debug: macOS has no debug contexts", name, major, minor);

    NSOpenGLPixelFormatAttribute profile_attr;
    if (profile == GL_PROFILE_COMPAT)
    {
        if (major > 2 || (major == 2 && minor > 1))
            return error_set("cannot create an OpenGL compatibility %d.%d context: macOS has 2.1 at most in the compatibility "
                             "profile",
                             major, minor);
        profile_attr = NSOpenGLProfileVersionLegacy;
    }
    else
    {
        if (major > 4 || (major == 4 && minor > 1))
            return error_set("cannot create an OpenGL core %d.%d context: macOS has 4.1 at most", major, minor);
        profile_attr = (major > 3 || (major == 3 && minor > 2)) ? NSOpenGLProfileVersion4_1Core : NSOpenGLProfileVersion3_2Core;
    }

    /* RGBA8, depth 24, stencil 8 and double buffering are what the library always gives; sizes in a pixel format are
       minimums, so a request is never met with less. AllowOfflineRenderers lets the context follow the window to a display
       on another GPU. */
    NSOpenGLPixelFormatAttribute attrs[24];
    int n = 0;
    attrs[n++] = NSOpenGLPFAOpenGLProfile;
    attrs[n++] = profile_attr;
    attrs[n++] = NSOpenGLPFADoubleBuffer;
    attrs[n++] = NSOpenGLPFAColorSize;
    attrs[n++] = 24;
    attrs[n++] = NSOpenGLPFAAlphaSize;
    attrs[n++] = 8;
    attrs[n++] = NSOpenGLPFADepthSize;
    attrs[n++] = 24;
    attrs[n++] = NSOpenGLPFAStencilSize;
    attrs[n++] = 8;
    attrs[n++] = NSOpenGLPFAAllowOfflineRenderers;
    if (gl->msaa > 0)
    {
        attrs[n++] = NSOpenGLPFAMultisample;
        attrs[n++] = NSOpenGLPFASampleBuffers;
        attrs[n++] = 1;
        attrs[n++] = NSOpenGLPFASamples;
        attrs[n++] = (NSOpenGLPixelFormatAttribute)gl->msaa;
    }
    attrs[n] = 0;

    b->gl_format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    int samples = 0;
    if (b->gl_format && gl->msaa > 0) /* the closest format may have fewer samples than asked */
        [b->gl_format getValues:&samples forAttribute:NSOpenGLPFASamples forVirtualScreen:0];
    if (!b->gl_format || samples < gl->msaa)
    {
        gl_destroy(b);
        return error_set("no pixel format with RGBA8, depth 24, stencil 8 and %d samples for an OpenGL %s %d.%d context",
                         gl->msaa, name, major, minor);
    }
    b->gl_ctx = [[NSOpenGLContext alloc] initWithFormat:b->gl_format shareContext:nil];
    if (!b->gl_ctx)
    {
        gl_destroy(b);
        return error_set("cannot create an OpenGL %s %d.%d context", name, major, minor);
    }
    [b->gl_ctx makeCurrentContext];

    if (profile != GL_PROFILE_COMPAT) /* a core context says its version; the legacy one is 2.1 by definition */
    {
        GlGetIntegerv get_integerv;
        *(void **)&get_integerv = backend_gl_proc_address("glGetIntegerv");
        int got_major = 0, got_minor = 0;
        if (get_integerv)
        {
            get_integerv(ZGL_MAJOR_VERSION, &got_major);
            get_integerv(ZGL_MINOR_VERSION, &got_minor);
        }
        if (got_major < major || (got_major == major && got_minor < minor))
        {
            gl_destroy(b);
            return error_set("cannot create an OpenGL core %d.%d context: the system's is %d.%d", major, minor, got_major,
                             got_minor);
        }
    }
    b->gl_cfg = *gl;
    return true;
}

/* Apple deprecated the view of an NSOpenGLContext (and the full resolution of the view's surface) in macOS 10.14, telling
   whoever has one to use NSOpenGLView, which is no use to a window whose content view is the library's own; the calls
   work as they did, and GL_SILENCE_DEPRECATION does not reach them. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* The context goes onto the window's view, at the resolution of the display's pixels (without that the surface would be
   point sized and scaled up on a Retina display, and would not match backend_get_fb_size). */
static void gl_attach(BackendWindow *b)
{
    [b->view setWantsBestResolutionOpenGLSurface:YES];
    [b->gl_ctx setView:b->view];
    [b->gl_ctx makeCurrentContext];
    b->gl_target = b;
    b->gl_drawer = b;
}

#pragma clang diagnostic pop

/* The surface moved or changed size, or the display it is on changed scale: the context has to be told, on the main thread
   (a context that is not told keeps drawing into the old surface). */
static void gl_update(BackendWindow *b)
{
    if (b->gl_drawer)
        [b->gl_drawer->gl_ctx update];
}

/* ========================================================================== */
/*  software surface                                                          */
/* ========================================================================== */

/* Draws the back buffer into the view, in pixels: at its own size from the top left corner, never scaled, as on the other
   backends (what the buffer does not cover is black). It is the view's drawRect that does it, so a window that is exposed
   again is repainted from the buffer as it is, and backend_present_pixels asks for a draw and waits for it, so the buffer
   is free to be written again when the present returns. The image is told to be sRGB, so the colours are the ones that were
   written on any display, wide gamut ones included. */
static void pixels_draw(BackendWindow *b)
{
    CGContextRef cg = [[NSGraphicsContext currentContext] CGContext];
    NSRect bounds = [b->view bounds];
    CGContextSetRGBFillColor(cg, 0, 0, 0, 1);
    CGContextFillRect(cg, NSRectToCGRect(bounds));
    if (!b->pixels)
        return;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef data = CGDataProviderCreateWithData(NULL, b->pixels, (size_t)b->px_w * (size_t)b->px_h * 4, NULL);
    CGImageRef image = CGImageCreate((size_t)b->px_w, (size_t)b->px_h, 8, 32, (size_t)b->px_w * 4, space,
                                     (CGBitmapInfo)(kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little), data, NULL,
                                     false, kCGRenderingIntentDefault);
    if (image)
    {
        CGFloat scale = [b->window backingScaleFactor];
        CGFloat w = b->px_w / scale, h = b->px_h / scale;
        CGContextSetInterpolationQuality(cg, kCGInterpolationNone);
        CGContextDrawImage(cg, CGRectMake(0, bounds.size.height - h, w, h), image);
        CGImageRelease(image);
    }
    CGDataProviderRelease(data);
    CGColorSpaceRelease(space);
}

/* ========================================================================== */
/*  live callback: AppKit runs a loop of its own while a window is dragged or  */
/*  resized and while a menu is open, and the application's does not          */
/* ========================================================================== */

static void live_tick(BackendWindow *b)
{
    if (b->live_cb && b->in_modal && !b->in_live)
    {
        b->in_live = true;
        b->live_cb(b->live_w, b->live_user);
        b->in_live = false;
    }
}

static void live_timer_stop(BackendWindow *b)
{
    if (b->live_timer)
    {
        [b->live_timer invalidate];
        [b->live_timer release];
        b->live_timer = nil;
    }
}

/* The timer goes on the common run loop modes, which include the one AppKit tracks events in; a timer in the default
   mode would stand still until the drag ends. */
static void live_timer_start(BackendWindow *b)
{
    if (b->live_timer)
        return;
    NSTimer *t = [NSTimer timerWithTimeInterval:LIVE_INTERVAL
                                         target:(id)b->delegate
                                       selector:@selector(liveTick:)
                                       userInfo:nil
                                        repeats:YES];
    b->live_timer = [t retain];
    [[NSRunLoop currentRunLoop] addTimer:t forMode:NSRunLoopCommonModes];
}

static void modal_begin(BackendWindow *b)
{
    if (b->in_modal)
        return;
    b->in_modal = true;
    b->modal_in_send = g.in_send;
    if (b->live_cb)
        live_timer_start(b);
}

static void modal_end(BackendWindow *b)
{
    b->in_modal = false;
    b->modal_in_send = false;
    live_timer_stop(b);
}

/* ========================================================================== */
/*  classes: application delegate, window delegate, content view              */
/* ========================================================================== */

@interface ZenAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation ZenAppDelegate

/* Cmd+Q and Quit in the menu ask the application to terminate. The library does not end the process: every window is
   told its close was requested, as if its close button had been pressed, and the program decides. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    (void)sender;
    for (BackendWindow *b = g.windows; b; b = b->next)
    {
        Event e = {.type = EVENT_WINDOW_CLOSE};
        push(b, &e);
    }
    return NSTerminateCancel;
}

- (void)menuTrackingBegan:(NSNotification *)note
{
    (void)note;
    for (BackendWindow *b = g.windows; b; b = b->next)
        modal_begin(b);
}

- (void)menuTrackingEnded:(NSNotification *)note
{
    (void)note;
    for (BackendWindow *b = g.windows; b; b = b->next)
        modal_end(b);
}

@end

@implementation ZenWindowDelegate

- (id)initWithOwner:(BackendWindow *)b
{
    self = [super init];
    if (self)
        owner = b;
    return self;
}

- (void)detach
{
    owner = NULL;
}

- (void)liveTick:(NSTimer *)timer
{
    (void)timer;
    if (owner)
        live_tick(owner);
}

/* The close button: the program is asked, the window stays until it destroys it. */
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    if (owner)
    {
        Event e = {.type = EVENT_WINDOW_CLOSE};
        push(owner, &e);
    }
    return NO;
}

- (void)windowDidResize:(NSNotification *)note
{
    (void)note;
    if (!owner)
        return;
    gl_update(owner);
    push_size(owner);
    bool zoomed = [owner->window isZoomed] ? true : false;
    if (zoomed != owner->zoomed)
    {
        owner->zoomed = zoomed;
        Event e = {.type = zoomed ? EVENT_WINDOW_MAXIMIZE : EVENT_WINDOW_RESTORE};
        push(owner, &e);
    }
    live_tick(owner);
}

- (void)windowDidMove:(NSNotification *)note
{
    (void)note;
    if (!owner)
        return;
    gl_update(owner);
    push_move(owner);
    live_tick(owner);
}

/* Moved to another display (the context has to be told, the display may be on another GPU): the scale, if it differs, is
   told by windowDidChangeBackingProperties. */
- (void)windowDidChangeScreen:(NSNotification *)note
{
    (void)note;
    if (owner)
        gl_update(owner);
}

/* The window is about to be moved by the user dragging it: AppKit runs its loop until the button is let go. */
- (void)windowWillMove:(NSNotification *)note
{
    (void)note;
    if (owner)
        modal_begin(owner);
}

/* Moved to a screen with another scale (or the scale was changed): the size in points stays, the one in pixels does not. */
- (void)windowDidChangeBackingProperties:(NSNotification *)note
{
    (void)note;
    if (!owner)
        return;
    gl_update(owner);
    float scale = (float)[owner->window backingScaleFactor];
    if (scale == owner->scale)
        return;
    owner->scale = scale;
    cursor_apply(owner); /* a cursor from an image is made for the scale */
    Event e = {.type = EVENT_WINDOW_SCALE_CHANGED};
    e.data.scale.scale = scale;
    push(owner, &e);
    push_fb_size(owner);
}

- (void)windowDidBecomeKey:(NSNotification *)note
{
    (void)note;
    if (!owner)
        return;
    if (owner->mouse_mode == MOUSE_MODE_CAPTURED)
        mouse_lock(owner);
    Event e = {.type = EVENT_WINDOW_FOCUS};
    e.data.focus.gained = true;
    push(owner, &e);
    press_held_keys(owner);
}

- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    if (!owner)
        return;
    /* the keys held are let go (the core lets go of the buttons itself, on the focus event), and the pointer is given back
       to the mouse: a program that is not in front must not hold the pointer */
    release_held_keys(owner);
    mouse_unlock(owner);
    text_abandon(owner);
    Event e = {.type = EVENT_WINDOW_FOCUS};
    e.data.focus.gained = false;
    push(owner, &e);
}

- (void)windowDidMiniaturize:(NSNotification *)note
{
    (void)note;
    if (!owner)
        return;
    Event e = {.type = EVENT_WINDOW_MINIMIZE};
    push(owner, &e);
}

- (void)windowDidDeminiaturize:(NSNotification *)note
{
    (void)note;
    if (!owner)
        return;
    Event e = {.type = EVENT_WINDOW_RESTORE};
    push(owner, &e);
}

@end

@implementation ZenView

- (id)initWithFrame:(NSRect)frame owner:(BackendWindow *)b
{
    self = [super initWithFrame:frame];
    if (self)
    {
        owner = b;
        [self registerForDraggedTypes:[NSArray arrayWithObject:NSPasteboardTypeFileURL]];
    }
    return self;
}

/* The tracking area keeps its owner (this view) alive, so the view cannot be freed while it is installed. */
- (void)detach
{
    owner = NULL;
    if (area)
    {
        [self removeTrackingArea:area];
        [area release];
        area = nil;
    }
}

- (void)dealloc
{
    [self detach];
    [super dealloc];
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

/* The pointer entering and leaving the window comes from a tracking area over the whole view. */
- (void)updateTrackingAreas
{
    if (area)
    {
        [self removeTrackingArea:area];
        [area release];
        area = nil;
    }
    if (owner)
    {
        NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect;
        area = [[NSTrackingArea alloc] initWithRect:[self bounds] options:options owner:self userInfo:nil];
        [self addTrackingArea:area];
    }
    [super updateTrackingAreas];
}

- (void)mouseEntered:(NSEvent *)event
{
    (void)event;
    if (!owner || owner->hovered)
        return;
    owner->hovered = true;
    Event e = {.type = EVENT_WINDOW_ENTER};
    e.data.enter.entered = true;
    push(owner, &e);
}

- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    if (!owner || !owner->hovered)
        return;
    owner->hovered = false;
    Event e = {.type = EVENT_WINDOW_ENTER};
    e.data.enter.entered = false;
    push(owner, &e);
}

- (void)viewWillStartLiveResize
{
    [super viewWillStartLiveResize];
    if (owner)
        modal_begin(owner);
}

- (void)viewDidEndLiveResize
{
    [super viewDidEndLiveResize];
    if (owner)
        modal_end(owner);
}

/* ---- keyboard. A key the view takes and does not pass on (no [super keyDown:]) is not answered with the system beep.
   Key equivalents are AppKit's first: the menu is asked before the key reaches here, so Cmd+Q is the menu's (Quit, which
   the program is told as a close request of every window) and does not arrive as a key. Everything else with Command held
   (Cmd+C, Cmd+V, Cmd+S, Cmd+Z, ...) is not in the menu and arrives as an ordinary key, with KEYMOD_SUPER. ---- */

- (void)keyDown:(NSEvent *)event
{
    if (!owner)
        return;
    push_key(owner, [event keyCode], true, [event isARepeat] ? true : false, translate_mods([event modifierFlags]));
    /* the key goes to the input context too, which turns it into text (insertText), a composition (setMarkedText) or a command
       (doCommandBySelector, which is ignored: the key event was told already); not with Command or Control held, which make
       shortcuts and not text */
    if (owner->text_input && !([event modifierFlags] & (NSEventModifierFlagCommand | NSEventModifierFlagControl)))
        [self interpretKeyEvents:[NSArray arrayWithObject:event]];
}

/* The release of a key whose press never came to this window says nothing, and is not passed on: the menu took the press of
   Cmd+Q, or the key went down while another window had the focus. */
- (void)keyUp:(NSEvent *)event
{
    if (!owner)
        return;
    unsigned short code = [event keyCode];
    if (code < KEYMAP_SIZE && !owner->held[code])
        return;
    push_key(owner, code, false, false, translate_mods([event modifierFlags]));
}

- (void)flagsChanged:(NSEvent *)event
{
    if (owner)
        modifiers_changed(owner, event);
}

/* ---- mouse. The first click on a window that is not the key one also goes to the program, as it does on the other
   systems; the window is not first made the key one and then ignored. ---- */

- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    (void)event;
    return YES;
}

- (void)mouseDown:(NSEvent *)event
{
    if (owner)
        mouse_button_event(owner, event, MOUSE_LEFT, true);
}

- (void)mouseUp:(NSEvent *)event
{
    if (owner)
        mouse_button_event(owner, event, MOUSE_LEFT, false);
}

- (void)rightMouseDown:(NSEvent *)event
{
    if (owner)
        mouse_button_event(owner, event, MOUSE_RIGHT, true);
}

- (void)rightMouseUp:(NSEvent *)event
{
    if (owner)
        mouse_button_event(owner, event, MOUSE_RIGHT, false);
}

- (void)otherMouseDown:(NSEvent *)event
{
    if (owner)
        mouse_button_event(owner, event, other_button([event buttonNumber]), true);
}

- (void)otherMouseUp:(NSEvent *)event
{
    if (owner)
        mouse_button_event(owner, event, other_button([event buttonNumber]), false);
}

/* A drag goes on being delivered here while the pointer is outside the window, until the button is let go: AppKit's own
   capture. */
- (void)mouseMoved:(NSEvent *)event
{
    if (owner)
        mouse_motion_event(owner, event);
}

- (void)mouseDragged:(NSEvent *)event
{
    if (owner)
        mouse_motion_event(owner, event);
}

- (void)rightMouseDragged:(NSEvent *)event
{
    if (owner)
        mouse_motion_event(owner, event);
}

- (void)otherMouseDragged:(NSEvent *)event
{
    if (owner)
        mouse_motion_event(owner, event);
}

- (void)scrollWheel:(NSEvent *)event
{
    if (owner)
        scroll_event(owner, event);
}

/* ---- file drop: the view takes files dragged onto it (NSDraggingDestination) ---- */

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    return owner && [pasteboard_files([sender draggingPasteboard]) count] ? NSDragOperationCopy : NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
    return owner && [pasteboard_files([sender draggingPasteboard]) count] ? NSDragOperationCopy : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    return owner && drop_files(owner, [sender draggingPasteboard]) ? YES : NO;
}

/* ---- text input: the NSTextInputClient the input context talks to. The view has no text of its own: what the program
   shows is the program's, so there is nothing to give back for a range, and the caret is wherever the program says
   (window_set_text_input_rect), which is where the candidate window goes. ---- */

- (BOOL)hasMarkedText
{
    return owner && owner->composing ? YES : NO;
}

- (NSRange)markedRange
{
    return owner && owner->composing ? NSMakeRange(0, (NSUInteger)owner->marked_len) : NSMakeRange(NSNotFound, 0);
}

- (NSRange)selectedRange
{
    return NSMakeRange(NSNotFound, 0);
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange
{
    (void)replacementRange;
    if (!owner)
        return;
    NSString *text = [string isKindOfClass:[NSAttributedString class]] ? [string string] : (NSString *)string;
    text_edit(owner, text, selectedRange);
}

- (void)unmarkText
{
    if (owner)
        text_edit_end(owner);
}

- (NSArray *)validAttributesForMarkedText
{
    return [NSArray array];
}

- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
    (void)range;
    (void)actualRange;
    return nil;
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange
{
    (void)replacementRange;
    if (!owner)
        return;
    NSString *text = [string isKindOfClass:[NSAttributedString class]] ? [string string] : (NSString *)string;
    text_commit(owner, text);
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point
{
    (void)point;
    return NSNotFound;
}

/* Where the candidate window goes: the rectangle the program gave for its caret, in window coordinates (points, top left
   origin), as a rectangle of the screen (AppKit's, bottom left origin). */
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
    (void)range;
    (void)actualRange;
    if (!owner)
        return NSZeroRect;
    NSRect r = NSMakeRect(owner->ti_x, [self bounds].size.height - owner->ti_y - owner->ti_h, owner->ti_w, owner->ti_h);
    return [[self window] convertRectToScreen:[self convertRect:r toView:nil]];
}

/* The commands the input context turns keys into (move the caret, delete, new line, tab) are the program's own business, and
   it was told of the keys. */
- (void)doCommandBySelector:(SEL)selector
{
    (void)selector;
}

/* ---- cursor ---- */

- (void)resetCursorRects
{
    if (owner && owner->cursor_now)
        [self addCursorRect:[self visibleRect] cursor:owner->cursor_now];
    else
        [super resetCursorRects];
}

/* ---- software surface ---- */

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    if (owner && owner->render == RENDER_PIXELS)
        pixels_draw(owner);
}

@end

/* ========================================================================== */
/*  init / shutdown                                                           */
/* ========================================================================== */

/* The menu bar of an application with no nib: the application menu, holding Quit (Cmd+Q). */
static void make_menu_bar(void)
{
    NSString *name = [[NSProcessInfo processInfo] processName];
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *app_item = [[NSMenuItem alloc] init];
    [bar addItem:app_item];
    NSMenu *app_menu = [[NSMenu alloc] initWithTitle:name];
    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:name]
                                                  action:@selector(terminate:)
                                           keyEquivalent:@"q"];
    [app_menu addItem:quit];
    [app_item setSubmenu:app_menu];
    [NSApp setMainMenu:bar];
    [quit release];
    [app_menu release];
    [app_item release];
    [bar release];
}

bool backend_init(void)
{
    if (!on_main_thread())
        return error_set("the macOS backend must be used from the main thread");
    @autoreleasepool
    {
        if (!g.launched)
        {
            /* NSApplication talks to the window server and ends the process when there is none (a program started over
               ssh, or before anybody logged in): ask for a graphical session first, so that is an error and not a crash. */
            CFDictionaryRef session = CGSessionCopyCurrentDictionary();
            if (!session)
                return error_set("no graphical session: the program has no connection to the window server");
            CFRelease(session);

            [NSApplication sharedApplication];
            /* a program that is not in an application bundle is a background process until it says otherwise: no menu
               bar, no Dock icon, and its windows do not come to the front */
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            g.app_delegate = [[ZenAppDelegate alloc] init];
            [NSApp setDelegate:g.app_delegate];
            make_menu_bar();
            [[NSNotificationCenter defaultCenter] addObserver:g.app_delegate
                                                     selector:@selector(menuTrackingBegan:)
                                                         name:NSMenuDidBeginTrackingNotification
                                                       object:nil];
            [[NSNotificationCenter defaultCenter] addObserver:g.app_delegate
                                                     selector:@selector(menuTrackingEnded:)
                                                         name:NSMenuDidEndTrackingNotification
                                                       object:nil];
            [NSApp finishLaunching];
            g.launched = true;
        }
        [NSApp activateIgnoringOtherApps:YES];
        g.ready = true;
    }
    return true;
}

void backend_shutdown(void)
{
    /* The application, its delegate and its menu stay for the life of the process: AppKit cannot be taken down and
       brought up again, and a later backend_init finds them as they were. A window the program did not destroy must not
       keep the pointer cut loose from the mouse. */
    for (BackendWindow *b = g.windows; b; b = b->next)
        mouse_unlock(b);
    g.ready = false;
}

/* ========================================================================== */
/*  monitors                                                                  */
/* ========================================================================== */

int backend_monitor_count(void)
{
    @autoreleasepool
    {
        int n = (int)[[NSScreen screens] count];
        return n < MAX_MONITORS ? n : MAX_MONITORS;
    }
}

bool backend_monitor_info(int index, MonitorInfo *out)
{
    if (!out)
        return false;
    @autoreleasepool
    {
        NSArray *screens = [NSScreen screens];
        if (index < 0 || index >= (int)[screens count] || index >= MAX_MONITORS)
            return false;
        NSScreen *screen = [screens objectAtIndex:(NSUInteger)index];

        memset(out, 0, sizeof *out);
        out->index = index;
        top_left([screen frame], &out->x, &out->y);
        out->width = round_int([screen frame].size.width);
        out->height = round_int([screen frame].size.height);
        top_left([screen visibleFrame], &out->work_x, &out->work_y);
        out->work_w = round_int([screen visibleFrame].size.width);
        out->work_h = round_int([screen visibleFrame].size.height);
        out->content_scale = (float)[screen backingScaleFactor];
        out->refresh_hz = 60;
        out->primary = index == 0;

        NSNumber *number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
        if (number)
        {
            CGDirectDisplayID display = (CGDirectDisplayID)[number unsignedIntValue];
            CGSize mm = CGDisplayScreenSize(display);
            out->phys_width_mm = round_int(mm.width);
            out->phys_height_mm = round_int(mm.height);
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
            if (mode)
            {
                double hz = CGDisplayModeGetRefreshRate(mode);
                if (hz >= 1.0) /* an LCD panel and a virtual display say 0: no refresh to speak of */
                    out->refresh_hz = (int)(hz + 0.5);
                CGDisplayModeRelease(mode);
            }
        }

        const char *name = [[screen localizedName] UTF8String];
        snprintf(g.monitor_names[index], MONITOR_NAME_CAP, "%s", name ? name : "monitor");
        out->name = g.monitor_names[index];
    }
    return true;
}

bool backend_mouse_global_position(int *x, int *y)
{
    @autoreleasepool
    {
        NSPoint p = [NSEvent mouseLocation];
        *x = round_int(p.x);
        *y = round_int(primary_height() - p.y);
    }
    return true;
}

/* ========================================================================== */
/*  create / destroy                                                          */
/* ========================================================================== */

static NSScreen *screen_at(int monitor)
{
    NSArray *screens = [NSScreen screens];
    if (monitor >= 0 && monitor < (int)[screens count])
        return [screens objectAtIndex:(NSUInteger)monitor];
    return [screens count] ? [screens objectAtIndex:0] : nil;
}

/* Where a new window goes: a position given, the middle of a monitor's usable area, or (undefined) the cascade of
   the application's windows. The frame is what the window manager places, so the content position is turned into one. */
static void place_window(NSWindow *window, const WindowConfig *cfg, int w, int h)
{
    bool undefined = cfg->x == WINDOW_POS_UNDEFINED || cfg->y == WINDOW_POS_UNDEFINED;
    bool centre_x = cfg->x == WINDOW_POS_CENTERED;
    bool centre_y = cfg->y == WINDOW_POS_CENTERED;

    if (undefined)
    {
        /* The first window goes to the middle of the screen, and each one after it is staggered from the one before
           with Apple's cascade, which also wraps around at the edge of the screen. */
        if (!g.cascaded)
        {
            [window center];
            NSRect f = [window frame];
            g.cascade = NSMakePoint(NSMinX(f), NSMaxY(f));
            g.cascaded = true;
        }
        g.cascade = [window cascadeTopLeftFromPoint:g.cascade];
        return;
    }

    CGFloat x = centre_x ? 0 : cfg->x;
    CGFloat y = centre_y ? 0 : primary_height() - cfg->y - h;
    NSRect frame = [window frameRectForContentRect:NSMakeRect(x, y, w, h)];
    if (centre_x || centre_y)
    {
        NSScreen *screen = screen_at(cfg->monitor);
        NSRect area = screen ? [screen visibleFrame] : NSMakeRect(0, 0, w, h);
        if (centre_x)
            frame.origin.x = area.origin.x + (area.size.width - frame.size.width) / 2;
        if (centre_y)
            frame.origin.y = area.origin.y + (area.size.height - frame.size.height) / 2;
    }
    [window setFrame:frame display:NO];
}

BackendWindow *backend_create(const WindowConfig *cfg)
{
    if (!on_main_thread())
    {
        error_set("the macOS backend must be used from the main thread");
        return NULL;
    }
    /* BackendWindow is plain C and cleared by calloc: every field is its "nothing" until something sets it */
    BackendWindow *b = calloc(1, sizeof *b);
    if (!b)
    {
        error_set("out of memory");
        return NULL;
    }
    b->render = cfg->render;

    /* The context comes first, before any AppKit object of the window exists: a request macOS cannot meet (a version it
       does not have, a sample count no format offers) is refused without a window having been made. */
    if (cfg->render == RENDER_GL)
    {
        bool made;
        @autoreleasepool
        {
            made = gl_create(b, &cfg->gl);
        }
        if (!made)
        {
            free(b);
            return NULL;
        }
    }

    @autoreleasepool
    {
        int w = cfg->width > 0 ? cfg->width : 640;
        int h = cfg->height > 0 ? cfg->height : 480;
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (cfg->resizable)
            style |= NSWindowStyleMaskResizable;

        b->window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
        if (!b->window)
        {
            gl_destroy(b);
            free(b);
            error_set("cannot create the window");
            return NULL;
        }
        /* the window is ours to release, and a restored-from-last-run window or a tab would only get in the way */
        [b->window setReleasedWhenClosed:NO];
        [b->window setRestorable:NO];
        [b->window setTabbingMode:NSWindowTabbingModeDisallowed];
        /* mouse moves go to the key window's first responder, which is the view; hover with no focus is not reported */
        [b->window setAcceptsMouseMovedEvents:YES];

        b->view = [[ZenView alloc] initWithFrame:NSMakeRect(0, 0, w, h) owner:b];
        b->delegate = [[ZenWindowDelegate alloc] initWithOwner:b];
        [b->window setContentView:b->view];
        [b->window setDelegate:b->delegate];
        [b->window makeFirstResponder:b->view];
        b->mouse_mode = MOUSE_MODE_NORMAL;
        b->cursor_shape = CURSOR_DEFAULT;
        b->text_input = true;
        cursor_apply(b);
        if (cfg->render == RENDER_GL)
        {
            gl_attach(b);
            backend_set_vsync(b, cfg->vsync);
        }
        NSString *title = [NSString stringWithUTF8String:cfg->title ? cfg->title : ""];
        [b->window setTitle:title ? title : @""];

        place_window(b->window, cfg, w, h);
        b->scale = (float)[b->window backingScaleFactor];
        b->zoomed = [b->window isZoomed] ? true : false;

        b->next = g.windows;
        g.windows = b;

        /* TODO(mac phase 7): fullscreen modes, undecorated windows and the window kinds (dialog, utility, popup, tooltip
           with a parent). The window is made as an ordinary one, and the program told. */
        if (cfg->mode != WINDOW_WINDOWED)
            error_set("fullscreen windows are not implemented on macOS yet, the window is windowed");
        else if (cfg->undecorated || cfg->kind != WINDOW_KIND_NORMAL)
            error_set("undecorated windows and window kinds are not implemented on macOS yet, the window is an ordinary one");

        if (!cfg->hidden)
        {
            [b->window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }
        gl_update(b); /* the surface is on screen now */
    }
    return b;
}

void backend_destroy(BackendWindow *b)
{
    if (!b)
        return;
    @autoreleasepool
    {
        live_timer_stop(b);
        capture_stop(b);
        mouse_unlock(b);
        for (BackendWindow **p = &g.windows; *p; p = &(*p)->next)
            if (*p == b)
            {
                *p = b->next;
                break;
            }
        /* nothing AppKit does from here on may reach the struct that is about to go */
        [b->delegate detach];
        [b->view detach];
        [b->window setDelegate:nil];
        gl_destroy(b);
        [b->cursor_now release];
        [b->window orderOut:nil];
        [b->window close];
        [b->window setContentView:nil];
        [b->view release];
        [b->delegate release];
        [b->window release];
    }
    drop_free(b);
    free(b->pixels);
    free(b);
}

/* ========================================================================== */
/*  events                                                                    */
/* ========================================================================== */

void backend_pump_events(BackendWindow *b, Core *core)
{
    b->core = core;
    for (int i = 0; i < b->pending_count; i++)
        core_push_event(core, &b->pending[i]);
    b->pending_count = 0;

    b->pumping = true;
    for (;;)
    {
        @autoreleasepool
        {
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (!event)
                break;
            g.in_send = true;
            /* NSApplication keeps the release of a key that was pressed with Command held to itself, and no key window ever
               sees it: the key would stay down. The key window is given it directly. */
            if ([event type] == NSEventTypeKeyUp && ([event modifierFlags] & NSEventModifierFlagCommand))
                [[NSApp keyWindow] sendEvent:event];
            else
                [NSApp sendEvent:event];
            g.in_send = false;
            /* AppKit's own loop, if this event started one, is over when sendEvent returns */
            for (BackendWindow *w = g.windows; w; w = w->next)
                if (w->in_modal && w->modal_in_send)
                    modal_end(w);
        }
    }
    b->pumping = false;
}

void backend_run(BackendWindow *b, PlatformWindow *w, FrameCallback frame, void *user)
{
    (void)b;
    while (!window_should_close(w))
    {
        window_begin_frame(w);
        frame(w, user);
        window_swap(w);
    }
}

void backend_set_live_callback(BackendWindow *b, PlatformWindow *w, FrameCallback cb, void *user)
{
    b->live_w = w;
    b->live_cb = cb;
    b->live_user = user;
    if (b->in_modal) /* set from inside AppKit's loop, which is already running */
    {
        @autoreleasepool
        {
            if (cb)
                live_timer_start(b);
            else
                live_timer_stop(b);
        }
    }
}

/* ========================================================================== */
/*  geometry                                                                  */
/*                                                                            */
/*  Every call that reaches AppKit has an autorelease pool of its own: a C     */
/*  program has none around main, and what AppKit autoreleases in a call (the  */
/*  window itself, among it) would stay in the thread's outermost pool and     */
/*  keep every window ever made alive (test_cocoa_window counted them).        */
/* ========================================================================== */

void backend_get_size(BackendWindow *b, int *w, int *h)
{
    @autoreleasepool
    {
        NSRect c = content_rect(b);
        if (w)
            *w = round_int(c.size.width);
        if (h)
            *h = round_int(c.size.height);
    }
}

void backend_get_fb_size(BackendWindow *b, int *w, int *h)
{
    int fw, fh;
    @autoreleasepool
    {
        fb_size(b, &fw, &fh);
    }
    if (w)
        *w = fw;
    if (h)
        *h = fh;
}

void backend_get_pos(BackendWindow *b, int *x, int *y)
{
    int px, py;
    @autoreleasepool
    {
        top_left(content_rect(b), &px, &py);
    }
    if (x)
        *x = px;
    if (y)
        *y = py;
}

/* The size limits as the other backends read them: a value of zero or less leaves that side free. */
static int clamp_limit(int v, int lo, int hi)
{
    if (lo > 0 && v < lo)
        v = lo;
    if (hi > 0 && v > hi)
        v = hi;
    return v;
}

void backend_set_size(BackendWindow *b, int w, int h)
{
    @autoreleasepool
    {
        /* the top left corner stays where it is: setContentSize would keep the bottom left */
        NSRect c = content_rect(b);
        w = clamp_limit(w, b->min_w, b->max_w);
        h = clamp_limit(h, b->min_h, b->max_h);
        NSRect want = NSMakeRect(c.origin.x, c.origin.y + c.size.height - h, w, h);
        [b->window setFrame:[b->window frameRectForContentRect:want] display:YES];
    }
}

void backend_set_pos(BackendWindow *b, int x, int y)
{
    @autoreleasepool
    {
        NSRect c = content_rect(b);
        NSRect want = NSMakeRect(x, primary_height() - y - c.size.height, c.size.width, c.size.height);
        [b->window setFrameOrigin:[b->window frameRectForContentRect:want].origin];
    }
}

float backend_content_scale(BackendWindow *b)
{
    float scale;
    @autoreleasepool
    {
        scale = (float)[b->window backingScaleFactor];
    }
    return scale;
}

void backend_set_title(BackendWindow *b, const char *title)
{
    @autoreleasepool
    {
        NSString *s = [NSString stringWithUTF8String:title ? title : ""];
        [b->window setTitle:s ? s : @""];
    }
}

void backend_set_size_limits(BackendWindow *b, int minw, int minh, int maxw, int maxh)
{
    b->min_w = minw;
    b->min_h = minh;
    b->max_w = maxw;
    b->max_h = maxh;
    @autoreleasepool
    {
        [b->window setContentMinSize:NSMakeSize(minw > 0 ? minw : 0, minh > 0 ? minh : 0)];
        [b->window setContentMaxSize:NSMakeSize(maxw > 0 ? maxw : FLT_MAX, maxh > 0 ? maxh : FLT_MAX)];
    }
}

/* ========================================================================== */
/*  window state                                                              */
/* ========================================================================== */

void backend_minimize(BackendWindow *b)
{
    @autoreleasepool
    {
        [b->window miniaturize:nil];
    }
}

void backend_maximize(BackendWindow *b)
{
    @autoreleasepool
    {
        if (![b->window isZoomed])
            [b->window zoom:nil]; /* zoom toggles: only when it is not zoomed already */
    }
}

void backend_restore(BackendWindow *b)
{
    @autoreleasepool
    {
        if ([b->window isMiniaturized])
            [b->window deminiaturize:nil];
        else if ([b->window isZoomed])
            [b->window zoom:nil];
    }
}

void backend_show(BackendWindow *b)
{
    @autoreleasepool
    {
        [b->window makeKeyAndOrderFront:nil];
    }
}

void backend_hide(BackendWindow *b)
{
    @autoreleasepool
    {
        [b->window orderOut:nil];
    }
}

void backend_focus(BackendWindow *b)
{
    @autoreleasepool
    {
        /* a window that is not on screen cannot take the focus (as on X11, asking is not an error and does nothing) */
        if (![b->window isVisible])
            return;
        [NSApp activateIgnoringOtherApps:YES];
        [b->window makeKeyAndOrderFront:nil];
    }
}

void backend_request_attention(BackendWindow *b)
{
    (void)b;
    @autoreleasepool
    {
        [NSApp requestUserAttention:NSInformationalRequest]; /* the Dock icon bounces once, if the application is not active */
    }
}

bool backend_get_flag(BackendWindow *b, int flag)
{
    if (flag == WIN_FLAG_HOVERED)
        return b->hovered;
    bool on = false;
    @autoreleasepool
    {
        switch (flag)
        {
        case WIN_FLAG_FOCUSED:
            on = [b->window isKeyWindow];
            break;
        case WIN_FLAG_MINIMIZED:
            on = [b->window isMiniaturized];
            break;
        case WIN_FLAG_MAXIMIZED:
            on = [b->window isZoomed];
            break;
        case WIN_FLAG_VISIBLE:
            /* a window in the Dock is still shown as far as the program is concerned, as on Win32 */
            on = [b->window isVisible] || [b->window isMiniaturized];
            break;
        default:
            break;
        }
    }
    return on;
}

void *backend_native_handle(BackendWindow *b, NativeHandleType type)
{
    switch (type)
    {
    case NATIVE_WINDOW:
        return (void *)b->window; /* an NSWindow *; its view is [window contentView] */
    case NATIVE_DISPLAY:
        return NULL;
    case NATIVE_GL_CONTEXT:
        return (void *)b->gl_ctx; /* an NSOpenGLContext *, NULL unless the window is RENDER_GL */
    }
    return NULL;
}

/* ========================================================================== */
/*  OpenGL: the window's context, the worker's, the address of a function     */
/* ========================================================================== */

/* The window's own context goes back onto the window's own view (and is the current one of this thread); another window's
   surface is made with backend_make_current_on. A context belongs to the thread that made it current, and its view to
   the main thread: the window's context is for the main thread, the other threads' work is what gl_worker_* is for. */
void backend_make_current(BackendWindow *b)
{
    backend_make_current_on(b, b);
}

void backend_make_current_on(BackendWindow *target, BackendWindow *context)
{
    if (target->render != RENDER_GL || context->render != RENDER_GL)
        return;
    @autoreleasepool
    {
        if (context->gl_target != target)
        {
            /* one view has one context drawing to it, and one context draws to one view: the old links are cut */
            if (context->gl_target)
                context->gl_target->gl_drawer = NULL;
            if (target->gl_drawer && target->gl_drawer != context)
            {
                [target->gl_drawer->gl_ctx clearDrawable];
                target->gl_drawer->gl_target = NULL;
            }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" /* see gl_attach */
            [context->gl_ctx setView:target->view];
#pragma clang diagnostic pop
            context->gl_target = target;
            target->gl_drawer = context;
            [context->gl_ctx update];
        }
        [context->gl_ctx makeCurrentContext];
    }
}

/* Presents what the context that draws to this window has drawn; a window nothing draws to (its context went to another
   window) has nothing to present. */
void backend_swap(BackendWindow *b)
{
    if (b->render != RENDER_GL || !b->gl_drawer)
        return;
    @autoreleasepool
    {
        [b->gl_drawer->gl_ctx flushBuffer];
    }
}

void backend_set_vsync(BackendWindow *b, bool on)
{
    if (b->render != RENDER_GL || !b->gl_ctx)
        return;
    @autoreleasepool
    {
        int interval = on ? 1 : 0;
        [b->gl_ctx setValues:&interval forParameter:NSOpenGLContextParameterSwapInterval];
    }
}

/* The functions of the OpenGL framework by name, for the program's loader (glad). The framework exports every function
   the core profiles have, and a name it does not export is NULL, as the loader expects. */
void *backend_gl_proc_address(const char *name)
{
    if (!g.gl_framework)
        g.gl_framework = dlopen(ZGL_FRAMEWORK, RTLD_LAZY | RTLD_LOCAL);
    return g.gl_framework ? dlsym(g.gl_framework, name) : NULL;
}

/* A second context that shares the window's objects, for a thread of its own. It is made with CGL, which has no view and
   no AppKit in it: nothing of it touches the window, so any thread may make it current and use it. It has no surface to
   draw to (the default framebuffer is not there; textures, buffers, shaders, programs and framebuffer objects are). The
   pixel format is the window's own, which sharing needs. It is made and destroyed on the window's thread, before the
   window goes. What a worker made is seen by the window's context after the worker has called glFlush (or glFinish). */
struct BackendGLWorker
{
    CGLContextObj ctx;
};

BackendGLWorker *backend_gl_worker_create(BackendWindow *b)
{
    if (b->render != RENDER_GL || !b->gl_ctx)
        return NULL;
    CGLContextObj ctx = NULL;
    CGLError err = CGLCreateContext([b->gl_format CGLPixelFormatObj], [b->gl_ctx CGLContextObj], &ctx);
    if (err != kCGLNoError || !ctx)
    {
        error_set("cannot create the shared OpenGL context: %s", CGLErrorString(err));
        return NULL;
    }
    BackendGLWorker *worker = calloc(1, sizeof *worker);
    if (!worker)
    {
        CGLDestroyContext(ctx);
        return NULL;
    }
    worker->ctx = ctx;
    return worker;
}

bool backend_gl_worker_make_current(BackendGLWorker *worker, bool current)
{
    return CGLSetCurrentContext(current ? worker->ctx : NULL) == kCGLNoError;
}

void backend_gl_worker_destroy(BackendGLWorker *worker)
{
    CGLDestroyContext(worker->ctx);
    free(worker);
}

/* ========================================================================== */
/*  software surface                                                          */
/* ========================================================================== */

/* The back buffer is as big as the window's framebuffer, in pixels (a scale 2 window has four times the pixels of its
   size in points), 32 bits a pixel, 0xAARRGGBB, `stride` the same as `width`. Its content is kept between frames, and
   starts as zeros (black) after a resize. */
bool backend_lock_pixels(BackendWindow *b, Framebuffer *out)
{
    if (b->render != RENDER_PIXELS || !out)
        return false;
    int w, h;
    @autoreleasepool
    {
        fb_size(b, &w, &h);
    }
    if (w <= 0 || h <= 0)
        return false;
    if (!b->pixels || b->px_w != w || b->px_h != h)
    {
        uint32_t *grown = calloc((size_t)w * (size_t)h, 4);
        if (!grown)
            return false;
        free(b->pixels);
        b->pixels = grown;
        b->px_w = w;
        b->px_h = h;
    }
    out->pixels = b->pixels;
    out->width = b->px_w;
    out->height = b->px_h;
    out->stride = b->px_w;
    return true;
}

void backend_present_pixels(BackendWindow *b)
{
    if (b->render != RENDER_PIXELS || !b->pixels)
        return;
    @autoreleasepool
    {
        [b->view display]; /* draws the view now (drawRect), so the buffer is not read after this returns */
    }
}

/* ========================================================================== */
/*  stubs: what the later phases bring. Each one does nothing, says no (false, NULL, 0) and, where the core has an      */
/*  error to set, says why.                                                                                          */
/* ========================================================================== */

static bool not_yet(const char *what)
{
    return error_set("%s is not implemented on macOS yet", what);
}

/* ---- keyboard and mouse ---- */

/* Caps Lock is the only lock macOS has (no Num Lock), and the system says it as it is now. */
int backend_lock_state(void)
{
    int state = 0;
    @autoreleasepool
    {
        if ([NSEvent modifierFlags] & NSEventModifierFlagCapsLock)
            state |= KEYMOD_CAPS_LOCK;
    }
    return state;
}

bool backend_mouse_capture(BackendWindow *b, bool on)
{
    bool in_effect = false;
    @autoreleasepool
    {
        if (!on)
            capture_stop(b);
        else if (b->capture_monitor)
            in_effect = true;
        else
        {
            NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
                               NSEventMaskOtherMouseDragged | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                               NSEventMaskRightMouseDown | NSEventMaskRightMouseUp | NSEventMaskOtherMouseDown |
                               NSEventMaskOtherMouseUp;
            id monitor = [NSEvent addGlobalMonitorForEventsMatchingMask:mask
                                                                handler:^(NSEvent *event) {
                                                                    capture_event(b, event);
                                                                }];
            b->capture_monitor = [monitor retain];
            in_effect = monitor != nil;
        }
    }
    return in_effect;
}

PlatformCursor *backend_cursor_create(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    PlatformCursor *pc = calloc(1, sizeof *pc);
    if (pc)
        pc->argb = malloc((size_t)w * (size_t)h * 4);
    if (!pc || !pc->argb)
    {
        free(pc);
        error_set("out of memory");
        return NULL;
    }
    memcpy(pc->argb, argb, (size_t)w * (size_t)h * 4);
    pc->w = w;
    pc->h = h;
    pc->hot_x = hot_x;
    pc->hot_y = hot_y;
    bool made;
    @autoreleasepool
    {
        NSScreen *screen = [NSScreen mainScreen];
        float scale = screen ? (float)[screen backingScaleFactor] : 1.0f;
        made = image_cursor(pc, scale < 1.0f ? 1.0f : scale) != nil;
    }
    if (!made)
    {
        free(pc->argb);
        free(pc);
        error_set("cannot create the cursor");
        return NULL;
    }
    return pc;
}

void backend_cursor_destroy(PlatformCursor *c)
{
    @autoreleasepool
    {
        for (BackendWindow *b = g.windows; b; b = b->next)
            if (b->cursor_image == c) /* a window still showing it goes back to its shape */
            {
                b->cursor_image = NULL;
                cursor_apply(b);
            }
        [c->cursor release];
    }
    free(c->argb);
    free(c);
}

void backend_set_cursor_image(BackendWindow *b, PlatformCursor *c)
{
    @autoreleasepool
    {
        b->cursor_image = c; /* NULL: back to the shape */
        cursor_apply(b);
    }
}

void backend_set_cursor(BackendWindow *b, int cursor)
{
    @autoreleasepool
    {
        b->cursor_shape = cursor < 0 || cursor >= CURSOR_COUNT ? CURSOR_DEFAULT : cursor;
        b->cursor_image = NULL; /* a shape replaces an image, as on the other backends */
        cursor_apply(b);
    }
}

/* Moves the pointer. CGWarpMouseCursorPosition takes the global coordinates the library uses (top left of the primary
   display, y down) and does not make the system send a motion event, so the program is told by one made here, as the
   other systems tell it; and the mouse is associated again at once, which cuts the quarter second during which the system
   would otherwise ignore the mouse after a warp. In MOUSE_MODE_CAPTURED the pointer is held and only the position the
   program follows is set, as on X11 and Win32. */
void backend_set_mouse_pos(BackendWindow *b, int x, int y)
{
    if (b->mouse_mode == MOUSE_MODE_CAPTURED)
    {
        b->virt_x = x;
        b->virt_y = y;
        return;
    }
    @autoreleasepool
    {
        int cx, cy;
        top_left(content_rect(b), &cx, &cy);
        CGWarpMouseCursorPosition(CGPointMake(cx + x, cy + y));
        CGAssociateMouseAndMouseCursorPosition(true);
    }
    b->last_x = x;
    b->last_y = y;
    push_mouse_move(b, x, y, 0, 0);
}

/* MOUSE_MODE_HIDDEN hides the pointer over the window (a cursor with no picture on the view's cursor rect: it comes back
   by itself when the pointer leaves or the window is not the key one, and there is no count of hides to keep balanced).
   MOUSE_MODE_CAPTURED does that and also holds the pointer and gives the program the motion of the mouse, for as long as
   the window is the key one: it is given back when the focus goes, and taken again when it returns. */
void backend_set_mouse_mode(BackendWindow *b, int mode)
{
    if (mode != MOUSE_MODE_HIDDEN && mode != MOUSE_MODE_CAPTURED)
        mode = MOUSE_MODE_NORMAL;
    @autoreleasepool
    {
        if (mode == MOUSE_MODE_CAPTURED && b->mouse_mode != MOUSE_MODE_CAPTURED)
            pointer_in_window(b, &b->virt_x, &b->virt_y);
        b->mouse_mode = mode;
        if (mode == MOUSE_MODE_CAPTURED)
        {
            if ([b->window isKeyWindow])
                mouse_lock(b);
        }
        else
            mouse_unlock(b);
        cursor_apply(b);
    }
}

/* ---- text input ---- */

/* On by default. Off, no key is passed to the input context (so no input method interferes with the keys and nothing is typed),
   and a composition that was on is dropped. */
void backend_set_text_input(BackendWindow *b, bool on)
{
    @autoreleasepool
    {
        b->text_input = on;
        if (!on)
            text_abandon(b);
    }
}

/* The caret's rectangle, in window coordinates; the candidate window is put next to it, and asked for it again now. */
void backend_set_text_input_rect(BackendWindow *b, int x, int y, int w, int h)
{
    b->ti_x = x;
    b->ti_y = y;
    b->ti_w = w;
    b->ti_h = h;
    @autoreleasepool
    {
        [[b->view inputContext] invalidateCharacterCoordinates];
    }
}

/* ---- clipboard: the general pasteboard ---- */

/* The pasteboard type of a MIME type: text and PNG have their own, anything else is a type named as the MIME type is. Files
   (text/uri-list) are not a type of an item but file URLs, written and read as such. */
static NSPasteboardType clipboard_type(const char *mime)
{
    if (strcmp(mime, CLIPBOARD_TEXT) == 0)
        return NSPasteboardTypeString;
    if (strcmp(mime, CLIPBOARD_PNG) == 0)
        return NSPasteboardTypePNG;
    return [NSString stringWithUTF8String:mime];
}

/* The URLs of a text/uri-list: one per line, comments (#) and empty lines left out. */
static void add_uri_list(NSMutableArray *urls, const void *data, size_t size)
{
    NSString *text = [[NSString alloc] initWithBytes:data length:size encoding:NSUTF8StringEncoding];
    for (NSString *line in [text componentsSeparatedByString:@"\n"])
    {
        NSString *trimmed = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        if ([trimmed length] == 0 || [trimmed hasPrefix:@"#"])
            continue;
        NSURL *url = [NSURL URLWithString:trimmed];
        if (url)
            [urls addObject:url];
    }
    [text release];
}

/* Replaces the clipboard: one pasteboard item with every representation of the content that is not files, and the files as the
   file URLs they are (each a pasteboard item of its own, as the Finder writes them). count 0 clears it. The pasteboard keeps the
   data itself, so it stays when the program ends. */
bool backend_clipboard_set(const ClipboardItem *items, int count)
{
    bool ok;
    @autoreleasepool
    {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        if (count <= 0)
            return true;
        NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
        NSMutableArray *objects = [NSMutableArray array];
        bool item_used = false;
        for (int i = 0; i < count; i++)
        {
            const ClipboardItem *it = &items[i];
            if (!it->mime || (!it->data && it->size))
                continue;
            if (strcmp(it->mime, CLIPBOARD_URIS) == 0)
            {
                add_uri_list(objects, it->data, it->size);
            }
            else if (strcmp(it->mime, CLIPBOARD_TEXT) == 0)
            {
                NSString *text = [[NSString alloc] initWithBytes:it->data length:it->size encoding:NSUTF8StringEncoding];
                if (text)
                {
                    [item setString:text forType:NSPasteboardTypeString];
                    item_used = true;
                }
                [text release];
            }
            else
            {
                [item setData:[NSData dataWithBytes:it->data length:it->size] forType:clipboard_type(it->mime)];
                item_used = true;
            }
        }
        if (item_used)
            [objects insertObject:item atIndex:0];
        ok = [objects count] == 0 || [pb writeObjects:objects];
        [item release];
    }
    return ok ? true : error_set("cannot write the clipboard");
}

bool backend_clipboard_has(const char *mime)
{
    bool has;
    @autoreleasepool
    {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        if (strcmp(mime, CLIPBOARD_URIS) == 0)
            has = [pasteboard_files(pb) count] > 0;
        else
            has = [pb availableTypeFromArray:[NSArray arrayWithObject:clipboard_type(mime)]] != nil;
    }
    return has;
}

/* A copy of the data of a type, with a NUL after it that the size does not count, or NULL when the clipboard has none of that
   type. Text comes as UTF-8, files as the URIs of the text/uri-list, one per line. */
void *backend_clipboard_get(const char *mime, size_t *size)
{
    void *copy = NULL;
    @autoreleasepool
    {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        const void *bytes = NULL;
        size_t n = 0;
        NSString *joined = nil;
        NSData *data = nil;
        if (strcmp(mime, CLIPBOARD_URIS) == 0)
        {
            NSArray *urls = pasteboard_files(pb);
            if ([urls count])
            {
                NSMutableArray *lines = [NSMutableArray array];
                for (NSURL *url in urls)
                    [lines addObject:[url absoluteString]];
                joined = [lines componentsJoinedByString:@"\n"];
            }
        }
        else if (strcmp(mime, CLIPBOARD_TEXT) == 0)
            joined = [pb stringForType:NSPasteboardTypeString];
        else
            data = [pb dataForType:clipboard_type(mime)];
        if (joined)
        {
            bytes = [joined UTF8String];
            n = bytes ? strlen(bytes) : 0;
        }
        else if (data)
        {
            bytes = [data bytes];
            n = [data length];
        }
        if (bytes || n == 0)
        {
            copy = malloc(n + 1);
            if (copy)
            {
                if (n)
                    memcpy(copy, bytes, n);
                ((char *)copy)[n] = '\0';
                if (size)
                    *size = n;
            }
        }
        if (!joined && !data)
        {
            free(copy);
            copy = NULL;
        }
    }
    return copy;
}

/* ---- phase 7: window modes and looks ---- */

void backend_set_mode(BackendWindow *b, WindowMode mode, int monitor)
{
    (void)b; /* TODO(mac phase 7) */
    (void)monitor;
    if (mode != WINDOW_WINDOWED)
        not_yet("fullscreen windows");
}
WindowMode backend_get_mode(BackendWindow *b)
{
    (void)b; /* TODO(mac phase 7): a window is always windowed until then */
    return WINDOW_WINDOWED;
}
void backend_set_icon(BackendWindow *b, int w, int h, const uint8_t *rgba)
{
    (void)b; /* TODO(mac phase 7) */
    (void)w;
    (void)h;
    (void)rgba;
    not_yet("window icons");
}
void backend_set_opacity(BackendWindow *b, float a)
{
    (void)b; /* TODO(mac phase 7) */
    (void)a;
    not_yet("window opacity");
}
void backend_set_always_on_top(BackendWindow *b, bool on)
{
    (void)b; /* TODO(mac phase 7) */
    (void)on;
    not_yet("always on top");
}
void backend_set_decorated(BackendWindow *b, bool on)
{
    (void)b; /* TODO(mac phase 7) */
    (void)on;
    not_yet("window_set_decorated");
}
void backend_set_hit_test(BackendWindow *b, PlatformWindow *w, HitTestFunc fn, void *user)
{
    (void)b; /* TODO(mac phase 7) */
    (void)w;
    (void)user;
    if (fn)
        not_yet("the window hit test");
}

/* ---- phase 8: Vulkan (through Metal) ---- */

const char *const *backend_vulkan_extensions(uint32_t *count)
{
    *count = 0; /* TODO(mac phase 8): VK_KHR_surface and VK_EXT_metal_surface */
    return NULL;
}
bool backend_vulkan_create_surface(BackendWindow *b, void *instance, const void *allocator, uint64_t *out_surface)
{
    (void)b; /* TODO(mac phase 8) */
    (void)instance;
    (void)allocator;
    (void)out_surface;
    return not_yet("Vulkan surface creation");
}
