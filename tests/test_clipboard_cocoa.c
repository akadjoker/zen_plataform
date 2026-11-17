/*
 * test_clipboard_cocoa.c - the clipboard on macOS (phase 6): the general pasteboard through the library's API, and the
 * pasteboard read and written with AppKit directly (through the Objective-C runtime) as the oracle, so that what the library
 * writes is what other programs find and what they put there is what the library reads. Text (UTF-8, empty, with unusual
 * characters), PNG (an image round trip), text/uri-list (file URLs), a custom MIME type (bytes with NULs), several
 * representations of one content, clearing, replacing, and the NUL the library puts after what it returns.
 *
 * WRITTEN, NEVER COMPILED OR RUN: made when no run of the macOS job was allowed (see docs/MAC-PORT.md); checked only for
 * syntax against stand-in headers. Skips (77) without a graphical session, unless ZP_REQUIRE_DISPLAY is set.
 */
#include "cocoa_test_util.h"

static id general(void)
{
    return msg_id(ns_class("NSPasteboard"), "generalPasteboard");
}

/* what AppKit says the pasteboard holds, for a type named as AppKit names it */
static char *pb_string(const char *type)
{
    id s = msg_id_id(general(), "stringForType:", ns_string(type));
    const char *u = s ? msg_cstr(s, "UTF8String") : NULL;
    return u ? strdup(u) : NULL;
}

static unsigned long pb_data_length(const char *type)
{
    id d = msg_id_id(general(), "dataForType:", ns_string(type));
    return d ? msg_ulong(d, "length") : (unsigned long)-1;
}

static void test_text(void)
{
    clipboard_set("hello clipboard");
    CHECK(strcmp(clipboard_get(), "hello clipboard") == 0);
    char *oracle = pb_string("public.utf8-plain-text");
    CHECK(oracle && strcmp(oracle, "hello clipboard") == 0);
    free(oracle);
    CHECK(clipboard_has_data(CLIPBOARD_TEXT));

    /* characters outside ASCII, a character outside the BMP, a newline */
    const char *odd = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80\nsecond line";
    clipboard_set(odd);
    CHECK(strcmp(clipboard_get(), odd) == 0);
    oracle = pb_string("public.utf8-plain-text");
    CHECK(oracle && strcmp(oracle, odd) == 0);
    free(oracle);

    /* what another program puts there is read */
    msg_void(general(), "clearContents");
    MSG(unsigned char, id, SEL, id, id)(general(), sel_registerName("setString:forType:"), ns_string("from outside"), ns_string("public.utf8-plain-text"));
    CHECK(strcmp(clipboard_get(), "from outside") == 0);

    /* cleared: no text, and clipboard_get is "" and never NULL */
    clipboard_set(NULL);
    CHECK(!clipboard_has_data(CLIPBOARD_TEXT));
    CHECK(clipboard_get()[0] == '\0');
    clipboard_set("");
    CHECK(clipboard_get()[0] == '\0');

    /* a long text */
    char *big = malloc(300001);
    for (int i = 0; i < 300000; i++)
        big[i] = (char)('a' + i % 26);
    big[300000] = '\0';
    clipboard_set(big);
    CHECK(strcmp(clipboard_get(), big) == 0);
    free(big);
}

static void test_data(void)
{
    /* the NUL after the data is not counted in its size */
    CHECK(clipboard_set_data(CLIPBOARD_TEXT, "abc", 3));
    size_t n = 99;
    char *d = clipboard_get_data(CLIPBOARD_TEXT, &n);
    CHECK(d && n == 3 && memcmp(d, "abc", 4) == 0);
    fs_free(d);

    /* a custom type: bytes as they are, NULs included */
    const unsigned char blob[] = {0, 1, 2, 0, 255, 254, 0, 7};
    CHECK(clipboard_set_data("application/x-zen-test", blob, sizeof blob));
    CHECK(clipboard_has_data("application/x-zen-test"));
    CHECK(!clipboard_has_data("application/x-zen-other"));
    d = clipboard_get_data("application/x-zen-test", &n);
    CHECK(d && n == sizeof blob && memcmp(d, blob, sizeof blob) == 0 && d[sizeof blob] == '\0');
    fs_free(d);
    CHECK(pb_data_length("application/x-zen-test") == sizeof blob);
    CHECK(clipboard_get_data("application/x-zen-other", &n) == NULL);

    /* several representations of one content, set together, each read by its own type */
    const ClipboardItem items[] = {{CLIPBOARD_TEXT, "both", 4}, {"application/x-zen-test", blob, sizeof blob}};
    CHECK(clipboard_set_items(items, 2));
    CHECK(clipboard_has_data(CLIPBOARD_TEXT) && clipboard_has_data("application/x-zen-test"));
    CHECK(strcmp(clipboard_get(), "both") == 0);
    d = clipboard_get_data("application/x-zen-test", &n);
    CHECK(d && n == sizeof blob);
    fs_free(d);

    /* setting again replaces, and count 0 clears everything */
    CHECK(clipboard_set_data(CLIPBOARD_TEXT, "only text", 9));
    CHECK(!clipboard_has_data("application/x-zen-test"));
    CHECK(clipboard_set_items(NULL, 0));
    CHECK(!clipboard_has_data(CLIPBOARD_TEXT));
}

static void test_image(void)
{
    Framebuffer fb;
    CHECK(framebuffer_alloc(&fb, 8, 6));
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 8; x++)
            fb.pixels[y * fb.stride + x] = 0xFF000000u | (uint32_t)(x * 30) << 16 | (uint32_t)(y * 40) << 8 | 0x55;
    CHECK(clipboard_set_image(&fb));
    CHECK(clipboard_has_data(CLIPBOARD_PNG));
    CHECK(pb_data_length("public.png") > 8); /* AppKit sees a PNG of its own name */
    Framebuffer back;
    CHECK(clipboard_get_image(&back));
    CHECK(back.width == 8 && back.height == 6);
    CHECK(back.width == 8 && back.pixels[3 * back.stride + 5] == fb.pixels[3 * fb.stride + 5]);
    framebuffer_free(&back);
    framebuffer_free(&fb);
    clipboard_set(NULL);
    CHECK(!clipboard_has_data(CLIPBOARD_PNG));
}

static void test_uris(void)
{
    const char *list = "file:///tmp/zen%20test/a.txt\nfile:///tmp/b.txt";
    CHECK(clipboard_set_data(CLIPBOARD_URIS, list, strlen(list)));
    CHECK(clipboard_has_data(CLIPBOARD_URIS));
    size_t n = 0;
    char *got = clipboard_get_data(CLIPBOARD_URIS, &n);
    CHECK(got && strcmp(got, list) == 0 && n == strlen(list));
    fs_free(got);
    /* the pasteboard has two file URLs, as the Finder would write them */
    id urls = MSG(id, id, SEL, id, id)(general(), sel_registerName("readObjectsForClasses:options:"),
                                       msg_id_id(ns_class("NSArray"), "arrayWithObject:", ns_class("NSURL")), NULL);
    CHECK(urls && msg_ulong(urls, "count") == 2);
    /* comments and empty lines of a list are not files */
    const char *with_comment = "# a list\nfile:///tmp/c.txt\n\n";
    CHECK(clipboard_set_data(CLIPBOARD_URIS, with_comment, strlen(with_comment)));
    got = clipboard_get_data(CLIPBOARD_URIS, &n);
    CHECK(got && strcmp(got, "file:///tmp/c.txt") == 0);
    fs_free(got);
    clipboard_set(NULL);
    CHECK(!clipboard_has_data(CLIPBOARD_URIS));
}

int main(void)
{
    alarm(60);
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    printf("text\n");
    test_text();
    printf("data and types\n");
    test_data();
    printf("image\n");
    test_image();
    printf("uri list\n");
    test_uris();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
