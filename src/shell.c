// The Slab shell: boot animation, lock slabs, home drawers, app transitions,
// home gesture, control center, touch circle.
#include "ui.h"

enum { ST_BOOT, ST_LOCK, ST_HOME };
static int st = ST_BOOT;
static double boot_t0;
static Canvas *layer;          // offscreen app layer
float shell_app_alpha = 1;

// ---- notifications (lock screen rows) -------------------------------------------
typedef struct { char from[24], text[96]; } Notif;
static Notif notifs[4];
static int nnotif;
void notify(const char *from, const char *text) {
    memmove(&notifs[1], &notifs[0], sizeof notifs[0] * 3);
    snprintf(notifs[0].from, sizeof notifs[0].from, "%s", from);
    snprintf(notifs[0].text, sizeof notifs[0].text, "%s", text);
    if (nnotif < 4) nnotif++;
    LOG("notify: %s: %s", from, text);
}

// ---- drawers ------------------------------------------------------------------------
#define NDRAWER 4
#define DR_TOP 214
#define TAB 62
typedef struct { const char *name; uint32_t col; App *apps[3]; int napps; } Drawer;
static Drawer drawers[NDRAWER] = {
    { "Watch", C_ORANGE, { &app_video, &app_web, &app_photos }, 3 },
    { "Make", C_LIME, { &app_notes, &app_clock, &app_calc }, 3 },
    { "Play", C_SKY, { &app_game }, 1 },
    { "System", C_PAPER, { &app_settings }, 1 },
};
static int open_drawer = 0;
static Spring dtop[NDRAWER];
static int drag_tab = -1, drag_to = -1;   // drawer drag in progress
static float drag_p;

static uint32_t drawer_col(int d) { return (d == 3 && !SET.dark) ? C_INK : drawers[d].col; }
static uint32_t drawer_fg(int d) { return drawer_col(d) == C_INK ? C_PAPER : C_INK; }
static float drawer_target(int i, int open) {
    return i <= open ? P(DR_TOP + i * TAB) : SH - P((NDRAWER - i) * TAB);
}
typedef struct { float x, y, w, h; } Rect;
static Rect tile_rect(int d, int i) {
    float gx = P(16), gap = P(10), tw = (SW - 2 * gx - 2 * gap) / 3, th = P(112);
    float top = dtop[d].x + P(TAB) + P(2);
    int col = i % 3, row = i / 3;
    if (d == 0) { col = i == 0 ? 0 : i == 1 ? 2 : 0; row = i == 2; }
    Rect r = { gx + col * (tw + gap), top + row * (th + gap), (d == 0 && i == 0) ? 2 * tw + gap : tw, th };
    return r;
}

// ---- app transition state ---------------------------------------------------------------
enum { AP_NONE, AP_OPENING, AP_OPEN, AP_CLOSING, AP_DRAG, AP_RETURN };
static App *app;
static int app_d, app_i, app_phase;
static Spring app_u;
static Rect from, to;
static uint32_t app_col;
static float content_a;
static Gest hg;                 // home gesture
static float hg_p;
static Rect cur_rect;

// ---- lock screen state ---------------------------------------------------------------------
static Spring lk[3];            // vertical offsets of the 3 bands (px, negative = up)
static int lk_drag, unlocking;
static Gest lg;
static char shown_hm[2][3];     // numerals currently displayed
static char prev_hm[2][3];
static Spring roll[2][2];       // per digit roll progress 0..1

// ---- control center ----------------------------------------------------------------------------
static Spring cc;               // 0 closed .. 1 open
static int cc_drag, cc_slider;  // slider: 1 bright, 2 volume
static Gest cg;
static float cc_h;              // total height in px

// ---------------------------------------------------------------------------------------------
static void date_str(char *buf, int n, bool month) {
    static const char *wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t t = time(0);
    struct tm lt;
    gmtime_r(&t, &lt);
    if (month) snprintf(buf, n, "%s %d %s", wd[lt.tm_wday], lt.tm_mday, mo[lt.tm_mon]);
    else snprintf(buf, n, "%s %d", wd[lt.tm_wday], lt.tm_mday);
}

void shell_init(void) {
    theme_apply();
    layer = canvas_new(SW, SH);
    for (int i = 0; i < NDRAWER; i++) dtop[i].x = dtop[i].t = drawer_target(i, open_drawer);
    boot_t0 = now();
}

// ---- lock screen drawing ---------------------------------------------------------------------------
typedef struct { float bx[3]; float typed; float word_a; float num_in; float content_a; } BootP;

static void numeral(Canvas *c, int which, float xa, bool right, float base, float band_y0, float band_y1, float num_in) {
    float px = P(330), tr = -0.06f;
    const char *s = shown_hm[which];
    float w = text_w(W800, px, tr, s);
    float x0 = right ? xa - w : xa;
    float bandh = band_y1 - band_y0;
    int sc[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
    g_clip(c, 0, band_y0, SW, band_y1);
    for (int i = 0; i < 2; i++) {
        float xi = x0 + (i ? text_wn(W800, px, tr, s, 1) + tr * px : 0);
        float r = roll[which][i].x;     // 1 = settled
        float in = (1 - num_in) * bandh * (1 + i * 0.25f);
        char d[2] = { s[i], 0 }, o[2] = { prev_hm[which][i], 0 };
        if (r < 0.999f && o[0]) text(c, W800, px, tr, xi, base - r * bandh + in, C_INK, o);
        text(c, W800, px, tr, xi, base + (1 - r) * bandh + in, C_INK, d);
    }
    c->cx0 = sc[0]; c->cy0 = sc[1]; c->cx1 = sc[2]; c->cy1 = sc[3];
}

static void draw_lock(Canvas *c, BootP *bp) {
    const uint32_t *ac = ACCENTS[SET.accent];
    float cap = font_cap(W800, P(330));
    // band 1: hour
    float y0 = lk[0].x;
    float x = bp ? bp->bx[0] : 0;
    g_rect(c, x, y0 - SH, SW, SH + P(330), ac[0]);
    float ca = bp ? bp->content_a : 1;
    if (ca > 0) {
        Canvas *cc_ = c;
        status_line(cc_, y0, c_alpha(C_INK, ca), false, true);
    }
    numeral(c, 0, x - P(14), false, y0 + P(40) + (P(270) + cap) / 2, y0 + P(36), y0 + P(310), bp ? bp->num_in : 1);
    // band 2: minutes
    float y1 = lk[1].x;
    x = bp ? bp->bx[1] : 0;
    g_rect(c, x, y1 + P(310), SW, P(320), ac[1]);
    numeral(c, 1, x + SW + P(14), true, y1 + P(310) + (P(290) + cap) / 2, y1 + P(310), y1 + P(600), bp ? bp->num_in : 1);
    if (bp && bp->typed > 0) {   // boot: "lumen" types in
        int n = (int)bp->typed;
        if (n > 5) n = 5;
        float fs = P(84);
        float w = text_w(W800, fs, -0.05f, "lumen");
        float tx = x + (SW - w) / 2, ty = y1 + P(310) + P(145) + font_cap(W800, fs) / 2;
        uint32_t col = c_alpha(C_INK, bp->word_a);
        float e = text_n(c, W800, fs, -0.05f, tx, ty, col, "lumen", n);
        if (bp->word_a > 0.99f && fmod(now(), 0.5) < 0.3) g_rect(c, e + P(4), ty - font_cap(W800, fs), P(6), font_cap(W800, fs), C_INK);
    }
    // band 3: lime slab
    float y2 = lk[2].x + P(600);
    x = bp ? bp->bx[2] : 0;
    g_rrect(c, x, y2, SW, P(645), P(30), 0, ac[2]);
    if (ca <= 0) return;
    uint32_t ink = c_alpha(C_INK, ca);
    g_rrect(c, x + SW / 2 - P(20), y2 + P(10), P(40), P(5), P(2.5f), P(2.5f), ink);
    char ds[32];
    date_str(ds, sizeof ds, true);
    text(c, W800, P(28), -0.03f, x + P(22), y2 + P(58), ink, ds);
    const char *wt = weather_text();
    text_r(c, W600, P(15), 0, x + P(368), y2 + P(55), ink, wt);
    float wtw = text_w(W600, P(15), 0, wt);
    icon(c, IC_CLOUD, x + P(368) - wtw - P(26), y2 + P(38), P(22), ink);
    float ny = y2 + P(84);
    for (int i = 0; i < 2 && i < nnotif; i++) {
        g_rect(c, x + P(22), ny, SW - P(44), P(1.5f), c_alpha(C_INK, ca * 0.25f));
        float fw = text(c, W800, P(15), 0, x + P(22), ny + P(26), ink, notifs[i].from);
        char *t = text_ellipsize(W400, P(15), notifs[i].text, SW - P(44) - (fw - x - P(22)) - P(12));
        text(c, W400, P(15), 0, fw + P(12), ny + P(26), ink, t);
        free(t);
        ny += P(38);
    }
    if (nnotif == 0) {
        g_rect(c, x + P(22), ny, SW - P(44), P(1.5f), c_alpha(C_INK, ca * 0.25f));
        text(c, W400, P(15), 0, x + P(22), ny + P(26), c_alpha(C_INK, ca * 0.6f), "No notifications");
    }
    float by = SH - P(34);
    icon(c, IC_UP, x + P(22), by - P(15), P(18), ink);
    text(c, W600, P(12.5f), 0.14f, x + P(46), by, ink, "PUSH THIS SLAB UP TO OPEN");
}

static void update_numerals(float dt) {
    time_t t = time(0);
    struct tm lt;
    gmtime_r(&t, &lt);
    char hm[2][3];
    snprintf(hm[0], 3, "%02d", lt.tm_hour);
    snprintf(hm[1], 3, "%02d", lt.tm_min);
    for (int w = 0; w < 2; w++) {
        for (int i = 0; i < 2; i++) {
            if (shown_hm[w][i] && shown_hm[w][i] != hm[w][i]) {   // slot-machine roll
                prev_hm[w][i] = shown_hm[w][i];
                roll[w][i].x = -0.12f * i;   // stagger the second digit
                roll[w][i].v = 0;
            }
            if (!shown_hm[w][i]) { roll[w][i].x = 1; prev_hm[w][i] = 0; }
            roll[w][i].t = 1;
            spring_step(&roll[w][i], dt, 1.6f, 0.75f);
        }
        memcpy(shown_hm[w], hm[w], 3);
    }
}

// ---- home drawing ---------------------------------------------------------------------------------
static void draw_tile(Canvas *c, int d, int i, float alpha) {
    Rect r = tile_rect(d, i);
    App *a = drawers[d].apps[i];
    bool pr = pressed(0x100 + d * 8 + i);
    if (pr) { r.x += P(3); r.y += P(3); r.w -= P(6); r.h -= P(6); }
    uint32_t fg = c_alpha(drawer_fg(d), alpha);
    if (app && app_phase != AP_NONE && app == a) {   // the tile is "lifted" into the app
        g_rrect_line(c, r.x, r.y, r.w, r.h, P(22), P(2), c_alpha(fg, 0.3f));
        hit(0x100 + d * 8 + i, r.x, r.y, r.w, r.h);
        return;
    }
    if (d == 0 && i == 0) {   // the big black Video tile
        g_rrect(c, r.x, r.y, r.w, r.h, P(22), P(22), c_alpha(C_INK, alpha));
        g_circle(c, r.x + P(34), r.y + P(34), P(20), c_alpha(C_ORANGE, alpha));
        icon(c, IC_PLAY_FILL, r.x + P(24), r.y + P(24), P(20), c_alpha(C_INK, alpha));
        text(c, W800, P(24), -0.03f, r.x + P(16), r.y + r.h - P(36), c_alpha(C_PAPER, alpha), "Video");
        const char *ci = video_continue_info();
        text(c, W600, P(13), 0, r.x + P(16), r.y + r.h - P(15), c_alpha(C_MUTED, alpha), ci ? ci : "Search YouTube");
    } else {
        g_rrect_line(c, r.x, r.y, r.w, r.h, P(22), P(2), fg);
        icon(c, a->icon, r.x + P(14), r.y + P(14), P(28), fg);
        text(c, W600, P(15), 0, r.x + P(14), r.y + r.h - P(16), fg, a->name);
    }
    hit(0x100 + d * 8 + i, r.x, r.y, r.w, r.h);
}

static void drawer_info(int d, char *buf, int n) {
    buf[0] = 0;
    if (d == 0) {
        if (video_playing()) snprintf(buf, n, "Now playing");
        else { const char *ci = video_continue_info(); if (ci) snprintf(buf, n, "%s", strstr(ci, "· ") ? strstr(ci, "· ") + 3 : ci); }
    } else if (d == 1) {
        float t = clock_timer_left();
        if (t >= 0) snprintf(buf, n, "Timer %d:%02d", (int)t / 60, (int)t % 60);
        else { int nn = store_geti("notes.count", 0); snprintf(buf, n, "%d note%s", nn, nn == 1 ? "" : "s"); }
    } else if (d == 2) snprintf(buf, n, "Best %d", game_best());
    else {
        NetInfo ni;
        net_info(&ni);
        snprintf(buf, n, "%s", net_state() == NET_UP ? ni.ip : SET.airplane ? "Airplane" : "Offline");
    }
}

static void draw_home(Canvas *c) {
    g_clear(c, TH.bg);
    status_line(c, 0, TH.fg, true, false);
    char ds[32];
    date_str(ds, sizeof ds, false);
    text(c, W800, P(66), -0.06f, P(16), P(120), TH.fg, ds);
    // small muted info on the right
    float t = clock_timer_left();
    char tl[32];
    if (t >= 0) snprintf(tl, sizeof tl, "Timer %d:%02d", (int)t / 60, (int)t % 60);
    else snprintf(tl, sizeof tl, "No timers");
    text_r(c, W600, P(13), 0, P(374), P(88), TH.sub, weather_text());
    text_r(c, W600, P(13), 0, P(374), P(108), t >= 0 ? C_ORANGE : TH.sub, tl);
    // search slab
    bool sp = pressed(0x50);
    g_rrect(c, P(16), P(138), P(358), P(58), P(22), P(22), sp ? C_LIME : (SET.dark ? C_PAPER : C_INK));
    uint32_t sfg = sp ? C_INK : (SET.dark ? C_INK : C_PAPER);
    icon(c, IC_SEARCH, P(32), P(155), P(24), sfg);
    text(c, W600, P(17), 0, P(66), P(173), c_alpha(sfg, 0.75f), "Search the web");
    hit(0x50, P(16), P(138), P(358), P(58));
    // drawers, top to bottom, each overlapping the previous
    for (int d = 0; d < NDRAWER; d++) {
        float y = dtop[d].x;
        uint32_t col = drawer_col(d), fg = drawer_fg(d);
        g_rrect(c, 0, y, SW, SH - y + P(40), P(30), 0, col);
        text(c, W800, P(24), -0.03f, P(22), y + P(40), fg, drawers[d].name);
        // live info + small icons
        float ix = P(368);
        for (int i = drawers[d].napps - 1; i >= 0; i--) {
            icon(c, drawers[d].apps[i]->icon, ix - P(18), y + P(23), P(18), fg);
            ix -= P(26);
        }
        char info[48];
        drawer_info(d, info, sizeof info);
        if (info[0]) text_r(c, W600, P(13), 0, ix - P(4), y + P(37), fg, info);
        hit(0x200 + d, 0, y, SW, P(TAB));
        float next = d + 1 < NDRAWER ? dtop[d + 1].x : SH;
        float avail = next - y - P(TAB);
        if (avail > P(20)) {
            float a = clampf((avail - P(40)) / P(200), 0, 1);
            int sc[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
            g_clip(c, 0, y + P(TAB) - P(4), SW, next + P(30));
            for (int i = 0; i < drawers[d].napps; i++) draw_tile(c, d, i, a);
            // big live numeral in the empty part of the drawer
            if (d == 2) {
                char b[16]; snprintf(b, sizeof b, "%d", game_best());
                text(c, W800, P(120), -0.06f, P(12), next - P(26), c_alpha(fg, a), b);
                text(c, W600, P(13), 0.1f, P(22), next - P(150), c_alpha(fg, a), "BEST SCORE");
            } else if (d == 1) {
                float tt = clock_timer_left();
                char b[16];
                if (tt >= 0) snprintf(b, sizeof b, "%d:%02d", (int)tt / 60, (int)tt % 60);
                else { time_t now_ = time(0); struct tm lt; gmtime_r(&now_, &lt); strftime(b, sizeof b, "%H:%M", &lt); }
                text(c, W800, P(120), -0.06f, P(12), next - P(26), c_alpha(fg, a), b);
                text(c, W600, P(13), 0.1f, P(22), next - P(150), c_alpha(fg, a), tt >= 0 ? "TIMER RUNNING" : "LOCAL TIME");
            } else if (d == 3) {
                text(c, W800, P(64), -0.05f, P(16), next - P(70), c_alpha(fg, a), "Lumen 2");
                NetInfo ni; net_info(&ni);
                char b[64];
                snprintf(b, sizeof b, "%s  ·  %s", net_state() == NET_UP ? "Online" : "Offline", ni.ip[0] ? ni.ip : "no address");
                text(c, W600, P(14), 0, P(22), next - P(40), c_alpha(fg, a), b);
            }
            c->cx0 = sc[0]; c->cy0 = sc[1]; c->cx1 = sc[2]; c->cy1 = sc[3];
        }
    }
}

// ---- control center -------------------------------------------------------------------------------
static void cc_slider_draw(Canvas *c, float y, int id, int ic, const char *label, int val) {
    float x = P(16), w = P(358), h = P(56);
    g_rrect_line(c, x, y, w, h, P(18), P(2), C_INK);
    float fw = fmaxf(h, w * val / 100.0f);
    g_rrect(c, x, y, fw, h, P(18), P(18), C_INK);
    icon(c, ic, x + P(16), y + P(16), P(24), C_PAPER);
    char b[16]; snprintf(b, sizeof b, "%d%%", val);
    text(c, W600, P(15), 0, x + P(50), y + P(34), fw > P(140) ? C_PAPER : C_INK, label);
    text_r(c, W800, P(18), 0, x + w - P(16), y + P(35), fw > w - P(50) ? C_PAPER : C_INK, b);
    hit(id, x, y - P(6), w, h + P(12));
}

static void draw_cc(Canvas *c) {
    bool np = video_playing() || video_now_title();
    float h1 = P(250), h2 = P(96), h3 = P(96), h4 = np ? P(110) : 0;
    cc_h = h1 + h2 + h3 + h4;
    float off = (cc.x - 1) * (cc_h + P(40));
    g_rect(c, 0, 0, SW, SH, c_alpha(C_INK, 0.6f * clampf(cc.x, 0, 1)));
    hit(0x300, 0, 0, SW, SH);   // scrim: tap closes
    float b1 = off + h1, b2 = b1 + h2, b3 = b2 + h3, b4 = b3 + h4;
    // bottom-most first so each upper slab overlaps the one below
    if (np) {
        g_rrect(c, 0, off - P(40), SW, b4 - off + P(40), 0, P(30), C_ORANGE);
        const char *t = video_now_title();
        text(c, W600, P(12), 0.12f, P(22), b3 + P(30), C_INK, "NOW PLAYING");
        char *tt = text_ellipsize(W800, P(19), t ? t : "", P(270));
        text(c, W800, P(19), -0.02f, P(22), b3 + P(60), C_INK, tt);
        free(tt);
        btn_icon(c, 0x310, P(312), b3 + P(26), P(52), video_playing() ? IC_PAUSE : IC_PLAY, C_INK, 0);
    }
    g_rrect(c, 0, off - P(40), SW, b3 - off + P(40), 0, P(30), C_LILAC);
    cc_slider_draw(c, b2 + P(16), 0x302, SET.muted ? IC_MUTE : IC_SPEAKER, "Volume", SET.vol);
    g_rrect(c, 0, off - P(40), SW, b2 - off + P(40), 0, P(30), C_SKY);
    cc_slider_draw(c, b1 + P(16), 0x301, IC_SUN, "Brightness", SET.bright);
    g_rrect(c, 0, off - P(40), SW, b1 - off + P(40), 0, P(30), C_LIME);
    status_line(c, off, C_INK, true, false);
    text(c, W800, P(40), -0.04f, P(20), off + P(92), C_INK, "Control");
    struct { int id, ic, on; const char *l; } tg[4] = {
        { 0x320, SET.wifi ? IC_WIFI : IC_WIFI_OFF, SET.wifi && !SET.airplane, "Wi-Fi" },
        { 0x321, IC_AIRPLANE, SET.airplane, "Airplane" },
        { 0x322, SET.dark ? IC_MOON : IC_SUN, SET.dark, SET.dark ? "Dark" : "Light" },
        { 0x323, SET.muted ? IC_MUTE : IC_SPEAKER, SET.muted, "Mute" },
    };
    float tw = P(82), gap = (P(358) - 4 * tw) / 3;
    for (int i = 0; i < 4; i++) {
        float x = P(16) + i * (tw + gap), y = off + P(112);
        bool pr = pressed(tg[i].id);
        if (tg[i].on || pr) g_rrect(c, x, y, tw, tw, P(22), P(22), pr ? C_PAPER : C_INK);
        else g_rrect_line(c, x, y, tw, tw, P(22), P(2), C_INK);
        uint32_t fg = tg[i].on && !pr ? C_LIME : C_INK;
        icon(c, tg[i].ic, x + P(14), y + P(14), P(26), fg);
        text(c, W600, P(13), 0, x + P(14), y + tw - P(13), tg[i].on && !pr ? C_PAPER : C_INK, tg[i].l);
        if (i == 0 && net_state() == NET_UP && SET.wifi) g_circle(c, x + tw - P(18), y + P(20), P(4), C_LIME);
        hit(tg[i].id, x, y, tw, tw);
    }
    if (cc.x > 0.5f) g_rrect(c, SW / 2 - P(20), b4 - P(14) - (np ? 0 : 0), P(40), P(5), P(2.5f), P(2.5f), C_INK);
}

static void cc_set_slider(float x) {
    int v = (int)clampf((x - P(16)) / P(358) * 100, 0, 100);
    if (cc_slider == 1) SET.bright = maxi(v, 5);
    else { SET.vol = v; if (v > 0) SET.muted = 0; }
}

static void cc_toggle(int id) {
    if (id == 0x320) { SET.wifi = !SET.wifi; if (SET.wifi) SET.airplane = 0; net_set_enabled(SET.wifi && !SET.airplane); }
    else if (id == 0x321) { SET.airplane = !SET.airplane; net_set_enabled(SET.wifi && !SET.airplane); }
    else if (id == 0x322) { SET.dark = !SET.dark; theme_apply(); }
    else if (id == 0x323) SET.muted = !SET.muted;
    else if (id == 0x310) video_toggle_play();
    settings_save();
}

// ---- app launch / close ------------------------------------------------------------------------------
static Rect full_rect(void) { return (Rect){ 0, 0, SW, SH }; }
static Rect lerp_rect(Rect a, Rect b, float t) {
    return (Rect){ lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.w, b.w, t), lerpf(a.h, b.h, t) };
}

static void launch(App *a, Rect src) {
    for (int d = 0; d < NDRAWER; d++)
        for (int i = 0; i < drawers[d].napps; i++)
            if (drawers[d].apps[i] == a) { app_d = d; app_i = i; }
    app = a;
    app_col = drawer_col(app_d);
    from = src; to = full_rect();
    app_u = (Spring){ 0, 0, 1 };
    app_phase = AP_OPENING;
    content_a = 0;
    LOG("shell: open %s", a->name);
    if (a->open) a->open();
}

static const char *launch_arg;
const char *shell_launch_arg(void) { return launch_arg; }
void shell_launch(App *a, const char *arg) {
    launch_arg = arg;
    if (app && app_phase != AP_NONE) { app = a; if (a->open) a->open(); return; }
    Rect src = { P(16), P(138), P(358), P(58) };
    for (int d = 0; d < NDRAWER; d++)
        for (int i = 0; i < drawers[d].napps; i++)
            if (drawers[d].apps[i] == a && !arg) src = tile_rect(d, i);
    launch(a, src);
}

static void close_app(void) {
    if (!app) return;
    // make sure the app's drawer is open so it can shrink back into its tile
    if (open_drawer != app_d) {
        open_drawer = app_d;
        for (int i = 0; i < NDRAWER; i++) dtop[i].x = dtop[i].t = drawer_target(i, open_drawer);
    }
    from = cur_rect;
    to = tile_rect(app_d, app_i);
    app_u = (Spring){ 0, 0, 1 };
    app_phase = AP_CLOSING;
    LOG("shell: close %s", app->name);
}
void shell_go_home(void) { if (app && (app_phase == AP_OPEN || app_phase == AP_OPENING)) { cur_rect = full_rect(); close_app(); } }

// ---- input --------------------------------------------------------------------------------------------
static void set_drawer(int d) {
    if (d < 0 || d >= NDRAWER) return;
    open_drawer = d;
    for (int i = 0; i < NDRAWER; i++) dtop[i].t = drawer_target(i, d);
}

static void home_event(InputEv *e) {
    static Gest g;
    int r = gest(&g, e);
    if (r == G_TAP) {
        int id = hit_at(e->x, e->y);
        if (id == 0x50) shell_launch(&app_web, "search");
        else if (id >= 0x200 && id < 0x210) set_drawer(id - 0x200);
        else if (id >= 0x100 && id < 0x200) {
            int d = (id - 0x100) / 8, i = (id - 0x100) % 8;
            launch(drawers[d].apps[i], tile_rect(d, i));
        }
    } else if (r == G_DRAG_START && g.vert) {
        int id = hit_at(g.x0, g.y0);
        int tab = -1;
        for (int d = NDRAWER - 1; d >= 0; d--) if (g.y0 >= dtop[d].x) { tab = d; break; }
        (void)id;
        if (tab >= 0) {
            // dragging up on a collapsed drawer (below the open one) opens it;
            // dragging down on the open drawer (or above) opens the one above
            if (g.y < g.y0 && tab > open_drawer) { drag_tab = tab; drag_to = tab; }
            else if (g.y > g.y0 && tab >= 1 && tab <= open_drawer) { drag_tab = tab; drag_to = tab - 1; }
            else if (g.y < g.y0 && tab == open_drawer && tab + 1 < NDRAWER) { drag_tab = tab + 1; drag_to = tab + 1; }
        }
    }
    if ((r == G_DRAG || r == G_DRAG_START) && drag_tab >= 0) {
        float a = drawer_target(drag_tab, open_drawer), b = drawer_target(drag_tab, drag_to);
        drag_p = clampf((g.y - g.y0) / (b - a), 0, 1);
        for (int i = 0; i < NDRAWER; i++) {
            dtop[i].x = lerpf(drawer_target(i, open_drawer), drawer_target(i, drag_to), drag_p);
            dtop[i].v = 0;
        }
    }
    if (r == G_DRAG_END && drag_tab >= 0) {
        float dir = drawer_target(drag_tab, drag_to) - drawer_target(drag_tab, open_drawer);
        bool fling = (dir < 0 && g.vy < -P(500)) || (dir > 0 && g.vy > P(500));
        if (drag_p > 0.3f || fling) set_drawer(drag_to);
        else set_drawer(open_drawer);
        for (int i = 0; i < NDRAWER; i++) dtop[i].v = g.vy * 0.3f;
        drag_tab = -1;
    }
}

static void lock_event(InputEv *e) {
    int r = gest(&lg, e);
    if (r == G_DOWN) lk_drag = e->y > lk[2].x + P(590);
    if (!lk_drag || unlocking) return;
    if (r == G_DRAG || r == G_DRAG_START) {
        float dy = lg.y - lg.y0;
        lk[2].x = dy < 0 ? dy : dy * 0.25f;
        lk[2].v = 0;
    } else if (r == G_DRAG_END || r == G_TAP) {
        float dy = lg.y - lg.y0;
        if (dy < -P(160) || (lg.vy < -P(700) && dy < -P(30))) {
            unlocking = 1;
            for (int i = 0; i < 3; i++) lk[i].t = -SH - P(40);
            lk[2].v = fminf(lg.vy, -P(1500));
            LOG("shell: unlock");
        } else for (int i = 0; i < 3; i++) lk[i].t = 0;
        lk_drag = 0;
    }
}

static bool cc_event(InputEv *e) {
    int r = gest(&cg, e);
    if (cc_drag) {   // pulling the control center down (or pushing it up)
        float H = fmaxf(cc_h, P(400));
        if (r == G_DRAG || r == G_DRAG_START) {
            cc.x = cc_drag == 1 ? clampf(cg.y / H, 0, 1.05f) : clampf(1 + (cg.y - cg.y0) / H, 0, 1);
            cc.v = 0;
        }
        if (r == G_DRAG_END || r == G_TAP) {
            bool open = cc_drag == 1 ? (cc.x > 0.3f || cg.vy > P(600)) && cg.vy > -P(300) : !(cc.x < 0.75f || cg.vy < -P(500));
            cc.t = open ? 1 : 0;
            cc.v = cg.vy / fmaxf(cc_h, P(400));
            cc_drag = 0;
        }
        return true;
    }
    if (cc.t < 0.5f) return false;
    if (r == G_DOWN) {
        int id = hit_at(e->x, e->y);
        if (id == 0x301 || id == 0x302) { cc_slider = id == 0x301 ? 1 : 2; cc_set_slider(e->x); }
    } else if ((r == G_DRAG || r == G_DRAG_START) && cc_slider) cc_set_slider(e->x);
    else if (r == G_DRAG_START && cg.vert && cg.y < cg.y0) cc_drag = 2;
    else if (r == G_TAP) {
        int id = hit_at(e->x, e->y);
        if (cc_slider) { cc_slider = 0; settings_save(); }
        else if (id == 0x300 && e->y > cc_h) cc.t = 0;
        else if (id >= 0x310 && id < 0x330) cc_toggle(id);
    } else if (r == G_DRAG_END && cc_slider) { cc_slider = 0; settings_save(); }
    return true;
}

void shell_event(InputEv *e) {
    if (e->type == T_KEY) {
        if (e->key == K_ESC && app) { shell_go_home(); return; }
        if (app && app_phase == AP_OPEN && app->key && cc.t < 0.5f) app->key(e->key);
        return;
    }
    if (st == ST_BOOT) return;
    // control center: open, being dragged, or a pull from the top edge
    if (e->type == T_DOWN && cc.t < 0.5f && e->y < P(26) && !cc_drag) { cc_drag = 1; gest(&cg, e); return; }
    if (cc_drag || cc.t >= 0.5f || cc.x > 0.02f) { if (cc_event(e)) return; }
    if (st == ST_LOCK) { lock_event(e); return; }
    if (app && app_phase != AP_NONE) {
        // home gesture: swipe up from the bottom edge
        if (e->type == T_DOWN && e->y > SH - P(24) && app_phase == AP_OPEN) { gest(&hg, e); app_phase = AP_DRAG; hg_p = 0; return; }
        if (app_phase == AP_DRAG) {
            int r = gest(&hg, e);
            if (r == G_DRAG || r == G_DRAG_START) hg_p = clampf((hg.y0 - hg.y) / P(520), 0, 1);
            if (r == G_DRAG_END || r == G_TAP) {
                if (hg_p > 0.16f || hg.vy < -P(700)) close_app();
                else { from = cur_rect; to = full_rect(); app_u = (Spring){ 0, 0, 1 }; app_phase = AP_RETURN; }
            }
            return;
        }
        if (app_phase == AP_OPEN && app->touch) app->touch(e);
        return;
    }
    home_event(e);
}

// ---- frame --------------------------------------------------------------------------------------------
static void draw_touch(Canvas *c) {
    if (!touch_down) return;
    g_circle(c, touch_x, touch_y, P(21), 0x50F3F0E8u);
    g_ring(c, touch_x, touch_y, P(21), P(1.6f), 0x70141414u);
}

void shell_frame(Canvas *c, float dt) {
    hit_frame();
    clock_tick(dt);
    update_numerals(dt);
    for (int i = 0; i < NDRAWER; i++) if (drag_tab < 0) spring_step(&dtop[i], dt, 2.6f, 0.78f);

    if (st == ST_BOOT) {
        // bands slam in from alternating sides with overshoot, "lumen" types in, then settle
        float t = (float)(now() - boot_t0);
        BootP bp = { 0 };
        static Spring bx[3];
        static int init;
        if (!init) { for (int i = 0; i < 3; i++) bx[i] = (Spring){ i % 2 ? SW * 1.1f : -SW * 1.1f, 0, 0 }; init = 1; }
        static int chimed;
        static const float notes[3] = { 523.25f, 659.25f, 783.99f };
        for (int i = 0; i < 3; i++) if (t > 0.25f + i * 0.17f) {
            if (chimed <= i) { audio_tone(notes[i], 0.35f, 0.5f); chimed = i + 1; }   // boot chime, one note per slab
            spring_step(&bx[i], dt, 1.9f, 0.42f);
        }
        for (int i = 0; i < 3; i++) bp.bx[i] = bx[i].x;
        bp.typed = t < 1.05f ? 0 : (t - 1.05f) / 0.09f + 1;
        bp.word_a = clampf(1 - (t - 2.25f) / 0.25f, 0, 1);
        static Spring nin = { 0, 0, 0 };
        if (t > 2.3f) { nin.t = 1; spring_step(&nin, dt, 1.5f, 0.7f); }
        bp.num_in = nin.x;
        bp.content_a = clampf((t - 2.5f) / 0.4f, 0, 1);
        g_clear(c, C_BLACK);
        draw_lock(c, &bp);
        if (t > 3.2f) { st = ST_LOCK; LOG("shell: boot animation done (%.2f s)", t); }
        draw_touch(c);
        return;
    }

    if (st == ST_LOCK) {
        for (int i = 0; i < 3; i++) {
            if (lk_drag) break;
            float f = unlocking ? (i == 2 ? 2.4f : i == 1 ? 1.9f : 1.6f) : (i == 2 ? 3.5f : i == 1 ? 3.0f : 2.4f);
            spring_step(&lk[i], dt, f, unlocking ? 0.9f : 0.62f);
        }
        if (lk_drag) {   // the bands above get pushed with lag, like stacked paper
            lk[1].t = lk[2].x * 0.93f;
            lk[0].t = lk[2].x * 0.86f;
            spring_step(&lk[1], dt, 5.5f, 0.85f);
            spring_step(&lk[0], dt, 4.2f, 0.85f);
        }
        if (lk[2].x < -P(4) || unlocking) draw_home(c);
        draw_lock(c, NULL);
        if (unlocking && lk[0].x < -P(640) - P(40)) { st = ST_HOME; unlocking = 0; LOG("shell: home"); }
    } else {
        draw_home(c);
    }

    if (app && app_phase != AP_NONE) {
        if (app_phase == AP_DRAG) {
            float s = 1 - 0.55f * hg_p;
            Rect r = { 0, 0, SW * s, SH * s };
            r.x = SW / 2 - r.w / 2 + (hg.x - hg.x0) * 0.5f;
            r.y = hg.y + (SH - hg.y0) - r.h + (hg.y0 - hg.y) * 0.25f;
            r.y = fminf(r.y, SH - r.h);
            cur_rect = r;
            content_a = 1;
        } else {
            float f = app_phase == AP_CLOSING ? 2.6f : 2.3f;
            spring_step(&app_u, dt, f, app_phase == AP_CLOSING ? 0.9f : 0.86f);
            float u = clampf(app_u.x, 0, 1.03f);
            cur_rect = lerp_rect(from, to, u);
            if (app_phase == AP_OPENING) content_a = clampf((app_u.x - 0.72f) / 0.25f, 0, 1);
            else if (app_phase == AP_CLOSING) content_a = clampf(1 - (app_u.x - 0.25f) / 0.45f, 0, 1);
            else if (app_phase == AP_RETURN) content_a = 1;
            if ((app_phase == AP_OPENING || app_phase == AP_RETURN) && spring_rest(&app_u, 0.002f)) { app_phase = AP_OPEN; cur_rect = full_rect(); }
            if (app_phase == AP_CLOSING && app_u.x > 0.995f) { app_phase = AP_NONE; app = NULL; }
        }
        if (app) {
            Rect r = cur_rect;
            float full = clampf(r.w / SW, 0, 1);
            float rad = app_phase == AP_OPEN ? 0 : lerpf(P(22), P(34), full);
            if (app_phase == AP_OPEN) {
                shell_app_alpha = 1;
                app->draw(c, dt);
            } else {
                g_rrect(c, r.x, r.y, r.w, r.h, rad, rad, app_col);
                if (full < 0.98f) g_rrect_line(c, r.x, r.y, r.w, r.h, rad, P(2), c_alpha(drawer_fg(app_d), 1 - full));
                if (content_a > 0.01f) {
                    shell_app_alpha = content_a;
                    app->draw(layer, dt);
                    g_layer_rect(c, layer, r.x, r.y, r.w, r.h, rad, (int)(content_a * 255));
                }
            }
        }
    }

    if (cc.x > 0.002f || cc_drag || cc.t > 0) {
        if (!cc_drag) spring_step(&cc, dt, 2.4f, 0.8f);
        if (cc.x < 0.002f && cc.t == 0 && !cc_drag) cc.x = 0;
        else draw_cc(c);
    }
    if (SET.bright < 100) g_dim(c, 0.3f + 0.7f * SET.bright / 100.0f);
    draw_touch(c);
}
