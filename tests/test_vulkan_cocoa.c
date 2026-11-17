/*
 * test_vulkan_cocoa.c - the Vulkan surface on macOS (phase 8): the instance extensions the window system needs (VK_KHR_surface and
 * VK_EXT_metal_surface), what vulkan_create_surface refuses, and, where a Vulkan loader with MoltenVK is installed (the CI machine
 * has none, so there it is said and skipped), a real instance and a surface made on a window, whose view must then have a
 * CAMetalLayer as its layer, as large as the view and at the display's scale, that follows a resize.
 *
 * WRITTEN, NEVER COMPILED OR RUN: made when no run of the macOS job was allowed (see docs/MAC-PORT.md); checked only for
 * syntax against stand-in headers. Skips (77) without a graphical session, unless ZP_REQUIRE_DISPLAY is set.
 */
#include "cocoa_test_util.h"

/* the little of Vulkan the test needs: layouts and values of vulkan_core.h */
typedef struct
{
    int sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} VkAppInfo;

typedef struct
{
    int sType;
    const void *pNext;
    uint32_t flags;
    const VkAppInfo *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstInfo;

typedef int (*PfnCreateInstance)(const VkInstInfo *, const void *, void **);
typedef void (*PfnDestroyInstance)(void *, const void *);
typedef void (*PfnDestroySurface)(void *, uint64_t, const void *);
typedef void *(*PfnGetInstanceProcAddr)(void *, const char *);

#define VK_STRUCTURE_TYPE_APPLICATION_INFO 0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 1u

static void test_extensions(void)
{
    uint32_t n = 0;
    const char *const *ext = vulkan_instance_extensions(&n);
    CHECK(ext != NULL && n == 2);
    if (ext && n == 2)
        CHECK(strcmp(ext[0], "VK_KHR_surface") == 0 && strcmp(ext[1], "VK_EXT_metal_surface") == 0);
}

static void test_refused(void)
{
    WindowConfig gl = {.title = "gl", .width = 100, .height = 80, .render = RENDER_GL};
    PlatformWindow *g = window_create(&gl);
    uint64_t surface = 0;
    if (g)
    {
        platform_clear_error();
        CHECK(!vulkan_create_surface(g, (void *)1, NULL, &surface));
        CHECK(strstr(platform_get_error(), "RENDER_VULKAN") != NULL);
        window_destroy(g);
    }
    WindowConfig vk = {.title = "vk", .width = 100, .height = 80, .render = RENDER_VULKAN};
    PlatformWindow *w = window_create(&vk);
    CHECK(w != NULL);
    if (!w)
        return;
    platform_clear_error();
    CHECK(!vulkan_create_surface(w, NULL, NULL, &surface)); /* no instance */
    CHECK(platform_get_error()[0] != '\0');
    CHECK(!vulkan_create_surface(NULL, (void *)1, NULL, &surface));
    if (!vulkan_supported())
    {
        /* no loader: an instance that is not one is refused with the reason */
        platform_clear_error();
        CHECK(!vulkan_create_surface(w, (void *)1, NULL, &surface));
        CHECK(strstr(platform_get_error(), "Vulkan") != NULL);
    }
    window_destroy(w);
}

static void test_real_surface(void)
{
    if (!vulkan_supported())
    {
        NOTE("no Vulkan loader with MoltenVK here (%s): the surface itself was not made", platform_get_error());
        return;
    }
    PfnGetInstanceProcAddr gipa = (PfnGetInstanceProcAddr)vulkan_get_proc_addr();
    PfnCreateInstance create_instance = gipa ? (PfnCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
    CHECK(create_instance != NULL);
    if (!create_instance)
        return;
    uint32_t n = 0;
    const char *const *want = vulkan_instance_extensions(&n);
    const char *ext[4];
    uint32_t count = 0;
    for (uint32_t i = 0; i < n && count < 3; i++)
        ext[count++] = want[i];
    ext[count++] = "VK_KHR_portability_enumeration"; /* MoltenVK is a portability implementation */
    VkAppInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "test_vulkan_cocoa", .apiVersion = (1u << 22) | (1u << 12)};
    VkInstInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
                       .pApplicationInfo = &app, .enabledExtensionCount = count, .ppEnabledExtensionNames = ext};
    void *instance = NULL;
    int r = create_instance(&info, NULL, &instance);
    if (r != 0 || !instance)
    {
        NOTE("vkCreateInstance failed (VkResult %d): no surface was tried", r);
        return;
    }
    WindowConfig cfg = {.title = "vulkan surface", .width = 200, .height = 150, .x = 80, .y = 80, .render = RENDER_VULKAN, .resizable = true};
    PlatformWindow *w = window_create(&cfg);
    CHECK(w != NULL);
    if (w)
    {
        pump_for(w, 200);
        uint64_t surface = 0;
        bool ok = vulkan_create_surface(w, instance, NULL, &surface);
        printf("  vulkan_create_surface: %s %s\n", ok ? "made" : "failed:", ok ? "" : platform_get_error());
        CHECK(ok && surface != 0);
        if (ok)
        {
            id view = ns_view(w);
            id layer = msg_id(view, "layer");
            const char *name = layer ? object_getClassName(layer) : "(none)";
            printf("  the view's layer is a %s, wantsLayer %d\n", name, msg_bool(view, "wantsLayer"));
            CHECK(layer != NULL && strstr(name, "CAMetalLayer") != NULL);
            CHECK(msg_bool(view, "wantsLayer"));
            CGRect b = msg_rect(layer, "bounds");
            CHECK(b.size.width == 200.0 && b.size.height == 150.0);
            CHECK(msg_double(layer, "contentsScale") == (double)window_content_scale(w));
            /* it follows a resize, and a second surface on the same window uses the same layer */
            window_set_size(w, 320, 240);
            pump_for(w, 200);
            b = msg_rect(layer, "bounds");
            CHECK(b.size.width == 320.0 && b.size.height == 240.0);
            uint64_t second = 0;
            CHECK(vulkan_create_surface(w, instance, NULL, &second) && second != 0);
            CHECK(msg_id(view, "layer") == layer);
            PfnDestroySurface destroy_surface = (PfnDestroySurface)gipa(instance, "vkDestroySurfaceKHR");
            if (destroy_surface)
            {
                destroy_surface(instance, second, NULL);
                destroy_surface(instance, surface, NULL);
            }
        }
        window_destroy(w);
    }
    PfnDestroyInstance destroy_instance = (PfnDestroyInstance)gipa(instance, "vkDestroyInstance");
    if (destroy_instance)
        destroy_instance(instance, NULL);
}

int main(void)
{
    alarm(60);
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    printf("extensions\n");
    test_extensions();
    printf("refused\n");
    test_refused();
    printf("a real surface\n");
    test_real_surface();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
