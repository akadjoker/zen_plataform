#include "gamepad_internal.h"
#include "error_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define INPUT_DIR "/dev/input"
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define EVENT_BATCH 32
#define PAD_PATH_CAP 64

typedef struct
{
    bool used;
    bool dropped;
    int fd;
    int slot;
    char path[PAD_PATH_CAP];
    struct
    {
        bool present;
        int min, max;
    } abs[ABS_CNT];
} EvdevPad;

static EvdevPad g_dev[GAMEPAD_MAX];
static int g_inotify = -1;

static const struct
{
    int code;
    int button;
} k_key_map[] = {
    {BTN_SOUTH, GAMEPAD_BUTTON_A},
    {BTN_EAST, GAMEPAD_BUTTON_B},
    {BTN_NORTH, GAMEPAD_BUTTON_X},
    {BTN_WEST, GAMEPAD_BUTTON_Y},
    {BTN_SELECT, GAMEPAD_BUTTON_BACK},
    {BTN_MODE, GAMEPAD_BUTTON_GUIDE},
    {BTN_START, GAMEPAD_BUTTON_START},
    {BTN_THUMBL, GAMEPAD_BUTTON_LEFT_STICK},
    {BTN_THUMBR, GAMEPAD_BUTTON_RIGHT_STICK},
    {BTN_TL, GAMEPAD_BUTTON_LEFT_SHOULDER},
    {BTN_TR, GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {BTN_DPAD_UP, GAMEPAD_BUTTON_DPAD_UP},
    {BTN_DPAD_DOWN, GAMEPAD_BUTTON_DPAD_DOWN},
    {BTN_DPAD_LEFT, GAMEPAD_BUTTON_DPAD_LEFT},
    {BTN_DPAD_RIGHT, GAMEPAD_BUTTON_DPAD_RIGHT},
};

static const struct
{
    int code;
    int axis;
} k_stick_map[] = {
    {ABS_X, GAMEPAD_AXIS_LEFT_X},
    {ABS_Y, GAMEPAD_AXIS_LEFT_Y},
    {ABS_RX, GAMEPAD_AXIS_RIGHT_X},
    {ABS_RY, GAMEPAD_AXIS_RIGHT_Y},
};

static const struct
{
    int code;
    int axis;
} k_trigger_map[] = {
    {ABS_Z, GAMEPAD_AXIS_TRIGGER_LEFT},
    {ABS_RZ, GAMEPAD_AXIS_TRIGGER_RIGHT},
};

static bool test_bit(const unsigned long *bits, int bit)
{
    unsigned b = (unsigned)bit;
    return (bits[b / BITS_PER_LONG] >> (b % BITS_PER_LONG)) & 1UL;
}

static bool evdev_is_gamepad(const unsigned long *key_bits, const unsigned long *abs_bits)
{
    return test_bit(key_bits, BTN_SOUTH) && test_bit(abs_bits, ABS_X) && test_bit(abs_bits, ABS_Y);
}

static float norm_stick(int value, int min, int max)
{
    if (max <= min)
        return 0.0f;
    double half = ((double)max - (double)min) / 2.0;
    double v = ((double)value - ((double)min + (double)max) / 2.0) / half;
    return (float)(v < -1.0 ? -1.0 : v > 1.0 ? 1.0
                                             : v);
}

static float norm_trigger(int value, int min, int max)
{
    if (max <= min)
        return 0.0f;
    double v = ((double)value - (double)min) / ((double)max - (double)min);
    return (float)(v < 0.0 ? 0.0 : v > 1.0 ? 1.0
                                           : v);
}

static void apply_hat(EvdevPad *d, int negative, int positive, int value)
{
    gamepad_internal_set_button(d->slot, negative, value < 0);
    gamepad_internal_set_button(d->slot, positive, value > 0);
}

static void apply_abs(EvdevPad *d, int code, int value)
{
    if (code < 0 || code >= ABS_CNT || !d->abs[code].present)
        return;
    int min = d->abs[code].min;
    int max = d->abs[code].max;
    if (code == ABS_HAT0X)
    {
        apply_hat(d, GAMEPAD_BUTTON_DPAD_LEFT, GAMEPAD_BUTTON_DPAD_RIGHT, value);
        return;
    }
    if (code == ABS_HAT0Y)
    {
        apply_hat(d, GAMEPAD_BUTTON_DPAD_UP, GAMEPAD_BUTTON_DPAD_DOWN, value);
        return;
    }
    for (size_t i = 0; i < sizeof k_stick_map / sizeof k_stick_map[0]; i++)
    {
        if (k_stick_map[i].code == code)
            gamepad_internal_set_axis(d->slot, k_stick_map[i].axis, norm_stick(value, min, max));
    }
    for (size_t i = 0; i < sizeof k_trigger_map / sizeof k_trigger_map[0]; i++)
    {
        if (k_trigger_map[i].code == code)
            gamepad_internal_set_axis(d->slot, k_trigger_map[i].axis, norm_trigger(value, min, max));
    }
}

static void apply_key(EvdevPad *d, int code, bool down)
{
    for (size_t i = 0; i < sizeof k_key_map / sizeof k_key_map[0]; i++)
    {
        if (k_key_map[i].code == code)
            gamepad_internal_set_button(d->slot, k_key_map[i].button, down);
    }
    if (code == BTN_TL2 && !d->abs[ABS_Z].present)
        gamepad_internal_set_axis(d->slot, GAMEPAD_AXIS_TRIGGER_LEFT, down ? 1.0f : 0.0f);
    if (code == BTN_TR2 && !d->abs[ABS_RZ].present)
        gamepad_internal_set_axis(d->slot, GAMEPAD_AXIS_TRIGGER_RIGHT, down ? 1.0f : 0.0f);
}

static void sync_state(EvdevPad *d)
{
    for (int code = 0; code < ABS_CNT; code++)
    {
        struct input_absinfo info;
        if (d->abs[code].present && ioctl(d->fd, EVIOCGABS((unsigned)code), &info) == 0)
            apply_abs(d, code, info.value);
    }

    unsigned long keys[BITS_TO_LONGS(KEY_CNT)];
    memset(keys, 0, sizeof keys);
    if (ioctl(d->fd, EVIOCGKEY(sizeof keys), keys) < 0)
        return;
    for (size_t i = 0; i < sizeof k_key_map / sizeof k_key_map[0]; i++)
        apply_key(d, k_key_map[i].code, test_bit(keys, k_key_map[i].code));
    apply_key(d, BTN_TL2, test_bit(keys, BTN_TL2));
    apply_key(d, BTN_TR2, test_bit(keys, BTN_TR2));
}

static void apply_event(EvdevPad *d, const struct input_event *ev)
{
    if (ev->type == EV_SYN)
    {
        if (ev->code == SYN_DROPPED)
            d->dropped = true;
        else if (ev->code == SYN_REPORT && d->dropped)
        {
            d->dropped = false;
            sync_state(d);
        }
        return;
    }
    if (d->dropped)
        return;
    if (ev->type == EV_KEY && ev->code <= KEY_MAX)
        apply_key(d, ev->code, ev->value != 0);
    else if (ev->type == EV_ABS)
        apply_abs(d, ev->code, ev->value);
}

static void close_pad(EvdevPad *d)
{
    if (!d || !d->used)
        return;
    close(d->fd);
    gamepad_internal_disconnect(d->slot);
    memset(d, 0, sizeof *d);
}

static bool read_pad(EvdevPad *d)
{
    struct input_event batch[EVENT_BATCH];
    for (;;)
    {
        ssize_t n = read(d->fd, batch, sizeof batch);
        if (n > 0)
        {
            for (size_t i = 0; i < (size_t)n / sizeof batch[0]; i++)
                apply_event(d, &batch[i]);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return true;
        return false;
    }
}

static EvdevPad *find_pad(const char *path)
{
    for (int i = 0; i < GAMEPAD_MAX; i++)
    {
        if (g_dev[i].used && strcmp(g_dev[i].path, path) == 0)
            return &g_dev[i];
    }
    return NULL;
}

static bool probe_fd(int fd, const char *path)
{
    unsigned long types[BITS_TO_LONGS(EV_CNT)];
    unsigned long keys[BITS_TO_LONGS(KEY_CNT)];
    unsigned long abs_bits[BITS_TO_LONGS(ABS_CNT)];
    memset(types, 0, sizeof types);
    memset(keys, 0, sizeof keys);
    memset(abs_bits, 0, sizeof abs_bits);

    if (ioctl(fd, EVIOCGBIT(0, sizeof types), types) < 0 || !test_bit(types, EV_KEY) || !test_bit(types, EV_ABS))
        return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 || ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), abs_bits) < 0)
        return false;
    if (!evdev_is_gamepad(keys, abs_bits))
        return false;

    EvdevPad *d = NULL;
    for (int i = 0; i < GAMEPAD_MAX && !d; i++)
    {
        if (!g_dev[i].used)
            d = &g_dev[i];
    }
    if (!d)
        return false;

    char name[128] = "";
    if (ioctl(fd, EVIOCGNAME(sizeof name), name) < 0)
        name[0] = '\0';

    memset(d, 0, sizeof *d);
    for (int code = 0; code < ABS_CNT; code++)
    {
        struct input_absinfo info;
        if (test_bit(abs_bits, code) && ioctl(fd, EVIOCGABS((unsigned)code), &info) == 0)
        {
            d->abs[code].present = true;
            d->abs[code].min = info.minimum;
            d->abs[code].max = info.maximum;
        }
    }

    d->slot = gamepad_internal_connect(name);
    if (d->slot < 0)
    {
        memset(d, 0, sizeof *d);
        return false;
    }
    d->used = true;
    d->fd = fd;
    snprintf(d->path, sizeof d->path, "%s", path);
    sync_state(d);
    return true;
}

static void probe_path(const char *path)
{
    if (find_pad(path))
        return;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    if (!probe_fd(fd, path))
        close(fd);
}

static bool is_event_node(const char *name)
{
    return strncmp(name, "event", 5) == 0;
}

static bool scan_entry(const char *path, PathType type, void *user)
{
    (void)type;
    (void)user;
    if (is_event_node(path_filename(path)))
        probe_path(path);
    return true;
}

static void handle_inotify(void)
{
    char buf[sizeof(struct inotify_event) + NAME_MAX + 1] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;)
    {
        ssize_t n = read(g_inotify, buf, sizeof buf);
        if (n <= 0)
            return;
        for (char *p = buf; p < buf + n;)
        {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            p += sizeof *ev + ev->len;
            if (ev->len == 0 || !is_event_node(ev->name))
                continue;
            char path[PAD_PATH_CAP];
            if (snprintf(path, sizeof path, INPUT_DIR "/%s", ev->name) >= (int)sizeof path)
                continue;
            if (ev->mask & IN_DELETE)
                close_pad(find_pad(path));
            else if (ev->mask & (IN_CREATE | IN_ATTRIB))
                probe_path(path);
        }
    }
}

void gamepad_backend_init(void)
{
    memset(g_dev, 0, sizeof g_dev);
    g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotify >= 0 && inotify_add_watch(g_inotify, INPUT_DIR, IN_CREATE | IN_DELETE | IN_ATTRIB) < 0)
    {
        close(g_inotify);
        g_inotify = -1;
    }
    if (!fs_enumerate_directory(INPUT_DIR, false, scan_entry, NULL))
        platform_clear_error();
}

void gamepad_backend_poll(void)
{
    if (g_inotify >= 0)
        handle_inotify();
    for (int i = 0; i < GAMEPAD_MAX; i++)
    {
        if (g_dev[i].used && !read_pad(&g_dev[i]))
            close_pad(&g_dev[i]);
    }
}

void gamepad_backend_shutdown(void)
{
    for (int i = 0; i < GAMEPAD_MAX; i++)
        close_pad(&g_dev[i]);
    if (g_inotify >= 0)
        close(g_inotify);
    g_inotify = -1;
}
