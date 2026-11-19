/* mcount.c - LD_PRELOAD heap counter: calls, bytes asked for, live and peak live bytes (as the allocator sizes
   them), written to stderr at exit as one line. malloc, calloc, realloc and free of the process and of every
   library it loads; the allocator's own internal calls are not seen. Not thread safe on purpose: the bench programs
   that use it run one thread (the zen_platform thread API is not started). */
#define _GNU_SOURCE
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);

static unsigned long calls, frees, bytes, live, peak;
static int armed;

static void add(void *p, size_t n)
{
    calls++;
    bytes += n;
    live += malloc_usable_size(p);
    if (live > peak)
        peak = live;
}

void *malloc(size_t n)
{
    void *p = __libc_malloc(n);
    if (p)
        add(p, n);
    return p;
}

void *calloc(size_t a, size_t b)
{
    void *p = __libc_calloc(a, b);
    if (p)
        add(p, a * b);
    return p;
}

void *realloc(void *old, size_t n)
{
    unsigned long was = old ? malloc_usable_size(old) : 0;
    void *p = __libc_realloc(old, n);
    if (p)
    {
        live -= was;
        add(p, n);
    }
    return p;
}

void free(void *p)
{
    if (p)
    {
        frees++;
        live -= malloc_usable_size(p);
    }
    __libc_free(p);
}

__attribute__((destructor)) static void report(void)
{
    if (armed++)
        return;
    fprintf(stderr, "heap: allocs=%lu frees=%lu bytes=%lu live_at_exit=%lu peak_live=%lu\n", calls, frees, bytes, live,
            peak);
}
