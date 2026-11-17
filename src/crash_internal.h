#ifndef CRASH_INTERNAL_H
#define CRASH_INTERNAL_H

/* Called by the threads create_thread starts (POSIX), first thing and last thing: a stack for the crash handler of
   that thread, when crash_handler_install is in effect. Not for Windows and the web, where nothing is installed. */
void crash_thread_enter(void);
void crash_thread_leave(void);

#endif /* CRASH_INTERNAL_H */
