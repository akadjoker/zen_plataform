#ifndef GAMEPAD_INTERNAL_H
#define GAMEPAD_INTERNAL_H

#include "platform.h"

void gamepad_init(void);
void gamepad_poll(void);
void gamepad_shutdown(void);

int gamepad_internal_connect(const char *name);
void gamepad_internal_disconnect(int slot);
void gamepad_internal_set_button(int slot, int button, bool down);
void gamepad_internal_set_axis(int slot, int axis, float value);

void gamepad_backend_init(void);
void gamepad_backend_poll(void);
void gamepad_backend_shutdown(void);

#endif /* GAMEPAD_INTERNAL_H */
