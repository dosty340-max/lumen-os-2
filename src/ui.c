// Slab UI toolkit: springs, gestures, scroll physics, hit regions, line icons,
// status line, on-screen keyboard, common app chrome.
#include "lumen.h"
#include "ui.h"

Theme TH;
Settings SET = { .accent = 0, .dark = 1, .bright = 100, .vol = 70, .textsize = 1, .wifi = 1 };

void theme_apply(void) {
    if (SET.dark) TH = (Theme){ C_INK, C_PAPER, C_MUTED, 0x26F3F0E8u, C_PAPER };
    else TH = (Theme){ C_PAPER, C_INK, 0xFF6E6A62u, 0x26141414u, C_INK };
}

const uint32_t ACCENTS[NACCENT][3] = {
    { C_ORANGE, C_LILAC, C_LIME },
    { C_SKY, C_LIME, C_ORANGE },
    { C_LILAC, C_ORANGE, C_SKY },
    { C_LIME, C_SKY, C_LILAC },
    { C_ORANGE, C_SKY, C_PAPER },
};
const char *ACCENT_NAMES[NACCENT] = { "Original", "Pool", "Sunset", "Citrus", "Signal" };

// ---- springs ------------------------------------------------------------------
void spring_step(Spring *s, float dt, float freq, float zeta) {
    float w = 2 * (float)M_PI * freq;
    int n = (int)ceilf(dt / 0.002f);
    if (n < 1) n = 1;
    float h = dt / n;
    for (int i = 0; i < n; i++) {
        float a = -w * w * (s->x - s->t) - 2 * zeta * w * s->v;
        s->v += a * h;
        s->x += s->v * h;
    }
}
bool spring_rest(Spring *s, float eps) { return fabsf(s->x - s->t) < eps && fabsf(s->v) < eps * 10; }

// ---- hit regions (registered while drawing, queried on input) --------------------
#define MAXHIT 512
typedef struct { int id; float x, y, w, h; } Hit;
static Hit hits[2][MAXHIT];
static int nhit[2], cur;
void hit_frame(void) { cur ^= 1; nhit[cur] = 0; }
void hit(int id, float x, float y, float w, float h) {
    if (nhit[cur] < MAXHIT) hits[cur][nhit[cur]++] = (Hit){ id, x, y, w, h };
}
int hit_at(float x, float y) {   // query last completed frame, topmost first
    int f = cur ^ 1;
    for (int i = nhit[f] - 1; i >= 0; i--) {
        Hit *h = &hits[f][i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) return h->id;
    }
    // also the frame being built (for regions registered earlier this frame)
    for (int i = nhit[cur] - 1; i >= 0; i--) {
        Hit *h = &hits[cur][i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) return h->id;
    }
    return 0;
}
int press_id;     // id under the finger while pressed (0 = none)
bool pressed(int id) { return id && press_id == id; }

// ---- gestures -------------------------------------------------------------------
int gest(Gest *g, InputEv *e) {
    double t = now();
    if (e->type == T_DOWN) {
        *g = (Gest){ .x0 = e->x, .y0 = e->y, .x = e->x, .y = e->y, .t = t, .down = 1 };
        press_id = hit_at(e->x, e->y);
        return G_DOWN;
    }
    if (!g->down) return G_NONE;
    if (e->type == T_MOVE) {
        float dt = (float)(t - g->t);
        float dx = e->x - g->x, dy = e->y - g->y;
        if (dt > 0.0005f) {   // low-pass velocity
            float k = clampf(dt / 0.05f, 0.15f, 1);
            g->vx = lerpf(g->vx, dx / dt, k);
            g->vy = lerpf(g->vy, dy / dt, k);
        }
        g->dx = dx; g->dy = dy;
        g->x = e->x; g->y = e->y; g->t = t;
        if (!g->drag && hypotf(g->x - g->x0, g->y - g->y0) > P(8)) {
            g->drag = 1;
            g->vert = fabsf(g->y - g->y0) >= fabsf(g->x - g->x0);
            press_id = 0;
            return G_DRAG_START;
        }
        return g->drag ? G_DRAG : G_NONE;
    }
    if (e->type == T_UP) {
        g->down = 0;
        press_id = 0;
        if (t - g->t > 0.08) { g->vx = 0; g->vy = 0; }   // finger rested before lifting
        return g->drag ? G_DRAG_END : G_TAP;
    }
    return G_NONE;
}

// ---- scroll physics -----------------------------------------------------------------
void scroll_drag(Scroll *s, float dy) {
    float over = s->pos < 0 ? -s->pos : s->pos > s->max ? s->pos - s->max : 0;
    s->pos -= dy * (over > 0 ? 0.35f / (1 + over / P(120)) : 1);
    s->vel = 0;
    s->held = 1;
}
void scroll_fling(Scroll *s, float vy) { s->held = 0; s->vel = -vy; }
void scroll_step(Scroll *s, float dt) {
    if (s->max < 0) s->max = 0;
    if (s->held) return;
    s->pos += s->vel * dt;
    s->vel *= expf(-dt * 2.6f);
    if (fabsf(s->vel) < 5) s->vel = 0;
    float target = s->pos < 0 ? 0 : s->pos > s->max ? s->max : s->pos;
    if (target != s->pos) {   // spring back from overscroll
        Spring sp = { s->pos, s->vel, target };
        spring_step(&sp, dt, 3.2f, 1.0f);
        s->pos = sp.x; s->vel = sp.v;
        if (fabsf(s->pos - target) < 0.5f) { s->pos = target; s->vel = 0; }
    }
}

// ---- icons: 2px line icons on a 24-unit grid ------------------------------------------
static Canvas *ic_c; static float ic_x, ic_y, ic_k, ic_w; static uint32_t ic_col;
#define X(v) (ic_x + (v) * ic_k)
#define Y(v) (ic_y + (v) * ic_k)
static void L(float a, float b, float c, float d) { g_line(ic_c, X(a), Y(b), X(c), Y(d), ic_w, ic_col); }
static void A(float cx, float cy, float r, float a0, float a1) {
    g_arc(ic_c, X(cx), Y(cy), r * ic_k, a0 * (float)M_PI / 180, a1 * (float)M_PI / 180, ic_w, ic_col);
}
static void O(float cx, float cy, float r) { g_ring(ic_c, X(cx), Y(cy), r * ic_k + ic_w / 2, ic_w, ic_col); }
static void D(float cx, float cy, float r) { g_circle(ic_c, X(cx), Y(cy), r * ic_k, ic_col); }
static void R(float x, float y, float w, float h, float r) {
    g_rrect_line(ic_c, X(x) - ic_w / 2, Y(y) - ic_w / 2, w * ic_k + ic_w, h * ic_k + ic_w, r * ic_k + ic_w / 2, ic_w, ic_col);
}
static void F(float x, float y, float w, float h, float r) { g_rrect(ic_c, X(x), Y(y), w * ic_k, h * ic_k, r * ic_k, r * ic_k, ic_col); }

void icon(Canvas *c, int id, float x, float y, float size, uint32_t col) {
    ic_c = c; ic_x = x; ic_y = y; ic_k = size / 24; ic_col = col;
    ic_w = fmaxf(P(2) * size / P(24), 1.6f);
    if (size < P(20)) ic_w = P(1.8f);
    switch (id) {
    case IC_VIDEO: R(2, 5, 20, 14, 4); L(10, 9, 10, 15); L(10, 9, 15, 12); L(10, 15, 15, 12); break;
    case IC_WEB: O(12, 12, 9); L(3, 12, 21, 12); A(12, 12, 9, -90, 90); A(18, 12, 0, 0, 0);
        A(4, 12, 11, -55, 55); A(20, 12, 11, 125, 235); break;
    case IC_PHOTOS: R(3, 4, 18, 16, 3); L(3, 17, 9, 11); L(9, 11, 14, 16); L(13, 15, 16, 12); L(16, 12, 21, 17); O(16, 8.5, 1.5); break;
    case IC_NOTES: R(5, 3, 14, 18, 3); L(9, 8, 15, 8); L(9, 12, 15, 12); L(9, 16, 13, 16); break;
    case IC_CLOCK: O(12, 12, 9); L(12, 7, 12, 12); L(12, 12, 15.5, 14); break;
    case IC_CALC: R(5, 3, 14, 18, 3); L(8.5, 7.5, 15.5, 7.5); D(9, 12, 1.1); D(12, 12, 1.1); D(15, 12, 1.1); D(9, 16, 1.1); D(12, 16, 1.1); D(15, 16, 1.1); break;
    case IC_GAME: R(3, 3, 8, 8, 2); R(13, 3, 8, 8, 2); R(3, 13, 8, 8, 2); F(13, 13, 8, 8, 2); break;
    case IC_SETTINGS: L(4, 7, 20, 7); L(4, 17, 20, 17); D(9, 7, 2.6); D(15, 17, 2.6); break;
    case IC_SEARCH: O(10.5, 10.5, 6.5); L(15.5, 15.5, 20.5, 20.5); break;
    case IC_WIFI: A(12, 19, 15, 220, 320); A(12, 19, 10, 220, 320); A(12, 19, 5, 220, 320); D(12, 19, 1.4); break;
    case IC_WIFI_OFF: A(12, 19, 15, 220, 320); A(12, 19, 10, 220, 320); D(12, 19, 1.4); L(4, 3, 20, 21); break;
    case IC_AIRPLANE: L(12, 2.5, 12, 20); L(12, 9, 3, 14); L(12, 9, 21, 14); L(12, 18, 8, 21); L(12, 18, 16, 21); break;
    case IC_MOON: A(12, 12, 8.5, 20, 340); A(17.5, 7.5, 7, 75, 200); break;
    case IC_SUN: O(12, 12, 4); for (int i = 0; i < 8; i++) { float a = i * (float)M_PI / 4; L(12 + 7 * cosf(a), 12 + 7 * sinf(a), 12 + 9.5f * cosf(a), 12 + 9.5f * sinf(a)); } break;
    case IC_SPEAKER: L(3, 9, 3, 15); L(3, 9, 7, 9); L(3, 15, 7, 15); L(7, 9, 12, 4.5f); L(7, 15, 12, 19.5f); L(12, 4.5f, 12, 19.5f);
        A(13, 12, 4, -50, 50); A(13, 12, 8, -50, 50); break;
    case IC_MUTE: L(3, 9, 3, 15); L(3, 9, 7, 9); L(3, 15, 7, 15); L(7, 9, 12, 4.5f); L(7, 15, 12, 19.5f); L(12, 4.5f, 12, 19.5f);
        L(16, 9, 22, 15); L(22, 9, 16, 15); break;
    case IC_PLAY: L(7, 4, 7, 20); L(7, 4, 20, 12); L(7, 20, 20, 12); break;
    case IC_PLAY_FILL: { // solid triangle via stacked lines
        for (float t = 0; t <= 1; t += 0.08f) L(7 + t * 11, 4 + t * 8, 7 + t * 11, 20 - t * 8);
        L(7, 4, 7, 20); L(7, 4, 19, 12); L(7, 20, 19, 12); } break;
    case IC_PAUSE: F(5.5, 4, 4.5, 16, 1.5); F(14, 4, 4.5, 16, 1.5); break;
    case IC_FULL: L(3, 3, 9, 3); L(3, 3, 3, 9); L(21, 3, 15, 3); L(21, 3, 21, 9); L(3, 21, 9, 21); L(3, 21, 3, 15); L(21, 21, 15, 21); L(21, 21, 21, 15); break;
    case IC_UNFULL: L(9, 3, 9, 9); L(3, 9, 9, 9); L(15, 3, 15, 9); L(21, 9, 15, 9); L(9, 21, 9, 15); L(3, 15, 9, 15); L(15, 21, 15, 15); L(21, 15, 15, 15); break;
    case IC_BACK: L(15, 4, 7, 12); L(7, 12, 15, 20); break;
    case IC_FWD: L(9, 4, 17, 12); L(17, 12, 9, 20); break;
    case IC_PLUS: L(12, 4, 12, 20); L(4, 12, 20, 12); break;
    case IC_TRASH: L(4, 6, 20, 6); L(9, 6, 9, 3); L(9, 3, 15, 3); L(15, 3, 15, 6); R(6, 6, 12, 15, 2); L(10, 10, 10, 17); L(14, 10, 14, 17); break;
    case IC_CLOSE: L(5, 5, 19, 19); L(19, 5, 5, 19); break;
    case IC_UP: L(12, 20, 12, 4); L(5, 11, 12, 4); L(19, 11, 12, 4); break;
    case IC_REFRESH: A(12, 12, 8, -60, 230); L(20, 4, 20, 10); L(14, 10, 20, 10); break;
    case IC_BOOKMARK: L(6, 3, 18, 3); L(6, 3, 6, 21); L(18, 3, 18, 21); L(6, 21, 12, 16); L(18, 21, 12, 16); break;
    case IC_BOOKMARK_FILL: for (float t = 6; t <= 18; t += 1.5f) L(t, 3, t, 20 - fabsf(t - 12) * 0.6f); L(6, 3, 18, 3); L(6, 3, 6, 21); L(18, 3, 18, 21); L(6, 21, 12, 16); L(18, 21, 12, 16); break;
    case IC_TABS: R(3, 7, 14, 14, 3); L(7, 3, 18, 3); L(21, 7, 21, 17); A(18, 6, 3, -90, 0); break;
    case IC_CHECK: L(4, 12, 10, 18); L(10, 18, 20, 6); break;
    case IC_TIMER: O(12, 13, 8); L(12, 13, 12, 8.5f); L(9, 2.5f, 15, 2.5f); break;
    case IC_STOPWATCH: O(12, 13, 8); L(12, 13, 15, 10); L(12, 2.5f, 12, 5); L(18.5, 5.5, 20, 4); break;
    case IC_CHEVRON: L(9, 5, 16, 12); L(16, 12, 9, 19); break;
    case IC_BATTERY: R(2, 7, 18, 10, 2.5); F(4.5, 9.5, 13, 5, 1); F(21, 10, 1.5, 4, 0.7); break;
    case IC_LOCK: R(5, 10, 14, 11, 2.5); A(12, 10, 4.5, 180, 360); L(7.5, 10, 7.5, 9); L(16.5, 10, 16.5, 9); break;
    case IC_CLOUD: A(8, 15, 4.5, 90, 270); A(12, 11, 5.5, 180, 360); A(17, 14, 4, 270, 450); L(8, 19.5, 17, 18); break;
    case IC_NET_CONN: D(6, 17, 1.6); D(12, 17, 1.6); D(18, 17, 1.6); break;
    case IC_KEYBOARD: R(2, 6, 20, 13, 3); L(6, 15, 18, 15); D(6, 10.5, 0.9); D(10, 10.5, 0.9); D(14, 10.5, 0.9); D(18, 10.5, 0.9); break;
    case IC_SHIFT: L(12, 4, 4, 13); L(12, 4, 20, 13); L(4, 13, 8, 13); L(20, 13, 16, 13); L(8, 13, 8, 20); L(16, 13, 16, 20); L(8, 20, 16, 20); break;
    case IC_BKSP: L(8, 5, 21, 5); L(21, 5, 21, 19); L(21, 19, 8, 19); L(8, 5, 2, 12); L(8, 19, 2, 12); L(11, 9, 17, 15); L(17, 9, 11, 15); break;
    case IC_RETURN: L(20, 5, 20, 14); L(20, 14, 5, 14); L(5, 14, 10, 9); L(5, 14, 10, 19); break;
    case IC_GLOBE_SMALL: O(12, 12, 8); L(4, 12, 20, 12); break;
    case IC_HISTORY: A(12, 12, 8.5, 150, 480); L(12, 7, 12, 12); L(12, 12, 15, 14); L(3, 6, 3.5, 10.5); L(3.5, 10.5, 8, 10); break;
    case IC_SWIPE: L(12, 21, 12, 5); L(7, 10, 12, 5); L(17, 10, 12, 5); break;
    }
}

// ---- status line ------------------------------------------------------------------------
void status_line(Canvas *c, float y, uint32_t col, bool show_time, bool show_brand) {
    char tm[16];
    time_t t = time(0);
    struct tm lt;
    gmtime_r(&t, &lt);   // RTC runs in local time (-rtc base=localtime)
    if (show_brand) text(c, W600, P(14), 0, P(22), y + P(27), col, "lumen");
    else if (show_time) { strftime(tm, sizeof tm, "%H:%M", &lt); text(c, W600, P(15), 0, P(22), y + P(28), col, tm); }
    // right: network state + battery
    float x = P(368);
    icon(c, IC_BATTERY, x - P(24), y + P(10), P(24), col);
    x -= P(32);
    int ns = net_state();
    if (ns == NET_UP) icon(c, IC_WIFI, x - P(20), y + P(11), P(20), col);
    else if (ns == NET_CONNECTING) {
        float ph = fmodf((float)now() * 3, 3);
        for (int i = 0; i < 3; i++) g_circle(c, x - P(17) + i * P(7), y + P(23), P(2.2f), c_alpha(col, (int)ph == i ? 1 : 0.35f));
    } else icon(c, IC_WIFI_OFF, x - P(20), y + P(11), P(20), col);
}

void home_bar(Canvas *c, uint32_t col) {
    g_rrect(c, P(195 - 60), SH - P(10), P(120), P(5), P(2.5f), P(2.5f), col);
}

// ---- common app chrome -----------------------------------------------------------------
// coloured header slab with big heading; returns bottom y of header (px)
float app_header(Canvas *c, uint32_t col, const char *title, float h) {
    g_rect(c, 0, 0, SW, P(h), col);
    status_line(c, 0, C_INK, true, false);
    text(c, W800, P(44), -0.04f, P(20), P(h - 22), C_INK, title);
    return P(h);
}

// outlined round button with icon; registers hit id
void btn_icon(Canvas *c, int id, float x, float y, float d, int ic, uint32_t fg, uint32_t fill) {
    bool pr = pressed(id);
    if (fill) g_circle(c, x + d / 2, y + d / 2, d / 2, pr ? c_mix(fill, C_LIME, 0.6f) : fill);
    else {
        if (pr) g_circle(c, x + d / 2, y + d / 2, d / 2, C_LIME);
        g_ring(c, x + d / 2, y + d / 2, d / 2, P(2), fg);
    }
    float is = d * 0.5f;
    icon(c, ic, x + (d - is) / 2, y + (d - is) / 2, is, pr && !fill ? C_INK : fg);
    hit(id, x, y, d, d);
}
// text pill button
void btn_text(Canvas *c, int id, float x, float y, float w, float h, const char *label, uint32_t fg, uint32_t fill) {
    bool pr = pressed(id);
    if (fill) g_rrect(c, x, y, w, h, h / 2, h / 2, pr ? C_LIME : fill);
    else { if (pr) g_rrect(c, x, y, w, h, h / 2, h / 2, C_LIME); g_rrect_line(c, x, y, w, h, h / 2, P(2), fg); }
    text_c(c, W600, P(15), 0, x + w / 2, y + h / 2 + P(5.5f), pr ? C_INK : fg, label);
    hit(id, x, y, w, h);
}

// ---- on-screen keyboard ------------------------------------------------------------------
static const char *KB_ROWS[2][3] = {
    { "qwertyuiop", "asdfghjkl", "zxcvbnm" },
    { "1234567890", "-/:;()&@\"", ".,?!'#%" },
};
static int kb_mode, kb_shift;
static int kb_flash_id; static double kb_flash_t;
#define KB_H 250
float kb_height(void) { return P(KB_H); }

static void kb_key(Canvas *c, float x, float y, float w, float h, int id, const char *label, int ic, uint32_t fill, uint32_t fg) {
    bool fl = (pressed(id)) || (kb_flash_id == id && now() - kb_flash_t < 0.12);
    g_rrect(c, x, y, w, h, P(10), P(10), fl ? C_LIME : fill);
    uint32_t col = fl ? C_INK : fg;
    if (ic) icon(c, ic, x + w / 2 - P(10), y + h / 2 - P(10), P(20), col);
    else text_c(c, W600, label[1] ? P(15) : P(22), 0, x + w / 2, y + h / 2 + (label[1] ? P(5) : P(8)), col, label);
    hit(id, x, y, w, h);
}

// draws the keyboard with its top at y (px). Key hit ids are 0x4B00 + code.
void kb_draw(Canvas *c, float y, const char *ret_label) {
    g_rrect(c, 0, y, SW, SH - y + 2, P(30), 0, C_INK);
    float kw = P(32.4f), kh = P(46), gap = P(5.4f), top = y + P(16);
    uint32_t kf = 0xFF2B2A28u;   // ink lifted by paper at 10%
    for (int r = 0; r < 3; r++) {
        const char *row = KB_ROWS[kb_mode][r];
        int n = strlen(row);
        float rw = n * kw + (n - 1) * gap, x = (SW - rw) / 2;
        float ry = top + r * (kh + P(9));
        for (int i = 0; i < n; i++) {
            char l[2] = { row[i], 0 };
            if (kb_shift && kb_mode == 0) l[0] = row[i] - 32;
            kb_key(c, x + i * (kw + gap), ry, kw, kh, 0x4B00 + (unsigned char)l[0], l, 0, kf, C_PAPER);
        }
        if (r == 2) {
            kb_key(c, P(6), ry, P(44), kh, 0x4B00 + 1, "", kb_mode ? 0 : IC_SHIFT, kb_shift ? C_PAPER : kf, kb_shift ? C_INK : C_PAPER);
            kb_key(c, SW - P(50), ry, P(44), kh, 0x4B00 + K_BACKSPACE, "", IC_BKSP, kf, C_PAPER);
        }
    }
    float ry = top + 3 * (kh + P(9));
    kb_key(c, P(6), ry, P(78), kh, 0x4B00 + 2, kb_mode ? "abc" : "123", 0, kf, C_MUTED);
    kb_key(c, P(90), ry, P(198), kh, 0x4B00 + ' ', "space", 0, kf, C_MUTED);
    kb_key(c, P(294), ry, P(90), kh, 0x4B00 + K_ENTER, ret_label ? ret_label : "return", 0, C_ORANGE, C_INK);
}

// handle a tap on the keyboard; returns key code or 0
int kb_tap(float x, float y) {
    int id = hit_at(x, y);
    if (id < 0x4B00 || id > 0x4BFF) return 0;
    kb_flash_id = id; kb_flash_t = now();
    int k = id - 0x4B00;
    if (k == 1) { kb_shift = !kb_shift; return 0; }
    if (k == 2) { kb_mode = !kb_mode; return 0; }
    if (k >= 'A' && k <= 'Z') kb_shift = 0;
    return k;
}
bool kb_hit(int id) { return id >= 0x4B00 && id <= 0x4BFF; }

// ---- text field editing ----------------------------------------------------------------
// applies a key to a UTF-8 buffer (editing at the end); returns 1 if Enter was pressed
int field_key(char *buf, int cap, int k) {
    int n = strlen(buf);
    if (k == K_BACKSPACE) {
        if (n) { n--; while (n > 0 && ((unsigned char)buf[n] & 0xc0) == 0x80) n--; buf[n] = 0; }
        return 0;
    }
    if (k == K_ENTER) return 1;
    if (k < 32 || k >= 0x110000) return 0;
    if (n + 5 < cap) { n += utf8_put(buf + n, k); buf[n] = 0; }
    return 0;
}

// a paper input slab with text + blinking caret
void field_draw(Canvas *c, float x, float y, float w, float h, const char *buf, const char *ph, bool focus, uint32_t bg, uint32_t fg) {
    g_rrect(c, x, y, w, h, P(16), P(16), bg);
    int sc[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
    float fs = P(16);
    float base = y + h / 2 + P(5.5f);
    g_clip(c, x + P(12), y, x + w - P(12), y + h);
    float tw = text_w(W400, fs, 0, buf);
    float tx = x + P(16);
    if (tw > w - P(40)) tx = x + w - P(24) - tw;     // keep the end visible
    if (!buf[0] && ph) text(c, W400, fs, 0, x + P(16), base, c_alpha(fg, 0.45f), ph);
    else text(c, W400, fs, 0, tx, base, fg, buf);
    if (focus && fmod(now(), 1.0) < 0.6) g_rect(c, tx + tw + P(1), y + h / 2 - P(11), P(2), P(22), C_ORANGE);
    c->cx0 = sc[0]; c->cy0 = sc[1]; c->cx1 = sc[2]; c->cy1 = sc[3];
}
