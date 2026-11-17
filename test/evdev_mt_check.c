#include "evdev_mt.h"
#include "ui.h"
#include <assert.h>

static EvdevMt mt;
static InputEv out;

double now(void) { return 0; }

static int feed(unsigned type, unsigned code, int value) {
    return evdev_mt_event(&mt, &(struct input_event){ .type = type, .code = code, .value = value }, &out);
}

static void abs_event(unsigned code, int value) { assert(!feed(EV_ABS, code, value)); }

static void start(int slot, int id, int x, int y) {
    abs_event(ABS_MT_SLOT, slot);
    abs_event(ABS_MT_TRACKING_ID, id);
    abs_event(ABS_MT_POSITION_X, x);
    abs_event(ABS_MT_POSITION_Y, y);
}

static void report(int type, int x, int y) {
    assert(feed(EV_SYN, SYN_REPORT, 0) == (type != 0));
    if (type) assert(out.type == type && out.x == x && out.y == y);
}

int main(void) {
    evdev_mt_init(&mt, 10, 0, (EvdevAxis){0, 4095}, (EvdevAxis){0, 4095});
    start(9, 42, 4095, 0);
    assert(!feed(EV_KEY, BTN_TOUCH, 1));
    report(T_DOWN, SW - 1, 0);
    for (int slot = 0; slot < 9; slot++) start(slot, 43 + slot, 0, 4095);
    report(0, 0, 0);
    for (int slot = 1; slot < 9; slot++) {
        abs_event(ABS_MT_SLOT, slot);
        abs_event(ABS_MT_TRACKING_ID, -1);
    }
    report(0, 0, 0);
    abs_event(ABS_MT_SLOT, 9);
    abs_event(ABS_MT_POSITION_Y, 4096);
    report(T_MOVE, SW - 1, SH - 1);
    abs_event(ABS_MT_SLOT, 10);
    abs_event(ABS_MT_TRACKING_ID, -1);
    report(0, 0, 0);
    abs_event(ABS_MT_SLOT, 9);
    abs_event(ABS_MT_POSITION_X, 0);
    abs_event(ABS_MT_TRACKING_ID, -1);
    report(T_UP, 0, SH - 1);
    abs_event(ABS_MT_SLOT, 0);
    abs_event(ABS_MT_POSITION_X, 4095);
    report(0, 0, 0);
    abs_event(ABS_MT_TRACKING_ID, -1);
    report(0, 0, 0);
    // Coordinates in a slot can be reused without being sent again.
    abs_event(ABS_MT_TRACKING_ID, 44);
    report(T_DOWN, SW - 1, SH - 1);
    Gest gesture = {0};
    assert(gest(&gesture, &out) == G_DOWN);
    assert(feed(EV_SYN, SYN_DROPPED, 0) && out.type == T_CANCEL);
    assert(gest(&gesture, &out) == G_DRAG_END && !gesture.down);
    out.type = T_UP;
    assert(gest(&gesture, &out) == G_NONE);
    start(0, 45, 0, 0);
    report(0, 0, 0);
    assert(mt.slot == -1);
    start(0, 46, -4, 0);
    report(T_DOWN, 0, 0);
    abs_event(ABS_MT_TRACKING_ID, -1);
    abs_event(ABS_MT_TRACKING_ID, 46);
    report(T_UP, 0, 0);
    report(0, 0, 0);

    evdev_mt_init(&mt, 10, 0, (EvdevAxis){100, 200}, (EvdevAxis){-100, 100});
    abs_event(ABS_MT_TRACKING_ID, 50);
    abs_event(ABS_MT_POSITION_X, 150);
    report(0, 0, 0);
    abs_event(ABS_MT_POSITION_Y, 0);
    report(T_DOWN, (SW - 1) / 2, (SH - 1) / 2);
    puts("evdev MT OK");
}
