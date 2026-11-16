// evdev protocol B -> the UI's single pointer, committed at SYN_REPORT.
#include "evdev_mt.h"

int evdev_scale(EvdevAxis axis, int value, int extent) {
    if (axis.max <= axis.min) return 0;
    value = mini(maxi(value, axis.min), axis.max);
    return ((int64_t)value - axis.min) * (extent - 1) / ((int64_t)axis.max - axis.min);
}

void evdev_mt_init(EvdevMt *mt, int count, int slot, EvdevAxis x, EvdevAxis y) {
    *mt = (EvdevMt){ .count = mini(maxi(count, 0), EVDEV_MT_MAX_SLOTS),
                     .slot = -1, .primary = -1, .x = x, .y = y };
    if (slot >= 0 && slot < mt->count) mt->slot = slot;
    for (int i = 0; i < mt->count; i++) mt->contacts[i].id = -1;
}

static bool release(EvdevMt *mt, InputEv *out) {
    if (mt->primary < 0) return false;
    *out = (InputEv){ T_UP, mt->last_x, mt->last_y, 0 };
    mt->primary = -1;
    return true;
}

static bool report(EvdevMt *mt, InputEv *out) {
    if (mt->primary >= 0) {
        EvdevContact *primary = &mt->contacts[mt->primary];
        bool ended = primary->id != mt->primary_id || primary->fresh;
        // A secondary finger already touching must not become a new UI press.
        for (int i = 0; i < mt->count; i++) mt->contacts[i].fresh = false;
        if (ended) {
            // A normal release can include the contact's final position.
            if (primary->id < 0) {
                mt->last_x = evdev_scale(mt->x, primary->x, SW);
                mt->last_y = evdev_scale(mt->y, primary->y, SH);
            }
            return release(mt, out);
        }
    }
    bool down = mt->primary < 0;
    if (down) {
        for (int i = 0; i < mt->count; i++) {
            EvdevContact *c = &mt->contacts[i];
            if (c->fresh && c->have_x && c->have_y) {
                mt->primary = i;
                mt->primary_id = c->id;
                break;
            }
        }
        if (mt->primary < 0) return false;
        for (int i = 0; i < mt->count; i++) mt->contacts[i].fresh = false;
    }
    EvdevContact *c = &mt->contacts[mt->primary];
    int x = evdev_scale(mt->x, c->x, SW), y = evdev_scale(mt->y, c->y, SH);
    if (!down && x == mt->last_x && y == mt->last_y) return false;
    mt->last_x = x;
    mt->last_y = y;
    *out = (InputEv){ down ? T_DOWN : T_MOVE, x, y, 0 };
    return true;
}

bool evdev_mt_event(EvdevMt *mt, const struct input_event *e, InputEv *out) {
    if (e->type == EV_SYN && e->code == SYN_DROPPED) {
        bool emitted = release(mt, out);
        if (emitted) out->type = T_CANCEL;
        evdev_mt_init(mt, mt->count, -1, mt->x, mt->y);
        mt->dropping = true;
        return emitted;
    }
    if (e->type == EV_SYN && e->code == SYN_REPORT) {
        if (!mt->dropping) return report(mt, out);
        mt->dropping = false;
        return false;
    }
    if (mt->dropping || e->type != EV_ABS) return false;
    if (e->code == ABS_MT_SLOT) {
        mt->slot = e->value >= 0 && e->value < mt->count ? e->value : -1;
    } else if (mt->slot >= 0) {
        EvdevContact *c = &mt->contacts[mt->slot];
        switch (e->code) {
        case ABS_MT_TRACKING_ID:
            if (e->value != c->id) c->fresh = e->value >= 0;
            c->id = e->value;
            break;
        case ABS_MT_POSITION_X: c->x = e->value; c->have_x = true; break;
        case ABS_MT_POSITION_Y: c->y = e->value; c->have_y = true; break;
        }
    }
    return false;
}
