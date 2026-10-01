#include "bmpcmp.h"
#include "scene.h"
#include "zv_raster.h"

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

static uint32_t g_seed = 777;

static uint32_t rnd(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static float rndf(float lo, float hi)
{
    return lo + (hi - lo) * (float)(rnd() % 100000u) / 100000.0f;
}

/* Independent reference: the exact area of a simple polygon inside a pixel,
   by clipping the polygon to the pixel square (Sutherland-Hodgman) and the
   shoelace formula. Valid for any simple polygon, either orientation. */

#define MAX_CLIP 64

static int clip_edge(const double *in, int n, double *out, int axis, double value, int keep_greater)
{
    int m = 0;
    for (int i = 0; i < n; i++)
    {
        const double *a = in + 2 * i;
        const double *b = in + 2 * ((i + 1) % n);
        double da = a[axis] - value, db = b[axis] - value;
        bool ina = keep_greater ? da >= 0 : da <= 0;
        bool inb = keep_greater ? db >= 0 : db <= 0;
        if (ina)
        {
            out[2 * m] = a[0];
            out[2 * m + 1] = a[1];
            m++;
        }
        if (ina != inb)
        {
            double t = da / (da - db);
            out[2 * m] = a[0] + t * (b[0] - a[0]);
            out[2 * m + 1] = a[1] + t * (b[1] - a[1]);
            m++;
        }
    }
    return m;
}

static double pixel_area(const ZvPoint *pts, int n, int px, int py)
{
    double a[MAX_CLIP * 2], b[MAX_CLIP * 2];
    for (int i = 0; i < n; i++)
    {
        a[2 * i] = pts[i].x;
        a[2 * i + 1] = pts[i].y;
    }
    int m = clip_edge(a, n, b, 0, px, 1);
    m = clip_edge(b, m, a, 0, px + 1, 0);
    m = clip_edge(a, m, b, 1, py, 1);
    m = clip_edge(b, m, a, 1, py + 1, 0);
    double area = 0;
    for (int i = 0; i < m; i++)
    {
        const double *p = a + 2 * i;
        const double *q = a + 2 * ((i + 1) % m);
        area += p[0] * q[1] - q[0] * p[1];
    }
    return fabs(area) * 0.5;
}

static ZvSurface fill_polygon(int w, int h, const ZvPoint *pts, int n, ZvFillRule rule, ZvPixel color, const ZvBounds *clip)
{
    ZvSurface s;
    zv_surface_init(&s, NULL, w, h);
    ZvPolyline poly;
    zv_polyline_init(&poly, NULL);
    zv_polyline_add_contour(&poly, true);
    for (int i = 0; i < n; i++)
        zv_polyline_add_point(&poly, pts[i].x, pts[i].y);
    CHECK(zv_fill_polyline_solid(&s, &poly, rule, color, clip, NULL));
    zv_polyline_release(&poly);
    return s;
}

static int alpha_at(const ZvSurface *s, int x, int y)
{
    return (int)(s->pixels[(size_t)y * (size_t)s->stride + (size_t)x] >> 24);
}

/* Compares a filled polygon with the exact coverage, pixel by pixel. Returns
   the largest difference. */
static int max_error_against_exact(int w, int h, const ZvPoint *pts, int n, ZvFillRule rule)
{
    /* the rasterizer works on coordinates rounded to 1/256 pixel: the
       reference gets the same points, so only the algorithm is measured */
    ZvPoint q[MAX_CLIP];
    for (int i = 0; i < n; i++)
    {
        q[i].x = floorf(pts[i].x * 256.0f + 0.5f) / 256.0f;
        q[i].y = floorf(pts[i].y * 256.0f + 0.5f) / 256.0f;
    }
    ZvSurface s = fill_polygon(w, h, q, n, rule, 0xFF000000u, NULL);
    int worst = 0;
    double sum_mine = 0, sum_exact = 0;
    int edge_pixels = 0;
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            double exact = pixel_area(q, n, x, y);
            int expect = (int)floor(exact * 255.0 + 0.5);
            int mine = alpha_at(&s, x, y);
            int d = abs(mine - expect);
            if (d > worst)
                worst = d;
            sum_mine += mine;
            sum_exact += exact * 255.0;
            if (exact > 0.0 && exact < 1.0)
                edge_pixels++;
        }
    }
    /* total coverage equals the area, up to the rounding of each edge pixel */
    CHECK(fabs(sum_mine - sum_exact) <= 0.6 * edge_pixels + 2.0);
    zv_surface_release(&s);
    return worst;
}

static void test_exact_rect(void)
{
    ZvPoint r[4] = {{2.25f, 3.5f}, {12.75f, 3.5f}, {12.75f, 9.125f}, {2.25f, 9.125f}};
    CHECK(max_error_against_exact(16, 12, r, 4, ZV_FILL_NONZERO) <= 1);
    ZvSurface s = fill_polygon(16, 12, r, 4, ZV_FILL_NONZERO, 0xFF000000u, NULL);
    CHECK(alpha_at(&s, 2, 3) == (int)floor(0.75 * 0.5 * 255 + 0.5));
    CHECK(alpha_at(&s, 5, 5) == 255);
    CHECK(alpha_at(&s, 12, 5) == (int)floor(0.75 * 255 + 0.5));
    CHECK(alpha_at(&s, 5, 9) == (int)floor(0.125 * 255 + 0.5));
    CHECK(alpha_at(&s, 1, 5) == 0 && alpha_at(&s, 13, 5) == 0 && alpha_at(&s, 5, 2) == 0 && alpha_at(&s, 5, 10) == 0);
    zv_surface_release(&s);

    /* integer rectangle: exactly 255 inside, 0 outside, and reversed winding */
    ZvPoint q[4] = {{3, 2}, {3, 8}, {10, 8}, {10, 2}};
    s = fill_polygon(16, 12, q, 4, ZV_FILL_EVENODD, 0xFF000000u, NULL);
    for (int y = 0; y < 12; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            bool inside = x >= 3 && x < 10 && y >= 2 && y < 8;
            if (alpha_at(&s, x, y) != (inside ? 255 : 0))
                CHECK(!"integer rectangle pixel");
        }
    }
    CHECK(alpha_at(&s, 3, 2) == 255 && alpha_at(&s, 9, 7) == 255 && alpha_at(&s, 10, 7) == 0);
    zv_surface_release(&s);
}

static void test_exact_polygons(void)
{
    static const ZvPoint tri[3] = {{4.5f, 4.5f}, {27.5f, 6.25f}, {9.75f, 27.5f}};
    int e = max_error_against_exact(32, 32, tri, 3, ZV_FILL_NONZERO);
    printf("  triangle: max error %d/255 against exact coverage\n", e);
    CHECK(e <= 1);

    static const ZvPoint rev[3] = {{9.75f, 27.5f}, {27.5f, 6.25f}, {4.5f, 4.5f}};
    CHECK(max_error_against_exact(32, 32, rev, 3, ZV_FILL_NONZERO) <= 1);
    CHECK(max_error_against_exact(32, 32, rev, 3, ZV_FILL_EVENODD) <= 1);

    static const ZvPoint sliver[3] = {{2, 2}, {60, 28}, {60, 28.6f}};
    e = max_error_against_exact(64, 32, sliver, 3, ZV_FILL_NONZERO);
    printf("  sliver: max error %d/255\n", e);
    CHECK(e <= 1);

    /* an L shape (concave, simple) */
    static const ZvPoint ell[6] = {{2.5f, 2.5f}, {20.5f, 2.5f}, {20.5f, 8.25f}, {8.75f, 8.25f}, {8.75f, 20.5f}, {2.5f, 20.5f}};
    e = max_error_against_exact(24, 24, ell, 6, ZV_FILL_NONZERO);
    printf("  L shape: max error %d/255\n", e);
    CHECK(e <= 1);
    CHECK(max_error_against_exact(24, 24, ell, 6, ZV_FILL_EVENODD) <= 1);

    /* a spiky simple polygon */
    static const ZvPoint spiky[8] = {{12, 1}, {14, 10}, {23, 12}, {14, 14}, {12, 23}, {10, 14}, {1, 12}, {10, 10}};
    e = max_error_against_exact(24, 24, spiky, 8, ZV_FILL_NONZERO);
    printf("  spikes: max error %d/255\n", e);
    CHECK(e <= 1);

    /* random convex polygons (points sorted by angle around a centre) */
    int worst = 0;
    for (int k = 0; k < 40; k++)
    {
        int n = 3 + (int)(rnd() % 6);
        ZvPoint pts[8];
        float cx = rndf(8, 24), cy = rndf(8, 24);
        float angles[8];
        for (int i = 0; i < n; i++)
            angles[i] = rndf(0, 6.2831f);
        for (int i = 1; i < n; i++)
        {
            float key = angles[i];
            int j = i - 1;
            while (j >= 0 && angles[j] > key)
            {
                angles[j + 1] = angles[j];
                j--;
            }
            angles[j + 1] = key;
        }
        for (int i = 0; i < n; i++)
        {
            float r = rndf(2, 9);
            pts[i].x = cx + r * cosf(angles[i]);
            pts[i].y = cy + r * sinf(angles[i]);
        }
        int err = max_error_against_exact(32, 32, pts, n, (k & 1) ? ZV_FILL_EVENODD : ZV_FILL_NONZERO);
        if (err > worst)
            worst = err;
    }
    printf("  40 random convex polygons: max error %d/255\n", worst);
    CHECK(worst <= 2);

    /* thin random triangles (coverage below one pixel everywhere) */
    worst = 0;
    for (int k = 0; k < 40; k++)
    {
        ZvPoint pts[3];
        pts[0].x = rndf(1, 30);
        pts[0].y = rndf(1, 30);
        pts[1].x = rndf(1, 30);
        pts[1].y = rndf(1, 30);
        pts[2].x = pts[1].x + rndf(-0.4f, 0.4f);
        pts[2].y = pts[1].y + rndf(-0.4f, 0.4f);
        int err = max_error_against_exact(32, 32, pts, 3, ZV_FILL_NONZERO);
        if (err > worst)
            worst = err;
    }
    printf("  40 random thin triangles: max error %d/255\n", worst);
    CHECK(worst <= 1);
}

static void test_fill_rules(void)
{
    static const ZvPoint star[5] = {{32, 4}, {48.458f, 54.652f}, {5.370f, 23.348f}, {58.630f, 23.348f}, {15.542f, 54.652f}};
    ZvSurface nz = fill_polygon(64, 64, star, 5, ZV_FILL_NONZERO, 0xFF000000u, NULL);
    ZvSurface eo = fill_polygon(64, 64, star, 5, ZV_FILL_EVENODD, 0xFF000000u, NULL);
    CHECK(alpha_at(&nz, 32, 30) == 255);
    CHECK(alpha_at(&eo, 32, 30) == 0);
    CHECK(alpha_at(&nz, 32, 10) == 255 && alpha_at(&eo, 32, 10) == 255);
    CHECK(alpha_at(&nz, 1, 1) == 0 && alpha_at(&eo, 1, 1) == 0);
    /* outside the centre pentagon the two agree everywhere */
    int differ = 0;
    for (int y = 0; y < 64; y++)
    {
        for (int x = 0; x < 64; x++)
        {
            if (alpha_at(&nz, x, y) != alpha_at(&eo, x, y))
                differ++;
        }
    }
    CHECK(differ > 100 && differ < 900);
    zv_surface_release(&nz);
    zv_surface_release(&eo);

    /* two overlapping squares in one path, same orientation: nonzero fills
       both, evenodd leaves the overlap empty; opposite orientation: nonzero
       also leaves the overlap empty */
    ZvSurface s;
    zv_surface_init(&s, NULL, 20, 20);
    ZvPolyline poly;
    zv_polyline_init(&poly, NULL);
    zv_polyline_add_contour(&poly, true);
    zv_polyline_add_point(&poly, 2, 2);
    zv_polyline_add_point(&poly, 12, 2);
    zv_polyline_add_point(&poly, 12, 12);
    zv_polyline_add_point(&poly, 2, 12);
    zv_polyline_add_contour(&poly, true);
    zv_polyline_add_point(&poly, 8, 8);
    zv_polyline_add_point(&poly, 18, 8);
    zv_polyline_add_point(&poly, 18, 18);
    zv_polyline_add_point(&poly, 8, 18);
    CHECK(zv_fill_polyline_solid(&s, &poly, ZV_FILL_NONZERO, 0xFF000000u, NULL, NULL));
    CHECK(alpha_at(&s, 10, 10) == 255 && alpha_at(&s, 4, 4) == 255 && alpha_at(&s, 15, 15) == 255);
    memset(s.pixels, 0, sizeof(ZvPixel) * 400);
    CHECK(zv_fill_polyline_solid(&s, &poly, ZV_FILL_EVENODD, 0xFF000000u, NULL, NULL));
    CHECK(alpha_at(&s, 10, 10) == 0 && alpha_at(&s, 4, 4) == 255 && alpha_at(&s, 15, 15) == 255);
    poly.contours[1].first = 4;
    ZvPoint tmp = poly.points[5];
    poly.points[5] = poly.points[7];
    poly.points[7] = tmp;
    memset(s.pixels, 0, sizeof(ZvPixel) * 400);
    CHECK(zv_fill_polyline_solid(&s, &poly, ZV_FILL_NONZERO, 0xFF000000u, NULL, NULL));
    CHECK(alpha_at(&s, 10, 10) == 0 && alpha_at(&s, 4, 4) == 255 && alpha_at(&s, 15, 15) == 255);
    zv_polyline_release(&poly);
    zv_surface_release(&s);

    /* two rectangles sharing an edge in one path: no seam */
    zv_surface_init(&s, NULL, 20, 10);
    zv_polyline_init(&poly, NULL);
    zv_polyline_add_contour(&poly, true);
    zv_polyline_add_point(&poly, 1, 1);
    zv_polyline_add_point(&poly, 10.5f, 1);
    zv_polyline_add_point(&poly, 10.5f, 9);
    zv_polyline_add_point(&poly, 1, 9);
    zv_polyline_add_contour(&poly, true);
    zv_polyline_add_point(&poly, 10.5f, 1);
    zv_polyline_add_point(&poly, 19, 1);
    zv_polyline_add_point(&poly, 19, 9);
    zv_polyline_add_point(&poly, 10.5f, 9);
    CHECK(zv_fill_polyline_solid(&s, &poly, ZV_FILL_NONZERO, 0xFF000000u, NULL, NULL));
    CHECK(alpha_at(&s, 10, 5) == 255);
    zv_polyline_release(&poly);
    zv_surface_release(&s);
}

/* Equal up to 1 per channel: clipping splits edges at the box sides and the
   split point is rounded to 1/256 pixel. */
static bool surfaces_equal_in(const ZvSurface *a, const ZvSurface *b, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++)
        {
            ZvPixel p = a->pixels[y * a->stride + x], q = b->pixels[y * b->stride + x];
            for (int shift = 0; shift < 32; shift += 8)
            {
                if (abs((int)((p >> shift) & 0xFF) - (int)((q >> shift) & 0xFF)) > 1)
                    return false;
            }
        }
    }
    return true;
}

static bool surface_blank_outside(const ZvSurface *s, int x0, int y0, int x1, int y1)
{
    for (int y = 0; y < s->height; y++)
    {
        for (int x = 0; x < s->width; x++)
        {
            bool inside = x >= x0 && x < x1 && y >= y0 && y < y1;
            if (!inside && s->pixels[y * s->stride + x] != 0)
                return false;
        }
    }
    return true;
}

static void test_clipping(void)
{
    /* a shape crossing all four sides of the clip box: clipped drawing equals
       the unclipped drawing inside the box and nothing outside */
    static const ZvPoint big[6] = {{-10.5f, 5.25f}, {20.75f, -8.5f}, {45.5f, 10.5f}, {38.25f, 44.5f}, {12.5f, 50.75f}, {-6.5f, 30.5f}};
    ZvSurface full = fill_polygon(40, 40, big, 6, ZV_FILL_NONZERO, 0xFF3060C0u, NULL);
    ZvBounds clip = {8.2f, 6.9f, 30.5f, 33.0f};
    ZvSurface part = fill_polygon(40, 40, big, 6, ZV_FILL_NONZERO, 0xFF3060C0u, &clip);
    CHECK(surfaces_equal_in(&full, &part, 8, 6, 31, 33));
    CHECK(surface_blank_outside(&part, 8, 6, 31, 33));
    CHECK(alpha_at(&full, 0, 20) == 255 && alpha_at(&full, 39, 20) == 255);
    zv_surface_release(&full);
    zv_surface_release(&part);

    /* same with evenodd and a self-intersecting star crossing the sides */
    static const ZvPoint star[5] = {{20, -20}, {45, 50}, {-15, 10}, {55, 10}, {-5, 50}};
    full = fill_polygon(40, 40, star, 5, ZV_FILL_EVENODD, 0xFFFF0000u, NULL);
    ZvBounds c2 = {5, 3, 33, 37};
    part = fill_polygon(40, 40, star, 5, ZV_FILL_EVENODD, 0xFFFF0000u, &c2);
    CHECK(surfaces_equal_in(&full, &part, 5, 3, 33, 37));
    CHECK(surface_blank_outside(&part, 5, 3, 33, 37));
    zv_surface_release(&part);
    /* and against a crop of a bigger rendering (the clip to the surface itself) */
    ZvPoint shifted[5];
    for (int i = 0; i < 5; i++)
    {
        shifted[i].x = star[i].x + 30;
        shifted[i].y = star[i].y + 30;
    }
    ZvSurface wide = fill_polygon(100, 100, shifted, 5, ZV_FILL_EVENODD, 0xFFFF0000u, NULL);
    bool same = true;
    for (int y = 0; y < 40 && same; y++)
    {
        for (int x = 0; x < 40; x++)
        {
            int p = (int)(full.pixels[y * 40 + x] >> 24), q = (int)(wide.pixels[(y + 30) * 100 + x + 30] >> 24);
            if (abs(p - q) > 1)
            {
                same = false;
                break;
            }
        }
    }
    CHECK(same);
    zv_surface_release(&wide);
    zv_surface_release(&full);

    /* shapes entirely outside change nothing; a huge shape covers everything */
    static const ZvPoint away[3] = {{-100, -100}, {-50, -100}, {-50, -50}};
    ZvSurface s = fill_polygon(10, 10, away, 3, ZV_FILL_NONZERO, 0xFF000000u, NULL);
    CHECK(surface_blank_outside(&s, 0, 0, 0, 0));
    zv_surface_release(&s);
    static const ZvPoint right[3] = {{100, 1}, {150, 1}, {150, 5}};
    s = fill_polygon(10, 10, right, 3, ZV_FILL_NONZERO, 0xFF000000u, NULL);
    CHECK(surface_blank_outside(&s, 0, 0, 0, 0));
    zv_surface_release(&s);
    static const ZvPoint huge[4] = {{-1e6f, -1e6f}, {1e6f, -1e6f}, {1e6f, 1e6f}, {-1e6f, 1e6f}};
    s = fill_polygon(10, 10, huge, 4, ZV_FILL_NONZERO, 0xFF000000u, NULL);
    bool all = true;
    for (int i = 0; i < 100; i++)
        all = all && s.pixels[i] == 0xFF000000u;
    CHECK(all);
    zv_surface_release(&s);
    /* a shape left of the surface whose right edge crosses it */
    static const ZvPoint left[4] = {{-100, 2}, {4.5f, 2}, {4.5f, 7}, {-100, 7}};
    s = fill_polygon(10, 10, left, 4, ZV_FILL_NONZERO, 0xFF000000u, NULL);
    CHECK(alpha_at(&s, 0, 4) == 255 && alpha_at(&s, 3, 4) == 255 && alpha_at(&s, 4, 4) == 128 && alpha_at(&s, 5, 4) == 0);
    zv_surface_release(&s);
    /* an empty clip draws nothing */
    ZvBounds none = {5, 5, 5, 5};
    s = fill_polygon(10, 10, huge, 4, ZV_FILL_NONZERO, 0xFF000000u, &none);
    CHECK(surface_blank_outside(&s, 0, 0, 0, 0));
    zv_surface_release(&s);
}

typedef struct
{
    int count;
    int last_y, last_x_end;
    bool ordered;
    bool valid;
    long long total;
} Record;

static void record_solid(void *context, int y, int x, int length, uint32_t coverage)
{
    Record *r = context;
    r->count++;
    if (y < r->last_y || (y == r->last_y && x < r->last_x_end))
        r->ordered = false;
    if (coverage == 0 || coverage > 255 || length <= 0)
        r->valid = false;
    r->last_y = y;
    r->last_x_end = x + length;
    r->total += (long long)coverage * length;
}

static void record_mask(void *context, int y, int x, int length, const uint8_t *coverage)
{
    Record *r = context;
    r->count++;
    if (y < r->last_y || (y == r->last_y && x < r->last_x_end))
        r->ordered = false;
    if (length <= 0)
        r->valid = false;
    for (int i = 0; i < length; i++)
        r->total += coverage[i];
    r->last_y = y;
    r->last_x_end = x + length;
}

static void test_sink_and_degenerate(void)
{
    ZvRasterizer r;
    zv_rasterizer_init(&r, NULL);
    zv_rasterizer_set_clip(&r, 0, 0, 50, 50);
    Record rec = {0, -1, 0, true, true, 0};
    ZvSpanSink sink = {record_solid, record_mask, &rec};

    /* nothing added: nothing reported */
    CHECK(zv_rasterizer_sweep(&r, ZV_FILL_NONZERO, &sink));
    CHECK(rec.count == 0);

    /* only horizontal edges, a single point, NaN: nothing */
    zv_rasterizer_add_line(&r, 1, 1, 10, 1);
    zv_rasterizer_add_line(&r, 3, 3, 3, 3);
    zv_rasterizer_add_line(&r, NAN, 1, 5, 5);
    zv_rasterizer_add_line(&r, 1, 1, 5, INFINITY);
    CHECK(zv_rasterizer_sweep(&r, ZV_FILL_NONZERO, &sink));
    CHECK(rec.count == 0);

    /* a rotated square: spans ordered, coverage total equals the area */
    zv_rasterizer_reset(&r);
    zv_rasterizer_add_line(&r, 25, 5, 45, 25);
    zv_rasterizer_add_line(&r, 45, 25, 25, 45);
    zv_rasterizer_add_line(&r, 25, 45, 5, 25);
    zv_rasterizer_add_line(&r, 5, 25, 25, 5);
    CHECK(zv_rasterizer_sweep(&r, ZV_FILL_NONZERO, &sink));
    CHECK(rec.count > 40 && rec.ordered && rec.valid);
    double area = 800.0; /* diagonal 40: area = 40 * 40 / 2 */
    CHECK(fabs((double)rec.total / 255.0 - area) < 1.0);

    /* the rasterizer is reusable: same result twice; reset drops the edges */
    Record again = {0, -1, 0, true, true, 0};
    sink.context = &again;
    CHECK(zv_rasterizer_sweep(&r, ZV_FILL_NONZERO, &sink));
    CHECK(again.total == rec.total && again.count == rec.count);
    zv_rasterizer_reset(&r);
    Record none = {0, -1, 0, true, true, 0};
    sink.context = &none;
    CHECK(zv_rasterizer_sweep(&r, ZV_FILL_NONZERO, &sink));
    CHECK(none.count == 0);

    /* an inverted clip box draws nothing and does not crash */
    zv_rasterizer_set_clip(&r, 10, 10, 5, 5);
    zv_rasterizer_add_line(&r, 0, 0, 20, 20);
    zv_rasterizer_add_line(&r, 20, 20, 0, 20);
    zv_rasterizer_add_line(&r, 0, 20, 0, 0);
    CHECK(zv_rasterizer_sweep(&r, ZV_FILL_NONZERO, &sink));
    CHECK(none.count == 0);
    zv_rasterizer_release(&r);
}

typedef struct
{
    int remaining;
} Limited;

static void *limited_allocate(void *context, size_t size)
{
    Limited *l = context;
    if (l->remaining <= 0)
        return NULL;
    l->remaining--;
    return malloc(size);
}

static void limited_release(void *context, void *memory)
{
    (void)context;
    free(memory);
}

static void test_out_of_memory(void)
{
    Limited l = {0};
    ZvAllocator alloc = {limited_allocate, limited_release, &l};
    ZvRasterizer r;
    zv_rasterizer_init(&r, &alloc);
    zv_rasterizer_set_clip(&r, 0, 0, 100, 100);
    for (int i = 0; i < 50; i++)
        zv_rasterizer_add_line(&r, (float)i, 0, 50.0f + (float)i, 100);
    Record rec = {0, -1, 0, true, true, 0};
    ZvSpanSink sink = {record_solid, record_mask, &rec};
    CHECK(!zv_rasterizer_sweep(&r, ZV_FILL_NONZERO, &sink));
    CHECK(rec.count == 0);
    zv_rasterizer_reset(&r);
    l.remaining = 1; /* cells fit the first block, the sort does not */
    zv_rasterizer_add_line(&r, 1, 1, 5, 5);
    zv_rasterizer_add_line(&r, 5, 5, 1, 5);
    zv_rasterizer_add_line(&r, 1, 5, 1, 1);
    CHECK(!zv_rasterizer_sweep(&r, ZV_FILL_NONZERO, &sink));
    l.remaining = 100;
    CHECK(zv_rasterizer_sweep(&r, ZV_FILL_NONZERO, &sink));
    CHECK(rec.count > 0);
    zv_rasterizer_release(&r);
}

static char *scene_text(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, ZV_DIR "/scenes/%s.scene", name);
    return zv_io_read_text(path);
}

static bool load_reference(const char *name, ZvBitmap *out)
{
    char path[512];
    snprintf(path, sizeof path, ZV_DIR "/refs/%s.bmp", name);
    return zv_bitmap_load_bmp(out, path);
}

static void test_reference(const char *name, int tolerance, int max_over, int max_over_8_permille)
{
    char *text = scene_text(name);
    CHECK(text != NULL);
    if (!text)
        return;
    ZvBitmap mine, ref;
    char err[256];
    bool ok = zv_scene_run(text, &mine, err, sizeof err);
    if (!ok)
        printf("  %s: %s\n", name, err);
    CHECK(ok);
    CHECK(load_reference(name, &ref));

    ZvCompare r, r8;
    CHECK(zv_compare(&mine, &ref, tolerance, &r, NULL));
    CHECK(zv_compare(&mine, &ref, 8, &r8, NULL));
    long long permille = r8.pixels ? r8.over_tolerance * 1000 / r8.pixels : 0;
    printf("  %-16s max channel diff %3d, over %d: %lld, over 8: %lld of %lld (%lld permille)\n", name, r.max_channel, tolerance,
           r.over_tolerance, r8.over_tolerance, r8.pixels, permille);
    CHECK(r.over_tolerance <= max_over);
    CHECK(permille <= max_over_8_permille);
    zv_io_free(text);
    zv_bitmap_free(&mine);
    zv_bitmap_free(&ref);
}

/* How far the browser itself is from the exact coverage: the reference is
   not the truth, it is another renderer. */
static void browser_error(const char *name, const ZvPoint *pts, int n, uint32_t fill, uint32_t background)
{
    ZvBitmap ref;
    CHECK(load_reference(name, &ref));
    int worst_browser = 0;
    ZvSurface mine = fill_polygon(ref.width, ref.height, pts, n, ZV_FILL_NONZERO, 0xFF000000u, NULL);
    int worst_mine = 0;
    for (int y = 0; y < ref.height; y++)
    {
        for (int x = 0; x < ref.width; x++)
        {
            double exact = pixel_area(pts, n, x, y);
            /* red channel of the straight colour over the background */
            int fr = (int)((fill >> 16) & 0xFF), br = (int)((background >> 16) & 0xFF);
            int expect = (int)floor(br + (fr - br) * exact + 0.5);
            int got = (int)((ref.pixels[y * ref.stride + x] >> 16) & 0xFF);
            int d = abs(got - expect);
            if (d > worst_browser)
                worst_browser = d;
            int cov = alpha_at(&mine, x, y);
            int got_mine = (int)floor(br + (fr - br) * (cov / 255.0) + 0.5);
            d = abs(got_mine - expect);
            if (d > worst_mine)
                worst_mine = d;
        }
    }
    printf("  %-16s against exact coverage: browser max %d, zen_vector max %d\n", name, worst_browser, worst_mine);
    CHECK(worst_mine <= 2);
    zv_surface_release(&mine);
    zv_bitmap_free(&ref);
}

static void test_browser_against_exact(void)
{
    static const ZvPoint tri[3] = {{4.5f, 4.5f}, {27.5f, 6.25f}, {9.75f, 27.5f}};
    browser_error("tri_subpixel", tri, 3, 0x204080u, 0xFFFFFFu);
    static const ZvPoint sliver[3] = {{2, 2}, {60, 28}, {60, 28.6f}};
    browser_error("sliver", sliver, 3, 0x000000u, 0xFFFFFFu);
}

int main(void)
{
    test_exact_rect();
    test_exact_polygons();
    test_fill_rules();
    test_clipping();
    test_sink_and_degenerate();
    test_out_of_memory();

    printf("against the browser:\n");
    test_reference("rect_opaque", 0, 0, 0);
    test_reference("rect_subpixel", 1, 0, 0);
    test_reference("tri_subpixel", 32, 0, 10);
    test_reference("overlap_edges", 32, 0, 10);
    test_reference("sliver", 32, 0, 20);
    test_reference("star_nonzero", 48, 0, 25);
    test_reference("star_evenodd", 48, 2, 30);
    test_reference("circle_cubic", 48, 0, 20);
    test_reference("quad_blob", 48, 0, 25);
    test_reference("translucent", 8, 0, 0);
    test_reference("offscreen", 32, 0, 10);
    test_browser_against_exact();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
