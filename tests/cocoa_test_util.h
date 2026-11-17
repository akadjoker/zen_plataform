/*
 * cocoa_test_util.h - what the macOS tests share: the check macros, the frame pump, AppKit read (and driven) through the
 * Objective-C runtime's C interface, events made for the application's own queue, and the window's content read back as an
 * image. Plain C: the tests go through the public API only, and AppKit is the oracle, never the way in.
 *
 * Events are made with NSEvent's own factories and either posted to the application's queue (what the backend's pump drains,
 * so they travel the real path: NSApplication, the window, the first responder) or handed straight to the view's method
 * (which needs no focus and no routing). Neither needs permission, as events posted to the system (CGEventPost) do.
 */
#ifndef COCOA_TEST_UTIL_H
#define COCOA_TEST_UTIL_H

/* The Mach headers some Apple headers include declare a thread_create of their own, which platform.h's conflicts with when
   both are in one file. The tests that use the library's threads include platform.h first, without this. */
#ifndef COCOA_TEST_USES_THREADS
#define thread_create zen_platform_thread_create
#endif
#include "platform.h"
#ifndef COCOA_TEST_USES_THREADS
#undef thread_create
#endif

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <malloc/malloc.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UNUSED_FN __attribute__((unused)) static

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

#define NOTE(...)            \
    do                       \
    {                        \
        printf("note: ");    \
        printf(__VA_ARGS__); \
        printf("\n");        \
    } while (0)

/* ========================================================================== */
/*  the Objective-C runtime as a way to read AppKit                           */
/* ========================================================================== */

/* objc_msgSend has no prototype of its own (it is called through the type of the method), so each call casts it to that type. */
#define MSG(ret, ...) ((ret(*)(__VA_ARGS__))(void *)objc_msgSend)

UNUSED_FN id ns_class(const char *name)
{
    return (id)objc_getClass(name);
}
UNUSED_FN id msg_id(id o, const char *sel)
{
    return MSG(id, id, SEL)(o, sel_registerName(sel));
}
UNUSED_FN id msg_id_id(id o, const char *sel, id a)
{
    return MSG(id, id, SEL, id)(o, sel_registerName(sel), a);
}
UNUSED_FN id msg_id_ulong(id o, const char *sel, unsigned long a)
{
    return MSG(id, id, SEL, unsigned long)(o, sel_registerName(sel), a);
}
UNUSED_FN id msg_id_cstr(id o, const char *sel, const char *s)
{
    return MSG(id, id, SEL, const char *)(o, sel_registerName(sel), s);
}
UNUSED_FN void msg_void(id o, const char *sel)
{
    MSG(void, id, SEL)(o, sel_registerName(sel));
}
UNUSED_FN void msg_void_id(id o, const char *sel, id a)
{
    MSG(void, id, SEL, id)(o, sel_registerName(sel), a);
}
UNUSED_FN void msg_void_bool(id o, const char *sel, bool a)
{
    MSG(void, id, SEL, unsigned char)(o, sel_registerName(sel), a);
}
UNUSED_FN void msg_void_id_bool(id o, const char *sel, id a, bool b)
{
    MSG(void, id, SEL, id, unsigned char)(o, sel_registerName(sel), a, b);
}
UNUSED_FN bool msg_bool(id o, const char *sel)
{
    return MSG(unsigned char, id, SEL)(o, sel_registerName(sel)) != 0;
}
UNUSED_FN unsigned long msg_ulong(id o, const char *sel)
{
    return MSG(unsigned long, id, SEL)(o, sel_registerName(sel));
}
UNUSED_FN long msg_long(id o, const char *sel)
{
    return MSG(long, id, SEL)(o, sel_registerName(sel));
}
UNUSED_FN double msg_double(id o, const char *sel)
{
    return MSG(double, id, SEL)(o, sel_registerName(sel));
}
UNUSED_FN const char *msg_cstr(id o, const char *sel)
{
    return MSG(const char *, id, SEL)(o, sel_registerName(sel));
}
UNUSED_FN CGPoint msg_point(id o, const char *sel)
{
    return MSG(CGPoint, id, SEL)(o, sel_registerName(sel));
}
/* A rectangle is returned in registers on arm64 and through memory on x86-64, which has its own entry point for that. */
UNUSED_FN CGRect msg_rect(id o, const char *sel)
{
#if defined(__x86_64__)
    CGRect r;
    ((void (*)(CGRect *, id, SEL))(void *)objc_msgSend_stret)(&r, o, sel_registerName(sel));
    return r;
#else
    return MSG(CGRect, id, SEL)(o, sel_registerName(sel));
#endif
}

UNUSED_FN id ns_string(const char *utf8)
{
    return msg_id_cstr(ns_class("NSString"), "stringWithUTF8String:", utf8);
}
UNUSED_FN id ns_app(void)
{
    return msg_id(ns_class("NSApplication"), "sharedApplication");
}
UNUSED_FN id ns_window(PlatformWindow *w)
{
    return (id)window_native_handle(w, NATIVE_WINDOW);
}
UNUSED_FN id ns_view(PlatformWindow *w)
{
    return msg_id(ns_window(w), "contentView");
}

/* ========================================================================== */
/*  pumping                                                                   */
/* ========================================================================== */

#define ZP_REQUIRE getenv("ZP_REQUIRE_DISPLAY")

UNUSED_FN void pump_for(PlatformWindow *w, int ms)
{
    double t0 = time_seconds();
    do
    {
        window_begin_frame(w);
        time_sleep(4);
    } while (time_seconds() - t0 < ms / 1000.0);
}

/* The events of the frames pumped since the last reset, in order, for a test to look through. */
#define SEEN_MAX 1024
static Event g_seen[SEEN_MAX];
static int g_seen_count;

UNUSED_FN void seen_reset(void)
{
    g_seen_count = 0;
}

/* Pumps one frame and keeps what it brought. */
UNUSED_FN void pump_frame(PlatformWindow *w)
{
    window_begin_frame(w);
    Event e;
    while (poll_event(w, &e))
        if (g_seen_count < SEEN_MAX)
            g_seen[g_seen_count++] = e;
}

UNUSED_FN void pump_frames(PlatformWindow *w, int n)
{
    for (int i = 0; i < n; i++)
    {
        pump_frame(w);
        time_sleep(2);
    }
}

UNUSED_FN int seen_count(EventType type)
{
    int n = 0;
    for (int i = 0; i < g_seen_count; i++)
        n += g_seen[i].type == type;
    return n;
}

/* The i-th event of a type, or NULL. */
UNUSED_FN const Event *seen_nth(EventType type, int i)
{
    for (int k = 0; k < g_seen_count; k++)
        if (g_seen[k].type == type && i-- == 0)
            return &g_seen[k];
    return NULL;
}

/* ========================================================================== */
/*  events for the application's own queue                                    */
/* ========================================================================== */

/* NSEventType, NSEventModifierFlags, written here because a plain C file has no AppKit headers. */
enum
{
    NS_LEFT_DOWN = 1,
    NS_LEFT_UP = 2,
    NS_RIGHT_DOWN = 3,
    NS_RIGHT_UP = 4,
    NS_MOVED = 5,
    NS_LEFT_DRAGGED = 6,
    NS_RIGHT_DRAGGED = 7,
    NS_KEY_DOWN = 10,
    NS_KEY_UP = 11,
    NS_FLAGS_CHANGED = 12,
    NS_SCROLL = 22,
    NS_OTHER_DOWN = 25,
    NS_OTHER_UP = 26,
    NS_OTHER_DRAGGED = 27
};
#define NS_FLAG_CAPS (1UL << 16)
#define NS_FLAG_SHIFT (1UL << 17)
#define NS_FLAG_CTRL (1UL << 18)
#define NS_FLAG_OPT (1UL << 19)
#define NS_FLAG_CMD (1UL << 20)

UNUSED_FN id event_key(PlatformWindow *w, unsigned long type, unsigned short code, unsigned long flags, const char *chars, bool repeat)
{
    id s = ns_string(chars ? chars : "");
    return MSG(id, id, SEL, unsigned long, CGPoint, unsigned long, double, long, id, id, id, unsigned char, unsigned short)(
        ns_class("NSEvent"),
        sel_registerName("keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:characters:"
                         "charactersIgnoringModifiers:isARepeat:keyCode:"),
        type, (CGPoint){0, 0}, flags, time_seconds(), msg_long(ns_window(w), "windowNumber"), NULL, s, s, repeat, code);
}

/* A mouse event of the window, at x,y in the library's coordinates (points, top left origin, y down). */
UNUSED_FN id event_mouse(PlatformWindow *w, unsigned long type, int x, int y, unsigned long flags, long clicks)
{
    int cw, ch;
    window_get_size(w, &cw, &ch);
    return MSG(id, id, SEL, unsigned long, CGPoint, unsigned long, double, long, id, long, long, float)(
        ns_class("NSEvent"),
        sel_registerName("mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:pressure:"),
        type, (CGPoint){x, ch - y}, flags, time_seconds(), msg_long(ns_window(w), "windowNumber"), NULL, 0, clicks, 1.0f);
}

/* Events made from CoreGraphics ones, for what the factories above cannot say: the motion of a move, the number of an extra
   button, the scroll's deltas and units. They are never sent to the system, only turned into NSEvents. */
UNUSED_FN id event_from_cg(CGEventRef cg)
{
    if (!cg)
        return NULL;
    return msg_id_id(ns_class("NSEvent"), "eventWithCGEvent:", (id)cg);
}

UNUSED_FN id event_scroll(bool precise, int dx, int dy)
{
    CGEventRef cg = CGEventCreateScrollWheelEvent(NULL, precise ? kCGScrollEventUnitPixel : kCGScrollEventUnitLine, 2, dy, dx);
    id e = event_from_cg(cg);
    if (cg)
        CFRelease(cg);
    return e;
}

UNUSED_FN id event_other_button(bool down, int button, int x, int y)
{
    CGEventRef cg = CGEventCreateMouseEvent(NULL, down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp, CGPointMake(x, y), kCGMouseButtonCenter);
    if (cg)
        CGEventSetIntegerValueField(cg, kCGMouseEventButtonNumber, button);
    id e = event_from_cg(cg);
    if (cg)
        CFRelease(cg);
    return e;
}

UNUSED_FN id event_moved_by(int dx, int dy, bool drag)
{
    CGEventRef cg = CGEventCreateMouseEvent(NULL, drag ? kCGEventLeftMouseDragged : kCGEventMouseMoved, CGPointMake(10, 10), kCGMouseButtonLeft);
    if (cg)
    {
        CGEventSetIntegerValueField(cg, kCGMouseEventDeltaX, dx);
        CGEventSetIntegerValueField(cg, kCGMouseEventDeltaY, dy);
    }
    id e = event_from_cg(cg);
    if (cg)
        CFRelease(cg);
    return e;
}

/* Posted: the application's queue gets it and the next frame's pump sends it on, through NSApplication and the window. */
UNUSED_FN void post(id event)
{
    if (event)
        msg_void_id_bool(ns_app(), "postEvent:atStart:", event, false);
}

/* Handed straight to the view: its handler runs, with no routing and no need of focus. */
UNUSED_FN void to_view(PlatformWindow *w, const char *selector, id event)
{
    if (event)
        msg_void_id(ns_view(w), selector, event);
}

/* ========================================================================== */
/*  the window's content as an image                                          */
/* ========================================================================== */

typedef struct
{
    int w, h;
    uint32_t *px; /* 0xAARRGGBB, w * h, row 0 at the top; malloc'd */
} Image;

UNUSED_FN void image_free(Image *im)
{
    free(im->px);
    memset(im, 0, sizeof *im);
}

/* A CGImage into an Image of exactly its size in pixels: drawn into an sRGB bitmap, 0xAARRGGBB, not premultiplied away. */
UNUSED_FN bool image_from_cg(CGImageRef cg, Image *out)
{
    memset(out, 0, sizeof *out);
    if (!cg)
        return false;
    int w = (int)CGImageGetWidth(cg), h = (int)CGImageGetHeight(cg);
    if (w <= 0 || h <= 0)
        return false;
    uint32_t *px = calloc((size_t)w * (size_t)h, 4);
    if (!px)
        return false;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(px, (size_t)w, (size_t)h, 8, (size_t)w * 4, space,
                                             (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(space);
    if (!ctx)
    {
        free(px);
        return false;
    }
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    CGContextRelease(ctx);
    out->w = w;
    out->h = h;
    out->px = px;
    return true;
}

/* The window as the window server composites it, through CGWindowListCreateImage (looked up at run time: the call is
   deprecated, and gone from the headers of the newest SDKs). A process may read its own windows without the Screen Recording
   permission. Only the window's content is kept: the image has the title bar on top, so the content is the bottom
   `fb_h` rows. false when the call is not there or gives nothing. */
UNUSED_FN bool capture_window(PlatformWindow *w, Image *out)
{
    typedef CGImageRef (*CreateFn)(CGRect, uint32_t, uint32_t, uint32_t);
    static CreateFn create;
    static bool looked;
    if (!looked)
    {
        looked = true;
        *(void **)&create = dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    }
    memset(out, 0, sizeof *out);
    if (!create)
        return false;
    uint32_t number = (uint32_t)msg_long(ns_window(w), "windowNumber");
    /* kCGWindowListOptionIncludingWindow = 1 << 3, kCGWindowImageBoundsIgnoreFraming = 1 << 0 */
    CGImageRef cg = create(CGRectNull, 1u << 3, number, 1u << 0);
    Image whole;
    bool ok = image_from_cg(cg, &whole);
    if (cg)
        CGImageRelease(cg);
    if (!ok)
        return false;
    int fw, fh;
    window_get_framebuffer_size(w, &fw, &fh);
    if (whole.w < fw || whole.h < fh)
    {
        image_free(&whole);
        return false;
    }
    out->w = fw;
    out->h = fh;
    out->px = malloc((size_t)fw * (size_t)fh * 4);
    if (!out->px)
    {
        image_free(&whole);
        return false;
    }
    int top = whole.h - fh; /* the title bar */
    for (int y = 0; y < fh; y++)
        memcpy(out->px + (size_t)y * (size_t)fw, whole.px + (size_t)(top + y) * (size_t)whole.w, (size_t)fw * 4);
    image_free(&whole);
    return true;
}

/* The view drawn through its own drawRect into a bitmap, at the display's resolution: what the software surface paints, read
   back without the window server. It does not see an OpenGL surface. */
UNUSED_FN bool capture_view(PlatformWindow *w, Image *out)
{
    memset(out, 0, sizeof *out);
    id view = ns_view(w);
    CGRect bounds = msg_rect(view, "bounds");
    id rep = MSG(id, id, SEL, CGRect)(view, sel_registerName("bitmapImageRepForCachingDisplayInRect:"), bounds);
    if (!rep)
        return false;
    MSG(void, id, SEL, CGRect, id)(view, sel_registerName("cacheDisplayInRect:toBitmapImageRep:"), bounds, rep);
    CGImageRef cg = (CGImageRef)msg_id(rep, "CGImage");
    return image_from_cg(cg, out);
}

/* Writes an Image as a PNG, through ImageIO. */
UNUSED_FN bool image_save_png(const Image *im, const char *path)
{
    if (!im->px)
        return false;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreateWithData(im->px, (size_t)im->w, (size_t)im->h, 8, (size_t)im->w * 4, space,
                                                     (CGBitmapInfo)kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, NULL, NULL);
    CGColorSpaceRelease(space);
    if (!ctx)
        return false;
    CGImageRef image = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!image)
        return false;
    CFStringRef cfpath = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, cfpath, kCFURLPOSIXPathStyle, false);
    typedef void *(*DestFn)(CFURLRef, CFStringRef, size_t, CFDictionaryRef);
    typedef void (*AddFn)(void *, CGImageRef, CFDictionaryRef);
    typedef bool (*FinFn)(void *);
    DestFn dest_create = (DestFn)dlsym(RTLD_DEFAULT, "CGImageDestinationCreateWithURL");
    AddFn dest_add = (AddFn)dlsym(RTLD_DEFAULT, "CGImageDestinationAddImage");
    FinFn dest_finish = (FinFn)dlsym(RTLD_DEFAULT, "CGImageDestinationFinalize");
    bool ok = false;
    if (dest_create && dest_add && dest_finish)
    {
        void *dest = dest_create(url, CFSTR("public.png"), 1, NULL);
        if (dest)
        {
            dest_add(dest, image, NULL);
            ok = dest_finish(dest);
            CFRelease((CFTypeRef)dest);
        }
    }
    CFRelease(url);
    CFRelease(cfpath);
    CGImageRelease(image);
    return ok;
}

/* The folder for what a test writes for a person to look at: ZP_TEST_OUT, else the current one. */
UNUSED_FN void out_path(char *buf, size_t cap, const char *name)
{
    const char *dir = getenv("ZP_TEST_OUT");
    snprintf(buf, cap, "%s%s%s", dir ? dir : "", dir ? "/" : "", name);
}

/* The colour of a pixel, channel by channel (0xAARRGGBB), as the largest difference of any channel. */
UNUSED_FN int color_distance(uint32_t a, uint32_t b)
{
    int d = 0;
    for (int shift = 0; shift < 24; shift += 8)
    {
        int x = (int)((a >> shift) & 0xFF) - (int)((b >> shift) & 0xFF);
        if (x < 0)
            x = -x;
        if (x > d)
            d = x;
    }
    return d;
}

/* How many pixels of two images of one size differ by more than `tolerance` in a channel (alpha is not compared). */
UNUSED_FN long image_diff(const Image *a, const Image *b, int tolerance)
{
    if (a->w != b->w || a->h != b->h)
        return (long)a->w * a->h + 1;
    long n = 0;
    for (long i = 0; i < (long)a->w * a->h; i++)
        n += color_distance(a->px[i] | 0xFF000000u, b->px[i] | 0xFF000000u) > tolerance;
    return n;
}

/* ========================================================================== */
/*  the heap                                                                  */
/* ========================================================================== */

/* Under AddressSanitizer freed memory waits in a quarantine that malloc_zone_statistics counts as in use, so the heap says
   nothing about leaks there; the windows AppKit still holds are counted either way. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ZP_ASAN 1
#endif
#endif

UNUSED_FN size_t heap_in_use(unsigned *blocks)
{
    malloc_statistics_t st;
    memset(&st, 0, sizeof st);
    malloc_zone_statistics(NULL, &st);
    if (blocks)
        *blocks = st.blocks_in_use;
    return st.size_in_use;
}

/* The windows of this process that are on the window server's list (layer 0), as the window test counts them. */
UNUSED_FN int server_window_count(void)
{
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list)
        return 0;
    int n = 0;
    pid_t me = getpid();
    for (CFIndex i = 0; i < CFArrayGetCount(list); i++)
    {
        CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
        int pid = 0, layer = -1;
        CFNumberRef num = CFDictionaryGetValue(d, kCGWindowOwnerPID);
        if (num)
            CFNumberGetValue(num, kCFNumberIntType, &pid);
        num = CFDictionaryGetValue(d, kCGWindowLayer);
        if (num)
            CFNumberGetValue(num, kCFNumberIntType, &layer);
        n += pid == me && layer == 0;
    }
    CFRelease(list);
    return n;
}

#endif /* COCOA_TEST_UTIL_H */
