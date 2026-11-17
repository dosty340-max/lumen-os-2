#pragma once
#include "lumen.h"
#include <linux/input.h>

#define EVDEV_MT_MAX_SLOTS 32

typedef struct { int min, max; } EvdevAxis;
typedef struct {
    int id, x, y;
    bool fresh, have_x, have_y;
} EvdevContact;
typedef struct {
    EvdevContact contacts[EVDEV_MT_MAX_SLOTS];
    EvdevAxis x, y;
    int count, slot, primary, primary_id, last_x, last_y;
    bool dropping;
} EvdevMt;

int evdev_scale(EvdevAxis axis, int value, int extent);
void evdev_mt_init(EvdevMt *mt, int count, int slot, EvdevAxis x, EvdevAxis y);
bool evdev_mt_event(EvdevMt *mt, const struct input_event *event, InputEv *out);
