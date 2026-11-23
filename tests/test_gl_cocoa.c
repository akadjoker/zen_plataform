/*
 * test_gl_cocoa.c - RENDER_GL on macOS, modelled on test_gl_x11.c: the context a GLConfig asks for (version, profile, depth,
 * stencil, samples), what macOS cannot give refused with a reason, a clear read back, a triangle through a shader, vsync,
 * a window that is resized and moved while it draws, one context drawing to two windows, and 200 windows made and
 * destroyed. The window server's image of the window (CGWindowListCreateImage) is the oracle for what is presented.
 * Skips (77) without a graphical session, unless ZP_REQUIRE_DISPLAY is set.
 */
#define GL_SILENCE_DEPRECATION 1
#include "cocoa_test_util.h"

#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_VENDOR 0x1F00
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_SAMPLE_BUFFERS 0x80A8
#define GL_SAMPLES 0x80A9
#define GL_CONTEXT_FLAGS 0x821E
#define GL_CONTEXT_PROFILE_MASK 0x9126
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_CONTEXT_CORE_PROFILE_BIT 0x1
#define GL_CONTEXT_FLAG_DEBUG_BIT 0x2
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VIEWPORT 0x0BA2
#define GL_FRAMEBUFFER 0x8D40
#define GL_DEPTH 0x1801
#define GL_STENCIL 0x1802
#define GL_BACK_LEFT 0x0402
#define GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE 0x8212
#define GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE 0x8215
#define GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE 0x8216
#define GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE 0x8217
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 0x0004

typedef void (*FnGetIntegerv)(unsigned name, int *out);
typedef const unsigned char *(*FnGetString)(unsigned name);
typedef unsigned (*FnGetError)(void);
typedef void (*FnClearColor)(float r, float g, float b, float a);
typedef void (*FnClear)(unsigned mask);
typedef void (*FnReadPixels)(int x, int y, int w, int h, unsigned format, unsigned type, void *out);
typedef void (*FnViewport)(int x, int y, int w, int h);
typedef void (*FnGetFbAttach)(unsigned target, unsigned attachment, unsigned pname, int *out);
typedef unsigned (*FnCreateShader)(unsigned type);
typedef void (*FnShaderSource)(unsigned shader, int count, const char *const *src, const int *len);
typedef void (*FnCompileShader)(unsigned shader);
typedef void (*FnGetShaderiv)(unsigned shader, unsigned pname, int *out);
typedef void (*FnGetShaderInfoLog)(unsigned shader, int cap, int *len, char *out);
typedef unsigned (*FnCreateProgram)(void);
typedef void (*FnAttachShader)(unsigned program, unsigned shader);
typedef void (*FnLinkProgram)(unsigned program);
typedef void (*FnGetProgramiv)(unsigned program, unsigned pname, int *out);
typedef void (*FnUseProgram)(unsigned program);
typedef void (*FnDeleteObject)(unsigned object);
typedef void (*FnGenObjects)(int n, unsigned *out);
typedef void (*FnBindObject)(unsigned object);
typedef void (*FnBindBuffer)(unsigned target, unsigned buffer);
typedef void (*FnBufferData)(unsigned target, long size, const void *data, unsigned usage);
typedef void (*FnEnableVertexAttribArray)(unsigned index);
typedef void (*FnVertexAttribPointer)(unsigned index, int size, unsigned type, unsigned char normalized, int stride, const void *ptr);
typedef void (*FnDrawArrays)(unsigned mode, int first, int count);
typedef void (*FnDeleteBuffers)(int n, const unsigned *buffers);
typedef void (*FnDeleteVertexArrays)(int n, const unsigned *arrays);

static FnGetIntegerv gl_get_integerv;
static FnGetString gl_get_string;
static FnGetError gl_get_error;
static FnClearColor gl_clear_color;
static FnClear gl_clear;
static FnReadPixels gl_read_pixels;
static FnViewport gl_viewport;
static FnGetFbAttach gl_get_fb_attach;
static FnCreateShader gl_create_shader;
static FnShaderSource gl_shader_source;
static FnCompileShader gl_compile_shader;
static FnGetShaderiv gl_get_shaderiv;
static FnGetShaderInfoLog gl_get_shader_info_log;
static FnCreateProgram gl_create_program;
static FnAttachShader gl_attach_shader;
static FnLinkProgram gl_link_program;
static FnGetProgramiv gl_get_programiv;
static FnUseProgram gl_use_program;
static FnDeleteObject gl_delete_shader, gl_delete_program;
static FnGenObjects gl_gen_vertex_arrays, gl_gen_buffers;
static FnBindObject gl_bind_vertex_array;
static FnBindBuffer gl_bind_buffer;
static FnBufferData gl_buffer_data;
static FnEnableVertexAttribArray gl_enable_vertex_attrib_array;
static FnVertexAttribPointer gl_vertex_attrib_pointer;
static FnDrawArrays gl_draw_arrays;
static FnDeleteBuffers gl_delete_buffers;
static FnDeleteVertexArrays gl_delete_vertex_arrays;

#define LOAD(var, name) (*(void **)&(var) = gl_proc_address(name))

static void load_gl(void)
{
    LOAD(gl_get_integerv, "glGetIntegerv");
    LOAD(gl_get_string, "glGetString");
    LOAD(gl_get_error, "glGetError");
    LOAD(gl_clear_color, "glClearColor");
    LOAD(gl_clear, "glClear");
    LOAD(gl_read_pixels, "glReadPixels");
    LOAD(gl_viewport, "glViewport");
    LOAD(gl_get_fb_attach, "glGetFramebufferAttachmentParameteriv");
    LOAD(gl_create_shader, "glCreateShader");
    LOAD(gl_shader_source, "glShaderSource");
    LOAD(gl_compile_shader, "glCompileShader");
    LOAD(gl_get_shaderiv, "glGetShaderiv");
    LOAD(gl_get_shader_info_log, "glGetShaderInfoLog");
    LOAD(gl_create_program, "glCreateProgram");
    LOAD(gl_attach_shader, "glAttachShader");
    LOAD(gl_link_program, "glLinkProgram");
    LOAD(gl_get_programiv, "glGetProgramiv");
    LOAD(gl_use_program, "glUseProgram");
    LOAD(gl_delete_shader, "glDeleteShader");
    LOAD(gl_delete_program, "glDeleteProgram");
    LOAD(gl_gen_vertex_arrays, "glGenVertexArrays");
    LOAD(gl_gen_buffers, "glGenBuffers");
    LOAD(gl_bind_vertex_array, "glBindVertexArray");
    LOAD(gl_bind_buffer, "glBindBuffer");
    LOAD(gl_buffer_data, "glBufferData");
    LOAD(gl_enable_vertex_attrib_array, "glEnableVertexAttribArray");
    LOAD(gl_vertex_attrib_pointer, "glVertexAttribPointer");
    LOAD(gl_draw_arrays, "glDrawArrays");
    LOAD(gl_delete_buffers, "glDeleteBuffers");
    LOAD(gl_delete_vertex_arrays, "glDeleteVertexArrays");
}

static int geti(unsigned name)
{
    int v = 0;
    gl_get_integerv(name, &v);
    return v;
}

static int version(void)
{
    return geti(GL_MAJOR_VERSION) * 10 + geti(GL_MINOR_VERSION);
}

static PlatformWindow *make_sized(GLConfig gl, int w, int h)
{
    WindowConfig cfg = {.title = "test_gl_cocoa", .width = w, .height = h, .x = 50, .y = 50, .gl = gl, .resizable = true};
    PlatformWindow *win = window_create(&cfg);
    if (win)
        load_gl();
    return win;
}

static PlatformWindow *make(GLConfig gl)
{
    return make_sized(gl, 64, 64);
}

/* The first GL error, if any, and the queue emptied. */
static unsigned gl_errors(void)
{
    unsigned first = 0, e;
    while ((e = gl_get_error()) != 0)
        if (!first)
            first = e;
    return first;
}

static void read_pixel(int x, int y, unsigned char out[4])
{
    out[0] = out[1] = out[2] = out[3] = 99;
    gl_read_pixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

static bool pixel_is(int x, int y, int r, int g, int b)
{
    unsigned char p[4];
    read_pixel(x, y, p);
    return abs(p[0] - r) <= 1 && abs(p[1] - g) <= 1 && abs(p[2] - b) <= 1 && p[3] == 255;
}

/* ========================================================================== */

static void test_default(void)
{
    PlatformWindow *w = make((GLConfig){0});
    CHECK(w != NULL);
    if (!w)
    {
        printf("  the default window: %s\n", platform_get_error());
        return;
    }
    printf("  GL_VERSION  %s\n  GL_RENDERER %s\n  GL_VENDOR   %s\n  GLSL        %s\n", (const char *)gl_get_string(GL_VERSION),
           (const char *)gl_get_string(GL_RENDERER), (const char *)gl_get_string(GL_VENDOR),
           (const char *)gl_get_string(GL_SHADING_LANGUAGE_VERSION));
    CHECK(version() >= 33); /* the default is 3.3 core: macOS gives 4.1, which is what any core context of 3.2 or later is */
    CHECK(geti(GL_CONTEXT_PROFILE_MASK) & GL_CONTEXT_CORE_PROFILE_BIT);
    CHECK(!(geti(GL_CONTEXT_FLAGS) & GL_CONTEXT_FLAG_DEBUG_BIT));
    window_set_vsync(w, true);
    window_set_vsync(w, false);
    window_swap(w);
    CHECK(gl_errors() == 0);

    /* the buffer is RGBA8 with depth 24 and stencil 8, double buffered: the library always gives that */
    int red = 0, alpha = 0, depth = 0, stencil = 0;
    gl_get_fb_attach(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &red);
    gl_get_fb_attach(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &alpha);
    gl_get_fb_attach(GL_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depth);
    gl_get_fb_attach(GL_FRAMEBUFFER, GL_STENCIL, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil);
    unsigned err = gl_errors();
    printf("  buffer: red %d, alpha %d, depth %d, stencil %d bits (GL error %u)\n", red, alpha, depth, stencil, err);
    CHECK(err == 0 && red >= 8 && alpha >= 8 && depth >= 24 && stencil >= 8);

    /* the drawable is as big as backend_get_fb_size says (the initial viewport is the size of the drawable) */
    int vp[4] = {0}, fw, fh;
    gl_get_integerv(GL_VIEWPORT, vp);
    window_get_framebuffer_size(w, &fw, &fh);
    printf("  initial viewport %d,%d %dx%d; framebuffer size %dx%d\n", vp[0], vp[1], vp[2], vp[3], fw, fh);
    CHECK(vp[2] == fw && vp[3] == fh);
    window_destroy(w);
}

static void test_profiles(void)
{
    /* core 3.3, 3.2 and 4.1 are the core profile the system has: macOS gives 4.1 for each */
    GLConfig cores[] = {{.profile = GL_PROFILE_CORE, .major = 3, .minor = 3},
                        {.profile = GL_PROFILE_CORE, .major = 3, .minor = 2},
                        {.profile = GL_PROFILE_CORE, .major = 4, .minor = 1},
                        {.profile = GL_PROFILE_CORE}};
    for (size_t i = 0; i < sizeof cores / sizeof cores[0]; i++)
    {
        platform_clear_error();
        PlatformWindow *w = make(cores[i]);
        CHECK(w != NULL);
        if (!w)
        {
            printf("  core %d.%d: %s\n", cores[i].major, cores[i].minor, platform_get_error());
            continue;
        }
        int want = cores[i].major * 10 + cores[i].minor;
        CHECK(version() >= want);
        CHECK(geti(GL_CONTEXT_PROFILE_MASK) & GL_CONTEXT_CORE_PROFILE_BIT);
        window_destroy(w);
    }

    /* the compatibility profile is the legacy 2.1 context */
    PlatformWindow *w = make((GLConfig){.profile = GL_PROFILE_COMPAT, .major = 2, .minor = 1});
    CHECK(w != NULL);
    if (w)
    {
        const char *v = (const char *)gl_get_string(GL_VERSION);
        printf("  compatibility 2.1: %s\n", v ? v : "(null)");
        CHECK(v && strncmp(v, "2.1", 3) == 0);
        window_destroy(w);
    }
    else
        printf("  compatibility 2.1: %s\n", platform_get_error());
    w = make((GLConfig){.profile = GL_PROFILE_COMPAT, .major = 2, .minor = 0});
    CHECK(w != NULL);
    if (w)
        window_destroy(w);
}

/* What macOS cannot give is refused with a reason, never replaced by something else. */
static void test_refused(void)
{
    struct
    {
        GLConfig cfg;
        const char *in_error;
    } cases[] = {
        {{.profile = GL_PROFILE_CORE, .major = 4, .minor = 6}, "core 4.6"},
        {{.profile = GL_PROFILE_CORE, .major = 9, .minor = 9}, "core 9.9"},
        {{.profile = GL_PROFILE_CORE, .major = 4, .minor = 2}, "core 4.2"},
        {{.profile = GL_PROFILE_COMPAT, .major = 3, .minor = 2}, "compatibility 3.2"},
        {{.profile = GL_PROFILE_COMPAT, .major = 2, .minor = 2}, "compatibility 2.2"},
        {{.profile = GL_PROFILE_COMPAT}, "compatibility 3.3"}, /* the default version is 3.3 */
        {{.profile = GL_PROFILE_ES, .major = 3, .minor = 0}, "ES 3.0"},
        {{.profile = GL_PROFILE_ES}, "ES 3.0"},
        {{.debug = true}, "debug"},
        {{.profile = GL_PROFILE_CORE, .major = 3, .minor = 3, .debug = true}, "core 3.3"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        platform_clear_error();
        PlatformWindow *w = make(cases[i].cfg);
        CHECK(w == NULL);
        if (w)
        {
            window_destroy(w);
            continue;
        }
        const char *e = platform_get_error();
        if (!strstr(e, cases[i].in_error))
            printf("  case %zu: the error is \"%s\", wanted it to say \"%s\"\n", i, e, cases[i].in_error);
        CHECK(strstr(e, cases[i].in_error) != NULL);
    }

    /* what the core refuses before a window is made (the checks of the library) */
    platform_clear_error();
    CHECK(make((GLConfig){.profile = (GLProfile)99}) == NULL);
    CHECK(strstr(platform_get_error(), "invalid OpenGL configuration") != NULL);
    CHECK(make((GLConfig){.msaa = -1}) == NULL);
    CHECK(make((GLConfig){.major = -1}) == NULL);

    /* and the failures leave nothing behind: a window with the default config still works */
    for (int i = 0; i < 20; i++)
        CHECK(make((GLConfig){.profile = GL_PROFILE_CORE, .major = 9, .minor = 9}) == NULL);
    PlatformWindow *w = make((GLConfig){0});
    CHECK(w != NULL);
    if (w)
        window_destroy(w);
}

static void test_msaa(void)
{
    platform_clear_error();
    PlatformWindow *w = make((GLConfig){.msaa = 4});
    if (!w)
    {
        printf("  msaa 4: %s\n", platform_get_error());
        CHECK(strstr(platform_get_error(), "4 samples") != NULL);
    }
    else
    {
        printf("  msaa 4: GL_SAMPLE_BUFFERS %d, GL_SAMPLES %d\n", geti(GL_SAMPLE_BUFFERS), geti(GL_SAMPLES));
        CHECK(geti(GL_SAMPLE_BUFFERS) == 1);
        CHECK(geti(GL_SAMPLES) >= 4);
        window_destroy(w);
    }
    platform_clear_error();
    CHECK(make((GLConfig){.msaa = 1024}) == NULL);
    CHECK(strstr(platform_get_error(), "1024 samples") != NULL);
    w = make((GLConfig){.msaa = 0});
    CHECK(w != NULL);
    if (w)
    {
        CHECK(geti(GL_SAMPLE_BUFFERS) == 0);
        window_destroy(w);
    }
}

static void test_clear_readback(void)
{
    PlatformWindow *w = make_sized((GLConfig){0}, 96, 80);
    CHECK(w != NULL);
    if (!w)
        return;
    window_make_current(w);
    int fw, fh;
    window_get_framebuffer_size(w, &fw, &fh);
    gl_viewport(0, 0, fw, fh);
    static const float colours[][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0.25f, 0.5f, 0.75f}};
    for (size_t i = 0; i < sizeof colours / sizeof colours[0]; i++)
    {
        gl_clear_color(colours[i][0], colours[i][1], colours[i][2], 1.0f);
        gl_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        int r = (int)lround(colours[i][0] * 255), g = (int)lround(colours[i][1] * 255), b = (int)lround(colours[i][2] * 255);
        /* the four corners and the middle: the whole drawable is there, in the framebuffer's own size */
        CHECK(pixel_is(0, 0, r, g, b));
        CHECK(pixel_is(fw - 1, 0, r, g, b));
        CHECK(pixel_is(0, fh - 1, r, g, b));
        CHECK(pixel_is(fw - 1, fh - 1, r, g, b));
        CHECK(pixel_is(fw / 2, fh / 2, r, g, b));
        window_swap(w);
    }
    CHECK(gl_errors() == 0);
    window_destroy(w);
}

static unsigned compile(unsigned type, const char *src)
{
    unsigned sh = gl_create_shader(type);
    gl_shader_source(sh, 1, &src, NULL);
    gl_compile_shader(sh);
    int ok = 0;
    gl_get_shaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512] = "";
        gl_get_shader_info_log(sh, sizeof log, NULL, log);
        printf("  shader did not compile: %s\n", log);
        gl_delete_shader(sh);
        return 0;
    }
    return sh;
}

/* A triangle through a shader: the vertex array, the buffer, the program, the draw, all of core 3.3. */
static void test_triangle(void)
{
    PlatformWindow *w = make_sized((GLConfig){0}, 100, 100);
    CHECK(w != NULL);
    if (!w)
        return;
    window_make_current(w);
    int fw, fh;
    window_get_framebuffer_size(w, &fw, &fh);
    gl_viewport(0, 0, fw, fh);
    gl_clear_color(0, 0, 0, 1);
    gl_clear(GL_COLOR_BUFFER_BIT);

    unsigned vs = compile(GL_VERTEX_SHADER, "#version 330 core\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n");
    unsigned fs = compile(GL_FRAGMENT_SHADER, "#version 330 core\nout vec4 c;\nvoid main() { c = vec4(0.0, 1.0, 0.0, 1.0); }\n");
    CHECK(vs && fs);
    if (vs && fs)
    {
        unsigned prog = gl_create_program();
        gl_attach_shader(prog, vs);
        gl_attach_shader(prog, fs);
        gl_link_program(prog);
        int ok = 0;
        gl_get_programiv(prog, GL_LINK_STATUS, &ok);
        CHECK(ok);
        /* a triangle with its corners on the lower left, the lower right and the top middle of the window */
        static const float tri[] = {-1.0f, -1.0f, 1.0f, -1.0f, 0.0f, 1.0f};
        unsigned vao = 0, vbo = 0;
        gl_gen_vertex_arrays(1, &vao);
        gl_bind_vertex_array(vao);
        gl_gen_buffers(1, &vbo);
        gl_bind_buffer(GL_ARRAY_BUFFER, vbo);
        gl_buffer_data(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
        gl_enable_vertex_attrib_array(0);
        gl_vertex_attrib_pointer(0, 2, GL_FLOAT, 0, 0, NULL);
        gl_use_program(prog);
        gl_draw_arrays(GL_TRIANGLES, 0, 3);
        /* green in the middle and near the bottom, black in the top corners, which the triangle leaves out; GL's y is up */
        CHECK(pixel_is(fw / 2, fh / 2, 0, 255, 0));
        CHECK(pixel_is(fw / 2, 3 * fh / 20, 0, 255, 0));
        CHECK(pixel_is(2, fh - 3, 0, 0, 0));
        CHECK(pixel_is(fw - 3, fh - 3, 0, 0, 0));
        CHECK(gl_errors() == 0);
        window_swap(w);
        gl_use_program(0);
        gl_delete_program(prog);
        gl_delete_buffers(1, &vbo);
        gl_delete_vertex_arrays(1, &vao);
    }
    if (vs)
        gl_delete_shader(vs);
    if (fs)
        gl_delete_shader(fs);
    window_destroy(w);
}

/* swap interval: the call is accepted, and the context says what it was given */
static void test_vsync(void)
{
    PlatformWindow *w = make((GLConfig){0});
    CHECK(w != NULL);
    if (!w)
        return;
    id ctx = (id)window_native_handle(w, NATIVE_GL_CONTEXT);
    CHECK(ctx != NULL);
    if (ctx)
    {
        int v = -1;
        window_set_vsync(w, true);
        MSG(void, id, SEL, int *, long)(ctx, sel_registerName("getValues:forParameter:"), &v, 222); /* NSOpenGLContextParameterSwapInterval */
        CHECK(v == 1);
        window_set_vsync(w, false);
        MSG(void, id, SEL, int *, long)(ctx, sel_registerName("getValues:forParameter:"), &v, 222);
        CHECK(v == 0);
    }
    /* a window made with vsync on has it on from the start */
    window_destroy(w);
    WindowConfig cfg = {.title = "vsync", .width = 64, .height = 64, .vsync = true};
    w = window_create(&cfg);
    CHECK(w != NULL);
    if (w)
    {
        id c2 = (id)window_native_handle(w, NATIVE_GL_CONTEXT);
        int v = -1;
        MSG(void, id, SEL, int *, long)(c2, sel_registerName("getValues:forParameter:"), &v, 222);
        CHECK(v == 1);
        window_destroy(w);
    }
}

/* What is presented is what the window server shows: the clear colour, in the window's content, for a colour and then another. */
static void test_presented(void)
{
    PlatformWindow *w = make_sized((GLConfig){0}, 120, 90);
    CHECK(w != NULL);
    if (!w)
        return;
    pump_for(w, 300);
    window_make_current(w);
    Image probe;
    bool have = capture_window(w, &probe);
    if (have)
        image_free(&probe);
    else
        NOTE("the window server's image of the window is not available here (CGWindowListCreateImage gave nothing), what is presented was not compared");
    static const uint32_t colours[] = {0xFFFF0000u, 0xFF0000FFu, 0xFF30A050u};
    for (size_t i = 0; have && i < sizeof colours / sizeof colours[0]; i++)
    {
        int fw, fh;
        window_get_framebuffer_size(w, &fw, &fh);
        gl_viewport(0, 0, fw, fh);
        gl_clear_color(((colours[i] >> 16) & 0xFF) / 255.0f, ((colours[i] >> 8) & 0xFF) / 255.0f, (colours[i] & 0xFF) / 255.0f, 1.0f);
        gl_clear(GL_COLOR_BUFFER_BIT);
        window_swap(w);
        double t0 = time_seconds();
        int best = 255;
        while (time_seconds() - t0 < 3.0)
        {
            pump_for(w, 50);
            Image got;
            if (capture_window(w, &got))
            {
                int d = color_distance(got.px[(size_t)(got.h / 2) * got.w + got.w / 2], colours[i]);
                image_free(&got);
                if (d < best)
                    best = d;
                if (d <= server_tolerance())
                    break;
            }
        }
        printf("  presented %08X: the window server shows it within %d of a channel\n", colours[i], best);
        CHECK(best <= server_tolerance());
    }
    window_destroy(w);
}

/* A window that is resized (and moved) keeps drawing to a drawable of its own size: after each change what is drawn reaches the
   last pixel of the new framebuffer, which a drawable that kept its old size would not have. */
static void test_resize(void)
{
    PlatformWindow *w = make_sized((GLConfig){0}, 100, 80);
    CHECK(w != NULL);
    if (!w)
        return;
    window_make_current(w);
    pump_for(w, 200);
    static const int sizes[][2] = {{300, 200}, {120, 90}, {260, 260}, {100, 80}, {101, 83}};
    for (int i = 0; i < 5; i++)
    {
        window_set_size(w, sizes[i][0], sizes[i][1]);
        int fw = 0, fh = 0;
        double t0 = time_seconds();
        do
        {
            pump_for(w, 10);
            window_get_framebuffer_size(w, &fw, &fh);
        } while (time_seconds() - t0 < 2.0 && ((int)lround((double)window_content_scale(w) * sizes[i][0]) != fw));
        gl_viewport(0, 0, fw, fh);
        gl_clear_color(0.2f, 0.4f, 0.6f, 1.0f);
        gl_clear(GL_COLOR_BUFFER_BIT);
        bool last = pixel_is(fw - 1, fh - 1, 51, 102, 153);
        bool first = pixel_is(0, 0, 51, 102, 153);
        printf("  %dx%d points -> framebuffer %dx%d: first pixel %s, last pixel %s\n", sizes[i][0], sizes[i][1], fw, fh,
               first ? "drawn" : "NOT drawn", last ? "drawn" : "NOT drawn");
        CHECK(first && last);
        window_swap(w);
    }
    /* moved: the context follows (a window that moves to another screen needs the context's update) */
    for (int i = 0; i < 6; i++)
    {
        window_set_position(w, 80 + 20 * i, 80 + 10 * i);
        pump_for(w, 20);
        gl_clear_color(0.9f, 0.1f, 0.1f, 1.0f);
        gl_clear(GL_COLOR_BUFFER_BIT);
        int fw, fh;
        window_get_framebuffer_size(w, &fw, &fh);
        CHECK(pixel_is(fw - 1, fh - 1, 230, 26, 26));
        window_swap(w);
    }
    CHECK(gl_errors() == 0);
    window_destroy(w);
}

/* The size of the drawable follows the window's own scale: the framebuffer is size times scale, and a GL window asks the
   system for the full resolution, so on a scale 2 display the two are 2:1. */
static void test_scale(void)
{
    PlatformWindow *w = make_sized((GLConfig){0}, 128, 96);
    CHECK(w != NULL);
    if (!w)
        return;
    int ww, wh, fw, fh;
    window_get_size(w, &ww, &wh);
    window_get_framebuffer_size(w, &fw, &fh);
    float scale = window_content_scale(w);
    printf("  size %dx%d points, framebuffer %dx%d pixels, scale %.2f\n", ww, wh, fw, fh, (double)scale);
    CHECK(fw == (int)lround((double)ww * (double)scale) && fh == (int)lround((double)wh * (double)scale));
    int vp[4];
    window_make_current(w);
    gl_get_integerv(GL_VIEWPORT, vp);
    CHECK(vp[2] == fw && vp[3] == fh);
    window_destroy(w);
}

/* One context draws to two windows, each keeps what was drawn to it (the test of the X11 backend, as it is). */
static void test_one_context_two_windows(void)
{
    WindowConfig cfg = {.title = "test_gl_cocoa", .width = 64, .height = 64, .x = 40, .y = 120};
    PlatformWindow *first = window_create(&cfg);
    cfg.x = 140;
    PlatformWindow *second = window_create(&cfg);
    CHECK(first != NULL && second != NULL);
    if (!first || !second)
    {
        if (first)
            window_destroy(first);
        if (second)
            window_destroy(second);
        return;
    }
    load_gl();
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(first);
        window_begin_frame(second);
    }
    window_make_current(first);
    gl_clear_color(1.0f, 0.0f, 0.0f, 1.0f);
    gl_clear(GL_COLOR_BUFFER_BIT);

    window_make_current_on(second, first);
    gl_clear_color(0.0f, 0.0f, 1.0f, 1.0f);
    gl_clear(GL_COLOR_BUFFER_BIT);
    CHECK(pixel_is(8, 8, 0, 0, 255));
    window_swap(second);

    /* what a window held before the context left it is not kept by every driver, so draw again before reading */
    window_make_current_on(first, first);
    gl_clear_color(1.0f, 0.0f, 0.0f, 1.0f);
    gl_clear(GL_COLOR_BUFFER_BIT);
    CHECK(pixel_is(8, 8, 255, 0, 0));
    window_swap(first);
    window_swap(second); /* nothing draws to it now: nothing to present, and nothing breaks */

    /* destroying the other window leaves the first context current and drawing */
    window_destroy(second);
    gl_clear_color(0.0f, 1.0f, 0.0f, 1.0f);
    gl_clear(GL_COLOR_BUFFER_BIT);
    CHECK(pixel_is(8, 8, 0, 255, 0));

    /* and the context that was away from its own window when that window went does not break the other one */
    cfg.x = 140;
    PlatformWindow *third = window_create(&cfg);
    CHECK(third != NULL);
    if (third)
    {
        window_make_current_on(third, first); /* first's context draws to third's view */
        gl_clear_color(1.0f, 1.0f, 0.0f, 1.0f);
        gl_clear(GL_COLOR_BUFFER_BIT);
        window_destroy(first); /* the window whose context it is goes first, while it draws to the other */
        window_make_current(third);
        gl_clear_color(0.0f, 1.0f, 1.0f, 1.0f);
        gl_clear(GL_COLOR_BUFFER_BIT);
        CHECK(pixel_is(8, 8, 0, 255, 255));
        window_swap(third);
        window_destroy(third);
    }
    else
        window_destroy(first);
}

static void test_native_handle(void)
{
    PlatformWindow *w = make((GLConfig){0});
    CHECK(w != NULL);
    if (!w)
        return;
    id ctx = (id)window_native_handle(w, NATIVE_GL_CONTEXT);
    CHECK(ctx != NULL);
    if (ctx)
    {
        const char *name = object_getClassName(ctx);
        printf("  NATIVE_GL_CONTEXT is a %s\n", name);
        CHECK(strstr(name, "OpenGLContext") != NULL);
        /* it is the current context of the thread, and it draws to the window's own view */
        CHECK(msg_id(ns_class("NSOpenGLContext"), "currentContext") == ctx);
        CHECK(msg_id(ctx, "view") == ns_view(w));
    }
    CHECK(window_native_handle(w, NATIVE_GL_CONTEXT) == ctx);
    CHECK(window_native_handle(w, NATIVE_WINDOW) != NULL);
    window_destroy(w);
    /* a window that is not RENDER_GL has no context */
    WindowConfig cfg = {.title = "px", .width = 64, .height = 64, .render = RENDER_PIXELS};
    w = window_create(&cfg);
    if (w)
    {
        CHECK(window_native_handle(w, NATIVE_GL_CONTEXT) == NULL);
        window_destroy(w);
    }
}

/* 200 windows made and destroyed with contexts of every kind, a failing request among them: no window and no context is left. */
static void test_cycles(void)
{
    enum { WARM = 20, ROUNDS = 200 };
    id app = ns_app();
    unsigned long windows_before = msg_ulong(msg_id(app, "windows"), "count");
    int made = 0, refused = 0;
    unsigned b0 = 0, b1 = 0;
    size_t h0 = 0, h1 = 0;
    for (int i = 0; i < WARM + ROUNDS; i++)
    {
        if (i == WARM)
            h0 = heap_in_use(&b0);
        GLConfig gl = {0};
        switch (i % 5)
        {
        case 1:
            gl = (GLConfig){.profile = GL_PROFILE_CORE, .major = 3, .minor = 2};
            break;
        case 2:
            gl = (GLConfig){.profile = GL_PROFILE_COMPAT, .major = 2, .minor = 1};
            break;
        case 3:
            gl = (GLConfig){.profile = GL_PROFILE_CORE, .major = 4, .minor = 6}; /* refused */
            break;
        case 4:
            gl = (GLConfig){.msaa = 4}; /* made where the system has it */
            break;
        default:
            break;
        }
        WindowConfig cfg = {.title = "cycle", .width = 80 + i % 7, .height = 60, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED, .gl = gl,
                            .resizable = true};
        PlatformWindow *w = window_create(&cfg);
        if (!w)
        {
            refused++;
            continue;
        }
        made++;
        window_make_current(w);
        gl_clear_color(0.1f, 0.2f, 0.3f, 1.0f);
        gl_clear(GL_COLOR_BUFFER_BIT);
        window_swap(w);
        if (i % 10 == 0)
            pump_for(w, 10);
        window_set_size(w, 90, 70 + i % 5);
        window_destroy(w);
    }
    h1 = heap_in_use(&b1);
    unsigned long windows_after = msg_ulong(msg_id(app, "windows"), "count");
    long grown = (long)h1 - (long)h0;
    printf("  %d rounds (%d windows made, %d refused): heap %zu -> %zu bytes (%+ld, %+.1f per round), blocks %u -> %u; NSApp windows %lu -> %lu\n",
           WARM + ROUNDS, made, refused, h0, h1, grown, (double)grown / ROUNDS, b0, b1, windows_before, windows_after);
    CHECK(windows_after == windows_before);
    CHECK(refused >= (WARM + ROUNDS) / 5 - 2); /* the 4.6 requests, every fifth */
    CHECK(msg_id(ns_class("NSOpenGLContext"), "currentContext") == NULL); /* the last window took its context with it */
#if defined(ZP_ASAN)
    NOTE("AddressSanitizer is on: the heap check is skipped (its quarantine counts as in use)");
#else
    /* A context or a pixel format that is not let go of leaves its object graph behind, a few kilobytes and some dozens of
       blocks each time; what stays after is AppKit's and the driver's own tables. */
    CHECK(grown < 1024 * 1024);
    /* The count of blocks is printed and not checked: on macOS 15 (M3) blocks_in_use grew some 90 a round here while the bytes in
       use stayed and the system's heap tool, asked after 130 and after 630 rounds of windows, counted fewer nodes after the
       longer run (29856 and 27138) and no class of ours growing. The bytes are what a leak cannot hide from. */
#endif
}

int main(void)
{
    alarm(110);
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    PlatformWindow *probe = make((GLConfig){0});
    if (!probe)
    {
        printf("%s: no default OpenGL window here (%s)\n", ZP_REQUIRE ? "FAIL" : "skip", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    window_destroy(probe);

    printf("default\n");
    test_default();
    printf("profiles\n");
    test_profiles();
    printf("refused\n");
    test_refused();
    printf("msaa\n");
    test_msaa();
    printf("clear and read back\n");
    test_clear_readback();
    printf("triangle\n");
    test_triangle();
    printf("vsync\n");
    test_vsync();
    printf("presented\n");
    test_presented();
    printf("resize and move\n");
    test_resize();
    printf("scale\n");
    test_scale();
    printf("one context, several windows\n");
    test_one_context_two_windows();
    printf("native handle\n");
    test_native_handle();
    printf("create and destroy\n");
    test_cycles();

    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
