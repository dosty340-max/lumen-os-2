// evdev input: virtio-tablet (absolute pointer = finger) and virtio-keyboard.
#include "lumen.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAXDEV 8
static int fds[MAXDEV], nfd;
static int absmax_x[MAXDEV], absmax_y[MAXDEV];
static int ax, ay, btn, prev_btn, moved, shift, ctrl;
float touch_x, touch_y;
bool touch_down;

void input_init(void) {
    for (int i = 0; i < 16 && nfd < MAXDEV; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char name[64] = "";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        struct input_absinfo ai = {0};
        absmax_x[nfd] = ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum ? ai.maximum : 32767;
        absmax_y[nfd] = ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0 && ai.maximum ? ai.maximum : 32767;
        LOG("input: %s = %s", p, name);
        fds[nfd++] = fd;
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

int input_poll(InputEv *out, int max) {
    int n = 0;
    struct input_event ev[64];
    for (int d = 0; d < nfd; d++) {
        int r;
        while ((r = read(fds[d], ev, sizeof ev)) > 0) {
            for (int i = 0; i < r / (int)sizeof ev[0]; i++) {
                struct input_event *e = &ev[i];
                if (e->type == EV_ABS && e->code == ABS_X) { ax = e->value * (SW - 1) / absmax_x[d]; moved = 1; }
                else if (e->type == EV_ABS && e->code == ABS_Y) { ay = e->value * (SH - 1) / absmax_y[d]; moved = 1; }
                else if (e->type == EV_KEY && (e->code == BTN_LEFT || e->code == BTN_TOUCH)) btn = e->value != 0;
                else if (e->type == EV_KEY && e->code < 256) {
                    if (e->code == KEY_LEFTSHIFT || e->code == KEY_RIGHTSHIFT) shift = e->value != 0;
                    else if (e->code == KEY_LEFTCTRL || e->code == KEY_RIGHTCTRL) ctrl = e->value != 0;
                    else if (e->value && n < max) {   // press or autorepeat
                        int k = keymap(e->code);
                        if (k) out[n++] = (InputEv){ EV_KEY, touch_x, touch_y, k };
                    }
                } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
                    touch_x = ax; touch_y = ay;
                    if (btn && !prev_btn && n < max) out[n++] = (InputEv){ EV_DOWN, ax, ay, 0 };
                    else if (btn && moved && n < max) out[n++] = (InputEv){ EV_MOVE, ax, ay, 0 };
                    else if (!btn && prev_btn && n < max) out[n++] = (InputEv){ EV_UP, ax, ay, 0 };
                    prev_btn = btn; moved = 0;
                    touch_down = btn;
                }
            }
        }
    }
    (void)ctrl;
    return n;
}
