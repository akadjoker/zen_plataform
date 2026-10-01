#include "zv_geom.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
        {                                                          \
            g_pass++;                                              \
        }                                                          \
        else                                                       \
        {                                                          \
            g_fail++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

static bool near(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

static bool matrix_near(const ZvMatrix *m, float a, float b, float c, float d, float e, float f, float eps)
{
    return near(m->a, a, eps) && near(m->b, b, eps) && near(m->c, c, eps) && near(m->d, d, eps) && near(m->e, e, eps) && near(m->f, f, eps);
}

static void test_matrix(void)
{
    ZvMatrix id = zv_matrix_identity();
    CHECK(zv_matrix_is_identity(&id));
    CHECK(zv_matrix_is_axis_aligned(&id));
    CHECK(near(zv_matrix_max_scale(&id), 1.0f, 1e-6f));

    ZvMatrix t = zv_matrix_translation(10, -5);
    ZvPoint p = zv_matrix_apply(&t, (ZvPoint){1, 2});
    CHECK(p.x == 11 && p.y == -3);
    ZvPoint v = zv_matrix_apply_vector(&t, (ZvPoint){1, 2});
    CHECK(v.x == 1 && v.y == 2);

    ZvMatrix s = zv_matrix_scaling(2, 3);
    p = zv_matrix_apply(&s, (ZvPoint){1, 2});
    CHECK(p.x == 2 && p.y == 6);
    CHECK(near(zv_matrix_max_scale(&s), 3.0f, 1e-5f));

    ZvMatrix r = zv_matrix_rotation((float)M_PI / 2);
    p = zv_matrix_apply(&r, (ZvPoint){1, 0});
    CHECK(near(p.x, 0, 1e-6f) && near(p.y, 1, 1e-6f));
    CHECK(!zv_matrix_is_axis_aligned(&r));
    CHECK(near(zv_matrix_max_scale(&r), 1.0f, 1e-5f));

    /* Order: concat(t, s) scales first, then translates. */
    ZvMatrix ts = zv_matrix_concat(&t, &s);
    p = zv_matrix_apply(&ts, (ZvPoint){1, 1});
    CHECK(p.x == 12 && p.y == -2);
    ZvMatrix st = zv_matrix_concat(&s, &t);
    p = zv_matrix_apply(&st, (ZvPoint){1, 1});
    CHECK(p.x == 22 && p.y == -12);

    /* Canvas-style translate/scale/rotate on a matrix apply first. */
    ZvMatrix m = zv_matrix_identity();
    zv_matrix_translate(&m, 10, 20);
    zv_matrix_scale(&m, 2, 2);
    zv_matrix_rotate(&m, (float)M_PI / 2);
    p = zv_matrix_apply(&m, (ZvPoint){1, 0});
    CHECK(near(p.x, 10, 1e-5f) && near(p.y, 22, 1e-5f));
    ZvMatrix t2 = zv_matrix_translation(10, 20);
    ZvMatrix s2 = zv_matrix_scaling(2, 2);
    ZvMatrix expect = zv_matrix_concat(&t2, &s2);
    expect = zv_matrix_concat(&expect, &r);
    CHECK(matrix_near(&m, expect.a, expect.b, expect.c, expect.d, expect.e, expect.f, 1e-5f));

    /* Inverse: table of matrices, apply then unapply gives the point back. */
    static const ZvMatrix table[] = {
        {1, 0, 0, 1, 0, 0},
        {2, 0, 0, 3, 5, 7},
        {0, 1, -1, 0, 0, 0},
        {0.5f, 0.25f, -0.75f, 1.5f, -100, 200},
        {1e-3f, 0, 0, 1e3f, 1, 1},
        {3, 1, 2, 4, 9, -9},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
    {
        ZvMatrix inv;
        CHECK(zv_matrix_invert(&table[i], &inv));
        ZvMatrix prod = zv_matrix_concat(&table[i], &inv);
        CHECK(matrix_near(&prod, 1, 0, 0, 1, 0, 0, 1e-3f));
        ZvPoint q = {3.5f, -1.25f};
        ZvPoint back = zv_matrix_apply(&inv, zv_matrix_apply(&table[i], q));
        CHECK(near(back.x, q.x, 1e-3f) && near(back.y, q.y, 1e-3f));
    }
    ZvMatrix singular = zv_matrix_make(1, 2, 2, 4, 0, 0);
    ZvMatrix untouched = zv_matrix_make(9, 9, 9, 9, 9, 9);
    CHECK(!zv_matrix_invert(&singular, &untouched));
    CHECK(untouched.a == 9 && untouched.f == 9);
    ZvMatrix zero = zv_matrix_make(0, 0, 0, 0, 1, 1);
    CHECK(!zv_matrix_invert(&zero, &untouched));

    /* max_scale for a shear: singular values of [1 1; 0 1] are (1 +/- sqrt5)/2 in magnitude. */
    ZvMatrix shear = zv_matrix_make(1, 0, 1, 1, 0, 0);
    CHECK(near(zv_matrix_max_scale(&shear), (1.0f + sqrtf(5.0f)) / 2.0f, 1e-5f));
    ZvMatrix flip = zv_matrix_make(-2, 0, 0, 1, 0, 0);
    CHECK(near(zv_matrix_max_scale(&flip), 2.0f, 1e-5f));

    ZvBounds b = {0, 0, 10, 10};
    ZvBounds rb = zv_matrix_apply_bounds(&r, &b);
    CHECK(near(rb.min_x, -10, 1e-5f) && near(rb.max_x, 0, 1e-5f) && near(rb.min_y, 0, 1e-5f) && near(rb.max_y, 10, 1e-5f));
}

static void test_bounds(void)
{
    ZvBounds e = zv_bounds_empty();
    CHECK(zv_bounds_is_empty(&e));
    zv_bounds_add(&e, 1, 2);
    CHECK(!zv_bounds_is_empty(&e) && e.min_x == 1 && e.max_x == 1 && e.min_y == 2 && e.max_y == 2);
    zv_bounds_add(&e, -1, 5);
    CHECK(e.min_x == -1 && e.max_x == 1 && e.min_y == 2 && e.max_y == 5);
    ZvBounds o = {0, 0, 3, 3};
    ZvBounds u = zv_bounds_union(&e, &o);
    CHECK(u.min_x == -1 && u.max_x == 3 && u.min_y == 0 && u.max_y == 5);
    ZvBounds i = zv_bounds_intersect(&e, &o);
    CHECK(i.min_x == 0 && i.max_x == 1 && i.min_y == 2 && i.max_y == 3);
    ZvBounds far = {10, 10, 11, 11};
    ZvBounds none = zv_bounds_intersect(&o, &far);
    CHECK(zv_bounds_is_empty(&none));
    ZvBounds empty = zv_bounds_empty();
    ZvBounds ue = zv_bounds_union(&empty, &o);
    CHECK(ue.min_x == 0 && ue.max_x == 3);
}

static int count_verb(const ZvPath *p, ZvVerb v)
{
    int n = 0;
    for (int i = 0; i < p->verb_count; i++)
        n += p->verbs[i] == (uint8_t)v;
    return n;
}

static void test_path_semantics(void)
{
    ZvPath p;
    zv_path_init(&p, NULL);
    CHECK(zv_path_is_empty(&p));
    CHECK(zv_path_close(&p)); /* no subpath: nothing happens */
    CHECK(p.verb_count == 0);

    /* line_to without a current point starts a subpath at its own point. */
    CHECK(zv_path_line_to(&p, 5, 6));
    CHECK(p.verb_count == 2 && p.verbs[0] == ZV_VERB_MOVE && p.verbs[1] == ZV_VERB_LINE);
    CHECK(p.points[0].x == 5 && p.points[0].y == 6 && p.points[1].x == 5);
    CHECK(zv_path_line_to(&p, 10, 6));
    CHECK(zv_path_line_to(&p, 10, 12));
    CHECK(zv_path_close(&p));
    CHECK(p.current.x == 5 && p.current.y == 6);
    CHECK(zv_path_close(&p)); /* second close is ignored */
    CHECK(count_verb(&p, ZV_VERB_CLOSE) == 1);

    /* after a close, a new segment starts at the closed subpath's start */
    CHECK(zv_path_line_to(&p, 0, 0));
    CHECK(p.verbs[p.verb_count - 2] == ZV_VERB_MOVE && p.verbs[p.verb_count - 1] == ZV_VERB_LINE);
    CHECK(p.points[p.point_count - 2].x == 5 && p.points[p.point_count - 2].y == 6);

    /* quad and cubic without a current point start at their control point */
    ZvPath q;
    zv_path_init(&q, NULL);
    CHECK(zv_path_quad_to(&q, 1, 2, 3, 4));
    CHECK(q.verbs[0] == ZV_VERB_MOVE && q.points[0].x == 1 && q.points[0].y == 2);
    CHECK(q.point_count == 3 && q.current.x == 3 && q.current.y == 4);
    CHECK(zv_path_cubic_to(&q, 5, 6, 7, 8, 9, 10));
    CHECK(q.point_count == 6 && q.current.x == 9 && q.current.y == 10);
    CHECK(q.start.x == 1);

    /* copy and append with a transform */
    ZvPath c;
    zv_path_init(&c, NULL);
    CHECK(zv_path_copy(&c, &p));
    CHECK(c.verb_count == p.verb_count && c.point_count == p.point_count);
    CHECK(memcmp(c.verbs, p.verbs, (size_t)p.verb_count) == 0);
    CHECK(c.current.x == p.current.x && c.after_close == p.after_close && c.has_current);
    ZvMatrix t = zv_matrix_translation(100, 0);
    CHECK(zv_path_append(&c, &q, &t));
    CHECK(c.point_count == p.point_count + q.point_count);
    CHECK(c.points[p.point_count].x == 101 && c.points[p.point_count].y == 2);
    CHECK(c.current.x == 109 && c.start.x == 101);

    zv_path_transform(&q, &t);
    CHECK(q.points[0].x == 101 && q.current.x == 109);

    zv_path_clear(&p);
    CHECK(zv_path_is_empty(&p) && !p.has_current);
    CHECK(zv_path_move_to(&p, 1, 1));
    CHECK(p.verb_count == 1);

    zv_path_release(&p);
    zv_path_release(&q);
    zv_path_release(&c);
    CHECK(p.verbs == NULL && p.points == NULL && p.verb_capacity == 0);
}

static void test_path_bounds(void)
{
    ZvPath p;
    zv_path_init(&p, NULL);
    ZvBounds b = zv_path_bounds(&p);
    CHECK(zv_bounds_is_empty(&b));

    /* a quadratic bulging above its end points: the control point is at
       y = -10 but the curve only reaches y = -5 */
    CHECK(zv_path_move_to(&p, 0, 0));
    CHECK(zv_path_quad_to(&p, 10, -10, 20, 0));
    b = zv_path_bounds(&p);
    CHECK(near(b.min_x, 0, 1e-6f) && near(b.max_x, 20, 1e-6f) && near(b.min_y, -5, 1e-5f) && near(b.max_y, 0, 1e-6f));
    ZvBounds cb = zv_path_control_bounds(&p);
    CHECK(near(cb.min_y, -10, 1e-6f));

    /* a cubic with both controls far out: extreme at t = 0.5 is 0.75 of the way */
    zv_path_clear(&p);
    CHECK(zv_path_move_to(&p, 0, 0));
    CHECK(zv_path_cubic_to(&p, 0, 10, 10, 10, 10, 0));
    b = zv_path_bounds(&p);
    CHECK(near(b.max_y, 7.5f, 1e-5f) && near(b.min_y, 0, 1e-6f) && near(b.min_x, 0, 1e-6f) && near(b.max_x, 10, 1e-6f));

    /* a cubic that overshoots in x on both sides */
    zv_path_clear(&p);
    CHECK(zv_path_move_to(&p, 0, 0));
    CHECK(zv_path_cubic_to(&p, -30, 0, 40, 0, 10, 0));
    b = zv_path_bounds(&p);
    CHECK(b.min_x < -3.0f && b.min_x > -10.0f && b.max_x > 13.0f && b.max_x < 20.0f);
    /* verify against dense sampling */
    float smin = 1e9f, smax = -1e9f;
    for (int i = 0; i <= 10000; i++)
    {
        ZvPoint q = zv_cubic_eval((ZvPoint){0, 0}, (ZvPoint){-30, 0}, (ZvPoint){40, 0}, (ZvPoint){10, 0}, (float)i / 10000.0f);
        if (q.x < smin)
            smin = q.x;
        if (q.x > smax)
            smax = q.x;
    }
    CHECK(near(b.min_x, smin, 1e-3f) && near(b.max_x, smax, 1e-3f));

    /* a straight cubic (a == 0 in the derivative) */
    zv_path_clear(&p);
    CHECK(zv_path_move_to(&p, 0, 0));
    CHECK(zv_path_cubic_to(&p, 1, 1, 2, 2, 3, 3));
    b = zv_path_bounds(&p);
    CHECK(b.min_x == 0 && b.max_x == 3 && b.min_y == 0 && b.max_y == 3);

    /* close adds nothing, several subpaths union */
    CHECK(zv_path_close(&p));
    CHECK(zv_path_move_to(&p, -1, 7));
    b = zv_path_bounds(&p);
    CHECK(b.min_x == -1 && b.max_y == 7);
    zv_path_release(&p);
}

/* Distance from q to segment ab. */
static float segment_distance(ZvPoint q, ZvPoint a, ZvPoint b)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (len2 > 0.0f)
    {
        t = ((q.x - a.x) * dx + (q.y - a.y) * dy) / len2;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
    }
    float px = a.x + t * dx - q.x, py = a.y + t * dy - q.y;
    return sqrtf(px * px + py * py);
}

static float polyline_distance(const ZvPolyline *poly, ZvPoint q)
{
    float best = 1e30f;
    for (int i = 0; i + 1 < poly->point_count; i++)
    {
        float d = segment_distance(q, poly->points[i], poly->points[i + 1]);
        if (d < best)
            best = d;
    }
    if (poly->point_count == 1)
    {
        float dx = poly->points[0].x - q.x, dy = poly->points[0].y - q.y;
        best = sqrtf(dx * dx + dy * dy);
    }
    return best;
}

typedef struct
{
    ZvPoint p[4];
    bool cubic;
} Curve;

static float flatten_error(const Curve *c, float tolerance, int *segments)
{
    ZvPath path;
    zv_path_init(&path, NULL);
    zv_path_move_to(&path, c->p[0].x, c->p[0].y);
    if (c->cubic)
        zv_path_cubic_to(&path, c->p[1].x, c->p[1].y, c->p[2].x, c->p[2].y, c->p[3].x, c->p[3].y);
    else
        zv_path_quad_to(&path, c->p[1].x, c->p[1].y, c->p[2].x, c->p[2].y);
    ZvPolyline poly;
    zv_polyline_init(&poly, NULL);
    bool ok = zv_path_flatten(&path, NULL, tolerance, &poly);
    CHECK(ok);
    CHECK(poly.contour_count == 1 && poly.contours[0].first == 0 && poly.contours[0].count == poly.point_count);
    *segments = poly.point_count - 1;
    float worst = 0.0f;
    for (int i = 0; i <= 2000; i++)
    {
        float t = (float)i / 2000.0f;
        ZvPoint q = c->cubic ? zv_cubic_eval(c->p[0], c->p[1], c->p[2], c->p[3], t) : zv_quad_eval(c->p[0], c->p[1], c->p[2], t);
        float d = polyline_distance(&poly, q);
        if (d > worst)
            worst = d;
    }
    zv_polyline_release(&poly);
    zv_path_release(&path);
    return worst;
}

static void test_flatten_error(void)
{
    static const Curve curves[] = {
        /* quads */
        {{{0, 0}, {50, 100}, {100, 0}}, false},
        {{{0, 0}, {100, 0}, {100, 100}}, false},
        {{{0, 0}, {0, 0}, {0, 0}}, false},         /* degenerate point */
        {{{0, 0}, {5, 5}, {10, 10}}, false},       /* straight */
        {{{0, 0}, {1000, 0}, {0, 1}}, false},      /* very wide and thin */
        {{{0, 0}, {0.1f, 0.3f}, {0.2f, 0}}, false}, /* tiny */
        {{{10, 10}, {-500, 300}, {700, 20}}, false},
        {{{3, 3}, {3, 3}, {9, 1}}, false}, /* control on the start */
        /* cubics */
        {{{0, 0}, {0, 100}, {100, 100}, {100, 0}}, true},
        {{{0, 0}, {100, 0}, {0, 100}, {100, 100}}, true}, /* S */
        {{{0, 0}, {300, 0}, {-200, 0}, {100, 0}}, true},  /* loops back on a line */
        {{{0, 0}, {200, 300}, {-200, 300}, {0, 0}}, true}, /* loop */
        {{{0, 0}, {1, 1}, {2, 2}, {3, 3}}, true},
        {{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, true},
        {{{5, 5}, {5, 5}, {5, 5}, {400, 5}}, true},
        {{{0, 0}, {2000, 0}, {2000, 2000}, {0, 2000}}, true},
        {{{0, 0}, {0.05f, 0.1f}, {0.1f, 0.1f}, {0.15f, 0}}, true},
        {{{-20, 40}, {60, -90}, {-70, 80}, {30, -10}}, true},
    };
    static const float tolerances[] = {0.25f, 0.1f, 1.0f, 0.02f};
    for (size_t t = 0; t < sizeof tolerances / sizeof tolerances[0]; t++)
    {
        float tol = tolerances[t];
        for (size_t i = 0; i < sizeof curves / sizeof curves[0]; i++)
        {
            int segments = 0;
            float err = flatten_error(&curves[i], tol, &segments);
            /* allow float rounding on big coordinates */
            float slack = tol * 0.02f + 1e-3f;
            if (err > tol + slack)
                printf("  curve %zu tol %g: error %g with %d segments\n", i, tol, err, segments);
            CHECK(err <= tol + slack);
            CHECK(segments >= 1);
        }
    }

    /* the segment count grows with the curvature and shrinks with the tolerance */
    ZvPoint a = {0, 0}, b = {50, 100}, c = {100, 0};
    int n1 = zv_quad_segments(a, b, c, 0.25f);
    int n2 = zv_quad_segments(a, b, c, 1.0f);
    int n3 = zv_quad_segments(a, b, c, 0.0625f);
    CHECK(n1 > n2 && n3 > n1);
    CHECK(n3 == 2 * n1 || n3 == 2 * n1 + 1 || n3 == 2 * n1 - 1);
    CHECK(zv_quad_segments(a, a, a, 0.25f) == 1);
    ZvPoint s = {5, 5}, e = {10, 10};
    CHECK(zv_quad_segments(a, s, e, 0.25f) == 1);
    /* a bad tolerance falls back to the default */
    int nd = zv_quad_segments(a, b, c, ZV_FLATTEN_TOLERANCE);
    CHECK(zv_quad_segments(a, b, c, 0.0f) == nd);
    CHECK(zv_quad_segments(a, b, c, -3.0f) == nd);
    /* the count is capped */
    ZvPoint huge = {1e9f, 1e9f};
    CHECK(zv_cubic_segments(a, huge, huge, a, 0.001f) == 4096);
    ZvPoint inf = {INFINITY, 0};
    CHECK(zv_quad_segments(a, inf, c, 0.25f) == 4096);
}

static void test_flatten_structure(void)
{
    ZvPath p;
    zv_path_init(&p, NULL);
    CHECK(zv_path_move_to(&p, 0, 0));
    CHECK(zv_path_line_to(&p, 10, 0));
    CHECK(zv_path_line_to(&p, 10, 10));
    CHECK(zv_path_close(&p));
    CHECK(zv_path_line_to(&p, -5, -5)); /* new subpath from (0,0) */
    CHECK(zv_path_move_to(&p, 50, 50)); /* lone point */
    CHECK(zv_path_move_to(&p, 60, 60));
    CHECK(zv_path_quad_to(&p, 70, 80, 80, 60));

    ZvPolyline poly;
    zv_polyline_init(&poly, NULL);
    ZvMatrix m = zv_matrix_scaling(2, 2);
    CHECK(zv_path_flatten(&p, &m, 0.25f, &poly));
    CHECK(poly.contour_count == 4);
    CHECK(poly.contours[0].count == 3 && poly.contours[0].closed);
    CHECK(poly.points[1].x == 20 && poly.points[2].y == 20);
    CHECK(poly.contours[1].count == 2 && !poly.contours[1].closed);
    CHECK(poly.points[poly.contours[1].first].x == 0 && poly.points[poly.contours[1].first + 1].x == -10);
    CHECK(poly.contours[2].count == 1 && poly.points[poly.contours[2].first].x == 100);
    CHECK(poly.contours[3].count > 2 && !poly.contours[3].closed);
    CHECK(poly.points[poly.point_count - 1].x == 160 && poly.points[poly.point_count - 1].y == 120);
    ZvBounds b = zv_polyline_bounds(&poly);
    CHECK(b.min_x == -10 && b.max_x == 160);

    /* flattening appends; clear empties */
    CHECK(zv_path_flatten(&p, NULL, 0.25f, &poly));
    CHECK(poly.contour_count == 8);
    zv_polyline_clear(&poly);
    CHECK(poly.contour_count == 0 && poly.point_count == 0);
    /* a point added without a contour opens one */
    CHECK(zv_polyline_add_point(&poly, 1, 2));
    CHECK(poly.contour_count == 1 && poly.contours[0].count == 1);

    /* the transformed flattening of a curve equals flattening the transformed path */
    ZvPath q;
    zv_path_init(&q, NULL);
    CHECK(zv_path_copy(&q, &p));
    ZvMatrix rot = zv_matrix_rotation(0.7f);
    zv_matrix_translate(&rot, 3, 4);
    zv_path_transform(&q, &rot);
    ZvPolyline a, c;
    zv_polyline_init(&a, NULL);
    zv_polyline_init(&c, NULL);
    CHECK(zv_path_flatten(&p, &rot, 0.1f, &a));
    CHECK(zv_path_flatten(&q, NULL, 0.1f, &c));
    CHECK(a.point_count == c.point_count);
    bool same = a.point_count == c.point_count;
    for (int i = 0; same && i < a.point_count; i++)
        same = near(a.points[i].x, c.points[i].x, 1e-3f) && near(a.points[i].y, c.points[i].y, 1e-3f);
    CHECK(same);

    zv_polyline_release(&a);
    zv_polyline_release(&c);
    zv_polyline_release(&poly);
    zv_path_release(&p);
    zv_path_release(&q);
}

/* An allocator that fails after a number of allocations. */
typedef struct
{
    int remaining;
    int live;
} Limited;

static void *limited_allocate(void *context, size_t size)
{
    Limited *l = context;
    if (l->remaining <= 0)
        return NULL;
    l->remaining--;
    l->live++;
    return malloc(size);
}

static void limited_release(void *context, void *memory)
{
    Limited *l = context;
    if (memory)
        l->live--;
    free(memory);
}

static void test_out_of_memory(void)
{
    Limited l = {0, 0};
    ZvAllocator alloc = {limited_allocate, limited_release, &l};
    ZvPath p;
    zv_path_init(&p, &alloc);
    CHECK(!zv_path_move_to(&p, 1, 1));
    CHECK(p.verb_count == 0 && p.point_count == 0 && !p.has_current);

    l.remaining = 2;
    CHECK(zv_path_move_to(&p, 1, 1));
    for (int i = 0; i < 100; i++)
        zv_path_line_to(&p, (float)i, 0);
    CHECK(p.verb_count <= 16 && p.verb_count == p.point_count);
    CHECK(p.verb_count < 101);
    int kept = p.verb_count;
    CHECK(!zv_path_line_to(&p, 1, 1) || p.verb_count == kept + 1);

    l.remaining = 1000;
    ZvPolyline poly;
    zv_polyline_init(&poly, &alloc);
    CHECK(zv_path_flatten(&p, NULL, 0.25f, &poly));
    l.remaining = 0;
    zv_polyline_clear(&poly);
    CHECK(zv_path_flatten(&p, NULL, 0.25f, &poly)); /* fits in the kept memory */
    zv_polyline_release(&poly);
    zv_polyline_init(&poly, &alloc);
    CHECK(!zv_path_flatten(&p, NULL, 0.25f, &poly));
    zv_polyline_release(&poly);
    zv_path_release(&p);
    CHECK(l.live == 0);
}

int main(void)
{
    test_matrix();
    test_bounds();
    test_path_semantics();
    test_path_bounds();
    test_flatten_error();
    test_flatten_structure();
    test_out_of_memory();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
