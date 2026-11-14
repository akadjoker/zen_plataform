/*
 * test_gl_worker_x11.c - gl_worker_*: a second GL context on another thread that shares objects with the
 * window's, created and destroyed over and over while the window's thread keeps drawing. Skips (77) without a
 * display or when the GL stack has no pbuffers.
 */
#include "platform.h"

#include <stdio.h>
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

typedef unsigned (*FnCreateShader)(unsigned type);
typedef void (*FnShaderSource)(unsigned shader, int count, const char *const *src, const int *len);
typedef void (*FnCompileShader)(unsigned shader);
typedef void (*FnGetShaderiv)(unsigned shader, unsigned pname, int *out);
typedef void (*FnDeleteShader)(unsigned shader);
typedef void (*FnGenBuffers)(int n, unsigned *out);
typedef void (*FnBindBuffer)(unsigned target, unsigned buffer);
typedef void (*FnBufferData)(unsigned target, long size, const void *data, unsigned usage);
typedef unsigned char (*FnIsBuffer)(unsigned buffer);
typedef void (*FnDeleteBuffers)(int n, const unsigned *buffers);
typedef void (*FnGenTextures)(int n, unsigned *out);
typedef void (*FnBindTexture)(unsigned target, unsigned tex);
typedef void (*FnTexImage2D)(unsigned target, int level, int internal, int w, int h, int border, unsigned format, unsigned type, const void *data);
typedef void (*FnDeleteTextures)(int n, const unsigned *tex);
typedef void (*FnFinish)(void);
typedef const unsigned char *(*FnGetString)(unsigned name);

static FnCreateShader gl_create_shader;
static FnShaderSource gl_shader_source;
static FnCompileShader gl_compile_shader;
static FnGetShaderiv gl_get_shaderiv;
static FnDeleteShader gl_delete_shader;
static FnGenBuffers gl_gen_buffers;
static FnBindBuffer gl_bind_buffer;
static FnBufferData gl_buffer_data;
static FnIsBuffer gl_is_buffer;
static FnDeleteBuffers gl_delete_buffers;
static FnGenTextures gl_gen_textures;
static FnBindTexture gl_bind_texture;
static FnTexImage2D gl_tex_image_2d;
static FnDeleteTextures gl_delete_textures;
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

typedef struct
{
    GLWorker *worker;
    bool made_current;
    bool has_version;
    bool shader_ok;
    unsigned buffer;
    unsigned texture;
    int rounds;
} Job;

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
    gl_gen_buffers(1, &job->buffer);
    gl_bind_buffer(GL_ARRAY_BUFFER, job->buffer);
    static const char blob[64] = {1};
    gl_buffer_data(GL_ARRAY_BUFFER, sizeof blob, blob, GL_STATIC_DRAW);
    unsigned pixel = 0xff00ff00u;
    gl_gen_textures(1, &job->texture);
    gl_bind_texture(GL_TEXTURE_2D, job->texture);
    gl_tex_image_2d(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
    gl_finish();
    CHECK(gl_worker_make_current(job->worker, false));
    return 0;
}

static void test_shared_objects(PlatformWindow *w)
{
    GLWorker *worker = gl_worker_create(w);
    CHECK(worker != NULL);
    if (!worker)
        return;
    Job job = {.worker = worker, .rounds = 20};
    PlatformThread *t = thread_create(worker_main, &job, "gl-worker");
    CHECK(t != NULL);
    if (!t)
    {
        gl_worker_destroy(worker);
        return;
    }
    /* The window's thread keeps going meanwhile: its own Xlib calls against the worker's GLX calls. */
    for (int i = 0; i < 40; i++)
    {
        window_begin_frame(w);
        window_swap(w);
        time_sleep(2);
    }
    CHECK(thread_join(t) == 0);
    CHECK(job.made_current);
    CHECK(job.has_version);
    CHECK(job.shader_ok);

    window_make_current(w);
    CHECK(job.buffer != 0 && gl_is_buffer(job.buffer));
    gl_delete_buffers(1, &job.buffer);
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
        PlatformThread *t = thread_create(worker_main, &job, "gl-cycle");
        if (!t)
        {
            CHECK(false);
            gl_worker_destroy(worker);
            return;
        }
        if (i % 2)
            window_swap(w);
        thread_join(t);
        made += job.made_current;
        gl_worker_destroy(worker);
        window_make_current(w);
        gl_delete_buffers(1, &job.buffer);
        gl_delete_textures(1, &job.texture);
    }
    CHECK(made == 40);
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
    if (!platform_init())
    {
        printf("skip: no display\n");
        return 77;
    }
    WindowConfig cfg = {.title = "gl worker", .width = 64, .height = 64, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
    PlatformWindow *w = window_create(&cfg);
    if (!w)
    {
        printf("skip: %s\n", platform_get_error());
        return 77;
    }
    LOAD(gl_create_shader, "glCreateShader");
    LOAD(gl_shader_source, "glShaderSource");
    LOAD(gl_compile_shader, "glCompileShader");
    LOAD(gl_get_shaderiv, "glGetShaderiv");
    LOAD(gl_delete_shader, "glDeleteShader");
    LOAD(gl_gen_buffers, "glGenBuffers");
    LOAD(gl_bind_buffer, "glBindBuffer");
    LOAD(gl_buffer_data, "glBufferData");
    LOAD(gl_is_buffer, "glIsBuffer");
    LOAD(gl_delete_buffers, "glDeleteBuffers");
    LOAD(gl_gen_textures, "glGenTextures");
    LOAD(gl_bind_texture, "glBindTexture");
    LOAD(gl_tex_image_2d, "glTexImage2D");
    LOAD(gl_delete_textures, "glDeleteTextures");
    LOAD(gl_finish, "glFinish");
    LOAD(gl_get_string, "glGetString");

    GLWorker *probe = gl_worker_create(w);
    if (!probe)
    {
        printf("skip: no GL worker here (%s)\n", platform_get_error());
        window_destroy(w);
        platform_shutdown();
        return 77;
    }
    gl_worker_destroy(probe);

    window_begin_frame(w);
    test_shared_objects(w);
    test_cycles(w);
    test_null_handles();
    window_destroy(w);
    test_not_gl();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
