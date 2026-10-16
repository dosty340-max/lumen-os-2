// Clock: live clock, stopwatch with laps, countdown timer with alarm sound.
#include "../ui.h"

static int tab;                       // 0 clock, 1 stopwatch, 2 timer
static Gest g;
static Spring tab_x;
// stopwatch
static double sw_acc, sw_start;
static int sw_run;
static double laps[32];
static int nlaps;
// timer (runs in the background via clock_tick)
static float tm_total = 60, tm_left = -1;
static int tm_run;
static double tm_done_t = -10;

float clock_timer_left(void) { return tm_left >= 0 ? tm_left : -1; }
void clock_tick(float dt) {
    if (!tm_run || tm_left < 0) return;
    tm_left -= dt;
    if (tm_left <= 0) {
        tm_left = -1; tm_run = 0; tm_done_t = now();
        audio_alarm();
        notify("Clock", "Timer finished");
        LOG("clock: timer finished");
    }
}
static double sw_now(void) { return sw_acc + (sw_run ? now() - sw_start : 0); }

static void fmt_sw(double t, char *o, int n) {
    int cs = (int)(t * 100);
    snprintf(o, n, "%02d:%02d.%02d", cs / 6000, cs / 100 % 60, cs % 100);
}

static void open_(void) {}

static void draw(Canvas *c, float dt) {
    g_clear(c, TH.bg);
    float hh = P(196);
    g_rect(c, 0, 0, SW, hh, C_LIME);
    status_line(c, 0, C_INK, true, false);
    text(c, W800, P(52), -0.05f, P(18), P(112), C_INK, "Clock");
    // segmented tabs with a sliding ink slab
    const char *tabs[3] = { "Clock", "Stopwatch", "Timer" };
    float tx = P(16), tw = P(358) / 3, ty = P(132), th = P(46);
    tab_x.t = tab;
    spring_step(&tab_x, dt, 3.5f, 0.8f);
    g_rrect_line(c, tx, ty, P(358), th, th / 2, P(2), C_INK);
    g_rrect(c, tx + tab_x.x * tw, ty, tw, th, th / 2, th / 2, C_INK);
    for (int i = 0; i < 3; i++) {
        float sel = 1 - clampf(fabsf(tab_x.x - i), 0, 1);
        text_c(c, W600, P(15), 0, tx + i * tw + tw / 2, ty + P(29), c_mix(C_INK, C_LIME, sel), tabs[i]);
        hit(0xC00 + i, tx + i * tw, ty, tw, th);
    }
    float y = hh + P(30);
    time_t t = time(0);
    struct tm lt;
    gmtime_r(&t, &lt);
    if (tab == 0) {
        char hm[8], ss[8], ds[64];
        strftime(hm, sizeof hm, "%H:%M", &lt);
        snprintf(ss, sizeof ss, "%02d", lt.tm_sec);
        text(c, W800, P(150), -0.06f, P(8), y + P(140), TH.fg, hm);
        g_rrect(c, P(16), y + P(180), P(120), P(84), P(22), P(22), C_ORANGE);
        text_c(c, W800, P(56), -0.04f, P(76), y + P(242), C_INK, ss);
        text(c, W600, P(13), 0.1f, P(150), y + P(206), TH.sub, "SECONDS");
        strftime(ds, sizeof ds, "%A", &lt);
        text(c, W800, P(28), -0.02f, P(150), y + P(240), TH.fg, ds);
        strftime(ds, sizeof ds, "%d %B %Y", &lt);
        text(c, W600, P(16), 0, P(18), y + P(310), TH.sub, ds);
        // seconds ring
        float cx = P(195), cy = y + P(470), r = P(110);
        g_ring(c, cx, cy, r, P(2), TH.line);
        float a = (lt.tm_sec + fmod(now(), 1.0)) / 60 * 2 * (float)M_PI;
        g_arc(c, cx, cy, r - P(1), -(float)M_PI / 2, -(float)M_PI / 2 + fmaxf(a, 0.01f), P(10), C_LILAC);
        char doy[32];
        snprintf(doy, sizeof doy, "Day %d", lt.tm_yday + 1);
        text_c(c, W800, P(34), -0.03f, cx, cy + P(4), TH.fg, doy);
        snprintf(doy, sizeof doy, "Week %d", (lt.tm_yday + 7 - (lt.tm_wday + 6) % 7) / 7);
        text_c(c, W600, P(14), 0, cx, cy + P(30), TH.sub, doy);
    } else if (tab == 1) {
        char b[32];
        fmt_sw(sw_now(), b, sizeof b);
        float fs = P(84);
        text(c, W800, fs, -0.05f, P(16), y + P(90), TH.fg, b);
        btn_text(c, 0xC10, P(16), y + P(130), P(172), P(64), sw_run ? "Stop" : "Start", C_INK, sw_run ? C_LILAC : C_ORANGE);
        btn_text(c, 0xC11, P(202), y + P(130), P(172), P(64), sw_run ? "Lap" : "Reset", TH.fg, 0);
        float ly = y + P(240);
        for (int i = nlaps - 1; i >= 0 && ly < SH - P(40); i--, ly += P(46)) {
            g_rect(c, P(16), ly, P(358), P(1.5f), TH.line);
            char l[16], v[32];
            snprintf(l, sizeof l, "Lap %d", i + 1);
            fmt_sw(laps[i] - (i ? laps[i - 1] : 0), v, sizeof v);
            text(c, W600, P(17), 0, P(20), ly + P(31), TH.fg, l);
            text_r(c, W800, P(19), 0, P(370), ly + P(31), TH.fg, v);
        }
    } else {
        float left = tm_left >= 0 ? tm_left : tm_total;
        char b[32];
        int s = (int)ceilf(left);
        snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60);
        float cx = P(195), cy = y + P(170), r = P(150);
        bool flash = now() - tm_done_t < 3 && fmod(now(), 0.4) < 0.2;
        if (flash) g_circle(c, cx, cy, r + P(6), C_ORANGE);
        g_ring(c, cx, cy, r, P(2), TH.line);
        float frac = tm_left >= 0 ? left / tm_total : 1;
        g_arc(c, cx, cy, r - P(1), -(float)M_PI / 2, -(float)M_PI / 2 + 2 * (float)M_PI * fmaxf(frac, 0.002f), P(12), C_ORANGE);
        text_c(c, W800, P(78), -0.05f, cx, cy + P(26), flash ? C_INK : TH.fg, b);
        text_c(c, W600, P(13), 0.1f, cx, cy + P(56), flash ? C_INK : TH.sub, tm_run ? "RUNNING" : tm_left >= 0 ? "PAUSED" : "READY");
        const char *pl[4] = { "10 s", "1 min", "5 min", "10 min" };
        const float pv[4] = { 10, 60, 300, 600 };
        float pw = (P(358) - 3 * P(8)) / 4, py = cy + r + P(30);
        for (int i = 0; i < 4; i++) {
            bool sel = tm_total == pv[i] && tm_left < 0;
            btn_text(c, 0xC20 + i, P(16) + i * (pw + P(8)), py, pw, P(46), pl[i], sel ? C_INK : TH.fg, sel ? C_LIME : 0);
        }
        btn_text(c, 0xC30, P(16), py + P(62), P(172), P(64), tm_run ? "Pause" : tm_left >= 0 ? "Resume" : "Start", C_INK, C_ORANGE);
        btn_text(c, 0xC31, P(202), py + P(62), P(172), P(64), "Cancel", TH.fg, 0);
    }
    home_bar(c, TH.fg);
}

static void touch(InputEv *e) {
    int r = gest(&g, e);
    if (r == G_DRAG_END && !g.vert && fabsf(g.x - g.x0) > P(60)) {   // swipe between tabs
        tab = g.x < g.x0 ? mini(tab + 1, 2) : maxi(tab - 1, 0);
        return;
    }
    if (r != G_TAP) return;
    int id = hit_at(e->x, e->y);
    if (id >= 0xC00 && id < 0xC03) tab = id - 0xC00;
    else if (id == 0xC10) {
        if (sw_run) { sw_acc += now() - sw_start; sw_run = 0; }
        else { sw_start = now(); sw_run = 1; }
    } else if (id == 0xC11) {
        if (sw_run) { if (nlaps < 32) laps[nlaps++] = sw_now(); }
        else { sw_acc = 0; nlaps = 0; }
    } else if (id >= 0xC20 && id < 0xC24) {
        static const float pv[4] = { 10, 60, 300, 600 };
        tm_total = pv[id - 0xC20]; tm_left = -1; tm_run = 0;
    } else if (id == 0xC30) {
        if (tm_run) tm_run = 0;
        else { if (tm_left < 0) tm_left = tm_total; tm_run = 1; LOG("clock: timer started %.0f s", tm_left); }
    } else if (id == 0xC31) { tm_run = 0; tm_left = -1; }
}
App app_clock = { "Clock", IC_CLOCK, open_, draw, touch, 0, 0 };
