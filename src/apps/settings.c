// Settings: accent set (lock band colours) with live preview, dark/light,
// brightness, volume, text size, Network page, About page.
#include "../ui.h"

static int page;              // 0 main, 1 network, 2 about
static Gest g;
static Scroll sc;
static int slider;            // 1 brightness, 2 volume
static float sl_x0, sl_w;
static Spring page_x;
static char test_result[128];
static volatile int testing;

void net_test(char *out, int n);   // net/net.c
static void *test_th(void *a) {
    (void)a;
    char r[128];
    net_test(r, sizeof r);
    snprintf(test_result, sizeof test_result, "%s", r);
    testing = 0;
    return 0;
}

static void slab_slider(Canvas *c, int id, float x, float y, float w, float h, int val, int ic, const char *label, uint32_t slab) {
    g_rrect(c, x, y, w, h, P(22), P(22), slab);
    float fw = fmaxf(h, w * val / 100.0f);
    g_rrect(c, x, y, fw, h, P(22), P(22), C_INK);
    icon(c, ic, x + P(18), y + h / 2 - P(12), P(24), slab);
    char b[16];
    snprintf(b, sizeof b, "%d%%", val);
    text(c, W600, P(16), 0, x + P(54), y + h / 2 + P(6), fw > P(150) ? C_PAPER : C_INK, label);
    text_r(c, W800, P(20), 0, x + w - P(18), y + h / 2 + P(7), fw > w - P(60) ? C_PAPER : C_INK, b);
    hit(id, x, y, w, h);
    if (id == 0xF10) { sl_x0 = x; sl_w = w; }
}

static void segmented(Canvas *c, int id0, float x, float y, float w, float h, const char **labels, int n, int sel) {
    float sw = w / n;
    g_rrect_line(c, x, y, w, h, h / 2, P(2), TH.fg);
    g_rrect(c, x + sel * sw, y, sw, h, h / 2, h / 2, TH.fg);
    for (int i = 0; i < n; i++) {
        text_c(c, W600, P(15), 0, x + i * sw + sw / 2, y + h / 2 + P(5.5f), i == sel ? TH.bg : TH.fg, labels[i]);
        hit(id0 + i, x + i * sw, y, sw, h);
    }
}

static void mini_lock(Canvas *c, float x, float y, float w, float h, int accent, bool numerals) {
    const uint32_t *ac = ACCENTS[accent];
    float b1 = h * 0.367f, b2 = h * 0.71f;
    g_rrect(c, x, y, w, h, P(16), P(16), ac[0]);
    g_rect(c, x, y + b1, w, b2 - b1 + P(10), ac[1]);
    g_rrect(c, x, y + b2, w, h - b2, P(10), P(16), ac[2]);
    if (numerals) {
        time_t t = time(0);
        struct tm lt;
        gmtime_r(&t, &lt);
        char hh[4], mm[4];
        snprintf(hh, 4, "%02d", lt.tm_hour); snprintf(mm, 4, "%02d", lt.tm_min);
        int sc_[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
        g_clip(c, x, y, x + w, y + h);
        float fs = h * 0.36f;
        text(c, W800, fs, -0.06f, x - P(3), y + b1 - P(8), C_INK, hh);
        text_r(c, W800, fs, -0.06f, x + w + P(3), y + b2 - P(8), C_INK, mm);
        c->cx0 = sc_[0]; c->cy0 = sc_[1]; c->cx1 = sc_[2]; c->cy1 = sc_[3];
        g_rrect(c, x + w / 2 - P(10), y + b2 + P(5), P(20), P(3), P(1.5f), P(1.5f), C_INK);
    }
    if (!SET.dark) g_rrect_line(c, x, y, w, h, P(16), P(1.5f), TH.line);
}

static float row(Canvas *c, int id, float y, const char *label, const char *value) {
    bool pr = pressed(id);
    if (pr) g_rrect(c, P(16), y, P(358), P(64), P(22), P(22), C_LIME);
    else g_rrect_line(c, P(16), y, P(358), P(64), P(22), P(2), TH.fg);
    uint32_t fg = pr ? C_INK : TH.fg;
    text(c, W800, P(19), -0.02f, P(34), y + P(39), fg, label);
    float vx = P(330);
    icon(c, IC_CHEVRON, vx, y + P(22), P(20), fg);
    if (value) text_r(c, W600, P(14), 0, vx - P(6), y + P(38), pr ? C_INK : TH.sub, value);
    hit(id, P(16), y, P(358), P(64));
    return y + P(76);
}

static void header(Canvas *c, const char *title, bool back) {
    g_rect(c, 0, 0, SW, P(176), SET.dark ? C_PAPER : C_INK);
    uint32_t fg = SET.dark ? C_INK : C_PAPER;
    status_line(c, 0, fg, true, false);
    if (back) btn_icon(c, 0xF01, P(14), P(56), P(48), IC_BACK, fg, 0);
    text(c, W800, P(52), -0.05f, P(18), P(150), fg, title);
}

static void draw_main(Canvas *c) {
    float y = P(176) + P(20) - sc.pos;
    float y0 = y;
    text(c, W600, P(13), 0.12f, P(22), y + P(14), TH.sub, "LOCK SCREEN ACCENT SET");
    y += P(28);
    mini_lock(c, P(16), y, P(140), P(300), SET.accent, true);
    text(c, W800, P(30), -0.03f, P(172), y + P(36), TH.fg, ACCENT_NAMES[SET.accent]);
    text(c, W400, P(14), 0, P(172), y + P(60), TH.sub, "The three bands on");
    text(c, W400, P(14), 0, P(172), y + P(78), TH.sub, "your lock screen.");
    for (int i = 0; i < NACCENT; i++) {
        float sx = P(172) + (i % 3) * P(68), sy = y + P(110) + (i / 3) * P(96);
        mini_lock(c, sx, sy, P(56), P(84), i, false);
        if (i == SET.accent) g_rrect_line(c, sx - P(5), sy - P(5), P(66), P(94), P(20), P(3), TH.fg);
        hit(0xF20 + i, sx - P(5), sy - P(5), P(66), P(94));
    }
    y += P(326);
    text(c, W600, P(13), 0.12f, P(22), y + P(14), TH.sub, "APPEARANCE");
    const char *dl[2] = { "Dark", "Light" };
    segmented(c, 0xF30, P(16), y + P(26), P(358), P(52), dl, 2, SET.dark ? 0 : 1);
    y += P(96);
    slab_slider(c, 0xF10, P(16), y, P(358), P(60), SET.bright, IC_SUN, "Brightness", C_SKY);
    y += P(72);
    slab_slider(c, 0xF11, P(16), y, P(358), P(60), SET.vol, SET.muted ? IC_MUTE : IC_SPEAKER, "Volume", C_LILAC);
    y += P(84);
    text(c, W600, P(13), 0.12f, P(22), y + P(14), TH.sub, "TEXT SIZE  ·  WEB AND NOTES");
    const char *ts[3] = { "Small", "Medium", "Large" };
    segmented(c, 0xF40, P(16), y + P(26), P(358), P(52), ts, 3, SET.textsize);
    y += P(100);
    NetInfo ni;
    net_info(&ni);
    y = row(c, 0xF50, y, "Network", net_state() == NET_UP ? ni.ip : "Offline");
    y = row(c, 0xF51, y, "About", "Lumen OS 2");
    btn_text(c, 0xF52, P(16), y + P(6), P(358), P(56), "Restart phone", C_INK, C_ORANGE);
    y += P(100);
    sc.max = y + sc.pos - y0 - (SH - P(176)) + P(40);
}

static void kv(Canvas *c, float *y, const char *k, const char *v) {
    g_rect(c, P(16), *y, P(358), P(1.5f), TH.line);
    text(c, W600, P(15), 0, P(22), *y + P(32), TH.sub, k);
    text_r(c, W800, P(17), 0, P(368), *y + P(32), TH.fg, v);
    *y += P(48);
}

static void draw_network(Canvas *c) {
    float y = P(176) + P(24);
    NetInfo ni;
    net_info(&ni);
    int ns = net_state();
    g_rrect(c, P(16), y, P(358), P(110), P(26), P(26), ns == NET_UP ? C_LIME : ns == NET_CONNECTING ? C_SKY : C_LILAC);
    icon(c, ns == NET_UP ? IC_WIFI : IC_WIFI_OFF, P(32), y + P(22), P(30), C_INK);
    text(c, W800, P(34), -0.03f, P(32), y + P(92), C_INK, ns == NET_UP ? "Connected" : ns == NET_CONNECTING ? "Connecting…" : "No link");
    text_r(c, W600, P(14), 0, P(358), y + P(42), C_INK, "virtio-net · DHCP");
    y += P(132);
    kv(c, &y, "IP address", ni.ip[0] ? ni.ip : "—");
    kv(c, &y, "Gateway", ni.gw[0] ? ni.gw : "—");
    kv(c, &y, "DNS", ni.dns[0] ? ni.dns : "—");
    kv(c, &y, "MAC", ni.mac[0] ? ni.mac : "—");
    kv(c, &y, "Wi-Fi", SET.airplane ? "Airplane mode" : SET.wifi ? "On" : "Off");
    y += P(16);
    btn_text(c, 0xF60, P(16), y, P(358), P(60), testing ? "Testing…" : "Test connection", C_INK, C_ORANGE);
    y += P(80);
    if (test_result[0]) {
        bool ok = !strncmp(test_result, "OK", 2);
        g_rrect(c, P(16), y, P(358), P(70), P(22), P(22), ok ? C_LIME : C_LILAC);
        char *t = text_ellipsize(W600, P(15), test_result, P(320));
        text(c, W600, P(15), 0, P(32), y + P(42), C_INK, t);
        free(t);
    }
}

static void draw_about(Canvas *c) {
    float y = P(176) + P(30);
    text(c, W800, P(64), -0.06f, P(14), y + P(56), TH.fg, "Lumen OS 2");
    text(c, W600, P(16), 0, P(20), y + P(88), TH.sub, "Version 2.0 \"Slab\" · built by Claude");
    y += P(116);
    g_rrect(c, P(16), y, P(358), P(84), P(22), P(22), C_LIME);
    text(c, W800, P(18), 0, P(32), y + P(34), C_INK, "Written from scratch");
    text(c, W400, P(14), 0, P(32), y + P(58), C_INK, "init, compositor, UI, apps, DHCP, DNS, HTTP,");
    text(c, W400, P(14), 0, P(32), y + P(75), C_INK, "HTML/CSS engine, audio & video playback");
    y += P(100);
    g_rrect(c, P(16), y, P(358), P(64), P(22), P(22), C_SKY);
    text(c, W800, P(17), 0, P(32), y + P(28), C_INK, "No JavaScript");
    text(c, W400, P(14), 0, P(32), y + P(49), C_INK, "Web renders HTML and CSS only.");
    y += P(84);
    text(c, W600, P(13), 0.12f, P(22), y, TH.sub, "THE ONLY THIRD-PARTY CODE");
    y += P(14);
    const char *tp[][2] = {
        { "Linux kernel 6.12", "GPL-2.0" }, { "musl libc", "MIT" }, { "stb_truetype", "public domain" },
        { "stb_image (PNG/JPEG)", "public domain" }, { "BearSSL (TLS)", "MIT" }, { "Mozilla CA bundle", "MPL-2.0 data" },
        { "Bricolage Grotesque", "OFL font" },
    };
    for (int i = 0; i < 7; i++) kv(c, &y, tp[i][0], tp[i][1]);
}

static void open_(void) { page = 0; sc = (Scroll){ 0 }; }

static void draw(Canvas *c, float dt) {
    g_clear(c, TH.bg);
    scroll_step(&sc, dt);
    int s[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
    g_clip(c, 0, P(176), SW, SH);
    if (page == 0) draw_main(c);
    else if (page == 1) draw_network(c);
    else draw_about(c);
    c->cx0 = s[0]; c->cy0 = s[1]; c->cx1 = s[2]; c->cy1 = s[3];
    header(c, page == 0 ? "Settings" : page == 1 ? "Network" : "About", page != 0);
    home_bar(c, TH.fg);
}

static void set_slider(float x) {
    int v = (int)clampf((x - sl_x0) / sl_w * 100, 0, 100);
    if (slider == 1) SET.bright = maxi(v, 5);
    else { SET.vol = v; if (v) SET.muted = 0; }
}

static void touch(InputEv *e) {
    int r = gest(&g, e);
    if (r == G_DOWN && page == 0) {
        int id = hit_at(e->x, e->y);
        if (id == 0xF10 || id == 0xF11) { slider = id - 0xF0F; set_slider(e->x); }
        return;
    }
    if (slider) {
        if (r == G_DRAG || r == G_DRAG_START) set_slider(e->x);
        if (r == G_DRAG_END || r == G_TAP) { slider = 0; settings_save(); if (SET.vol) audio_tone(880, 0.12f, 0.6f); }
        return;
    }
    if (r == G_DRAG_START || r == G_DRAG) { if (g.vert && page == 0) scroll_drag(&sc, r == G_DRAG_START ? g.y - g.y0 : g.dy); }
    else if (r == G_DRAG_END) { if (sc.held) scroll_fling(&sc, g.vy); }
    else if (r == G_TAP) {
        int id = hit_at(e->x, e->y);
        if (id >= 0xF20 && id < 0xF20 + NACCENT) { SET.accent = id - 0xF20; LOG("settings: accent %s", ACCENT_NAMES[SET.accent]); }
        else if (id == 0xF30 || id == 0xF31) { SET.dark = id == 0xF30; theme_apply(); LOG("settings: %s mode", SET.dark ? "dark" : "light"); }
        else if (id >= 0xF40 && id < 0xF43) SET.textsize = id - 0xF40;
        else if (id == 0xF50) page = 1;
        else if (id == 0xF51) page = 2;
        else if (id == 0xF01) page = 0;
        else if (id == 0xF52) { settings_save(); request_reboot(); }
        else if (id == 0xF60 && !testing) {
            testing = 1; test_result[0] = 0;
            pthread_t t; pthread_create(&t, 0, test_th, 0); pthread_detach(t);
        }
        settings_save();
    }
}
App app_settings = { "Settings", IC_SETTINGS, open_, draw, touch, 0, 0 };
