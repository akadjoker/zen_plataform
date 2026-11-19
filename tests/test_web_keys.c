/*
 * test_web_keys.c - the web backend's decisions about a browser key event (src/web_key_rules.h, which has nothing of
 * Emscripten in it): which keys are the browser's clipboard commands (they must not be prevented, or the browser's paste,
 * copy and cut events never fire), and which keys type text (not with Ctrl or Meta, except AltGr). The same rules, run in a
 * real browser, are in tests/web/.
 */
#include "web_key_rules.h"

#include <stdio.h>

static int g_pass, g_fail;

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
            g_pass++;                                              \
        else                                                       \
        {                                                          \
            g_fail++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

#define CMD(key, code, ctrl, shift, alt, meta) webkey_clip_command(key, code, ctrl, shift, alt, meta)

static void test_commands(void)
{
    /* Ctrl or Meta with V, C, X, in either case (Shift makes it a capital) */
    CHECK(CMD("v", "KeyV", true, false, false, false) == WEBKEY_PASTE);
    CHECK(CMD("V", "KeyV", true, true, false, false) == WEBKEY_PASTE);
    CHECK(CMD("v", "KeyV", false, false, false, true) == WEBKEY_PASTE);
    CHECK(CMD("c", "KeyC", true, false, false, false) == WEBKEY_COPY);
    CHECK(CMD("c", "KeyC", false, false, false, true) == WEBKEY_COPY);
    CHECK(CMD("x", "KeyX", true, false, false, false) == WEBKEY_CUT);
    CHECK(CMD("X", "KeyX", false, true, false, true) == WEBKEY_CUT);
    /* without the modifier it is a letter */
    CHECK(CMD("v", "KeyV", false, false, false, false) == WEBKEY_NONE);
    CHECK(CMD("V", "KeyV", false, true, false, false) == WEBKEY_NONE);
    /* other letters, digits and symbols are shortcuts the browser does not need to run */
    CHECK(CMD("a", "KeyA", true, false, false, false) == WEBKEY_NONE);
    CHECK(CMD("1", "Digit1", true, false, false, false) == WEBKEY_NONE);
    CHECK(CMD("-", "Minus", true, false, false, false) == WEBKEY_NONE);
    /* Alt makes it another thing: AltGr (Ctrl and Alt on a Windows layout) types a character */
    CHECK(CMD("v", "KeyV", true, false, true, false) == WEBKEY_NONE);
    CHECK(CMD("@", "KeyQ", true, false, true, false) == WEBKEY_NONE);
    /* the keyboard follows the character, not the key: Dvorak has its "c" on the physical J */
    CHECK(CMD("c", "KeyJ", true, false, false, false) == WEBKEY_COPY);
    CHECK(CMD("j", "KeyC", true, false, false, false) == WEBKEY_NONE);
    /* a layout without Latin letters (Cyrillic): the browser takes the key of a US keyboard */
    CHECK(CMD("\xD1\x81", "KeyC", true, false, false, false) == WEBKEY_COPY);   /* es */
    CHECK(CMD("\xD0\xBC", "KeyV", true, false, false, false) == WEBKEY_PASTE);  /* em */
    CHECK(CMD("\xD1\x87", "KeyX", true, false, false, false) == WEBKEY_CUT);    /* che */
    CHECK(CMD("\xD1\x84", "KeyA", true, false, false, false) == WEBKEY_NONE);
    /* a modifier on its own, a named key, a key that is not there */
    CHECK(CMD("Control", "ControlLeft", true, false, false, false) == WEBKEY_NONE);
    CHECK(CMD("Enter", "Enter", true, false, false, false) == WEBKEY_NONE);
    CHECK(CMD("", "", true, false, false, false) == WEBKEY_NONE);
    CHECK(CMD(NULL, NULL, true, false, false, false) == WEBKEY_NONE);
}

static void test_insert_delete(void)
{
    /* the clipboard keys of the old desktops */
    CHECK(CMD("Insert", "Insert", false, true, false, false) == WEBKEY_PASTE);  /* Shift+Insert */
    CHECK(CMD("Insert", "Insert", true, false, false, false) == WEBKEY_COPY);   /* Ctrl+Insert */
    CHECK(CMD("Delete", "Delete", false, true, false, false) == WEBKEY_CUT);    /* Shift+Delete */
    CHECK(CMD("Insert", "Insert", false, false, false, false) == WEBKEY_NONE);  /* Insert alone toggles overwrite */
    CHECK(CMD("Delete", "Delete", false, false, false, false) == WEBKEY_NONE);
    CHECK(CMD("Delete", "Delete", true, false, false, false) == WEBKEY_NONE);   /* Ctrl+Delete deletes a word */
    CHECK(CMD("Insert", "Insert", true, true, false, false) == WEBKEY_NONE);
    CHECK(CMD("Insert", "Insert", false, true, true, false) == WEBKEY_NONE);
    CHECK(CMD("Insert", "Insert", false, true, false, true) == WEBKEY_NONE);
}

static void test_text(void)
{
    /* webkey_types_text(ctrl, alt, meta, altgr) */
    CHECK(webkey_types_text(false, false, false, false));  /* a */
    CHECK(webkey_types_text(false, false, false, false));  /* Shift+a: Shift is not in the rule */
    CHECK(!webkey_types_text(true, false, false, false));  /* Ctrl+a: a shortcut, not an "a" */
    CHECK(!webkey_types_text(false, false, true, false));  /* Meta+a */
    CHECK(!webkey_types_text(true, false, true, false));
    CHECK(webkey_types_text(false, true, false, false));   /* Alt+a, or AltGr on Linux and the Mac, where it is not Ctrl */
    CHECK(webkey_types_text(true, true, false, true));     /* AltGr on a Windows layout: Ctrl and Alt, and the browser says AltGraph */
    CHECK(!webkey_types_text(true, true, false, false));   /* Ctrl+Alt+a on a layout with no AltGr: a shortcut */
    CHECK(!webkey_types_text(true, true, true, true));     /* with Meta it is not AltGr */
    CHECK(!webkey_types_text(true, false, false, true));   /* AltGraph without Alt: a browser that reports it oddly */
}

int main(void)
{
    test_commands();
    test_insert_delete();
    test_text();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
