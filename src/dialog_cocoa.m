/*
 * dialog_cocoa.m - open_url, the message boxes and the file dialogs on macOS, with the system's own: NSAlert, NSOpenPanel,
 * NSSavePanel and NSWorkspace. The public API is the one of dialog.c (which does the same with zenity and kdialog on Linux and
 * with the system's dialogs on Windows); when the native backend is built, dialog.c leaves these to this file (ZEN_DIALOG_COCOA).
 *
 * Manual retain/release (-fno-objc-arc, see CMakeLists.txt), and an autorelease pool around everything that reaches AppKit.
 * Every dialog is modal for the whole application and blocks until the user answers (runModal), which is what the API says; it
 * has to be used from the main thread, where AppKit lives. `parent` only decides where the dialog comes from: a modal panel is
 * the application's, not a sheet of one window.
 *
 * WRITTEN, NEVER COMPILED OR RUN: made when no run of the macOS job was allowed (see docs/MAC-PORT.md).
 */
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h> /* CGSessionCopyCurrentDictionary */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "error_internal.h"

static bool bad_url(const char *url)
{
    if (!url || !url[0] || url[0] == '-')
        return true;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p < 0x20 || *p == 0x7F)
            return true;
    return false;
}

static bool on_main_thread(void)
{
    return pthread_main_np() != 0;
}

/* A C string as an NSString (an empty one for NULL and for what is not UTF-8). */
static NSString *ns(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

/* The application has to exist, and be in front, for its dialog to be seen. A dialog may come before platform_init (a program
   that tells the user why it could not start): the application is made a regular one here too (backend_init does the same, and
   asking twice changes nothing), because a program that is not in an application bundle is a background process until it says
   otherwise, and the dialog of a background process does not come to the front. NSApplication ends the process when there is no
   window server to talk to, so that is asked first and is an error. */
static bool bring_up(void)
{
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (!session)
        return error_set("no graphical session: the program has no connection to the window server");
    CFRelease(session);
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    return true;
}

/* ---- message boxes ---- */

bool message_box(PlatformWindow *parent, MessageKind kind, const char *title, const char *message)
{
    (void)parent;
    platform_clear_error();
    if (!on_main_thread())
        return error_set("the dialogs of macOS must be used from the main thread");
    @autoreleasepool
    {
        if (!bring_up())
            return false;
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:ns(title)];
        [alert setInformativeText:ns(message)];
        [alert setAlertStyle:kind == MESSAGE_ERROR ? NSAlertStyleCritical : kind == MESSAGE_WARNING ? NSAlertStyleWarning : NSAlertStyleInformational];
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
        [alert release];
    }
    return true;
}

/* 1 for Yes, 0 for No (and for a dialog that was closed or aborted), -1 when it could not be shown. */
int confirm_box(PlatformWindow *parent, const char *title, const char *message)
{
    (void)parent;
    platform_clear_error();
    if (!on_main_thread())
    {
        error_set("the dialogs of macOS must be used from the main thread");
        return -1;
    }
    int answer;
    @autoreleasepool
    {
        if (!bring_up())
            return -1;
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:ns(title)];
        [alert setInformativeText:ns(message)];
        [alert setAlertStyle:NSAlertStyleWarning];
        [alert addButtonWithTitle:@"Yes"];
        [alert addButtonWithTitle:@"No"];
        [[[alert buttons] objectAtIndex:1] setKeyEquivalent:@"\033"]; /* Escape is No */
        answer = [alert runModal] == NSAlertFirstButtonReturn ? 1 : 0;
        [alert release];
    }
    return answer;
}

/* ---- file dialogs ---- */

/* The path of a file URL in UTF-8 as the URL says it (the name as the file system reports it), not -fileSystemRepresentation, which
   is the decomposed (NFD) form of it: see file_url_path in backend_cocoa.m, which a dropped file gets the same way. */
static const char *file_url_path(NSURL *url)
{
    NSURL *path_url = [url filePathURL];
    NSString *path = [(path_url ? path_url : url) path];
    return path ? [path UTF8String] : NULL;
}

/* The extensions the filters allow: "*.png;*.jpg" is png and jpg. The panel has no list of filters to choose from, so the
   filters are one list of all their extensions; a filter that allows every file ("*" or "*.*") leaves the panel unrestricted. */
static NSArray *allowed_extensions(const FileFilter *filters, int count)
{
    if (!filters || count <= 0)
        return nil;
    NSMutableArray *exts = [NSMutableArray array];
    for (int i = 0; i < count; i++)
    {
        const char *p = filters[i].patterns ? filters[i].patterns : "";
        while (*p)
        {
            while (*p == ';' || *p == ' ')
                p++;
            const char *end = p;
            while (*end && *end != ';' && *end != ' ')
                end++;
            size_t n = (size_t)(end - p);
            if ((n == 1 && p[0] == '*') || (n == 3 && strncmp(p, "*.*", 3) == 0))
                return nil;
            if (n > 2 && p[0] == '*' && p[1] == '.')
            {
                NSString *ext = [[NSString alloc] initWithBytes:p + 2 length:n - 2 encoding:NSUTF8StringEncoding];
                if (ext)
                    [exts addObject:ext];
                [ext release];
            }
            p = end;
        }
    }
    return [exts count] ? exts : nil;
}

/* A folder to start at, or a file: its folder, and (for saving) its name. */
static void start_at(NSSavePanel *panel, const char *path, bool save)
{
    if (!path || !path[0])
        return;
    NSString *p = ns(path);
    BOOL is_dir = NO;
    if ([[NSFileManager defaultManager] fileExistsAtPath:p isDirectory:&is_dir] && is_dir)
    {
        [panel setDirectoryURL:[NSURL fileURLWithPath:p isDirectory:YES]];
        return;
    }
    NSString *dir = [p stringByDeletingLastPathComponent];
    if ([dir length])
        [panel setDirectoryURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
    if (save)
        [panel setNameFieldStringValue:[p lastPathComponent]];
}

typedef enum
{
    PANEL_OPEN,
    PANEL_SAVE,
    PANEL_FOLDER,
    PANEL_OPEN_MANY
} PanelKind;

/* The panel's answer as the paths the caller gets, one per line in `out`. The count of paths, 0 when it was cancelled or
   could not be shown (the error is set only for the second). */
static int run_panel(PanelKind kind, const char *title, const char *default_path, const FileFilter *filters, int filter_count,
                     char *out, size_t cap)
{
    platform_clear_error();
    if (!on_main_thread())
    {
        error_set("the dialogs of macOS must be used from the main thread");
        return 0;
    }
    int count = 0;
    @autoreleasepool
    {
        if (!bring_up())
            return 0;
        NSSavePanel *panel = kind == PANEL_SAVE ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
        if (title && title[0])
        {
            [panel setTitle:ns(title)];
            [panel setMessage:ns(title)];
        }
        if (kind != PANEL_SAVE)
        {
            NSOpenPanel *open = (NSOpenPanel *)panel;
            [open setCanChooseFiles:kind != PANEL_FOLDER];
            [open setCanChooseDirectories:kind == PANEL_FOLDER];
            [open setAllowsMultipleSelection:kind == PANEL_OPEN_MANY];
        }
        if (kind != PANEL_FOLDER)
        {
            NSArray *exts = allowed_extensions(filters, filter_count);
            if (exts)
            {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" /* allowedFileTypes: deprecated for allowedContentTypes (macOS 12) */
                [panel setAllowedFileTypes:exts];
#pragma clang diagnostic pop
            }
        }
        start_at(panel, default_path, kind == PANEL_SAVE);

        if ([panel runModal] == NSModalResponseOK)
        {
            NSArray *urls = kind == PANEL_SAVE ? ([panel URL] ? [NSArray arrayWithObject:[panel URL]] : [NSArray array])
                                               : [(NSOpenPanel *)panel URLs];
            size_t used = 0;
            bool fits = out && cap > 0;
            for (NSUInteger i = 0; i < [urls count] && fits; i++)
            {
                const char *path = file_url_path([urls objectAtIndex:i]);
                size_t n = path ? strlen(path) : 0;
                if (used + n + 1 > cap)
                {
                    fits = false;
                    break;
                }
                memcpy(out + used, path, n);
                used += n;
                if (i + 1 < [urls count] && used + 1 < cap)
                    out[used++] = '\n';
                out[used] = '\0';
                count++;
            }
            if (!fits)
            {
                error_set("the path does not fit in the buffer");
                count = 0;
            }
        }
    }
    return count;
}

bool dialog_open_file(PlatformWindow *parent, const char *title, const char *default_path, const FileFilter *filters, int filter_count,
                      char *out, size_t cap)
{
    (void)parent;
    return run_panel(PANEL_OPEN, title, default_path, filters, filter_count, out, cap) == 1;
}

bool dialog_save_file(PlatformWindow *parent, const char *title, const char *default_path, const FileFilter *filters, int filter_count,
                      char *out, size_t cap)
{
    (void)parent;
    return run_panel(PANEL_SAVE, title, default_path, filters, filter_count, out, cap) == 1;
}

bool dialog_pick_folder(PlatformWindow *parent, const char *title, const char *default_path, char *out, size_t cap)
{
    (void)parent;
    return run_panel(PANEL_FOLDER, title, default_path, NULL, 0, out, cap) == 1;
}

int dialog_open_files(PlatformWindow *parent, const char *title, const char *default_path, const FileFilter *filters, int filter_count,
                      char *out, size_t cap)
{
    (void)parent;
    return run_panel(PANEL_OPEN_MANY, title, default_path, filters, filter_count, out, cap);
}

/* ---- URLs ---- */

/* Launch Services: a URL with a scheme goes to the application that has it, a path is opened as a file. */
bool open_url(const char *url)
{
    platform_clear_error();
    if (bad_url(url))
        return error_set("invalid URL");
    bool opened = false;
    @autoreleasepool
    {
        NSString *s = [NSString stringWithUTF8String:url];
        NSURL *u = s ? [NSURL URLWithString:s] : nil;
        if (u && ![u scheme])
            u = [NSURL fileURLWithPath:s];
        if (u)
        {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            opened = [[NSWorkspace sharedWorkspace] openURL:u] ? true : false;
#pragma clang diagnostic pop
        }
    }
    return opened ? true : error_set("cannot open the URL");
}
