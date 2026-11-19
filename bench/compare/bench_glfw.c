/* bench_glfw.c - GLFW 3 side of the comparison. glfwPollEvents is the turn of the event loop. */
#include "bench.h"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>

static Counts *g_c;

const char *lib_name(void)
{
    return "glfw";
}

static void on_cursor(GLFWwindow *w, double x, double y)
{
    (void)w, (void)x, (void)y;
    g_c->motion++;
}

static void on_key(GLFWwindow *w, int key, int scancode, int action, int mods)
{
    (void)w, (void)key, (void)scancode, (void)mods;
    if (action == GLFW_PRESS)
        g_c->key_down++;
    else if (action == GLFW_RELEASE)
        g_c->key_up++;
}

static void on_char(GLFWwindow *w, unsigned int cp)
{
    (void)w, (void)cp;
    g_c->text++;
}

int lib_init(void)
{
    return glfwInit() == GLFW_TRUE;
}

void *lib_window(int gl, int x, int y, int w, int h)
{
    if (gl)
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    }
    else
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *win = glfwCreateWindow(w, h, "bench", NULL, NULL);
    if (!win)
        return NULL;
    if (x >= 0)
        glfwSetWindowPos(win, x, y);
    glfwSetCursorPosCallback(win, on_cursor);
    glfwSetKeyCallback(win, on_key);
    glfwSetCharCallback(win, on_char);
    if (gl)
    {
        glfwMakeContextCurrent(win);
        glfwSwapInterval(0);
    }
    return win;
}

void lib_window_free(void *win)
{
    glfwDestroyWindow(win);
}

void lib_exit(void)
{
    glfwTerminate();
}

void lib_poll(void *win, Counts *c)
{
    (void)win;
    g_c = c;
    glfwPollEvents();
}

Display *lib_display(void *win)
{
    (void)win;
    return glfwGetX11Display();
}

unsigned long lib_xwin(void *win)
{
    return glfwGetX11Window(win);
}
