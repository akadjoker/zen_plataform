/*
 * backend_web.c - Emscripten backend. The browser owns the loop, so app_run
 * registers a main-loop callback and never returns. html5.h event callbacks fire
 * asynchronously; they queue normalized events into a pending buffer that
 * backend_pump_events drains into the core, the same shape as the fake backend.
 *
 * Geometry has two real spaces here: the canvas CSS size is screen coordinates,
 * the canvas drawing-buffer size is pixels, and devicePixelRatio bridges them.
 */
#include "core_internal.h"
#include "backend.h"
#include "error_internal.h"
#include "clipboard_mem.h"
#include "web_key_rules.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define WEB_PENDING_MAX 256
#define CANVAS_TARGET "#canvas"

typedef struct
{
    bool is_char;
    Event ev;
    uint32_t cp;
} WebItem;

struct BackendWindow
{
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl;
    Core *core;

    WebItem pending[WEB_PENDING_MAX];
    int pending_count;

    int css_w, css_h; /* screen coords */
    bool focused, visible;
    int cursor_mode;

    RenderMode render;
    uint32_t *px; /* pixel mode staging buffer (0xAARRGGBB) */
    int px_w, px_h;
};

/* The browser hands a single canvas, so a single backend window is in play. */
static BackendWindow *g_active;

/* Everything that touches the page (Module.canvas, the document) is MAIN_THREAD_EM_ASM and not EM_ASM: the same thing in a build
   without threads, and in one with the program on a worker (PROXY_TO_PTHREAD) the call goes to the browser's main thread,
   where the page is (a worker has no document, and its Module.canvas is an OffscreenCanvas). */

/* Which thread runs what. In a build without threads, or with them but main() on the browser's main thread, everything below
   is one thread. With PROXY_TO_PTHREAD main() and the frame run on a worker, while the browser's events (JS) only exist on the
   main browser thread: the callbacks of mouse, wheel, touch and focus are called back on the worker (html5.h: the thread
   that registered them), but the keyboard callbacks are registered to run on the browser's main thread (on_key: a worker cannot
   answer preventDefault, and the paste and copy events are there too), and so are the functions the JS side calls
   (pf_web_paste, pf_web_copy_text, pf_web_flush_held and the drop ones). So the event queue, the held key and the clipboard
   store are shared between two threads, and one spin lock covers them; what is done under it is a copy of an event or of a
   string, never a call into the application. Without threads the lock is a plain flag that is never contended. */
static volatile int g_lock;

static void web_lock(void)
{
    while (__sync_lock_test_and_set(&g_lock, 1))
    {
    }
}

static void web_unlock(void)
{
    __sync_lock_release(&g_lock);
}

/* the queue, with the lock held */
static void enqueue_event_locked(BackendWindow *b, const Event *ev)
{
    if (b->pending_count < WEB_PENDING_MAX)
    {
        b->pending[b->pending_count].is_char = false;
        b->pending[b->pending_count].ev = *ev;
        b->pending_count++;
    }
}

static void enqueue_char_locked(BackendWindow *b, uint32_t cp)
{
    if (b->pending_count < WEB_PENDING_MAX)
    {
        b->pending[b->pending_count].is_char = true;
        b->pending[b->pending_count].cp = cp;
        b->pending_count++;
    }
}

static void enqueue_event(BackendWindow *b, const Event *ev)
{
    web_lock();
    enqueue_event_locked(b, ev);
    web_unlock();
}

/* ========================================================================== */
/*  DOM code -> KEY_ translation                                              */
/* ========================================================================== */

static int translate_code(const char *code)
{
    if (strncmp(code, "Key", 3) == 0 && code[3])
        return KEY_A + (code[3] - 'A');
    if (strncmp(code, "Digit", 5) == 0 && code[5])
        return KEY_ZERO + (code[5] - '0');
    if (strncmp(code, "Numpad", 6) == 0 && code[6] >= '0' && code[6] <= '9' && !code[7])
        return KEY_KP_0 + (code[6] - '0');
    if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9')
    {
        int n = atoi(code + 1);
        if (n >= 1 && n <= 12)
            return KEY_F1 + (n - 1);
    }

    struct
    {
        const char *name;
        int key;
    } map[] = {
        {"Space", KEY_SPACE},
        {"Enter", KEY_ENTER},
        {"Escape", KEY_ESCAPE},
        {"Tab", KEY_TAB},
        {"Backspace", KEY_BACKSPACE},
        {"Delete", KEY_DELETE},
        {"Insert", KEY_INSERT},
        {"ArrowLeft", KEY_LEFT},
        {"ArrowRight", KEY_RIGHT},
        {"ArrowUp", KEY_UP},
        {"ArrowDown", KEY_DOWN},
        {"Home", KEY_HOME},
        {"End", KEY_END},
        {"PageUp", KEY_PAGE_UP},
        {"PageDown", KEY_PAGE_DOWN},
        {"Minus", KEY_MINUS},
        {"Equal", KEY_EQUAL},
        {"Comma", KEY_COMMA},
        {"Period", KEY_PERIOD},
        {"Slash", KEY_SLASH},
        {"Backslash", KEY_BACKSLASH},
        {"Semicolon", KEY_SEMICOLON},
        {"Quote", KEY_APOSTROPHE},
        {"Backquote", KEY_GRAVE},
        {"BracketLeft", KEY_LEFT_BRACKET},
        {"BracketRight", KEY_RIGHT_BRACKET},
        {"ShiftLeft", KEY_LEFT_SHIFT},
        {"ShiftRight", KEY_RIGHT_SHIFT},
        {"ControlLeft", KEY_LEFT_CONTROL},
        {"ControlRight", KEY_RIGHT_CONTROL},
        {"AltLeft", KEY_LEFT_ALT},
        {"AltRight", KEY_RIGHT_ALT},
        {"MetaLeft", KEY_LEFT_SUPER},
        {"MetaRight", KEY_RIGHT_SUPER},
        {"CapsLock", KEY_CAPS_LOCK},
        {"NumLock", KEY_NUM_LOCK},
        {"ScrollLock", KEY_SCROLL_LOCK},
        {"PrintScreen", KEY_PRINT_SCREEN},
        {"Pause", KEY_PAUSE},
        {"ContextMenu", KEY_MENU},
        {"NumpadDecimal", KEY_KP_DECIMAL},
        {"NumpadDivide", KEY_KP_DIVIDE},
        {"NumpadMultiply", KEY_KP_MULTIPLY},
        {"NumpadSubtract", KEY_KP_SUBTRACT},
        {"NumpadAdd", KEY_KP_ADD},
        {"NumpadEnter", KEY_KP_ENTER},
        {"NumpadEqual", KEY_KP_EQUAL},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(code, map[i].name) == 0)
            return map[i].key;
    return KEY_NULL;
}

static int translate_kb_mods(const EmscriptenKeyboardEvent *e)
{
    int mods = 0;
    if (e->shiftKey)
        mods |= KEYMOD_SHIFT;
    if (e->ctrlKey)
        mods |= KEYMOD_CTRL;
    if (e->altKey)
        mods |= KEYMOD_ALT;
    if (e->metaKey)
        mods |= KEYMOD_SUPER;
    return mods;
}

/* Decode a single UTF-8 codepoint; returns 0 if the string is a named key. */
static uint32_t decode_one_codepoint(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t cp;
    int len;
    if (p[0] < 0x80)
    {
        cp = p[0];
        len = 1;
    }
    else if ((p[0] >> 5) == 0x6)
    {
        cp = p[0] & 0x1F;
        len = 2;
    }
    else if ((p[0] >> 4) == 0xE)
    {
        cp = p[0] & 0x0F;
        len = 3;
    }
    else if ((p[0] >> 3) == 0x1E)
    {
        cp = p[0] & 0x07;
        len = 4;
    }
    else
        return 0;
    for (int i = 1; i < len; i++)
    {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    return p[len] == '\0' ? cp : 0; /* trailing bytes mean a named key like "Tab" */
}

/* ========================================================================== */
/*  html5 event callbacks                                                     */
/* ========================================================================== */

/* ---- the clipboard keys and the browser's paste, copy and cut events ----
 *
 * A page reads the clipboard only inside a `paste` event, and writes it reliably only inside a `copy` or `cut` event (the
 * async clipboard API does not exist on a page opened over plain http), and the browser fires those only when the key that
 * asked for them was not prevented. So Ctrl or Meta with V, C, X and Shift+Insert, Ctrl+Insert, Shift+Delete are left to the
 * browser (on_key does not preventDefault them), and web/pf_web.js listens to the three events and calls the functions below.
 * The order the application sees is the one a native backend gives: the text is the clipboard's before the paste key reaches
 * it, and the clipboard is what it set in answer to the copy or cut key before the browser takes the text.
 *
 *  paste   keydown: the key is held, not queued. paste event: the text becomes the clipboard content (clipboard_get returns
 *          it), then the key is queued. No paste event (the browser refused it, nothing was there): a timer of the page
 *          queues the held key at the end of the task. A paste from the browser's menu has no key: Ctrl+V (Meta+V on a Mac)
 *          is queued for it.
 *  copy    keydown: the key is queued and the copy is armed. copy event (the same task, right after): the application has not
 *          run yet, so one frame runs now, on this very call, inside the browser's handler (where the text can still be
 *          handed to the browser), and what the application set with clipboard_set during it is given to the browser. The
 *          application that sets nothing (nothing selected) leaves the browser's clipboard as it is. When the application
 *          runs on another thread (PROXY_TO_PTHREAD) this thread cannot run its frame, and waiting for the worker's does not
 *          work (measured: the worker's frame is driven by the page's frame scheduler, which does not run while this thread
 *          waits): the event gets what the application had already answered, if anything, and otherwise it is left to the
 *          browser; the text the application sets a frame later goes out through the write below (navigator.clipboard,
 *          inside the user activation the key started, which lasts a few seconds). A build with threads needs
 *          SharedArrayBuffer, and so a secure context, where that API exists.
 */
static unsigned g_clip_seq;    /* every clipboard_set and every paste: what a copy compares to see that the application answered */
static bool g_held_valid;      /* a paste key waits for its paste event */
static WebItem g_held;
static double g_paste_flush_ms; /* when a paste key last went to the application without its paste event: one a moment later is its */
static bool g_copy_armed;
static unsigned g_copy_seq;    /* g_clip_seq when the copy key came */
static bool g_in_tick;
static pthread_t g_loop_thread; /* the thread that runs the loop (app_run): the program's frame, in a build with PROXY_TO_PTHREAD a
                                   worker; not emscripten_is_main_runtime_thread(), which in such a build is the browser's thread */
static char *g_copy_out;       /* what pf_web_copy_text last returned */

/* the held paste key goes to the queue, behind what was queued before it; with the lock held */
static void held_flush_locked(void)
{
    if (g_held_valid && g_active)
        enqueue_event_locked(g_active, &g_held.ev);
    g_held_valid = false;
}

/* a key and its release, as a keyboard shortcut that came from the browser's menu and not from the keyboard; lock held */
static void synth_key_locked(int key, int mods)
{
    if (!g_active)
        return;
    Event ev = {.type = EVENT_KEY};
    ev.data.key.key = key;
    ev.data.key.mods = mods;
    ev.data.key.down = true;
    enqueue_event_locked(g_active, &ev);
    ev.data.key.down = false;
    enqueue_event_locked(g_active, &ev);
}

/* Implemented in web/pf_web.js */
extern void pf_web_defer_flush(void);
extern int pf_web_altgr(void);

static EM_BOOL on_key(int type, const EmscriptenKeyboardEvent *e, void *user)
{
    (void)user;
    BackendWindow *b = g_active;
    if (!b)
        return EM_TRUE;
    const bool down = type == EMSCRIPTEN_EVENT_KEYDOWN;

    Event ev = {.type = EVENT_KEY};
    ev.data.key.key = translate_code(e->code);
    ev.data.key.down = down;
    ev.data.key.repeat = e->repeat;
    ev.data.key.mods = translate_kb_mods(e);

    const WebClipCommand cmd =
        down ? webkey_clip_command(e->key, e->code, e->ctrlKey, e->shiftKey, e->altKey, e->metaKey) : WEBKEY_NONE;

    /* A key that types its character, but not with Ctrl or Meta held (a shortcut; the rule in platform.h): Ctrl+A in a field
       is a select-all, not an "a". AltGr on a Windows layout is Ctrl+Alt, and the browser says it is AltGr. */
    uint32_t cp = 0;
    if (down && webkey_types_text(e->ctrlKey, e->altKey, e->metaKey, e->ctrlKey && e->altKey && pf_web_altgr() != 0))
    {
        cp = decode_one_codepoint(e->key);
        if (cp < 0x20 || cp == 0x7F)
            cp = 0;
    }

    web_lock();
    held_flush_locked(); /* a held paste key goes before any later key */
    if (cmd == WEBKEY_PASTE)
    {
        g_held.is_char = false;
        g_held.ev = ev;
        g_held_valid = true;
    }
    else
        enqueue_event_locked(b, &ev);
    if (cmd == WEBKEY_COPY || cmd == WEBKEY_CUT)
    {
        g_copy_armed = true;
        g_copy_seq = g_clip_seq;
    }
    if (cp)
        enqueue_char_locked(b, cp);
    web_unlock();

    if (cmd != WEBKEY_NONE)
    {
        pf_web_defer_flush(); /* the end of this task: a paste or copy event that did not come must not hold the key */
        return EM_FALSE;      /* the browser does what the key asks: paste, copy, cut (fires the event) */
    }
    return EM_TRUE; /* preventDefault: keep Tab/arrows/Space from scrolling */
}

/* JS: the timer after a clipboard key. A held paste key whose paste event never came goes to the application now, and a
   copy that was armed is over. */
EMSCRIPTEN_KEEPALIVE void pf_web_flush_held(void)
{
    web_lock();
    if (g_held_valid)
        g_paste_flush_ms = emscripten_get_now();
    held_flush_locked();
    g_copy_armed = false;
    web_unlock();
}

/* JS: the paste event. text is a malloc'd UTF-8 string (taken over here), or NULL when the clipboard holds no text. */
EMSCRIPTEN_KEEPALIVE void pf_web_paste(char *text, int mac)
{
    web_lock();
    if (text)
    {
        const ClipboardItem it = {CLIPBOARD_TEXT, text, strlen(text)};
        clipmem_set(&it, 1);
        g_clip_seq++;
    }
    if (g_held_valid)
        held_flush_locked(); /* the key that asked for this paste: the text is the clipboard's before it is queued */
    else if (emscripten_get_now() - g_paste_flush_ms > 50.0)
        synth_key_locked(KEY_V, mac ? KEYMOD_SUPER : KEYMOD_CTRL); /* from the browser's menu: no key was pressed */
    web_unlock();
    free(text);
}

static void main_tick(void);

/* One frame now, from inside a browser event handler. Not when a frame is already running (a loop that yields). */
static bool web_tick_now(void);

/* JS: the copy or cut event. Returns the text the application put on the clipboard in answer to the key, in a buffer that
   stays valid until the next call, or NULL when it put none (the browser's clipboard is left alone). */
EMSCRIPTEN_KEEPALIVE const char *pf_web_copy_text(int cut, int mac)
{
    web_lock();
    if (!g_copy_armed) /* from the browser's menu: no key was pressed, the application is asked as if it was */
    {
        g_copy_armed = true;
        g_copy_seq = g_clip_seq;
        synth_key_locked(cut ? KEY_X : KEY_C, mac ? KEYMOD_SUPER : KEYMOD_CTRL);
    }
    bool answered = g_clip_seq != g_copy_seq;
    web_unlock();

    if (!answered && pthread_equal(pthread_self(), g_loop_thread))
        web_tick_now(); /* the application's thread is this one: its frame, here, inside the event */
    /* (on another thread: see above; nothing to wait for) */

    web_lock();
    g_copy_armed = false;
    free(g_copy_out);
    g_copy_out = NULL;
    if (g_clip_seq != g_copy_seq)
    {
        const ClipMemItem *t = clipmem_find(CLIPBOARD_TEXT);
        if (t)
            g_copy_out = strdup((const char *)t->data);
    }
    const char *out = g_copy_out;
    web_unlock();
    return out;
}

static EM_BOOL on_mouse(int type, const EmscriptenMouseEvent *e, void *user)
{
    BackendWindow *b = user;
    if (type == EMSCRIPTEN_EVENT_MOUSEMOVE)
    {
        Event ev = {.type = EVENT_MOUSE_MOVE};
        ev.data.mouse.x = (int)e->targetX;
        ev.data.mouse.y = (int)e->targetY;
        ev.data.mouse.dx = (int)e->movementX;
        ev.data.mouse.dy = (int)e->movementY;
        enqueue_event(b, &ev);
    }
    else
    {
        /* The browser only grants the pointer lock inside a gesture handler, so a
           captured mouse that lost it (Escape, or asked for before any gesture)
           takes it back on the next press. */
        if (type == EMSCRIPTEN_EVENT_MOUSEDOWN && b->cursor_mode == MOUSE_MODE_CAPTURED)
        {
            EmscriptenPointerlockChangeEvent lock;
            if (emscripten_get_pointerlock_status(&lock) == EMSCRIPTEN_RESULT_SUCCESS && !lock.isActive)
                emscripten_request_pointerlock(CANVAS_TARGET, EM_FALSE);
        }
        int btn = e->button == 0 ? MOUSE_LEFT : e->button == 1 ? MOUSE_MIDDLE
                                            : e->button == 2   ? MOUSE_RIGHT
                                                               : -1;
        if (btn < 0)
            return EM_FALSE;
        Event ev = {.type = EVENT_MOUSE_BUTTON};
        ev.data.mouse.button = btn;
        ev.data.mouse.down = type == EMSCRIPTEN_EVENT_MOUSEDOWN;
        ev.data.mouse.x = (int)e->targetX;
        ev.data.mouse.y = (int)e->targetY;
        enqueue_event(b, &ev);
    }
    return EM_TRUE;
}

static EM_BOOL on_wheel(int type, const EmscriptenWheelEvent *e, void *user)
{
    (void)type;
    BackendWindow *b = user;
    Event ev = {.type = EVENT_MOUSE_WHEEL};
    ev.data.wheel.x = e->deltaX > 0 ? -1.0f : e->deltaX < 0 ? 1.0f
                                                            : 0.0f;
    ev.data.wheel.y = e->deltaY > 0 ? -1.0f : e->deltaY < 0 ? 1.0f
                                                            : 0.0f;
    enqueue_event(b, &ev);
    return EM_TRUE;
}

static EM_BOOL on_focus(int type, const EmscriptenFocusEvent *e, void *user)
{
    (void)e;
    BackendWindow *b = user;
    b->focused = type == EMSCRIPTEN_EVENT_FOCUS;
    Event ev = {.type = EVENT_WINDOW_FOCUS};
    ev.data.focus.gained = b->focused;
    enqueue_event(b, &ev);
    return EM_TRUE;
}

static EM_BOOL on_resize(int type, const EmscriptenUiEvent *e, void *user)
{
    (void)type;
    (void)e;
    BackendWindow *b = user;
    double css_w, css_h;
    emscripten_get_element_css_size(CANVAS_TARGET, &css_w, &css_h);
    double dpr = emscripten_get_device_pixel_ratio();
    b->css_w = (int)css_w;
    b->css_h = (int)css_h;
    emscripten_set_canvas_element_size(CANVAS_TARGET, (int)(css_w * dpr), (int)(css_h * dpr));

    Event r = {.type = EVENT_WINDOW_RESIZE};
    r.data.resize.w = b->css_w;
    r.data.resize.h = b->css_h;
    enqueue_event(b, &r);
    Event fb = {.type = EVENT_WINDOW_FB_RESIZE};
    fb.data.resize.w = (int)(css_w * dpr);
    fb.data.resize.h = (int)(css_h * dpr);
    enqueue_event(b, &fb);
    return EM_TRUE;
}

static EM_BOOL on_touch(int type, const EmscriptenTouchEvent *e, void *user)
{
    BackendWindow *b = user;
    TouchPhase phase = type == EMSCRIPTEN_EVENT_TOUCHSTART ? TOUCH_DOWN
                       : type == EMSCRIPTEN_EVENT_TOUCHMOVE
                           ? TOUCH_MOVE
                       : type == EMSCRIPTEN_EVENT_TOUCHEND ? TOUCH_UP
                                                           : TOUCH_CANCEL;
    for (int i = 0; i < e->numTouches; i++)
    {
        const EmscriptenTouchPoint *t = &e->touches[i];
        if (!t->isChanged)
            continue;
        Event ev = {.type = EVENT_TOUCH};
        ev.data.touch.id = t->identifier;
        ev.data.touch.x = (float)t->targetX;
        ev.data.touch.y = (float)t->targetY;
        ev.data.touch.phase = phase;
        enqueue_event(b, &ev);
    }
    return EM_TRUE;
}

/* ========================================================================== */
/*  file drop and fetch: files land in the MEMFS, then EVENT_WINDOW_DROP      */
/* ========================================================================== */

#define WEB_DROP_MAX 64
/* Each batch of paths stays valid after it is queued: events wait in the queue until the frame drains it, and several
   batches (fetched files finishing in the same frame) can be waiting. A slot is reused only after
   WEB_DROP_BATCHES later batches. */
#define WEB_DROP_BATCHES 256

typedef struct
{
    char *paths[WEB_DROP_MAX];
    int n;
} DropBatch;

static DropBatch g_drop_ring[WEB_DROP_BATCHES];
static int g_drop_next;
static int g_drop_staged;

static void web_drop_clear(DropBatch *d)
{
    for (int i = 0; i < d->n; i++)
        free(d->paths[i]);
    d->n = 0;
}

/* Called from the JS side, one path per file, then the end call. */
EMSCRIPTEN_KEEPALIVE void pf_web_drop_add(char *path)
{
    DropBatch *d = &g_drop_ring[g_drop_next];
    if (g_drop_staged == 0)
        web_drop_clear(d);
    if (g_drop_staged < WEB_DROP_MAX)
        d->paths[g_drop_staged++] = path;
    else
        free(path);
}

EMSCRIPTEN_KEEPALIVE void pf_web_drop_end(void)
{
    DropBatch *d = &g_drop_ring[g_drop_next];
    d->n = g_drop_staged;
    g_drop_staged = 0;
    if (d->n == 0 || !g_active)
        return;
    Event e = {.type = EVENT_WINDOW_DROP};
    e.data.drop.count = d->n;
    e.data.drop.paths = (const char **)d->paths;
    enqueue_event(g_active, &e);
    g_drop_next = (g_drop_next + 1) % WEB_DROP_BATCHES;
}

/* Implemented in web/pf_web.js (a --js-library the platform target adds to the link). */
extern void pf_web_install_drop(void);
extern void pf_web_install_clipboard(void);
extern void pf_web_clipboard_write(char *text);
extern void pf_web_fetch_js(const char *url, const char *path);
extern int pf_web_download_js(const char *path, const char *name);

void platform_web_fetch_file(const char *url, const char *path)
{
    pf_web_fetch_js(url, path);
}

bool platform_web_download(const char *path, const char *name)
{
    if (!path || !name)
        return false;
    return pf_web_download_js(path, name) != 0;
}

/* ========================================================================== */
/*  init / create                                                             */
/* ========================================================================== */

bool backend_init(void)
{
    return true;
}

void backend_shutdown(void)
{
}

/* WebGL2 is GLES 3.0 and WebGL1 is GLES 2.0; nothing else exists in a browser. */
static bool web_gl_version(const GLConfig *gl, int *webgl_major)
{
    int major = gl->major ? gl->major : 3;
    int minor = gl->major ? gl->minor : 0;
    if (gl->profile != GL_PROFILE_DEFAULT && gl->profile != GL_PROFILE_ES)
        return error_set("only OpenGL ES contexts are available on the web");
    if ((major != 2 && major != 3) || minor > 0)
        return error_set("OpenGL ES %d.%d is not available on the web", major, minor);
    *webgl_major = major == 3 ? 2 : 1;
    return true;
}

BackendWindow *backend_create(const WindowConfig *cfg)
{
    BackendWindow *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->render = cfg->render;
    b->css_w = cfg->width > 0 ? cfg->width : 640;
    b->css_h = cfg->height > 0 ? cfg->height : 480;
    /* The page's stylesheet sizes the canvas, and resizes follow it: when no size was asked for, or when the canvas carries
       data-fill-window (the page makes it the whole window, whatever size the config says). */
    const bool page_sized = cfg->width <= 0 || cfg->height <= 0 ||
                            MAIN_THREAD_EM_ASM_INT({ var c = Module['canvas']; return c && c.hasAttribute('data-fill-window') ? 1 : 0; });
    if (page_sized)
    {
        double css_w, css_h;
        emscripten_get_element_css_size(CANVAS_TARGET, &css_w, &css_h);
        if (css_w >= 1.0 && css_h >= 1.0)
        {
            b->css_w = (int)css_w;
            b->css_h = (int)css_h;
        }
    }

    if (cfg->render == RENDER_PIXELS)
    {
        /* A 2D context, sized 1:1 with the canvas, presented via putImageData. */
        if (!page_sized)
            emscripten_set_element_css_size(CANVAS_TARGET, b->css_w, b->css_h);
        emscripten_set_canvas_element_size(CANVAS_TARGET, b->css_w, b->css_h);
        MAIN_THREAD_EM_ASM({ Module._pf_ctx = Module['canvas'].getContext('2d'); });
    }
    else
    {
        int webgl_major;
        if (!web_gl_version(&cfg->gl, &webgl_major))
        {
            free(b);
            return NULL;
        }

        EmscriptenWebGLContextAttributes attrs;
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.majorVersion = webgl_major;
        attrs.minorVersion = 0;
        attrs.antialias = cfg->gl.msaa > 0;
        attrs.alpha = false;
        attrs.depth = true;
        attrs.stencil = true;

        b->gl = emscripten_webgl_create_context(CANVAS_TARGET, &attrs);
        if (b->gl <= 0)
        {
            error_set("cannot create a WebGL %d context", attrs.majorVersion);
            free(b);
            return NULL;
        }
        emscripten_webgl_make_context_current(b->gl);

        double dpr = emscripten_get_device_pixel_ratio();
        if (!page_sized)
            emscripten_set_element_css_size(CANVAS_TARGET, b->css_w, b->css_h);
        emscripten_set_canvas_element_size(CANVAS_TARGET, (int)(b->css_w * dpr), (int)(b->css_h * dpr));
    }
    b->focused = true;
    b->visible = true;
    b->cursor_mode = MOUSE_MODE_NORMAL;

    /* The clipboard listeners go first: the AltGr mark and the focus guard they install must see an event before the
       callbacks below. The key callbacks are called on the browser's main thread, as the event happens, and their answer
       (preventDefault or not) is the browser's: a callback called back on another thread could not give it. */
    pf_web_install_clipboard();
    emscripten_set_keydown_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, b, EM_TRUE, on_key,
                                              EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
    emscripten_set_keyup_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, b, EM_TRUE, on_key,
                                            EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
    emscripten_set_mousemove_callback(CANVAS_TARGET, b, EM_TRUE, on_mouse);
    emscripten_set_mousedown_callback(CANVAS_TARGET, b, EM_TRUE, on_mouse);
    emscripten_set_mouseup_callback(CANVAS_TARGET, b, EM_TRUE, on_mouse);
    emscripten_set_wheel_callback(CANVAS_TARGET, b, EM_TRUE, on_wheel);
    emscripten_set_focus_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, b, EM_TRUE, on_focus);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, b, EM_TRUE, on_focus);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, b, EM_TRUE, on_resize);
    emscripten_set_touchstart_callback(CANVAS_TARGET, b, EM_TRUE, on_touch);
    emscripten_set_touchmove_callback(CANVAS_TARGET, b, EM_TRUE, on_touch);
    emscripten_set_touchend_callback(CANVAS_TARGET, b, EM_TRUE, on_touch);
    emscripten_set_touchcancel_callback(CANVAS_TARGET, b, EM_TRUE, on_touch);

    pf_web_install_drop();
    g_active = b;
    return b;
}

void backend_destroy(BackendWindow *b)
{
    if (!b)
        return;
    if (b->gl > 0)
        emscripten_webgl_destroy_context(b->gl);
    free(b->px);
    web_lock();
    if (g_active == b)
        g_active = NULL;
    g_held_valid = false;
    g_copy_armed = false;
    web_unlock();
    free(b);
}

/* Nothing can be slept on in a browser: the page calls the frame, so a wait is a frame ready to run. */
bool backend_wait_events(int64_t timeout_ns)
{
    (void)timeout_ns;
    return true;
}

void backend_wake(void)
{
}

void backend_pump_events(BackendWindow *b, Core *core)
{
    b->core = core;
    /* what the browser's threads queued, taken under the lock; the core is called without it */
    static WebItem work[WEB_PENDING_MAX]; /* not on the stack: the stack of a wasm module is small (64 KB by default) */
    web_lock();
    const int n = b->pending_count;
    memcpy(work, b->pending, (size_t)n * sizeof work[0]);
    b->pending_count = 0;
    web_unlock();
    for (int i = 0; i < n; i++)
    {
        if (work[i].is_char)
            core_push_char(core, work[i].cp);
        else
            core_push_event(core, &work[i].ev);
    }
}

/* ========================================================================== */
/*  native handles and Vulkan: none on the web                                */
/* ========================================================================== */

void *backend_native_handle(BackendWindow *b, NativeHandleType type)
{
    (void)b;
    (void)type;
    return NULL;
}

const char *const *backend_vulkan_extensions(uint32_t *count)
{
    *count = 0;
    return NULL;
}

bool backend_vulkan_create_surface(BackendWindow *b, void *instance, const void *allocator, uint64_t *out_surface)
{
    (void)b;
    (void)instance;
    (void)allocator;
    (void)out_surface;
    return error_set("Vulkan is not available on the web");
}

void backend_swap(BackendWindow *b)
{
    (void)b; /* the browser presents the canvas after the frame callback returns */
}

/* ========================================================================== */
/*  loop                                                                      */
/* ========================================================================== */

typedef struct
{
    PlatformWindow *w;
    FrameCallback frame;
    void *user;
} LoopCtx;

static LoopCtx g_loop;

/* Each tick is a scope of its own (core_scope_enter): app_run does not come back from the browser's loop, so the scope it
   holds on the desktop would never end here and a window the frame destroyed would never be freed. The window is freed when
   the tick leaves the scope; it is not touched after that, and the loop ends if it asked to close. */
static void main_tick(void)
{
    g_in_tick = true;
    core_scope_enter();
    window_begin_frame(g_loop.w);
    g_loop.frame(g_loop.w, g_loop.user);
    window_swap(g_loop.w);
    bool done = window_should_close(g_loop.w);
    core_scope_leave();
    g_in_tick = false;
    if (done)
        emscripten_cancel_main_loop();
}

static bool web_tick_now(void)
{
    if (g_in_tick || !g_loop.frame || !g_loop.w)
        return false;
    main_tick();
    return true;
}

void backend_run(BackendWindow *b, PlatformWindow *w, FrameCallback frame, void *user)
{
    (void)b;
    g_loop.w = w;
    g_loop.frame = frame;
    g_loop.user = user;
    g_loop_thread = pthread_self();
    /* fps 0 = requestAnimationFrame; simulate_infinite_loop 1 = never returns */
    emscripten_set_main_loop(main_tick, 0, 1);
}

/* ========================================================================== */
/*  GL context                                                                */
/* ========================================================================== */

void backend_make_current(BackendWindow *b)
{
    if (b->render == RENDER_GL)
        emscripten_webgl_make_context_current(b->gl);
}

void backend_make_current_on(BackendWindow *target, BackendWindow *context)
{
    if (target == context)
        backend_make_current(context);
}

void backend_set_vsync(BackendWindow *b, bool on)
{
    (void)b;
    (void)on; /* presentation is tied to requestAnimationFrame; always vsynced */
}

void *backend_gl_proc_address(const char *name)
{
    return emscripten_webgl_get_proc_address(name);
}

BackendGLWorker *backend_gl_worker_create(BackendWindow *b)
{
    (void)b;
    return NULL;
}
bool backend_gl_worker_make_current(BackendGLWorker *worker, bool current)
{
    (void)worker;
    (void)current;
    return false;
}
void backend_gl_worker_destroy(BackendGLWorker *worker)
{
    (void)worker;
}

bool backend_lock_pixels(BackendWindow *b, Framebuffer *out)
{
    if (b->render != RENDER_PIXELS || !out)
        return false;
    if (!b->px || b->px_w != b->css_w || b->px_h != b->css_h)
    {
        free(b->px);
        b->px_w = b->css_w;
        b->px_h = b->css_h;
        b->px = calloc((size_t)b->px_w * b->px_h, sizeof *b->px);
    }
    out->pixels = b->px;
    out->width = b->px_w;
    out->height = b->px_h;
    out->stride = b->px_w;
    return b->px != NULL;
}

void backend_present_pixels(BackendWindow *b)
{
    if (b->render != RENDER_PIXELS || !b->px)
        return;
    /* Swizzle 0xAARRGGBB -> RGBA bytes and blit through the 2D context. */
    MAIN_THREAD_EM_ASM(
        {
            var ctx = Module._pf_ctx;
            if (!ctx)
                return;
            var w = $1;
            var h = $2;
            var img = ctx.createImageData(w, h);
            var d = img.data;
            var src = $0 >> 2;
            for (var i = 0; i < w * h; i++)
            {
                var p = HEAPU32[src + i];
                d[i * 4 + 0] = (p >> 16) & 0xff;
                d[i * 4 + 1] = (p >> 8) & 0xff;
                d[i * 4 + 2] = p & 0xff;
                d[i * 4 + 3] = (p >> 24) & 0xff;
            }
            ctx.putImageData(img, 0, 0);
        },
        b->px, b->px_w, b->px_h);
}

/* ========================================================================== */
/*  geometry                                                                  */
/* ========================================================================== */

void backend_get_size(BackendWindow *b, int *w, int *h)
{
    if (w)
        *w = b->css_w;
    if (h)
        *h = b->css_h;
}

void backend_set_size(BackendWindow *b, int w, int h)
{
    b->css_w = w;
    b->css_h = h;
    double dpr = emscripten_get_device_pixel_ratio();
    emscripten_set_element_css_size(CANVAS_TARGET, w, h);
    emscripten_set_canvas_element_size(CANVAS_TARGET, (int)(w * dpr), (int)(h * dpr));
}

void backend_get_fb_size(BackendWindow *b, int *w, int *h)
{
    (void)b;
    emscripten_get_canvas_element_size(CANVAS_TARGET, w, h);
}

void backend_get_pos(BackendWindow *b, int *x, int *y)
{
    (void)b;
    if (x)
        *x = 0;
    if (y)
        *y = 0;
}

void backend_set_pos(BackendWindow *b, int x, int y)
{
    (void)b;
    (void)x;
    (void)y; /* a canvas has no window position */
}

float backend_content_scale(BackendWindow *b)
{
    (void)b;
    return (float)emscripten_get_device_pixel_ratio();
}

void backend_set_title(BackendWindow *b, const char *title)
{
    (void)b;
    if (title)
        emscripten_set_window_title(title);
}

void backend_set_size_limits(BackendWindow *b, int minw, int minh, int maxw, int maxh)
{
    (void)b;
    (void)minw;
    (void)minh;
    (void)maxw;
    (void)maxh;
}

/* ========================================================================== */
/*  window state (mostly inert on a canvas)                                   */
/* ========================================================================== */

void backend_minimize(BackendWindow *b)
{
    (void)b;
}
void backend_maximize(BackendWindow *b)
{
    (void)b;
}
void backend_restore(BackendWindow *b)
{
    (void)b;
}
void backend_show(BackendWindow *b)
{
    b->visible = true;
}
void backend_hide(BackendWindow *b)
{
    b->visible = false;
}
void backend_focus(BackendWindow *b)
{
    (void)b;
}
void backend_request_attention(BackendWindow *b)
{
    (void)b;
}

bool backend_get_flag(BackendWindow *b, int flag)
{
    switch (flag)
    {
    case WIN_FLAG_FOCUSED:
        return b->focused;
    case WIN_FLAG_VISIBLE:
        return b->visible;
    default:
        return false;
    }
}

void backend_set_mode(BackendWindow *b, WindowMode mode, int monitor)
{
    (void)monitor;
    if (mode == WINDOW_WINDOWED)
        emscripten_exit_fullscreen();
    else
        emscripten_request_fullscreen(CANVAS_TARGET, EM_TRUE);
}

WindowMode backend_get_mode(BackendWindow *b)
{
    EmscriptenFullscreenChangeEvent fs;
    if (emscripten_get_fullscreen_status(&fs) == EMSCRIPTEN_RESULT_SUCCESS && fs.isFullscreen)
        return WINDOW_FULLSCREEN;
    (void)b;
    return WINDOW_WINDOWED;
}

void backend_set_icon(BackendWindow *b, int w, int h, const uint8_t *rgba)
{
    (void)b;
    (void)w;
    (void)h;
    (void)rgba;
}
void backend_set_opacity(BackendWindow *b, float a)
{
    (void)b;
    (void)a;
}
void backend_set_always_on_top(BackendWindow *b, bool on)
{
    (void)b;
    (void)on;
}

/* ========================================================================== */
/*  mouse                                                                     */
/* ========================================================================== */

void backend_set_mouse_pos(BackendWindow *b, int x, int y)
{
    (void)b;
    (void)x;
    (void)y; /* the browser does not allow warping the pointer */
}

void backend_set_cursor(BackendWindow *b, int cursor)
{
    (void)b;
    const char *css;
    switch (cursor)
    {
    case CURSOR_IBEAM:
        css = "text";
        break;
    case CURSOR_CROSSHAIR:
        css = "crosshair";
        break;
    case CURSOR_HAND:
        css = "pointer";
        break;
    case CURSOR_RESIZE_EW:
        css = "ew-resize";
        break;
    case CURSOR_RESIZE_NS:
        css = "ns-resize";
        break;
    case CURSOR_RESIZE_NWSE:
        css = "nwse-resize";
        break;
    case CURSOR_RESIZE_NESW:
        css = "nesw-resize";
        break;
    case CURSOR_RESIZE_ALL:
        css = "move";
        break;
    case CURSOR_NOT_ALLOWED:
        css = "not-allowed";
        break;
    default:
        css = "default";
        break;
    }
    MAIN_THREAD_EM_ASM({ if (Module['canvas']) Module['canvas'].style.cursor = UTF8ToString($0); }, css);
}

void backend_set_live_callback(BackendWindow *b, PlatformWindow *w, FrameCallback cb, void *user)
{
    (void)b, (void)w, (void)cb, (void)user;
}

int backend_lock_state(void)
{
    return 0;
}

bool backend_mouse_capture(BackendWindow *b, bool on)
{
    (void)b, (void)on;
    return false;
}

void backend_set_decorated(BackendWindow *b, bool on)
{
    (void)b, (void)on;
}

void backend_set_hit_test(BackendWindow *b, PlatformWindow *w, HitTestFunc fn, void *user)
{
    (void)b, (void)w, (void)fn, (void)user;
}

PlatformCursor *backend_cursor_create(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    (void)argb, (void)w, (void)h, (void)hot_x, (void)hot_y;
    error_set("cursors from an image are not supported here");
    return NULL;
}

void backend_cursor_destroy(PlatformCursor *c)
{
    (void)c;
}

void backend_set_cursor_image(BackendWindow *b, PlatformCursor *c)
{
    (void)b, (void)c;
}

void backend_set_text_input(BackendWindow *b, bool on)
{
    (void)b, (void)on;
}

void backend_set_text_input_rect(BackendWindow *b, int x, int y, int w, int h)
{
    (void)b, (void)x, (void)y, (void)w, (void)h;
}

void backend_set_mouse_mode(BackendWindow *b, int mode)
{
    b->cursor_mode = mode;
    if (mode == MOUSE_MODE_CAPTURED)
        emscripten_request_pointerlock(CANVAS_TARGET, EM_TRUE);
    else
        emscripten_exit_pointerlock();
    MAIN_THREAD_EM_ASM({ if (Module['canvas']) Module['canvas'].style.cursor = ($0 == 0) ? 'default' : 'none'; }, mode == MOUSE_MODE_NORMAL ? 0 : 1);
}

/* ========================================================================== */
/*  clipboard                                                                 */
/* ========================================================================== */

/* There is no primary selection here (the middle button pastes nothing): the calls say so (clipboard_has_primary_support). */
bool backend_primary_supported(void)
{
    return false;
}

bool backend_primary_set(const char *utf8, size_t len)
{
    (void)utf8, (void)len;
    return false;
}

void *backend_primary_get(size_t *size)
{
    if (size)
        *size = 0;
    return NULL;
}

/* The browser's clipboard cannot be read synchronously from wasm (the async API asks permission, and does not exist over
   plain http), so what the application reads is what it put there, or what a paste event brought (pf_web_paste, above: the
   text of the browser's clipboard, handed over inside the event). Text writes are mirrored out to the browser's clipboard
   (web/pf_web.js: the async API when there is one, else a hidden textarea and execCommand("copy")); a copy or cut key hands
   the text over inside the event (pf_web_copy_text). */
bool backend_clipboard_set(const ClipboardItem *items, int count)
{
    bool has_text = false;
    for (int i = 0; i < count; i++)
        has_text |= clip_mime_equal(items[i].mime, CLIPBOARD_TEXT);
    char *copy = NULL;
    web_lock();
    bool ok = clipmem_set(items, count);
    if (ok)
    {
        g_clip_seq++;
        const ClipMemItem *t = has_text ? clipmem_find(CLIPBOARD_TEXT) : NULL; /* NUL-terminated copy */
        if (t)
            copy = strdup((const char *)t->data);
    }
    web_unlock();
    if (copy)
        pf_web_clipboard_write(copy); /* the JS side takes the string over */
    return ok;
}
bool backend_clipboard_has(const char *mime)
{
    web_lock();
    bool has = clipmem_has(mime);
    web_unlock();
    return has;
}
void *backend_clipboard_get(const char *mime, size_t *size)
{
    web_lock();
    void *data = clipmem_get(mime, size);
    web_unlock();
    return data;
}

/* ========================================================================== */
/*  monitors (one screen)                                                     */
/* ========================================================================== */

bool backend_mouse_global_position(int *x, int *y)
{
    (void)x;
    (void)y;
    return false;
}

int backend_monitor_count(void)
{
    return 1;
}

bool backend_monitor_info(int index, MonitorInfo *out)
{
    if (index != 0 || !out)
        return false;
    int sw = 0, sh = 0;
    emscripten_get_screen_size(&sw, &sh);
    memset(out, 0, sizeof *out);
    out->index = 0;
    out->name = "screen";
    out->width = sw;
    out->height = sh;
    out->work_w = sw;
    out->work_h = sh;
    out->refresh_hz = 60;
    out->content_scale = (float)emscripten_get_device_pixel_ratio();
    out->primary = true;
    return true;
}
