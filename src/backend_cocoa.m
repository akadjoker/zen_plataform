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
 * Phase markers: what is not here yet is a stub that says so, marked TODO(mac phase N). 3 = OpenGL and software
 * present, 4 = keyboard and mouse, 5 = text input, 6 = clipboard and file drop, 7 = fullscreen, icon, opacity,
 * always on top, undecorated windows and hit test, 8 = Vulkan (Metal) surface.
 */
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h> /* CGSessionCopyCurrentDictionary, CGDisplay* */

#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "backend.h"
#include "error_internal.h"

#define PENDING_MAX 256
#define MAX_MONITORS 16
#define MONITOR_NAME_CAP 64
#define LIVE_INTERVAL 0.016 /* seconds between live callbacks while AppKit runs its own loop */

@class ZenAppDelegate;
@class ZenView;

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
} g;

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
    push_move(owner);
    live_tick(owner);
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
    float scale = (float)[owner->window backingScaleFactor];
    if (scale == owner->scale)
        return;
    owner->scale = scale;
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
    Event e = {.type = EVENT_WINDOW_FOCUS};
    e.data.focus.gained = true;
    push(owner, &e);
}

- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    if (!owner)
        return;
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

@interface ZenView : NSView
{
    BackendWindow *owner;
    NSTrackingArea *area;
}
- (id)initWithFrame:(NSRect)frame owner:(BackendWindow *)b;
- (void)detach;
@end

@implementation ZenView

- (id)initWithFrame:(NSRect)frame owner:(BackendWindow *)b
{
    self = [super initWithFrame:frame];
    if (self)
        owner = b;
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

/* TODO(mac phase 4): keyboard and mouse events. Until then a key that reaches the window is taken and dropped, so
   AppKit does not answer each one with the system beep. */
- (void)keyDown:(NSEvent *)event
{
    (void)event;
}

- (void)keyUp:(NSEvent *)event
{
    (void)event;
}

- (void)flagsChanged:(NSEvent *)event
{
    (void)event;
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
       brought up again, and a later backend_init finds them as they were. */
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
    /* TODO(mac phase 3): OpenGL contexts and the software present; a window with nothing to draw with is refused,
       not handed out as if it worked */
    if (cfg->render == RENDER_GL)
    {
        error_set("RENDER_GL windows are not implemented on macOS yet (OpenGL is not available)");
        return NULL;
    }
    if (cfg->render == RENDER_PIXELS)
    {
        error_set("RENDER_PIXELS windows are not implemented on macOS yet (the pixel surface cannot be shown)");
        return NULL;
    }

    BackendWindow *b = calloc(1, sizeof *b);
    if (!b)
    {
        error_set("out of memory");
        return NULL;
    }
    b->render = cfg->render;

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
            free(b);
            error_set("cannot create the window");
            return NULL;
        }
        /* the window is ours to release, and a restored-from-last-run window or a tab would only get in the way */
        [b->window setReleasedWhenClosed:NO];
        [b->window setRestorable:NO];
        [b->window setTabbingMode:NSWindowTabbingModeDisallowed];

        b->view = [[ZenView alloc] initWithFrame:NSMakeRect(0, 0, w, h) owner:b];
        b->delegate = [[ZenWindowDelegate alloc] initWithOwner:b];
        [b->window setContentView:b->view];
        [b->window setDelegate:b->delegate];
        [b->window makeFirstResponder:b->view];
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
        [b->window orderOut:nil];
        [b->window close];
        [b->window setContentView:nil];
        [b->view release];
        [b->delegate release];
        [b->window release];
    }
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
        if (cb)
            live_timer_start(b);
        else
            live_timer_stop(b);
    }
}

/* ========================================================================== */
/*  geometry                                                                  */
/* ========================================================================== */

void backend_get_size(BackendWindow *b, int *w, int *h)
{
    NSRect c = content_rect(b);
    if (w)
        *w = round_int(c.size.width);
    if (h)
        *h = round_int(c.size.height);
}

void backend_get_fb_size(BackendWindow *b, int *w, int *h)
{
    int fw, fh;
    fb_size(b, &fw, &fh);
    if (w)
        *w = fw;
    if (h)
        *h = fh;
}

void backend_get_pos(BackendWindow *b, int *x, int *y)
{
    int px, py;
    top_left(content_rect(b), &px, &py);
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
    /* the top left corner stays where it is: setContentSize would keep the bottom left */
    NSRect c = content_rect(b);
    w = clamp_limit(w, b->min_w, b->max_w);
    h = clamp_limit(h, b->min_h, b->max_h);
    NSRect want = NSMakeRect(c.origin.x, c.origin.y + c.size.height - h, w, h);
    [b->window setFrame:[b->window frameRectForContentRect:want] display:YES];
}

void backend_set_pos(BackendWindow *b, int x, int y)
{
    NSRect c = content_rect(b);
    NSRect want = NSMakeRect(x, primary_height() - y - c.size.height, c.size.width, c.size.height);
    [b->window setFrameOrigin:[b->window frameRectForContentRect:want].origin];
}

float backend_content_scale(BackendWindow *b)
{
    return (float)[b->window backingScaleFactor];
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
    [b->window setContentMinSize:NSMakeSize(minw > 0 ? minw : 0, minh > 0 ? minh : 0)];
    [b->window setContentMaxSize:NSMakeSize(maxw > 0 ? maxw : FLT_MAX, maxh > 0 ? maxh : FLT_MAX)];
}

/* ========================================================================== */
/*  window state                                                              */
/* ========================================================================== */

void backend_minimize(BackendWindow *b)
{
    [b->window miniaturize:nil];
}

void backend_maximize(BackendWindow *b)
{
    if (![b->window isZoomed])
        [b->window zoom:nil]; /* zoom toggles: only when it is not zoomed already */
}

void backend_restore(BackendWindow *b)
{
    if ([b->window isMiniaturized])
        [b->window deminiaturize:nil];
    else if ([b->window isZoomed])
        [b->window zoom:nil];
}

void backend_show(BackendWindow *b)
{
    [b->window makeKeyAndOrderFront:nil];
}

void backend_hide(BackendWindow *b)
{
    [b->window orderOut:nil];
}

void backend_focus(BackendWindow *b)
{
    /* a window that is not on screen cannot take the focus (as on X11, asking is not an error and does nothing) */
    if (![b->window isVisible])
        return;
    [NSApp activateIgnoringOtherApps:YES];
    [b->window makeKeyAndOrderFront:nil];
}

void backend_request_attention(BackendWindow *b)
{
    (void)b;
    [NSApp requestUserAttention:NSInformationalRequest]; /* the Dock icon bounces once, if the application is not active */
}

bool backend_get_flag(BackendWindow *b, int flag)
{
    switch (flag)
    {
    case WIN_FLAG_FOCUSED:
        return [b->window isKeyWindow] ? true : false;
    case WIN_FLAG_MINIMIZED:
        return [b->window isMiniaturized] ? true : false;
    case WIN_FLAG_MAXIMIZED:
        return [b->window isZoomed] ? true : false;
    case WIN_FLAG_VISIBLE:
        /* a window in the Dock is still shown as far as the program is concerned, as on Win32 */
        return [b->window isVisible] || [b->window isMiniaturized];
    case WIN_FLAG_HOVERED:
        return b->hovered;
    default:
        return false;
    }
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
        return NULL; /* TODO(mac phase 3): the NSOpenGLContext */
    }
    return NULL;
}

/* ========================================================================== */
/*  stubs: what the later phases bring. Each one does nothing, says no (false, NULL, 0) and, where the core has an      */
/*  error to set, says why.                                                                                          */
/* ========================================================================== */

static bool not_yet(const char *what)
{
    return error_set("%s is not implemented on macOS yet", what);
}

/* ---- phase 3: OpenGL and the pixel surface ---- */

void backend_swap(BackendWindow *b)
{
    (void)b; /* TODO(mac phase 3) */
}
void backend_make_current(BackendWindow *b)
{
    (void)b; /* TODO(mac phase 3) */
    not_yet("window_make_current");
}
void backend_make_current_on(BackendWindow *target, BackendWindow *context)
{
    (void)target; /* TODO(mac phase 3) */
    (void)context;
    not_yet("window_make_current_on");
}
void backend_set_vsync(BackendWindow *b, bool on)
{
    (void)b; /* TODO(mac phase 3) */
    (void)on;
}
void *backend_gl_proc_address(const char *name)
{
    (void)name; /* TODO(mac phase 3) */
    return NULL;
}
BackendGLWorker *backend_gl_worker_create(BackendWindow *b)
{
    (void)b; /* TODO(mac phase 3) */
    not_yet("a shared OpenGL worker context");
    return NULL;
}
bool backend_gl_worker_make_current(BackendGLWorker *worker, bool current)
{
    (void)worker; /* TODO(mac phase 3) */
    (void)current;
    return false;
}
void backend_gl_worker_destroy(BackendGLWorker *worker)
{
    (void)worker; /* TODO(mac phase 3) */
}
bool backend_lock_pixels(BackendWindow *b, Framebuffer *out)
{
    (void)b; /* TODO(mac phase 3) */
    (void)out;
    return false;
}
void backend_present_pixels(BackendWindow *b)
{
    (void)b; /* TODO(mac phase 3) */
}

/* ---- phase 4: keyboard and mouse ---- */

int backend_lock_state(void)
{
    return 0; /* TODO(mac phase 4): Caps Lock; macOS has no Num Lock */
}
bool backend_mouse_capture(BackendWindow *b, bool on)
{
    (void)b; /* TODO(mac phase 4) */
    (void)on;
    return false;
}
PlatformCursor *backend_cursor_create(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    (void)argb; /* TODO(mac phase 4) */
    (void)w;
    (void)h;
    (void)hot_x;
    (void)hot_y;
    not_yet("cursor images");
    return NULL;
}
void backend_cursor_destroy(PlatformCursor *c)
{
    (void)c; /* TODO(mac phase 4): no cursor is ever made, so there is none to free */
}
void backend_set_cursor_image(BackendWindow *b, PlatformCursor *c)
{
    (void)b; /* TODO(mac phase 4) */
    (void)c;
}
void backend_set_mouse_pos(BackendWindow *b, int x, int y)
{
    (void)b; /* TODO(mac phase 4) */
    (void)x;
    (void)y;
    not_yet("mouse_set_position");
}
void backend_set_cursor(BackendWindow *b, int cursor)
{
    (void)b; /* TODO(mac phase 4) */
    (void)cursor;
}
void backend_set_mouse_mode(BackendWindow *b, int mode)
{
    (void)b; /* TODO(mac phase 4) */
    (void)mode;
    if (mode != MOUSE_MODE_NORMAL)
        not_yet("the hidden and captured mouse modes");
}

/* ---- phase 5: text input ---- */

void backend_set_text_input(BackendWindow *b, bool on)
{
    (void)b; /* TODO(mac phase 5) */
    (void)on;
}
void backend_set_text_input_rect(BackendWindow *b, int x, int y, int w, int h)
{
    (void)b; /* TODO(mac phase 5) */
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

/* ---- phase 6: clipboard (and file drop, which has no function here: EVENT_WINDOW_DROP is never raised yet) ---- */

bool backend_clipboard_set(const ClipboardItem *items, int count)
{
    (void)items; /* TODO(mac phase 6) */
    (void)count;
    return not_yet("the clipboard");
}
bool backend_clipboard_has(const char *mime)
{
    (void)mime; /* TODO(mac phase 6) */
    not_yet("the clipboard");
    return false;
}
void *backend_clipboard_get(const char *mime, size_t *size)
{
    (void)mime; /* TODO(mac phase 6) */
    (void)size;
    not_yet("the clipboard");
    return NULL;
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
    return not_yet("Vulkan surfaces");
}
