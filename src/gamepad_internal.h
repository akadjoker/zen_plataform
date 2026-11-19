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

/* Joysticks (raw devices), filled by the backend like the gamepads. */
int joystick_internal_connect(const char *name, int axes, int buttons, int hats);
void joystick_internal_disconnect(int slot);
void joystick_internal_set_axis(int slot, int axis, float value);
void joystick_internal_set_button(int slot, int button, bool down);
void joystick_internal_set_hat(int slot, int hat, int mask);
void joystick_internal_link_gamepad(int slot, int gamepad_slot);

/* How many times a slot has been connected: a change means a different device, even
   when the slot was freed and taken again between two looks. */
/* Changes whenever a gamepad or a joystick connects or disconnects: a window that has seen the current value has nothing
   new to tell about devices (window_begin_frame skips its scan of the slots). */
unsigned gamepad_internal_epoch(void);
unsigned gamepad_internal_generation(int slot);
unsigned joystick_internal_generation(int slot);

/* Vibration (rumble) for the slot; false if the device cannot. */
bool gamepad_backend_rumble(int slot, float strong, float weak, uint32_t ms);

void gamepad_backend_init(void);
void gamepad_backend_poll(void);
void gamepad_backend_shutdown(void);

/* What a blocked platform_wait_events has to watch so that the devices are not starved by it.
   gamepad_wait_fds: the descriptors that turn readable when a device has input, or comes or goes (POSIX; none elsewhere);
   writes at most max of them and returns how many. gamepad_wait_ns: when the devices cannot be waited on (XInput and winmm
   have to be asked), the longest the wait may sleep before the next poll, in nanoseconds; -1 when none needs it. *live is
   true when what is polled is a device that is plugged in (its state may have changed, so the tick counts as input), false
   when it is only a look for devices that came (a service tick). */
int gamepad_wait_fds(int *fds, int max);
int64_t gamepad_wait_ns(bool *live);
int gamepad_backend_wait_fds(int *fds, int max);
int64_t gamepad_backend_wait_ns(bool *live);

#endif /* GAMEPAD_INTERNAL_H */
