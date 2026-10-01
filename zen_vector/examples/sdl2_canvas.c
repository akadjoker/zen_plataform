/*
 * zen_vector with SDL2, no zen_platform: a canvas drawn into a streaming
 * ARGB8888 texture every frame.
 *
 *   cc examples/sdl2_canvas.c src/*.c -Iinclude $(sdl2-config --cflags --libs) -lm -O2
 */
#include "zv_canvas.h"

#include <SDL.h>
#include <math.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return 1;
    int width = 800, height = 500;
    SDL_Window *window = SDL_CreateWindow("zen_vector + SDL2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);

    ZvSurface surface;
    zv_surface_init(&surface, NULL, width, height);
    ZvCanvas canvas;
    zv_canvas_init(&canvas, &surface, NULL);

    bool running = true;
    float t = 0.0f;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT)
                running = false;
        }
        t += 0.016f;

        zv_canvas_reset(&canvas);
        zv_canvas_set_fill_color(&canvas, 0xFF101828u);
        zv_canvas_fill_rect(&canvas, 0, 0, (float)width, (float)height);

        zv_canvas_save(&canvas);
        zv_canvas_translate(&canvas, 400, 250);
        zv_canvas_rotate(&canvas, t);
        ZvPaint g = zv_paint_linear(-120, 0, 120, 0);
        zv_paint_add_stop(&g, 0, 0xFFFF7043u);
        zv_paint_add_stop(&g, 1, 0xFF4FC3F7u);
        zv_canvas_set_fill_paint(&canvas, &g);
        zv_canvas_begin_path(&canvas);
        zv_canvas_round_rect(&canvas, -120, -70, 240, 140, (float[]){30}, 1);
        zv_canvas_fill(&canvas, ZV_FILL_NONZERO);
        zv_canvas_set_stroke_color(&canvas, 0xFFFFFFFFu);
        zv_canvas_set_line_width(&canvas, 6);
        zv_canvas_set_line_dash(&canvas, (float[]){18, 10}, 2);
        zv_canvas_set_line_dash_offset(&canvas, -t * 60);
        zv_canvas_stroke(&canvas);
        zv_canvas_restore(&canvas);

        zv_canvas_set_stroke_color(&canvas, 0xFFA5D6A7u);
        zv_canvas_set_line_width(&canvas, 8);
        zv_canvas_set_line_join(&canvas, ZV_JOIN_ROUND);
        zv_canvas_set_line_cap(&canvas, ZV_CAP_ROUND);
        zv_canvas_begin_path(&canvas);
        for (int i = 0; i <= 40; i++)
        {
            float x = 60.0f + (float)i * 17.0f;
            float y = 420.0f + 40.0f * sinf(t * 2.0f + (float)i * 0.35f);
            if (i == 0)
                zv_canvas_move_to(&canvas, x, y);
            else
                zv_canvas_line_to(&canvas, x, y);
        }
        zv_canvas_stroke(&canvas);

        /* the surface is opaque here, so its pixels are plain ARGB: upload as is */
        SDL_UpdateTexture(texture, NULL, surface.pixels, surface.stride * 4);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }
    zv_canvas_release(&canvas);
    zv_surface_release(&surface);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
