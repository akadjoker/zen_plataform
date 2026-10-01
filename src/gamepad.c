#include "gamepad_internal.h"

#include <string.h>

#define GAMEPAD_NAME_CAP 128

typedef struct
{
    bool connected;
    char name[GAMEPAD_NAME_CAP];
    bool buttons[GAMEPAD_BUTTON_COUNT];
    float axes[GAMEPAD_AXIS_COUNT];
} Pad;

static Pad g_pads[GAMEPAD_MAX];

static bool valid_slot(int index)
{
    return index >= 0 && index < GAMEPAD_MAX;
}

void gamepad_init(void)
{
    memset(g_pads, 0, sizeof g_pads);
    gamepad_backend_init();
}

void gamepad_poll(void)
{
    gamepad_backend_poll();
}

void gamepad_shutdown(void)
{
    gamepad_backend_shutdown();
    memset(g_pads, 0, sizeof g_pads);
}

int gamepad_internal_connect(const char *name)
{
    for (int i = 0; i < GAMEPAD_MAX; i++)
    {
        if (g_pads[i].connected)
            continue;
        memset(&g_pads[i], 0, sizeof g_pads[i]);
        g_pads[i].connected = true;
        strncpy(g_pads[i].name, name && name[0] ? name : "Gamepad", GAMEPAD_NAME_CAP - 1);
        return i;
    }
    return -1;
}

void gamepad_internal_disconnect(int slot)
{
    if (valid_slot(slot))
        memset(&g_pads[slot], 0, sizeof g_pads[slot]);
}

void gamepad_internal_set_button(int slot, int button, bool down)
{
    if (valid_slot(slot) && g_pads[slot].connected && button >= 0 && button < GAMEPAD_BUTTON_COUNT)
        g_pads[slot].buttons[button] = down;
}

void gamepad_internal_set_axis(int slot, int axis, float value)
{
    if (valid_slot(slot) && g_pads[slot].connected && axis >= 0 && axis < GAMEPAD_AXIS_COUNT)
        g_pads[slot].axes[axis] = value;
}

bool gamepad_connected(int index)
{
    return valid_slot(index) && g_pads[index].connected;
}

const char *gamepad_name(int index)
{
    return gamepad_connected(index) ? g_pads[index].name : NULL;
}

bool gamepad_button_down(int index, int button)
{
    return gamepad_connected(index) && button >= 0 && button < GAMEPAD_BUTTON_COUNT && g_pads[index].buttons[button];
}

float gamepad_axis(int index, int axis)
{
    return gamepad_connected(index) && axis >= 0 && axis < GAMEPAD_AXIS_COUNT ? g_pads[index].axes[axis] : 0.0f;
}
