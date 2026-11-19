/*
 * web_key_rules.h - what the web backend decides about a browser key event, apart from Emscripten so that a test on the host
 * can run it (tests/test_web_keys.c). Header-only and static, like clipboard_mem.h.
 *
 * Two decisions:
 *  - Is it a clipboard command (paste, copy, cut)? Then the browser must be left to do it: no preventDefault on the key, or
 *    the browser's own paste, copy and cut events never fire (they are the only way a page can read the clipboard of a
 *    page opened over plain http, and the only place a page can put text on it everywhere).
 *  - Does the key type text? Not with Ctrl or Meta held (the rule in platform.h).
 */
#ifndef WEB_KEY_RULES_H
#define WEB_KEY_RULES_H

#include <stdbool.h>
#include <string.h>

typedef enum
{
    WEBKEY_NONE,
    WEBKEY_PASTE,
    WEBKEY_COPY,
    WEBKEY_CUT
} WebClipCommand;

/* The letter of a shortcut: the character the key makes when it is an ASCII letter (a Dvorak keyboard's Ctrl+C is the key
   that makes a "c"), else, on a layout that has no Latin letters (Cyrillic, Greek, Hebrew: the browser takes the key of a US
   keyboard for its own shortcuts), the physical key. 0 when it is neither. */
static char webkey_shortcut_letter(const char *key, const char *code)
{
    if (key && key[0] && !key[1])
    {
        char c = key[0];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c >= 'a' && c <= 'z')
            return c;
        if ((unsigned char)c < 0x80)
            return 0; /* a digit, a symbol: not a letter shortcut */
    }
    if (code && strncmp(code, "Key", 3) == 0 && code[3] && !code[4])
        return (char)(code[3] >= 'A' && code[3] <= 'Z' ? code[3] - 'A' + 'a' : code[3]);
    return 0;
}

/* The clipboard commands of a keyboard: Ctrl+V, Ctrl+C and Ctrl+X (Meta on a Mac: Cmd), and the older Shift+Insert,
   Ctrl+Insert and Shift+Delete. Alt makes it something else (Ctrl+Alt is AltGr on Windows layouts: the character it types). */
static WebClipCommand webkey_clip_command(const char *key, const char *code, bool ctrl, bool shift, bool alt, bool meta)
{
    if (alt)
        return WEBKEY_NONE;
    if (ctrl || meta)
    {
        switch (webkey_shortcut_letter(key, code))
        {
        case 'v':
            return WEBKEY_PASTE;
        case 'c':
            return WEBKEY_COPY;
        case 'x':
            return WEBKEY_CUT;
        default:
            break;
        }
    }
    if (code && !ctrl && !meta && shift && strcmp(code, "Insert") == 0)
        return WEBKEY_PASTE;
    if (code && ctrl && !meta && !shift && strcmp(code, "Insert") == 0)
        return WEBKEY_COPY;
    if (code && !ctrl && !meta && shift && strcmp(code, "Delete") == 0)
        return WEBKEY_CUT;
    return WEBKEY_NONE;
}

/* Does a key press type its character? Not with Meta, and not with Ctrl, which makes it a shortcut, except AltGr: Windows
   layouts report it as Ctrl and Alt together, and the browser says it is AltGr with getModifierState("AltGraph"), so the
   character is text. (A Ctrl+Alt that is not AltGr is Ctrl+Alt+letter on a layout that has no AltGr: a shortcut.) */
static bool webkey_types_text(bool ctrl, bool alt, bool meta, bool altgr)
{
    if (meta)
        return false;
    if (ctrl)
        return alt && altgr;
    return true;
}

#endif /* WEB_KEY_RULES_H */
