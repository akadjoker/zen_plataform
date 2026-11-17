/*
 * test_thread.c - threads, recursive mutexes, condition variables, detach, timeouts.
 */
#include "platform.h"

#include <stdio.h>
#include <string.h>

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

/* ---- join result, ids, many threads on one counter ---- */

static int returns_value(void *user)
{
    return *(int *)user * 2;
}

typedef struct
{
    PlatformMutex *m;
    long *counter;
    int times;
} Counter;

static int bump(void *user)
{
    Counter *c = user;
    for (int i = 0; i < c->times; i++)
    {
        lock_mutex(c->m);
        lock_mutex(c->m); /* recursive */
        ++*c->counter;
        unlock_mutex(c->m);
        unlock_mutex(c->m);
    }
    return 0;
}

static uint64_t g_other_id;
static int record_id(void *user)
{
    (void)user;
    g_other_id = current_thread_id();
    return 0;
}

static void test_basic(void)
{
    int arg = 21;
    PlatformThread *t = create_thread(returns_value, &arg, "worker");
    CHECK(t != NULL);
    CHECK(t && join_thread(t) == 42);

    CHECK(create_thread(NULL, NULL, NULL) == NULL);
    CHECK(platform_get_error()[0] != '\0');

    t = create_thread(record_id, NULL, NULL);
    CHECK(t != NULL);
    if (t)
        join_thread(t);
    CHECK(g_other_id != 0 && g_other_id != current_thread_id());

    CHECK(count_cpu() >= 1);
}

static void test_mutex_counter(void)
{
    PlatformMutex *m = create_mutex();
    CHECK(m != NULL);
    long counter = 0;
    Counter c = {m, &counter, 20000};
    PlatformThread *ts[8];
    for (int i = 0; i < 8; i++)
        ts[i] = create_thread(bump, &c, "bump");
    for (int i = 0; i < 8; i++)
        CHECK(ts[i] && join_thread(ts[i]) == 0);
    CHECK(counter == 8 * 20000L);
    destroy_mutex(m);
}

/* ---- try_lock across threads ---- */

typedef struct
{
    PlatformMutex *m;
    bool got;
} TryArg;

static int try_it(void *user)
{
    TryArg *a = user;
    a->got = try_lock_mutex(a->m);
    if (a->got)
        unlock_mutex(a->m);
    return 0;
}

static void test_try_lock(void)
{
    PlatformMutex *m = create_mutex();
    TryArg a = {m, true};

    lock_mutex(m);
    CHECK(try_lock_mutex(m)); /* the holder may lock again */
    unlock_mutex(m);
    PlatformThread *t = create_thread(try_it, &a, NULL);
    join_thread(t);
    CHECK(!a.got); /* another thread may not */

    unlock_mutex(m);
    t = create_thread(try_it, &a, NULL);
    join_thread(t);
    CHECK(a.got);
    destroy_mutex(m);
}

/* ---- producer / consumer on a condition ---- */

#define QUEUE_N 8
typedef struct
{
    PlatformMutex *m;
    PlatformCond *not_empty, *not_full;
    int items[QUEUE_N];
    int head, count;
    long sum;
} Queue;

static int consumer(void *user)
{
    Queue *q = user;
    for (;;)
    {
        lock_mutex(q->m);
        while (q->count == 0)
            wait_cond(q->not_empty, q->m);
        int v = q->items[q->head];
        q->head = (q->head + 1) % QUEUE_N;
        q->count--;
        signal_cond(q->not_full);
        unlock_mutex(q->m);
        if (v < 0)
            return 0;
        q->sum += v;
    }
}

static void test_cond(void)
{
    Queue q = {0};
    q.m = create_mutex();
    q.not_empty = create_cond();
    q.not_full = create_cond();
    CHECK(q.m && q.not_empty && q.not_full);

    PlatformThread *t = create_thread(consumer, &q, "consumer");
    long expect = 0;
    for (int i = 0; i <= 500; i++)
    {
        int v = i == 500 ? -1 : i; /* -1 ends the consumer */
        if (i < 500)
            expect += i;
        lock_mutex(q.m);
        while (q.count == QUEUE_N)
            wait_cond(q.not_full, q.m);
        q.items[(q.head + q.count) % QUEUE_N] = v;
        q.count++;
        signal_cond(q.not_empty);
        unlock_mutex(q.m);
    }
    CHECK(t && join_thread(t) == 0);
    CHECK(q.sum == expect);

    destroy_mutex(q.m);
    destroy_cond(q.not_empty);
    destroy_cond(q.not_full);
}

/* ---- timeouts ---- */

typedef struct
{
    PlatformMutex *m;
    PlatformCond *c;
} Signal;

static int signal_later(void *user)
{
    Signal *s = user;
    time_sleep(30);
    lock_mutex(s->m);
    signal_cond(s->c);
    unlock_mutex(s->m);
    return 0;
}

static void test_timeout(void)
{
    Signal s = {create_mutex(), create_cond()};

    lock_mutex(s.m);
    double t0 = time_seconds();
    bool signalled = wait_cond_timeout(s.c, s.m, 60);
    double dt = time_seconds() - t0;
    unlock_mutex(s.m);
    CHECK(!signalled);
    CHECK(dt >= 0.05 && dt < 1.0);

    PlatformThread *t = create_thread(signal_later, &s, NULL);
    lock_mutex(s.m);
    t0 = time_seconds();
    signalled = wait_cond_timeout(s.c, s.m, 5000);
    dt = time_seconds() - t0;
    unlock_mutex(s.m);
    CHECK(signalled);
    CHECK(dt < 2.0);
    join_thread(t);

    destroy_mutex(s.m);
    destroy_cond(s.c);
}

/* ---- detach ---- */

/* A mutex around the flag: a volatile int polled between sleeps is a data race, and ThreadSanitizer says so. */
static int g_detached_done;
static PlatformMutex *g_done_mutex;

static int detached_done(void)
{
    lock_mutex(g_done_mutex);
    int v = g_detached_done;
    unlock_mutex(g_done_mutex);
    return v;
}

static void set_detached_done(int v)
{
    lock_mutex(g_done_mutex);
    g_detached_done = v;
    unlock_mutex(g_done_mutex);
}

static int detached_work(void *user)
{
    (void)user;
    time_sleep(20);
    set_detached_done(1);
    return 0;
}

static void test_detach(void)
{
    g_done_mutex = create_mutex();
    PlatformThread *t = create_thread(detached_work, NULL, "detached");
    detach_thread(t); /* before it finishes: the thread frees its own record */
    for (int i = 0; i < 200 && !detached_done(); i++)
        time_sleep(5);
    CHECK(detached_done());

    set_detached_done(0);
    t = create_thread(detached_work, NULL, NULL);
    for (int i = 0; i < 200 && !detached_done(); i++)
        time_sleep(5);
    time_sleep(20);
    detach_thread(t); /* after it finished: detach frees the record */
    CHECK(detached_done());
    time_sleep(20);
    destroy_mutex(g_done_mutex);
}

int main(void)
{
    test_basic();
    test_mutex_counter();
    test_try_lock();
    test_cond();
    test_timeout();
    test_detach();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
