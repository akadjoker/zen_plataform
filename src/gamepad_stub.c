#include "gamepad_internal.h"

void gamepad_backend_init(void)
{
}

void gamepad_backend_poll(void)
{
}

void gamepad_backend_shutdown(void)
{
}

int gamepad_backend_wait_fds(int *fds, int max)
{
    (void)fds, (void)max;
    return 0;
}

int64_t gamepad_backend_wait_ns(bool *live)
{
    (void)live;
    return -1;
}

bool gamepad_backend_rumble(int slot, float strong, float weak, uint32_t ms)
{
    (void)slot, (void)strong, (void)weak, (void)ms;
    return false;
}
