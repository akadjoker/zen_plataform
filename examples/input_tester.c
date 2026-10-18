#include "platform.h"
#include "zen_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 1200
#define WIN_H 688
#define U 34
#define KB_X 12
#define KB_Y 60
#define LOG_N 18
#define GRAPH_N 600
#define TRAIL_N 160

#define C_BG 0xFF15171Au
#define C_PANEL 0xFF1E2126u
#define C_EDGE 0xFF33373Fu
#define C_TEXT 0xFFD8DCE2u
#define C_DIM 0xFF7D8590u
#define C_KEY 0xFF2B2F36u
#define C_DOWN 0xFF2EA043u
#define C_PRESS 0xFFE3B341u
#define C_RELEASE 0xFFDA3633u
#define C_LONG 0xFFF0883Eu
#define C_BLUE 0xFF4493F8u
#define C_BAD 0xFFFF5555u
#define C_GOOD 0xFF3FB950u

typedef struct
{
    const char *label;
    int key;
    float x, y, w, h;
} KeyCap;

typedef struct
{
    double t;
    uint32_t color;
    char text[100];
} LogLine;

static KeyCap g_caps[128];
static int g_ncaps;
static float g_cx, g_cy;

static void cap(const char *label, int key, float w)
{
    g_caps[g_ncaps++] = (KeyCap){label, key, g_cx, g_cy, w, 1};
    g_cx += w;
}

static void cap_tall(const char *label, int key, float w)
{
    g_caps[g_ncaps++] = (KeyCap){label, key, g_cx, g_cy, w, 2};
    g_cx += w;
}

static void row(float y, float x)
{
    g_cy = y;
    g_cx = x;
}

static void build_keyboard(void)
{
    static const char *digits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    static const char *letters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static const char *fkeys[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    static const char *kp[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    row(0, 0);
    cap("Esc", KEY_ESCAPE, 1);
    g_cx += 1;
    for (int i = 0; i < 12; i++)
    {
        cap(fkeys[i], KEY_F1 + i, 1);
        if (i == 3 || i == 7)
            g_cx += 0.5f;
    }
    row(0, 15.5f);
    cap("PrtSc", KEY_PRINT_SCREEN, 1);
    cap("ScrLk", KEY_SCROLL_LOCK, 1);
    cap("Pause", KEY_PAUSE, 1);

    row(1.5f, 0);
    cap("`", KEY_GRAVE, 1);
    for (int i = 0; i < 10; i++)
        cap(digits[i], i == 9 ? KEY_ZERO : KEY_ONE + i, 1);
    cap("-", KEY_MINUS, 1);
    cap("=", KEY_EQUAL, 1);
    cap("Backspace", KEY_BACKSPACE, 2);
    row(1.5f, 15.5f);
    cap("Ins", KEY_INSERT, 1);
    cap("Home", KEY_HOME, 1);
    cap("PgUp", KEY_PAGE_UP, 1);
    row(1.5f, 19);
    cap("Num", KEY_NUM_LOCK, 1);
    cap("/", KEY_KP_DIVIDE, 1);
    cap("*", KEY_KP_MULTIPLY, 1);
    cap("-", KEY_KP_SUBTRACT, 1);

    row(2.5f, 0);
    cap("Tab", KEY_TAB, 1.5f);
    static const int r1[] = {16, 22, 4, 17, 19, 24, 20, 8, 14, 15};
    for (int i = 0; i < 10; i++)
        cap(letters[r1[i]], KEY_A + r1[i], 1);
    cap("[", KEY_LEFT_BRACKET, 1);
    cap("]", KEY_RIGHT_BRACKET, 1);
    cap("\\", KEY_BACKSLASH, 1.5f);
    row(2.5f, 15.5f);
    cap("Del", KEY_DELETE, 1);
    cap("End", KEY_END, 1);
    cap("PgDn", KEY_PAGE_DOWN, 1);
    row(2.5f, 19);
    cap(kp[7], KEY_KP_7, 1);
    cap(kp[8], KEY_KP_8, 1);
    cap(kp[9], KEY_KP_9, 1);
    cap_tall("+", KEY_KP_ADD, 1);

    row(3.5f, 0);
    cap("Caps", KEY_CAPS_LOCK, 1.75f);
    static const int r2[] = {0, 18, 3, 5, 6, 7, 9, 10, 11};
    for (int i = 0; i < 9; i++)
        cap(letters[r2[i]], KEY_A + r2[i], 1);
    cap(";", KEY_SEMICOLON, 1);
    cap("'", KEY_APOSTROPHE, 1);
    cap("Enter", KEY_ENTER, 2.25f);
    row(3.5f, 19);
    cap(kp[4], KEY_KP_4, 1);
    cap(kp[5], KEY_KP_5, 1);
    cap(kp[6], KEY_KP_6, 1);

    row(4.5f, 0);
    cap("Shift", KEY_LEFT_SHIFT, 2.25f);
    static const int r3[] = {25, 23, 2, 21, 1, 13, 12};
    for (int i = 0; i < 7; i++)
        cap(letters[r3[i]], KEY_A + r3[i], 1);
    cap(",", KEY_COMMA, 1);
    cap(".", KEY_PERIOD, 1);
    cap("/", KEY_SLASH, 1);
    cap("Shift", KEY_RIGHT_SHIFT, 2.75f);
    row(4.5f, 16.5f);
    cap("Up", KEY_UP, 1);
    row(4.5f, 19);
    cap(kp[1], KEY_KP_1, 1);
    cap(kp[2], KEY_KP_2, 1);
    cap(kp[3], KEY_KP_3, 1);
    cap_tall("Ent", KEY_KP_ENTER, 1);

    row(5.5f, 0);
    cap("Ctrl", KEY_LEFT_CONTROL, 1.25f);
    cap("Super", KEY_LEFT_SUPER, 1.25f);
    cap("Alt", KEY_LEFT_ALT, 1.25f);
    cap("Space", KEY_SPACE, 6.25f);
    cap("AltGr", KEY_RIGHT_ALT, 1.25f);
    cap("Super", KEY_RIGHT_SUPER, 1.25f);
    cap("Menu", KEY_MENU, 1.25f);
    cap("Ctrl", KEY_RIGHT_CONTROL, 1.25f);
    row(5.5f, 15.5f);
    cap("Left", KEY_LEFT, 1);
    cap("Down", KEY_DOWN, 1);
    cap("Right", KEY_RIGHT, 1);
    row(5.5f, 19);
    cap(kp[0], KEY_KP_0, 2);
    cap(".", KEY_KP_DECIMAL, 1);
}

static const char *key_name(int key)
{
    static char buf[16];
    for (int i = 0; i < g_ncaps; i++)
    {
        if (g_caps[i].key != key)
            continue;
        if (key >= KEY_KP_0 && key <= KEY_KP_EQUAL)
        {
            snprintf(buf, sizeof buf, "KP %s", g_caps[i].label);
            return buf;
        }
        if (key == KEY_RIGHT_SHIFT || key == KEY_RIGHT_CONTROL || key == KEY_RIGHT_SUPER)
        {
            snprintf(buf, sizeof buf, "R%s", g_caps[i].label);
            return buf;
        }
        return g_caps[i].label;
    }
    snprintf(buf, sizeof buf, "#%d", key);
    return buf;
}

static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp = p[0];
    int n = 1;
    if (cp >= 0xF0 && p[1] && p[2] && p[3])
    {
        cp = ((cp & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        n = 4;
    }
    else if (cp >= 0xE0 && p[1] && p[2])
    {
        cp = ((cp & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        n = 3;
    }
    else if (cp >= 0xC0 && p[1])
    {
        cp = ((cp & 0x1F) << 6) | (p[1] & 0x3F);
        n = 2;
    }
    *s += n;
    return cp;
}

static int text(Framebuffer *fb, UiFont *f, int x, int y, uint32_t color, const char *s)
{
    int base = y + zui_font_ascent(f);
    while (*s)
    {
        UiGlyphInfo g;
        if (!zui_font_glyph(f, utf8_next(&s), &g))
            continue;
        for (int j = 0; j < g.h; j++)
        {
            for (int i = 0; i < g.w; i++)
            {
                uint8_t a = g.coverage[j * g.w + i];
                if (a)
                    draw_pixel(fb, x + g.xoff + i, base + g.yoff + j, (color & 0x00FFFFFFu) | ((uint32_t)a << 24),
                               BLEND_ALPHA);
            }
        }
        x += g.advance;
    }
    return x;
}

static int textf(Framebuffer *fb, UiFont *f, int x, int y, uint32_t color, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return text(fb, f, x, y, color, buf);
}

static void utf8_append(char *dst, size_t cap, uint32_t cp)
{
    char enc[5] = {0};
    if (cp < 0x80)
        enc[0] = (char)cp;
    else if (cp < 0x800)
    {
        enc[0] = (char)(0xC0 | (cp >> 6));
        enc[1] = (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        enc[0] = (char)(0xE0 | (cp >> 12));
        enc[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        enc[2] = (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        enc[0] = (char)(0xF0 | (cp >> 18));
        enc[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        enc[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        enc[3] = (char)(0x80 | (cp & 0x3F));
    }
    size_t len = strlen(dst), add = strlen(enc);
    if (len + add + 1 > cap)
    {
        size_t cut = len + add + 1 - cap;
        while (cut < len && ((unsigned char)dst[cut] & 0xC0) == 0x80)
            cut++;
        memmove(dst, dst + cut, len - cut + 1);
    }
    strcat(dst, enc);
}

typedef struct
{
    PlatformWindow *w;
    UiFont *font, *small, *big;
    double start;

    bool ev_key[KEY_MAX];
    double held_since[KEY_MAX];
    int key_events[KEY_MAX];
    bool ev_button[MOUSE_BUTTON_MAX];
    double button_since[MOUSE_BUTTON_MAX];
    double flash_press[KEY_MAX], flash_release[KEY_MAX];

    long presses, releases, repeats, unknown, taps, mismatches, button_events, button_mismatches;
    float wheel_x, wheel_y;
    long ev_dx, ev_dy, md_dx, md_dy;
    bool captured;

    float frame_ms[GRAPH_N], pump_ms[GRAPH_N];
    int graph_at;
    long frames, hitches;
    float worst_ms, worst_pump;

    int trail_x[TRAIL_N], trail_y[TRAIL_N];
    int trail_at, trail_n;

    char typed[160];
    char composition[64];
    LogLine log[LOG_N];
    int log_at;
    bool focused, hovered;
} App;

static void logf_(App *a, uint32_t color, const char *fmt, ...)
{
    LogLine *l = &a->log[a->log_at];
    a->log_at = (a->log_at + 1) % LOG_N;
    l->t = time_seconds() - a->start;
    l->color = color;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->text, sizeof l->text, fmt, ap);
    va_end(ap);
}

static void reset_counters(App *a)
{
    a->presses = a->releases = a->repeats = a->unknown = a->taps = 0;
    a->mismatches = a->button_events = a->button_mismatches = 0;
    a->wheel_x = a->wheel_y = 0;
    a->ev_dx = a->ev_dy = a->md_dx = a->md_dy = 0;
    a->hitches = 0;
    a->worst_ms = a->worst_pump = 0;
    memset(a->key_events, 0, sizeof a->key_events);
    a->typed[0] = '\0';
}

static const char *button_name(int b)
{
    static const char *names[] = {"esquerdo", "direito", "meio", "lateral 1", "lateral 2"};
    return b >= 0 && b < 5 ? names[b] : "?";
}

static void handle_events(App *a, double now)
{
    int downs[KEY_MAX] = {0}, ups[KEY_MAX] = {0};
    Event ev;
    while (poll_event(a->w, &ev))
    {
        switch (ev.type)
        {
        case EVENT_KEY:
        {
            int k = ev.data.key.key;
            if (k <= 0 || k >= KEY_MAX)
            {
                a->unknown++;
                logf_(a, C_DIM, "tecla sem nome (scancode %d) %s", ev.data.key.scancode,
                      ev.data.key.down ? "carregada" : "solta");
                break;
            }
            a->key_events[k]++;
            if (ev.data.key.down && ev.data.key.repeat)
            {
                a->repeats++;
                break;
            }
            if (ev.data.key.down)
            {
                downs[k]++;
                a->presses++;
                a->ev_key[k] = true;
                a->flash_press[k] = now;
                logf_(a, C_PRESS, "%s carregada", key_name(k));
            }
            else
            {
                ups[k]++;
                a->releases++;
                a->ev_key[k] = false;
                a->flash_release[k] = now;
                logf_(a, C_TEXT, "%s solta", key_name(k));
            }
            break;
        }
        case EVENT_CHAR:
            utf8_append(a->typed, sizeof a->typed, ev.data.codepoint);
            break;
        case EVENT_TEXT_EDIT:
            snprintf(a->composition, sizeof a->composition, "%s", ev.data.edit.text);
            break;
        case EVENT_MOUSE_BUTTON:
        {
            int b = ev.data.mouse.button;
            a->button_events++;
            if (b >= 0 && b < MOUSE_BUTTON_MAX)
                a->ev_button[b] = ev.data.mouse.down;
            logf_(a, ev.data.mouse.down ? C_PRESS : C_TEXT, "botao %s %s em %d,%d", button_name(b),
                  ev.data.mouse.down ? "carregado" : "solto", ev.data.mouse.x, ev.data.mouse.y);
            break;
        }
        case EVENT_MOUSE_MOVE:
            if (a->captured)
            {
                a->ev_dx += ev.data.mouse.dx;
                a->ev_dy += ev.data.mouse.dy;
            }
            break;
        case EVENT_MOUSE_WHEEL:
            a->wheel_x += ev.data.wheel.x;
            a->wheel_y += ev.data.wheel.y;
            logf_(a, C_BLUE, "roda %+.2f, %+.2f", ev.data.wheel.x, ev.data.wheel.y);
            break;
        case EVENT_WINDOW_FOCUS:
            a->focused = ev.data.focus.gained;
            if (!ev.data.focus.gained)
            {
                memset(a->ev_key, 0, sizeof a->ev_key);
                memset(a->ev_button, 0, sizeof a->ev_button);
            }
            logf_(a, C_BLUE, ev.data.focus.gained ? "a janela ganhou o foco" : "a janela perdeu o foco");
            break;
        case EVENT_WINDOW_ENTER:
            a->hovered = ev.data.enter.entered;
            logf_(a, C_DIM, ev.data.enter.entered ? "o ponteiro entrou" : "o ponteiro saiu");
            break;
        default:
            break;
        }
    }

    for (int k = 1; k < KEY_MAX; k++)
    {
        if (downs[k] && ups[k])
        {
            a->taps++;
            logf_(a, C_LONG, "%s carregada e solta no mesmo frame", key_name(k));
        }
        if (a->ev_key[k] != key_down(a->w, k))
        {
            a->mismatches++;
            logf_(a, C_BAD, "INCOERENTE: %s %s pelos eventos, %s pelo estado", key_name(k),
                  a->ev_key[k] ? "em baixo" : "em cima", key_down(a->w, k) ? "em baixo" : "em cima");
            a->ev_key[k] = key_down(a->w, k);
        }
        if (key_pressed(a->w, k))
            a->held_since[k] = now;
    }
    for (int b = 0; b < MOUSE_BUTTON_MAX; b++)
    {
        if (a->ev_button[b] != mouse_button_down(a->w, b))
        {
            a->button_mismatches++;
            logf_(a, C_BAD, "INCOERENTE: botao %s", button_name(b));
            a->ev_button[b] = mouse_button_down(a->w, b);
        }
        if (mouse_button_pressed(a->w, b))
            a->button_since[b] = now;
    }
    if (a->captured)
    {
        int dx, dy;
        mouse_delta(a->w, &dx, &dy);
        a->md_dx += dx;
        a->md_dy += dy;
    }
}

static void panel(Framebuffer *fb, int x, int y, int w, int h)
{
    draw_fill_round_rect(fb, x, y, w, h, 6, C_PANEL, BLEND_NONE);
    draw_round_rect(fb, x, y, w, h, 6, C_EDGE, BLEND_NONE);
}

static void draw_keyboard(App *a, Framebuffer *fb, double now)
{
    for (int i = 0; i < g_ncaps; i++)
    {
        KeyCap *c = &g_caps[i];
        int x = KB_X + (int)(c->x * U), y = KB_Y + (int)(c->y * U);
        int w = (int)(c->w * U) - 4, h = (int)(c->h * U) - 4;
        bool down = key_down(a->w, c->key);
        uint32_t fill = down ? C_DOWN : C_KEY;
        if (now - a->flash_press[c->key] < 0.15)
            fill = C_PRESS;
        else if (!down && now - a->flash_release[c->key] < 0.15)
            fill = C_RELEASE;
        draw_fill_round_rect(fb, x, y, w, h, 4, fill, BLEND_NONE);
        double held = now - a->held_since[c->key];
        if (down && held > 10)
        {
            draw_round_rect(fb, x, y, w, h, 4, C_LONG, BLEND_NONE);
            draw_round_rect(fb, x + 1, y + 1, w - 2, h - 2, 4, C_LONG, BLEND_NONE);
        }
        text(fb, a->small, x + 5, y + 4, down ? 0xFF0D1117u : C_TEXT, c->label);
        if (a->key_events[c->key])
            textf(fb, a->small, x + 5, y + h - 16, down ? 0xFF0D1117u : C_DIM, "%d", a->key_events[c->key]);
    }
}

static void draw_status(App *a, Framebuffer *fb)
{
    int x = 16;
    x = text(fb, a->big, x, 14, C_TEXT, "Teste de input");
    x += 24;
    draw_fill_circle(fb, x, 26, 7, a->focused ? C_GOOD : C_BAD, BLEND_NONE);
    x = text(fb, a->font, x + 12, 17, C_TEXT, a->focused ? "com foco" : "sem foco");
    x += 18;
    draw_fill_circle(fb, x, 26, 7, a->hovered ? C_GOOD : C_DIM, BLEND_NONE);
    x = text(fb, a->font, x + 12, 17, C_TEXT, a->hovered ? "ponteiro dentro" : "ponteiro fora");
    x += 18;
    x = textf(fb, a->font, x, 17, a->captured ? C_PRESS : C_TEXT, "rato %s", a->captured ? "CAPTURADO" : "normal");
    x += 18;
    textf(fb, a->font, x, 17, C_TEXT, "texto %s", window_text_input_active(a->w) ? "ligado" : "desligado");
    text(fb, a->small, 16, 44, C_DIM,
         "F1 captura o rato   F2 liga/desliga o texto   F5 zera os contadores   Esc larga o rato   "
         "segura teclas e botoes o tempo que quiseres e ve se o que esta aqui bate com os teus dedos");
}

static void draw_held(App *a, Framebuffer *fb, double now)
{
    int x0 = 12, y0 = 288;
    panel(fb, x0, y0, 782, 124);
    text(fb, a->font, x0 + 12, y0 + 6, C_TEXT, "Em baixo agora (tempo segurado)");
    int x = x0 + 12, y = y0 + 28;
    int count = 0;
    for (int k = 1; k < KEY_MAX; k++)
    {
        if (!key_down(a->w, k))
            continue;
        double held = now - a->held_since[k];
        char line[48];
        snprintf(line, sizeof line, "%s %.1fs", key_name(k), held);
        int tw = zui_text_width(a->font, line, -1) + 16;
        if (x + tw > x0 + 772)
        {
            x = x0 + 12;
            y += 24;
        }
        if (y > y0 + 52)
            break;
        draw_fill_round_rect(fb, x, y, tw - 6, 20, 4, held > 10 ? C_LONG : C_DOWN, BLEND_NONE);
        text(fb, a->font, x + 5, y + 2, 0xFF0D1117u, line);
        x += tw;
        count++;
    }
    for (int b = 0; b < 5; b++)
    {
        if (!mouse_button_down(a->w, b))
            continue;
        char line[48];
        snprintf(line, sizeof line, "rato %s %.1fs", button_name(b), now - a->button_since[b]);
        int tw = zui_text_width(a->font, line, -1) + 16;
        if (x + tw > x0 + 772)
        {
            x = x0 + 12;
            y += 24;
        }
        if (y > y0 + 52)
            break;
        draw_fill_round_rect(fb, x, y, tw - 6, 20, 4, C_BLUE, BLEND_NONE);
        text(fb, a->font, x + 5, y + 2, 0xFF0D1117u, line);
        x += tw;
        count++;
    }
    if (!count)
        text(fb, a->font, x0 + 12, y0 + 30, C_DIM, "nada - se largaste tudo, isto tem de estar vazio");
    textf(fb, a->font, x0 + 12, y0 + 78, C_TEXT, "escrito: %s", a->typed);
    if (a->composition[0])
        textf(fb, a->font, x0 + 12, y0 + 100, C_PRESS, "a compor: %s", a->composition);
    else
        text(fb, a->small, x0 + 12, y0 + 102, C_DIM, "teclas mortas (\xC2\xB4 ~ ^) e o metodo de entrada aparecem aqui enquanto compoem");
}

static void draw_mouse(App *a, Framebuffer *fb, double now)
{
    int x0 = 806, y0 = 60;
    panel(fb, x0, y0, 382, 352);
    text(fb, a->font, x0 + 12, y0 + 8, C_TEXT, "Rato");
    static const char *labels[] = {"Esq", "Dir", "Meio", "X1", "X2"};
    for (int b = 0; b < 5; b++)
    {
        int bx = x0 + 12 + b * 71, by = y0 + 30;
        bool down = mouse_button_down(a->w, b);
        draw_fill_round_rect(fb, bx, by, 66, 32, 5, down ? C_DOWN : C_KEY, BLEND_NONE);
        text(fb, a->font, bx + 8, by + 8, down ? 0xFF0D1117u : C_TEXT, labels[b]);
    }
    int mx, my, dx, dy;
    mouse_position(a->w, &mx, &my);
    mouse_delta(a->w, &dx, &dy);
    textf(fb, a->font, x0 + 12, y0 + 70, C_TEXT, "posicao %d, %d   delta %+d, %+d", mx, my, dx, dy);
    textf(fb, a->font, x0 + 12, y0 + 90, C_TEXT, "roda total  vertical %+.1f   horizontal %+.1f", a->wheel_y, a->wheel_x);

    int cx = x0 + 12, cy = y0 + 114, cw = 358, ch = 160;
    draw_fill_rect(fb, cx, cy, cw, ch, 0xFF101215u, BLEND_NONE);
    draw_rect(fb, cx, cy, cw, ch, C_EDGE, BLEND_NONE);
    int px = cx + ((mx % cw) + cw) % cw, py = cy + ((my % ch) + ch) % ch;
    if (!a->captured)
    {
        int ww, wh;
        window_get_size(a->w, &ww, &wh);
        px = cx + (ww > 0 ? mx * cw / ww : 0);
        py = cy + (wh > 0 ? my * ch / wh : 0);
    }
    a->trail_x[a->trail_at] = px;
    a->trail_y[a->trail_at] = py;
    a->trail_at = (a->trail_at + 1) % TRAIL_N;
    if (a->trail_n < TRAIL_N)
        a->trail_n++;
    for (int i = TRAIL_N - a->trail_n + 1; i < TRAIL_N; i++)
    {
        int p0 = (a->trail_at + i - 1) % TRAIL_N, p1 = (a->trail_at + i) % TRAIL_N;
        if (abs(a->trail_x[p1] - a->trail_x[p0]) < cw / 2 && abs(a->trail_y[p1] - a->trail_y[p0]) < ch / 2)
            draw_line(fb, a->trail_x[p0], a->trail_y[p0], a->trail_x[p1], a->trail_y[p1],
                      (0x40u + (uint32_t)i) << 24 | 0x4493F8u, BLEND_ALPHA);
    }
    draw_fill_circle(fb, px, py, 5, a->captured ? C_PRESS : C_BLUE, BLEND_NONE);
    text(fb, a->small, cx + 6, cy + ch - 18, C_DIM, a->captured ? "capturado: o ponto anda com o movimento" : "o ponteiro na janela");

    int ty = cy + ch + 8;
    if (a->captured)
    {
        bool ok = a->ev_dx == a->md_dx && a->ev_dy == a->md_dy;
        textf(fb, a->font, x0 + 12, ty, C_TEXT, "soma dos eventos  %+ld, %+ld", a->ev_dx, a->ev_dy);
        textf(fb, a->font, x0 + 12, ty + 22, ok ? C_GOOD : C_BAD, "soma do mouse_delta  %+ld, %+ld  %s", a->md_dx, a->md_dy,
              ok ? "certo" : "DIFERENTE");
    }
    else
        text(fb, a->small, x0 + 12, ty + 4, C_DIM, "F1 captura o rato para testar a camara (movimento relativo)");
    (void)now;
}

static void draw_graph(App *a, Framebuffer *fb)
{
    int x0 = 12, y0 = 420, w = 1176, h = 96;
    panel(fb, x0, y0, w, h + 26);
    textf(fb, a->font, x0 + 12, y0 + 6, C_TEXT,
          "tempo de frame (ms)   pior %.1f   frames acima de 50 ms: %ld   tempo a ler o input: pior %.2f ms", a->worst_ms,
          a->hitches, a->worst_pump);
    int gx = x0 + 12, gy = y0 + 24, gw = w - 24, gh = h - 4;
    draw_fill_rect(fb, gx, gy, gw, gh, 0xFF101215u, BLEND_NONE);
    float scale = (float)gh / 70.0f;
    int l16 = gy + gh - (int)(16.7f * scale), l33 = gy + gh - (int)(33.3f * scale), l50 = gy + gh - (int)(50 * scale);
    draw_line(fb, gx, l16, gx + gw, l16, 0xFF2B3A2Bu, BLEND_NONE);
    draw_line(fb, gx, l33, gx + gw, l33, 0xFF3A352Bu, BLEND_NONE);
    draw_line(fb, gx, l50, gx + gw, l50, 0xFF3A2B2Bu, BLEND_NONE);
    for (int i = 0; i < GRAPH_N && i < gw / 2; i++)
    {
        int idx = (a->graph_at - 1 - i + GRAPH_N) % GRAPH_N;
        float ms = a->frame_ms[idx];
        int bh = (int)(ms * scale);
        if (bh > gh)
            bh = gh;
        uint32_t c = ms > 50 ? C_BAD : ms > 33.3f ? C_LONG
                                                  : C_GOOD;
        int bx = gx + gw - 2 - i * 2;
        draw_fill_rect(fb, bx, gy + gh - bh, 2, bh, c, BLEND_NONE);
        int ph = (int)(a->pump_ms[idx] * scale);
        if (ph > 0)
            draw_fill_rect(fb, bx, gy + gh - ph, 2, ph, C_BLUE, BLEND_NONE);
    }
}

static void draw_log_and_checks(App *a, Framebuffer *fb)
{
    int x0 = 12, y0 = 548;
    panel(fb, x0, y0, 700, 134);
    text(fb, a->font, x0 + 12, y0 + 6, C_TEXT, "Eventos");
    for (int i = 0; i < 6; i++)
    {
        LogLine *l = &a->log[(a->log_at - 1 - i + LOG_N) % LOG_N];
        if (!l->text[0])
            break;
        textf(fb, a->small, x0 + 12, y0 + 24 + i * 17, l->color, "%8.2f  %s", l->t, l->text);
    }

    int x1 = 724;
    panel(fb, x1, y0, 464, 134);
    text(fb, a->font, x1 + 12, y0 + 6, C_TEXT, "Verificacoes");
    bool ok = a->mismatches == 0 && a->button_mismatches == 0;
    textf(fb, a->font, x1 + 12, y0 + 24, ok ? C_GOOD : C_BAD, "estado e eventos coerentes: %s (%ld teclas, %ld botoes)",
          ok ? "sim" : "NAO", a->mismatches, a->button_mismatches);
    textf(fb, a->font, x1 + 12, y0 + 42, C_TEXT, "carregadas %ld   soltas %ld   repeticoes %ld", a->presses, a->releases,
          a->repeats);
    textf(fb, a->font, x1 + 12, y0 + 60, a->taps ? C_LONG : C_TEXT, "toques mais curtos que um frame: %ld", a->taps);
    textf(fb, a->font, x1 + 12, y0 + 78, C_TEXT, "teclas sem nome: %ld   eventos de botoes: %ld", a->unknown,
          a->button_events);
    textf(fb, a->font, x1 + 12, y0 + 96, C_TEXT, "frames %ld   tempo %.0f s", a->frames, time_seconds() - a->start);
    text(fb, a->small, x1 + 12, y0 + 114, C_DIM, "carregadas - soltas = teclas em baixo, se nada falhou");
}

int main(void)
{
    if (!platform_init())
    {
        printf("platform_init: %s\n", platform_get_error());
        return 1;
    }
    WindowConfig cfg = {.title = "zen input tester", .width = WIN_W, .height = WIN_H, .render = RENDER_PIXELS};
    PlatformWindow *w = window_create(&cfg);
    if (!w)
    {
        printf("window_create: %s\n", platform_get_error());
        return 1;
    }
    App *a = calloc(1, sizeof *a);
    a->w = w;
    a->font = zui_font_default(15);
    a->small = zui_font_default(12);
    a->big = zui_font_default(20);
    a->start = time_seconds();
    a->focused = window_is_focused(w);
    build_keyboard();

    double last = time_seconds();
    while (!window_should_close(w))
    {
        double t0 = time_seconds();
        window_begin_frame(w);
        double now = time_seconds();
        float pump = (float)((now - t0) * 1000.0);
        float ms = (float)((now - last) * 1000.0);
        last = now;
        a->frames++;
        if (a->frames > 10)
        {
            if (ms > 50)
                a->hitches++;
            if (ms > a->worst_ms)
                a->worst_ms = ms;
            if (pump > a->worst_pump)
                a->worst_pump = pump;
        }
        a->frame_ms[a->graph_at] = ms;
        a->pump_ms[a->graph_at] = pump;
        a->graph_at = (a->graph_at + 1) % GRAPH_N;

        handle_events(a, now);

        if (key_pressed(w, KEY_F1))
        {
            a->captured = !a->captured;
            mouse_set_mode(w, a->captured ? MOUSE_MODE_CAPTURED : MOUSE_MODE_NORMAL);
            a->ev_dx = a->ev_dy = a->md_dx = a->md_dy = 0;
            a->trail_n = 0;
            logf_(a, C_BLUE, a->captured ? "rato capturado" : "rato normal");
        }
        if (key_pressed(w, KEY_ESCAPE) && a->captured)
        {
            a->captured = false;
            mouse_set_mode(w, MOUSE_MODE_NORMAL);
            a->trail_n = 0;
            logf_(a, C_BLUE, "rato normal");
        }
        if (key_pressed(w, KEY_F2))
        {
            if (window_text_input_active(w))
                window_text_input_stop(w);
            else
                window_text_input_start(w);
        }
        if (key_pressed(w, KEY_F5))
            reset_counters(a);

        Framebuffer fb;
        if (window_lock_pixels(w, &fb))
        {
            draw_clear(&fb, C_BG);
            draw_status(a, &fb);
            draw_keyboard(a, &fb, now);
            draw_held(a, &fb, now);
            draw_mouse(a, &fb, now);
            draw_graph(a, &fb);
            draw_log_and_checks(a, &fb);
            window_present_pixels(w);
        }

        double spent = time_seconds() - t0;
        if (spent < 1.0 / 60.0)
            time_sleep((uint32_t)((1.0 / 60.0 - spent) * 1000.0));
    }
    zui_font_free(a->font);
    zui_font_free(a->small);
    zui_font_free(a->big);
    free(a);
    window_destroy(w);
    platform_shutdown();
    return 0;
}
