/*
 * clipboard_web - the web backend's clipboard and text keys, in a page. One line of text is edited: typed characters are
 * appended, Backspace removes one, Ctrl+A (or Meta+A) selects it all, Ctrl+V / Meta+V / Shift+Insert paste the clipboard,
 * Ctrl+C / Meta+C / Ctrl+Insert copy the line when it is selected, Ctrl+X / Meta+X / Shift+Delete cut it, and F2 puts the
 * line on the clipboard whatever is selected (a "Copy" button: clipboard_set outside of a copy event). What the program saw
 * is kept in the page for a script to read (tests/web/run_clip_web.js): window.zpState = {doc, clip, frames, log}, where the
 * log is one string per event ("K:<key>:<mods>:<down|up>" for a key, "C:<codepoint>" for text, "paste", "copy", "cut").
 * Build with emcmake (examples/CMakeLists.txt) and the shell file tests/web/shell.html.
 */
#include "platform.h"

#include <emscripten/emscripten.h>
#ifdef CLIPBOARD_WEB_GL
#include <GLES3/gl3.h> /* -DCLIPBOARD_WEB_GL: a WebGL window, for a build with the program on a worker (PROXY_TO_PTHREAD) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOC_MAX 4096
#define LOG_MAX 96

static char g_doc[DOC_MAX];
static bool g_selected;
static int g_frames;

/* To the page, from the program's thread, without waiting for the page: in a build with the program on a worker the page is on
   another thread, and a call that waits for it (MAIN_THREAD_EM_ASM) would stop the frame while the browser's thread waits for
   this frame (the copy event, see src/backend_web.c). The strings are copied and the page frees them. */
static void log_event(const char *what)
{
    MAIN_THREAD_ASYNC_EM_ASM({
        var z = window.zpState || (window.zpState = {log : []});
        z.log.push(UTF8ToString($0));
        _free($0);
    }, strdup(what));
}

static void publish(void)
{
    MAIN_THREAD_ASYNC_EM_ASM({
        var z = window.zpState || (window.zpState = {log : []});
        z.doc = UTF8ToString($0);
        z.clip = UTF8ToString($1);
        z.frames = $2;
        z.selected = $3 != 0;
        _free($0);
        _free($1);
    }, strdup(g_doc), strdup(clipboard_get()), g_frames, g_selected);
}

static const char *key_name(int k)
{
    static char other[16];
    switch (k)
    {
    case KEY_A: return "A";
    case KEY_C: return "C";
    case KEY_V: return "V";
    case KEY_X: return "X";
    case KEY_Q: return "Q";
    case KEY_BACKSPACE: return "Backspace";
    case KEY_INSERT: return "Insert";
    case KEY_DELETE: return "Delete";
    case KEY_F2: return "F2";
    case KEY_LEFT_CONTROL: return "LCtrl";
    case KEY_LEFT_SHIFT: return "LShift";
    case KEY_LEFT_ALT: return "LAlt";
    case KEY_LEFT_SUPER: return "LSuper";
    default: break;
    }
    snprintf(other, sizeof other, "key%d", k);
    return other;
}

static const char *mods_name(int m)
{
    static char buf[32];
    snprintf(buf, sizeof buf, "%s%s%s%s", (m & KEYMOD_CTRL) ? "ctrl+" : "", (m & KEYMOD_SHIFT) ? "shift+" : "",
             (m & KEYMOD_ALT) ? "alt+" : "", (m & KEYMOD_SUPER) ? "meta+" : "");
    return buf;
}

static void append_utf8(uint32_t cp)
{
    size_t n = strlen(g_doc);
    if (n + 5 >= DOC_MAX)
        return;
    if (cp < 0x80)
        g_doc[n++] = (char)cp;
    else if (cp < 0x800)
    {
        g_doc[n++] = (char)(0xC0 | (cp >> 6));
        g_doc[n++] = (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        g_doc[n++] = (char)(0xE0 | (cp >> 12));
        g_doc[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        g_doc[n++] = (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        g_doc[n++] = (char)(0xF0 | (cp >> 18));
        g_doc[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        g_doc[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        g_doc[n++] = (char)(0x80 | (cp & 0x3F));
    }
    g_doc[n] = '\0';
}

static void frame(PlatformWindow *w, void *user)
{
    (void)user;
    Event e;
    while (poll_event(w, &e))
    {
        if (e.type == EVENT_CHAR)
        {
            char line[32];
            snprintf(line, sizeof line, "C:%u", (unsigned)e.data.codepoint);
            log_event(line);
            if (g_selected)
                g_doc[0] = '\0'; /* typing replaces the selection */
            g_selected = false;
            append_utf8(e.data.codepoint);
        }
        else if (e.type == EVENT_KEY)
        {
            char line[48];
            snprintf(line, sizeof line, "K:%s:%s:%s", key_name(e.data.key.key), mods_name(e.data.key.mods),
                     e.data.key.down ? "down" : "up");
            log_event(line);
            if (!e.data.key.down)
                continue;
            const int m = e.data.key.mods;
            const int k = e.data.key.key;
            const bool cmd = (m & (KEYMOD_CTRL | KEYMOD_SUPER)) != 0;
            if (k == KEY_BACKSPACE)
            {
                size_t n = strlen(g_doc);
                while (n > 0 && ((unsigned char)g_doc[n - 1] & 0xC0) == 0x80)
                    n--;
                if (n > 0)
                    n--;
                g_doc[n] = '\0';
                g_selected = false;
            }
            else if (cmd && k == KEY_A)
                g_selected = true;
            else if (k == KEY_F2)
            {
                log_event("button copy");
                clipboard_set(g_doc[0] ? g_doc : "from F2");
            }
            else if ((cmd && k == KEY_V) || ((m & KEYMOD_SHIFT) && k == KEY_INSERT))
            {
                log_event("paste");
                if (g_selected)
                    g_doc[0] = '\0';
                g_selected = false;
                const char *t = clipboard_get();
                if (strlen(g_doc) + strlen(t) + 1 < DOC_MAX)
                    strcat(g_doc, t);
            }
            else if (g_selected && ((cmd && (k == KEY_C || k == KEY_X)) || (cmd && k == KEY_INSERT) ||
                                    ((m & KEYMOD_SHIFT) && k == KEY_DELETE)))
            {
                const bool cut = k == KEY_X || k == KEY_DELETE;
                log_event(cut ? "cut" : "copy");
                clipboard_set(g_doc);
                if (cut)
                {
                    g_doc[0] = '\0';
                    g_selected = false;
                }
            }
        }
    }
    g_frames++;
#ifdef CLIPBOARD_WEB_GL
    glClearColor(0.1f, 0.12f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
#endif
    publish();
}

int main(void)
{
    if (!platform_init())
        return 1;
#ifdef CLIPBOARD_WEB_GL
    WindowConfig cfg = {.title = "clipboard_web", .width = 320, .height = 200, .gl = {.major = 3, .minor = 0}};
#else
    WindowConfig cfg = {.title = "clipboard_web", .width = 320, .height = 200, .render = RENDER_PIXELS};
#endif
    PlatformWindow *w = window_create(&cfg);
    if (!w)
        return 1;
#ifdef CLIPBOARD_WEB_GL
    window_make_current(w);
#endif
    window_text_input_start(w);
    app_run(w, frame, NULL);
    return 0;
}
