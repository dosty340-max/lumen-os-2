// evdev: virtio tablet/keyboard and protocol B multitouch devices.
#include "evdev_mt.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAXDEV 8
typedef struct {
    int fd;
    EvdevAxis x, y;
    EvdevMt mt;
    int ax, ay;
    bool btn, moved, dropping;
} InputDevice;

static InputDevice devices[MAXDEV];
static int ndev, owner = -1, shift;
float touch_x, touch_y;
bool touch_down;

static EvdevAxis get_axis(int fd, int code) {
    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(code), &ai) == 0 && ai.maximum > ai.minimum)
        return (EvdevAxis){ ai.minimum, ai.maximum };
    return (EvdevAxis){ 0, 0 };
}

// Recover coordinates that the kernel may omit on subsequent slot updates.
// Existing tracking IDs are ignored until a new contact begins.
static void mt_seed(InputDevice *dev) {
    EvdevMt *mt = &dev->mt;
    struct input_absinfo slot;
    if (ioctl(dev->fd, EVIOCGABS(ABS_MT_SLOT), &slot) == 0 &&
        slot.value >= 0 && slot.value < mt->count) mt->slot = slot.value;

    int x[EVDEV_MT_MAX_SLOTS + 1] = { ABS_MT_POSITION_X };
    int y[EVDEV_MT_MAX_SLOTS + 1] = { ABS_MT_POSITION_Y };
    size_t bytes = (mt->count + 1) * sizeof x[0];
    if (ioctl(dev->fd, EVIOCGMTSLOTS(bytes), x) < 0 ||
        ioctl(dev->fd, EVIOCGMTSLOTS(bytes), y) < 0) return;
    for (int i = 0; i < mt->count; i++) {
        mt->contacts[i].x = x[i + 1];
        mt->contacts[i].y = y[i + 1];
        mt->contacts[i].have_x = mt->contacts[i].have_y = true;
    }
}

void input_init(void) {
    for (int i = 0; i < 16 && ndev < MAXDEV; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        InputDevice *dev = &devices[ndev++];
        dev->fd = fd;
        char name[64] = "";
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        dev->x = get_axis(fd, ABS_X);
        dev->y = get_axis(fd, ABS_Y);
        EvdevAxis mx = get_axis(fd, ABS_MT_POSITION_X), my = get_axis(fd, ABS_MT_POSITION_Y);
        struct input_absinfo slots, tracking;
        if (mx.max > mx.min && my.max > my.min &&
            ioctl(fd, EVIOCGABS(ABS_MT_TRACKING_ID), &tracking) == 0 &&
            ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slots) == 0 &&
            slots.minimum == 0 && slots.maximum >= 0 && slots.maximum < EVDEV_MT_MAX_SLOTS) {
            evdev_mt_init(&dev->mt, slots.maximum + 1, slots.value, mx, my);
            mt_seed(dev);
        }
        LOG("input: %s = %s (MT slots: %d)", path, name, dev->mt.count);
    }
}

static const char km_lo[] = "\0\0331234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char km_hi[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

static int keymap(int code) {
    switch (code) {
    case KEY_LEFT: return K_LEFT;
    case KEY_RIGHT: return K_RIGHT;
    case KEY_UP: return K_UP;
    case KEY_DOWN: return K_DOWN;
    case KEY_TAB: return K_TAB;
    case KEY_KPENTER: return K_ENTER;
    }
    if (code < (int)sizeof km_lo - 1) return (unsigned char)(shift ? km_hi : km_lo)[code];
    return 0;
}

static bool pointer_event(int d, InputEv *e) {
    if (e->type == T_DOWN && owner < 0) owner = d;
    if (owner != d) return false;
    touch_x = e->x;
    touch_y = e->y;
    touch_down = e->type != T_UP && e->type != T_CANCEL;
    if (!touch_down) owner = -1;
    return true;
}

static bool decode(int d, const struct input_event *e, InputEv *out) {
    InputDevice *dev = &devices[d];
    if (dev->mt.count) {
        bool dropped = dev->mt.dropping;
        bool emitted = evdev_mt_event(&dev->mt, e, out);
        if (dropped && !dev->mt.dropping) mt_seed(dev);
        if (emitted) return pointer_event(d, out);
        if (dropped) return false;
    } else if (e->type == EV_SYN && e->code == SYN_DROPPED) {
        dev->dropping = true;
        dev->btn = dev->moved = false;
        shift = 0;
        *out = (InputEv){ T_CANCEL, touch_x, touch_y, 0 };
        return pointer_event(d, out);
    } else if (dev->dropping) {
        if (e->type == EV_SYN && e->code == SYN_REPORT) dev->dropping = false;
        return false;
    } else if (dev->x.max > dev->x.min && dev->y.max > dev->y.min) {
        if (e->type == EV_ABS && e->code == ABS_X) {
            dev->ax = evdev_scale(dev->x, e->value, SW);
            dev->moved = true;
        } else if (e->type == EV_ABS && e->code == ABS_Y) {
            dev->ay = evdev_scale(dev->y, e->value, SH);
            dev->moved = true;
        } else if (e->type == EV_KEY && (e->code == BTN_LEFT || e->code == BTN_TOUCH)) {
            dev->btn = e->value != 0;
        } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
            bool moved = dev->moved;
            dev->moved = false;
            if (owner == d || owner < 0) { touch_x = dev->ax; touch_y = dev->ay; }
            if (dev->btn == (owner == d) && !(dev->btn && moved)) return false;
            *out = (InputEv){ !dev->btn ? T_UP : owner == d ? T_MOVE : T_DOWN, dev->ax, dev->ay, 0 };
            return pointer_event(d, out);
        }
    }
    if (e->type == EV_KEY && e->code < 256) {
        if (e->code == KEY_LEFTSHIFT || e->code == KEY_RIGHTSHIFT) shift = e->value != 0;
        else if (e->value) {
            int k = keymap(e->code);
            if (k) {
                *out = (InputEv){ T_KEY, touch_x, touch_y, k };
                return true;
            }
        }
    }
    return false;
}

int input_poll(InputEv *out, int max) {
    int n = 0;
    struct input_event ev[64];
    for (int d = 0; d < ndev && n < max; d++) {
        InputDevice *dev = &devices[d];
        while (dev->fd >= 0 && n < max) {
            // Each kernel event can emit at most one UI event.
            ssize_t r = read(dev->fd, ev, mini(64, max - n) * sizeof ev[0]);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (r <= 0) {
                close(dev->fd);
                dev->fd = -1;
                out[n] = (InputEv){ T_CANCEL, touch_x, touch_y, 0 };
                if (pointer_event(d, &out[n])) n++;
                break;
            }
            for (size_t i = 0; i < (size_t)r / sizeof ev[0]; i++)
                if (decode(d, &ev[i], &out[n])) n++;
        }
    }
    return n;
}
