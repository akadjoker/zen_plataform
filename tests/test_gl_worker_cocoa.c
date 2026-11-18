/*
 * test_gl_worker_cocoa.c - gl_worker_*: a second GL context on another thread that shares objects with the window's, made
 * and destroyed over and over while the window's thread keeps drawing; modelled on test_gl_worker_x11.c, with the sharing
 * checked in both directions and by the contents (a buffer and a texture made on one context are read on the other, from a
 * real second thread). Plain C with no Apple headers.
 * Skips (77) without a graphical session, unless ZP_REQUIRE_DISPLAY is set.
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_VERSION 0x1F02
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

typedef unsigned (*FnCreateShader)(unsigned type);
typedef void (*FnShaderSource)(unsigned shader, int count, const char *const *src, const int *len);
typedef void (*FnCompileShader)(unsigned shader);
typedef void (*FnGetShaderiv)(unsigned shader, unsigned pname, int *out);
typedef void (*FnDeleteShader)(unsigned shader);
typedef void (*FnGenObjects)(int n, unsigned *out);
typedef void (*FnBindBuffer)(unsigned target, unsigned buffer);
typedef void (*FnBufferData)(unsigned target, long size, const void *data, unsigned usage);
typedef void (*FnGetBufferSubData)(unsigned target, long offset, long size, void *out);
typedef unsigned char (*FnIsBuffer)(unsigned buffer);
typedef void (*FnDeleteBuffers)(int n, const unsigned *buffers);
typedef void (*FnBindTexture)(unsigned target, unsigned tex);
typedef void (*FnTexImage2D)(unsigned target, int level, int internal, int w, int h, int border, unsigned format, unsigned type, const void *data);
typedef void (*FnDeleteTextures)(int n, const unsigned *tex);
typedef void (*FnBindFramebuffer)(unsigned target, unsigned fb);
typedef void (*FnFramebufferTexture2D)(unsigned target, unsigned attachment, unsigned textarget, unsigned tex, int level);
typedef unsigned (*FnCheckFramebufferStatus)(unsigned target);
typedef void (*FnDeleteFramebuffers)(int n, const unsigned *fbs);
typedef void (*FnReadPixels)(int x, int y, int w, int h, unsigned format, unsigned type, void *out);
typedef void (*FnFinish)(void);
typedef const unsigned char *(*FnGetString)(unsigned name);

static FnCreateShader gl_create_shader;
static FnShaderSource gl_shader_source;
static FnCompileShader gl_compile_shader;
static FnGetShaderiv gl_get_shaderiv;
static FnDeleteShader gl_delete_shader;
static FnGenObjects gl_gen_buffers, gl_gen_textures, gl_gen_framebuffers;
static FnBindBuffer gl_bind_buffer;
static FnBufferData gl_buffer_data;
static FnGetBufferSubData gl_get_buffer_sub_data;
static FnIsBuffer gl_is_buffer;
static FnDeleteBuffers gl_delete_buffers;
static FnBindTexture gl_bind_texture;
static FnTexImage2D gl_tex_image_2d;
static FnDeleteTextures gl_delete_textures;
static FnBindFramebuffer gl_bind_framebuffer;
static FnFramebufferTexture2D gl_framebuffer_texture_2d;
static FnCheckFramebufferStatus gl_check_framebuffer_status;
static FnDeleteFramebuffers gl_delete_framebuffers;
static FnReadPixels gl_read_pixels;
static FnFinish gl_finish;
static FnGetString gl_get_string;

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

#define LOAD(var, name) (*(void **)&(var) = gl_proc_address(name))

#define BLOB 64

typedef struct
{
    GLWorker *worker;
    bool made_current;
    bool has_version;
    bool shader_ok;
    bool saw_window_buffer;  /* the worker read what the window's context put in a shared buffer */
    unsigned window_buffer;  /* made by the window's context before the worker started */
    unsigned buffer;
    unsigned texture;
    int rounds;
} Job;

static void fill_blob(unsigned char *p, int seed)
{
    for (int i = 0; i < BLOB; i++)
        p[i] = (unsigned char)(seed * 7 + i * 3 + 1);
}

static int worker_main(void *p)
{
    Job *job = p;
    job->made_current = gl_worker_make_current(job->worker, true);
    if (!job->made_current)
        return 1;
    job->has_version = gl_get_string(GL_VERSION) != NULL;
    for (int i = 0; i < job->rounds; i++)
    {
        unsigned sh = gl_create_shader(GL_VERTEX_SHADER);
        const char *src = "#version 330 core\nvoid main() { gl_Position = vec4(0.0); }\n";
        gl_shader_source(sh, 1, &src, NULL);
        gl_compile_shader(sh);
        int ok = 0;
        gl_get_shaderiv(sh, GL_COMPILE_STATUS, &ok);
        job->shader_ok = ok != 0;
        gl_delete_shader(sh);
    }
    /* what the window's context made, seen from here */
    if (job->window_buffer)
    {
        unsigned char got[BLOB], want[BLOB];
        fill_blob(want, 5);
        memset(got, 0, sizeof got);
        gl_bind_buffer(GL_ARRAY_BUFFER, job->window_buffer);
        gl_get_buffer_sub_data(GL_ARRAY_BUFFER, 0, BLOB, got);
        job->saw_window_buffer = memcmp(got, want, BLOB) == 0;
    }
    /* what this context makes, for the window's to read */
    unsigned char blob[BLOB];
    fill_blob(blob, 9);
    gl_gen_buffers(1, &job->buffer);
    gl_bind_buffer(GL_ARRAY_BUFFER, job->buffer);
    gl_buffer_data(GL_ARRAY_BUFFER, BLOB, blob, GL_STATIC_DRAW);
    unsigned pixel = 0xff00ff00u; /* R 0, G 255, B 0, A 255 */
    gl_gen_textures(1, &job->texture);
    gl_bind_texture(GL_TEXTURE_2D, job->texture);
    gl_tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
    gl_finish(); /* the objects are visible to the other context once this one has finished with them */
    CHECK(gl_worker_make_current(job->worker, false));
    return 0;
}

static void test_shared_objects(PlatformWindow *w)
{
    /* a buffer of the window's context, made before the worker starts, for the worker to read */
    unsigned window_buffer = 0;
    unsigned char blob[BLOB];
    fill_blob(blob, 5);
    window_make_current(w);
    gl_gen_buffers(1, &window_buffer);
    gl_bind_buffer(GL_ARRAY_BUFFER, window_buffer);
    gl_buffer_data(GL_ARRAY_BUFFER, BLOB, blob, GL_STATIC_DRAW);
    gl_finish();

    GLWorker *worker = gl_worker_create(w);
    CHECK(worker != NULL);
    if (!worker)
    {
        printf("  gl_worker_create: %s\n", platform_get_error());
        return;
    }
    Job job = {.worker = worker, .rounds = 20, .window_buffer = window_buffer};
    PlatformThread *t = create_thread(worker_main, &job, "gl-worker");
    CHECK(t != NULL);
    if (!t)
    {
        gl_worker_destroy(worker);
        return;
    }
    /* The window's thread keeps going meanwhile: its own frames and swaps against the worker's GL calls. */
    for (int i = 0; i < 40; i++)
    {
        window_begin_frame(w);
        window_swap(w);
        time_sleep(2);
    }
    CHECK(join_thread(t) == 0);
    CHECK(job.made_current);
    CHECK(job.has_version);
    CHECK(job.shader_ok);
    CHECK(job.saw_window_buffer);

    /* what the worker made, read on the window's context: the buffer's bytes and the texture's pixel */
    window_make_current(w);
    CHECK(job.buffer != 0 && gl_is_buffer(job.buffer));
    unsigned char got[BLOB], want[BLOB];
    fill_blob(want, 9);
    memset(got, 0, sizeof got);
    gl_bind_buffer(GL_ARRAY_BUFFER, job.buffer);
    gl_get_buffer_sub_data(GL_ARRAY_BUFFER, 0, BLOB, got);
    CHECK(memcmp(got, want, BLOB) == 0);

    unsigned fbo = 0;
    gl_gen_framebuffers(1, &fbo);
    gl_bind_framebuffer(GL_FRAMEBUFFER, fbo);
    gl_framebuffer_texture_2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, job.texture, 0);
    CHECK(gl_check_framebuffer_status(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    unsigned char px[4] = {1, 1, 1, 1};
    gl_read_pixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    CHECK(px[0] == 0 && px[1] == 255 && px[2] == 0 && px[3] == 255);
    gl_bind_framebuffer(GL_FRAMEBUFFER, 0);
    gl_delete_framebuffers(1, &fbo);

    gl_delete_buffers(1, &job.buffer);
    gl_delete_buffers(1, &window_buffer);
    gl_delete_textures(1, &job.texture);
    gl_worker_destroy(worker);
}

static void test_cycles(PlatformWindow *w)
{
    int made = 0;
    for (int i = 0; i < 40; i++)
    {
        GLWorker *worker = gl_worker_create(w);
        if (!worker)
        {
            CHECK(false);
            return;
        }
        Job job = {.worker = worker, .rounds = 1};
        PlatformThread *t = create_thread(worker_main, &job, "gl-cycle");
        if (!t)
        {
            CHECK(false);
            gl_worker_destroy(worker);
            return;
        }
        if (i % 2)
            window_swap(w);
        join_thread(t);
        made += job.made_current;
        gl_worker_destroy(worker);
        window_make_current(w);
        gl_delete_buffers(1, &job.buffer);
        gl_delete_textures(1, &job.texture);
    }
    CHECK(made == 40);
}

/* several workers at once, each on a thread of its own: they are separate contexts of one share group */
typedef struct
{
    Job job[3];
    PlatformThread *thread[3];
} Group;

static void test_several_workers(PlatformWindow *w)
{
    Group group;
    memset(&group, 0, sizeof group);
    for (int i = 0; i < 3; i++)
    {
        group.job[i].worker = gl_worker_create(w);
        CHECK(group.job[i].worker != NULL);
        group.job[i].rounds = 10;
    }
    for (int i = 0; i < 3; i++)
        if (group.job[i].worker)
            group.thread[i] = create_thread(worker_main, &group.job[i], "gl-group");
    for (int i = 0; i < 20; i++)
    {
        window_begin_frame(w);
        window_swap(w);
        time_sleep(2);
    }
    window_make_current(w);
    for (int i = 0; i < 3; i++)
    {
        if (group.thread[i])
            CHECK(join_thread(group.thread[i]) == 0);
        CHECK(group.job[i].made_current && group.job[i].shader_ok);
        if (group.job[i].buffer)
        {
            CHECK(gl_is_buffer(group.job[i].buffer));
            gl_delete_buffers(1, &group.job[i].buffer);
            gl_delete_textures(1, &group.job[i].texture);
        }
        if (group.job[i].worker)
            gl_worker_destroy(group.job[i].worker);
    }
}

static void test_null_handles(void)
{
    gl_worker_destroy(NULL);
    CHECK(!gl_worker_make_current(NULL, true));
    CHECK(!gl_worker_make_current(NULL, false));
}

static void test_not_gl(void)
{
    WindowConfig cfg = {.title = "pixels", .width = 64, .height = 64, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
    cfg.render = RENDER_PIXELS;
    PlatformWindow *w = window_create(&cfg);
    if (!w)
        return;
    CHECK(gl_worker_create(w) == NULL); /* a window without a GL context has nothing to share */
    window_destroy(w);
}

int main(void)
{
    const char *require = getenv("ZP_REQUIRE_DISPLAY");
    if (!platform_init())
    {
        printf("%s: no display (%s)\n", require ? "FAIL" : "skip", platform_get_error());
        return require ? 1 : 77;
    }
    WindowConfig cfg = {.title = "gl worker", .width = 64, .height = 64, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
    PlatformWindow *w = window_create(&cfg);
    if (!w)
    {
        printf("%s: %s\n", require ? "FAIL" : "skip", platform_get_error());
        return require ? 1 : 77;
    }
    LOAD(gl_create_shader, "glCreateShader");
    LOAD(gl_shader_source, "glShaderSource");
    LOAD(gl_compile_shader, "glCompileShader");
    LOAD(gl_get_shaderiv, "glGetShaderiv");
    LOAD(gl_delete_shader, "glDeleteShader");
    LOAD(gl_gen_buffers, "glGenBuffers");
    LOAD(gl_gen_textures, "glGenTextures");
    LOAD(gl_gen_framebuffers, "glGenFramebuffers");
    LOAD(gl_bind_buffer, "glBindBuffer");
    LOAD(gl_buffer_data, "glBufferData");
    LOAD(gl_get_buffer_sub_data, "glGetBufferSubData");
    LOAD(gl_is_buffer, "glIsBuffer");
    LOAD(gl_delete_buffers, "glDeleteBuffers");
    LOAD(gl_bind_texture, "glBindTexture");
    LOAD(gl_tex_image_2d, "glTexImage2D");
    LOAD(gl_delete_textures, "glDeleteTextures");
    LOAD(gl_bind_framebuffer, "glBindFramebuffer");
    LOAD(gl_framebuffer_texture_2d, "glFramebufferTexture2D");
    LOAD(gl_check_framebuffer_status, "glCheckFramebufferStatus");
    LOAD(gl_delete_framebuffers, "glDeleteFramebuffers");
    LOAD(gl_read_pixels, "glReadPixels");
    LOAD(gl_finish, "glFinish");
    LOAD(gl_get_string, "glGetString");

    GLWorker *probe = gl_worker_create(w);
    if (!probe)
    {
        printf("%s: no GL worker here (%s)\n", require ? "FAIL" : "skip", platform_get_error());
        window_destroy(w);
        platform_shutdown();
        return require ? 1 : 77;
    }
    gl_worker_destroy(probe);

    window_begin_frame(w);
    printf("shared objects\n");
    test_shared_objects(w);
    printf("create and destroy\n");
    test_cycles(w);
    printf("several workers\n");
    test_several_workers(w);
    test_null_handles();
    window_destroy(w);
    test_not_gl();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
