// Shell (M2 test card: compositor, text, input, touch circle)
#include "ui.h"

static char typed[64];
void shell_init(void) { theme_apply(); }
void shell_event(InputEv *e) {
    if (e->type == T_KEY) field_key(typed, sizeof typed, e->key);
}
void shell_frame(Canvas *c, float dt) {
    (void)dt;
    hit_frame();
    g_clear(c, C_INK);
    status_line(c, 0, C_PAPER, true, false);
    text(c, W800, P(120), -0.06f, P(-6), P(190), C_PAPER, "12");
    text(c, W800, P(40), -0.03f, P(20), P(250), C_LIME, "lumen");
    text(c, W600, P(16), 0, P(20), P(280), C_MUTED, "Bricolage Grotesque 600 — The quick brown fox");
    text(c, W400, P(13), 0, P(20), P(302), C_MUTED, "400 at 13pt: jumps over the lazy dog 0123456789");
    uint32_t cols[5] = { C_ORANGE, C_LILAC, C_LIME, C_SKY, C_PAPER };
    for (int i = 0; i < 5; i++) g_rrect(c, 0, P(330 + i * 62), SW, SH, P(30), 0, cols[i]);
    for (int i = 1; i <= 46; i++) icon(c, i, P(16 + ((i - 1) % 9) * 40), P(352 + ((i - 1) / 9) * 40), P(24), C_INK);
    g_rrect_line(c, P(16), P(600), P(112), P(112), P(22), P(2), C_INK);
    field_draw(c, P(140), P(620), P(234), P(48), typed, "type on the keyboard", true, C_INK, C_PAPER);
    g_arc(c, P(80), P(760), P(30), 0, (float)fmod(now() * 3, 6.28), P(4), C_ORANGE);
    if (touch_down) {
        g_circle(c, touch_x, touch_y, P(22), 0x55F3F0E8u);
        g_ring(c, touch_x, touch_y, P(22), P(1.5f), 0x66141414u);
    }
}
