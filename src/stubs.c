// temporary stubs until the real modules land
#include "ui.h"
const char *video_continue_info(void) { return NULL; }
bool video_playing(void) { return false; }
const char *video_now_title(void) { return NULL; }
void video_toggle_play(void) {}
static void stub_draw(Canvas *c, float dt, const char *name, uint32_t col) {
    (void)dt;
    g_clear(c, TH.bg);
    app_header(c, col, name, 150);
    home_bar(c, TH.fg);
}
#define STUB(v, n, ic, col) static void v##_draw(Canvas *c, float dt) { stub_draw(c, dt, n, col); } App v = { n, ic, 0, v##_draw, 0, 0, 0 };
STUB(app_video, "Video", IC_VIDEO, C_ORANGE)
