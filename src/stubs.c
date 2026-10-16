// temporary stubs until the real modules land
#include "ui.h"
int net_state(void) { return NET_DOWN; }
void net_init(void) {}
void net_set_enabled(bool on) { (void)on; }
void net_info(NetInfo *ni) { memset(ni, 0, sizeof *ni); }
float clock_timer_left(void) { return -1; }
void clock_tick(float dt) { (void)dt; }
int game_best(void) { return 2048; }
const char *video_continue_info(void) { return "Continue · 12 min left"; }
bool video_playing(void) { return false; }
const char *video_now_title(void) { return NULL; }
void video_toggle_play(void) {}
const char *weather_text(void) { return "14° Cloudy"; }
static void stub_draw(Canvas *c, float dt, const char *name, uint32_t col) {
    (void)dt;
    g_clear(c, TH.bg);
    app_header(c, col, name, 150);
    home_bar(c, TH.fg);
}
#define STUB(v, n, ic, col) static void v##_draw(Canvas *c, float dt) { stub_draw(c, dt, n, col); } App v = { n, ic, 0, v##_draw, 0, 0, 0 };
STUB(app_video, "Video", IC_VIDEO, C_ORANGE)
STUB(app_web, "Web", IC_WEB, C_ORANGE)
STUB(app_photos, "Photos", IC_PHOTOS, C_ORANGE)
STUB(app_notes, "Notes", IC_NOTES, C_LIME)
STUB(app_clock, "Clock", IC_CLOCK, C_LIME)
STUB(app_calc, "Calculator", IC_CALC, C_LIME)
STUB(app_game, "Game", IC_GAME, C_SKY)
STUB(app_settings, "Settings", IC_SETTINGS, C_PAPER)
