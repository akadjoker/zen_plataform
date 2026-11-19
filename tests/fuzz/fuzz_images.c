/*
 * fuzz_images.c - the image decoders on bytes that come from outside: PNG_DECODE (the clipboard's image, any stb format),
 * stbi_load_from_memory of zen_ui's copy of stb_image, and framebuffer_load_bmp on a file with those bytes. Two targets in
 * one file, picked by the first byte of the input (even: png_decode and stbi_load_from_memory, odd: BMP through a file). libFuzzer entry point; tools/fuzz.sh builds it with
 * -fsanitize=fuzzer,address,undefined, tests/fuzz/standalone_main.c feeds it the files of tests/fuzz/corpus/images
 * (regression inputs: every crash found is kept there) when it is built as an ordinary test.
 */
#include "platform.h"
#include "png_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The copy of stb_image that zen_ui instantiates (src/ui/stb_impl.c): a public entry point of the library, linked into this
   target, fed the same bytes as png_decode. */
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp);
void stbi_image_free(void *p);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    const uint8_t *body = data + 1;
    size_t n = size - 1;
    if ((data[0] & 1) == 0)
    {
        bool stb_ok = false;
        if (n <= 0x7FFFFFFF)
        {
            int w, h, comp;
            unsigned char *px = stbi_load_from_memory(body, (int)n, &w, &h, &comp, 4);
            if (px)
            {
                unsigned acc = 0;
                for (long i = 0; i < (long)w * h && i < 4096; i++)
                    acc += px[i * 4];
                (void)acc;
                stbi_image_free(px);
                stb_ok = true;
            }
        }
        Framebuffer fb;
        memset(&fb, 0, sizeof fb);
        if (png_decode(body, n, &fb))
        {
            if (!stb_ok)
                __builtin_trap(); /* the two checks are the same: what the clipboard decodes, zen_ui's copy decodes */
            /* touch every pixel (a size that does not match the buffer is a read past it) and re-encode */
            uint32_t acc = 0;
            for (int y = 0; y < fb.height; y += 1 + fb.height / 64)
                for (int x = 0; x < fb.width; x += 1 + fb.width / 64)
                    acc += fb.pixels[(size_t)y * (size_t)fb.width + (size_t)x];
            if ((size_t)fb.width * (size_t)fb.height < (1u << 22))
            {
                size_t m = 0;
                uint8_t *png = png_encode(&fb, &m);
                free(png);
            }
            framebuffer_free(&fb);
            (void)acc;
        }
        return 0;
    }
    char path[128];
    snprintf(path, sizeof path, "/dev/shm/zp-fuzz-bmp-%d.bmp", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    fwrite(body, 1, n, f);
    fclose(f);
    Framebuffer fb;
    memset(&fb, 0, sizeof fb);
    if (framebuffer_load_bmp(&fb, path))
    {
        uint32_t acc = 0;
        for (int y = 0; y < fb.height; y += 1 + fb.height / 64)
            for (int x = 0; x < fb.width; x += 1 + fb.width / 64)
                acc += fb.pixels[(size_t)y * (size_t)fb.width + (size_t)x];
        (void)acc;
        framebuffer_free(&fb);
    }
    unlink(path);
    return 0;
}
