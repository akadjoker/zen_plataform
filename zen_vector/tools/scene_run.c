#include "scene.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: scene_run scene.txt out.bmp\n");
        return 2;
    }
    char *text = file_read_text(argv[1]);
    if (!text)
    {
        fprintf(stderr, "cannot read %s: %s\n", argv[1], platform_get_error());
        return 2;
    }
    Framebuffer fb;
    char err[256];
    bool ok = zv_scene_run(text, &fb, err, sizeof err);
    fs_free(text);
    if (!ok)
    {
        fprintf(stderr, "%s: %s\n", argv[1], err);
        return 1;
    }
    bool saved = framebuffer_save_bmp(&fb, argv[2]);
    framebuffer_free(&fb);
    if (!saved)
    {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 2;
    }
    return 0;
}
