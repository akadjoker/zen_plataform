/*
 * bench_ui_software.c - how long zui_render_software takes for a frame of text and rounded rectangles, headless (no
 * window, no display): a hand-made draw list, the embedded TTF at 16 px. The best of eight runs, in microseconds per
 * frame; compare two builds on the same machine, several runs each (a few percent is the alignment of the code).
 */
#include "platform.h"
#include "zen_ui.h"

#include <stdio.h>
#include <string.h>

#define ROWS 24
#define PER_ROW 4
#define CMDS (ROWS * PER_ROW * 2)

int main(void)
{
    UiFont *font = zui_font_default(16);
    Framebuffer fb;
    if (!font || !framebuffer_alloc(&fb, 1280, 800))
        return 1;
    static const char *label[PER_ROW] = {"A label with some text in it", "Button text", "Check box", "Slide 40.0"};
    static UiCmd cmds[CMDS];
    int n = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < PER_ROW; c++)
        {
            UiRect box = zui_rect(8 + c * 300, 8 + r * 32, 288, 28);
            cmds[n] = (UiCmd){.type = UI_CMD_RECT, .clip = zui_rect(0, 0, 1280, 800), .color = 0xFF3A3F4B};
            cmds[n].u.rect.rect = box;
            cmds[n].u.rect.rounding = 4;
            n++;
            cmds[n] = (UiCmd){.type = UI_CMD_TEXT, .clip = box, .color = 0xFFE0E0E0};
            cmds[n].u.text.x = box.x + 6;
            cmds[n].u.text.y = box.y + 5;
            cmds[n].u.text.str = label[c];
            cmds[n].u.text.len = (int)strlen(label[c]);
            cmds[n].u.text.font = (const struct UiFont *)font;
            n++;
        }
    UiDrawList list = {cmds, n, n, NULL, 0, 0};

    double best = 1e18;
    for (int rep = 0; rep < 8; rep++)
    {
        uint64_t spent = 0;
        for (int f = 0; f < 20; f++)
        {
            draw_clear(&fb, 0xFF202020); /* not timed: it is not the renderer's */
            uint64_t t0 = time_nanos();
            zui_render_software(&fb, &list);
            spent += time_nanos() - t0;
        }
        double t = (double)spent / 1000.0 / 20;
        if (t < best)
            best = t;
    }
    printf("%d commands, software render %.1f us per frame\n", n, best);
    framebuffer_free(&fb);
    zui_font_free(font);
    return 0;
}
