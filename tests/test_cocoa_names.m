/*
 * test_cocoa_names.m - the library's names against Apple's: platform.h, and the library's internal headers, included AFTER the
 * headers of Cocoa, Foundation, Carbon, OpenGL, QuartzCore and Mach, in an Objective-C file, which is how backend_cocoa.m and
 * dialog_cocoa.m meet them. A name the library takes that a system header already has (a function, a type, an enumerator, a
 * macro) is a compile error here, in a file that does nothing else, so the Mac build says which header and which name
 * instead of the backend failing to compile somewhere in 3000 lines. It is how thread_create (the library's old name for
 * create_thread) was found: <mach/task.h>, which Cocoa includes, declares a thread_create of its own.
 *
 * The program also uses a thread, a mutex and a condition variable of the library, so the names are linked and not only
 * declared. It needs no window and no graphical session. Built wherever the target is macOS (the native backend and the fake one).
 *
 * The one name Apple's headers force the library to rename around an include is EventType (Carbon's event manager has a
 * typedef of that name, and platform.h an enum): Carbon's is renamed for the length of its header, as backend_cocoa.m does.
 * Anything else that has to be worked around the same way shows up in this file first.
 */
#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#define EventType zen_carbon_EventType
#import <Carbon/Carbon.h>
#undef EventType
#import <OpenGL/OpenGL.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#if __has_include(<Metal/Metal.h>)
#import <Metal/Metal.h>
#endif
#if __has_include(<CoreVideo/CoreVideo.h>)
#import <CoreVideo/CoreVideo.h>
#endif
#if __has_include(<IOKit/IOKitLib.h>)
#import <IOKit/IOKitLib.h>
#endif
#if __has_include(<GameController/GameController.h>)
#import <GameController/GameController.h>
#endif
#if __has_include(<UniformTypeIdentifiers/UniformTypeIdentifiers.h>)
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h> /* thread_create, task_*, mach_port_* ... */
#include <mach/mach_time.h>
#include <mach/task.h>
#include <mach/thread_act.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* the library, after all of the above */
#include "platform.h"
#include "backend.h"
#include "core_internal.h"
#include "crash_internal.h"
#include "error_internal.h"
#include "gamepad_internal.h"
#include "gesture_internal.h"
#include "io_internal.h"
#include "mime_util.h"
#include "os_backend.h"
#include "png_internal.h"
#include "preedit.h"
#include "vulkan_internal.h"

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

typedef struct
{
    PlatformMutex *mutex;
    PlatformCond *cond;
    int started;   /* the worker is running */
    int released;  /* the main thread let it go on */
    uint64_t id;   /* the worker's thread id */
} Shared;

static int worker_main(void *user)
{
    Shared *s = user;
    lock_mutex(s->mutex);
    s->id = current_thread_id();
    s->started = 1;
    signal_cond(s->cond);
    while (!s->released)
        wait_cond(s->cond, s->mutex);
    unlock_mutex(s->mutex);
    return 7;
}

int main(void)
{
    @autoreleasepool
    {
        /* an Objective-C object next to the library's types: the file is compiled as Objective-C, not just included from one */
        NSString *name = [NSString stringWithUTF8String:"names"];
        CHECK([name length] == 5);
        CHECK([[NSProcessInfo processInfo] processName] != nil);

        Shared s = {0};
        s.mutex = create_mutex();
        s.cond = create_cond();
        CHECK(s.mutex != NULL && s.cond != NULL);
        if (!s.mutex || !s.cond)
            return 1;

        PlatformThread *t = create_thread(worker_main, &s, "names");
        CHECK(t != NULL);
        if (!t)
            return 1;

        /* the worker has started and noted its id; waiting with a timeout cannot hang the test */
        lock_mutex(s.mutex);
        int waits = 0;
        while (!s.started && waits++ < 500)
            wait_cond_timeout(s.cond, s.mutex, 20);
        CHECK(s.started == 1);
        CHECK(s.id != 0 && s.id != current_thread_id());
        CHECK(try_lock_mutex(s.mutex)); /* the mutex is recursive: the thread that holds it can take it again */
        unlock_mutex(s.mutex);
        s.released = 1;
        broadcast_cond(s.cond);
        unlock_mutex(s.mutex);

        CHECK(join_thread(t) == 7);
        destroy_cond(s.cond);
        destroy_mutex(s.mutex);

        CHECK(cpu_count() >= 1);
        CHECK(time_seconds() >= 0.0);
    }
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
