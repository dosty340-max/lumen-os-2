// Photos: 14 procedurally generated "photos" (no stock assets), grid + fullscreen
// viewer with finger-follow swiping between photos.
#include "../ui.h"

#define NPH 14
#define PW 600
#define PH 800
static Image *ph[NPH], *th[NPH];
static volatile int ready;
static const char *names[NPH] = {
    "Alpine dusk", "Noon sea", "Dune sea", "Night city", "Aurora", "Still lake", "Planet rise",
    "Canyon", "Snow peak", "Cloud fire", "Lavender hills", "Moon tide", "Slab study", "Green valley",
};

// ---- tiny noise toolkit -----------------------------------------------------------------
static uint32_t hash(int x, int y, int s) {
    uint32_t h = x * 374761393u + y * 668265263u + s * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float rnd(int x, int y, int s) { return (hash(x, y, s) & 0xffffff) / (float)0xffffff; }
static float sm(float t) { return t * t * (3 - 2 * t); }
static float vnoise(float x, float y, int s) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = sm(x - xi), fy = sm(y - yi);
    float a = rnd(xi, yi, s), b = rnd(xi + 1, yi, s), c = rnd(xi, yi + 1, s), d = rnd(xi + 1, yi + 1, s);
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy);
}
static float fbm(float x, float y, int s, int oct) {
    float v = 0, a = 0.5f, f = 1;
    for (int i = 0; i < oct; i++, a *= 0.5f, f *= 2.03f) v += a * vnoise(x * f, y * f, s + i * 17);
    return v;
}
static uint32_t rgb(float r, float g, float b) {
    return 0xff000000 | (uint32_t)clampf(r, 0, 255) << 16 | (uint32_t)clampf(g, 0, 255) << 8 | (uint32_t)clampf(b, 0, 255);
}
static uint32_t mixc(uint32_t a, uint32_t b, float t) { return c_mix(a, b, t) | 0xff000000; }

typedef struct {
    uint32_t sky0, sky1, sun; float sunx, suny, sunr;
    int nl; uint32_t lc[5]; float lh[5], rough[5];
    int water, stars, clouds, city, planet, aurora, dunes, slabs;
    uint32_t haze;
} Scene;

static Image *render(const Scene *sc, int seed) {
    Image *im = image_new(PW, PH);
    float ridge[5][PW];
    for (int l = 0; l < sc->nl; l++)
        for (int x = 0; x < PW; x++) {
            float n = sc->dunes ? 0.5f + 0.5f * sinf(x * 0.008f + l * 1.7f + seed) * 0.6f + fbm(x / 400.0f, l, seed + l, 2) * 0.4f
                                : fbm(x / (260.0f - l * 30), l * 3.1f, seed + l * 7, 5);
            ridge[l][x] = PH * sc->lh[l] - n * PH * sc->rough[l];
        }
    float horizon = sc->water ? PH * 0.62f : PH;
    for (int y = 0; y < PH; y++) {
        for (int x = 0; x < PW; x++) {
            int yy = y;
            bool refl = sc->water && y > horizon;
            if (refl) yy = (int)(2 * horizon - y);    // mirror for the reflection
            float ty = yy / (float)PH;
            uint32_t col = mixc(sc->sky0, sc->sky1, clampf(ty / 0.75f, 0, 1));
            if (sc->stars && rnd(x, yy, seed + 99) > 0.9985f) col = mixc(col, 0xFFFFFFFF, 0.6f + rnd(x, yy, 3) * 0.4f);
            if (sc->aurora) {
                float a = fbm(x / 140.0f, yy / 400.0f + 3, seed, 4);
                float band = expf(-powf((ty - 0.25f - 0.12f * sinf(x / 90.0f + a * 4)) / 0.07f, 2));
                col = mixc(col, rgb(80, 255, 170), band * 0.7f * a * 1.4f);
            }
            float dx = x - sc->sunx * PW, dy = yy - sc->suny * PH, d = sqrtf(dx * dx + dy * dy);
            if (sc->sunr > 0) {
                col = mixc(col, sc->sun, clampf(1 - (d - sc->sunr) / 2, 0, 1));
                col = mixc(col, sc->sun, 0.35f * expf(-d / (sc->sunr * 3)));
            }
            if (sc->planet) {
                float px = x - PW * 0.62f, py = yy - PH * 0.42f, pr = PW * 0.38f, pd = sqrtf(px * px + py * py);
                if (pd < pr) {
                    float bands = fbm(0, py / 30.0f + fbm(px / 200.0f, py / 60.0f, seed, 3) * 2, seed + 5, 3);
                    uint32_t pc = mixc(rgb(230, 150, 90), rgb(120, 70, 160), bands);
                    float shade = clampf((px * -0.6f + py * -0.4f) / pr + 0.6f, 0.15f, 1);
                    col = mixc(col, mixc(0xff000000, pc, shade), clampf(pr - pd, 0, 1));
                }
                float mx = x - PW * 0.18f, my = yy - PH * 0.2f;
                if (mx * mx + my * my < 30 * 30) col = mixc(col, rgb(220, 220, 230), 0.9f);
            }
            if (sc->clouds) {
                float cl = fbm(x / 220.0f, yy / 70.0f, seed + 50, 5);
                float k = clampf((cl - 0.48f) * 3.2f, 0, 1) * clampf(1.2f - ty * 1.6f, 0, 1);
                col = mixc(col, mixc(sc->sun, rgb(255, 245, 235), 0.4f), k * 0.85f);
            }
            for (int l = 0; l < sc->nl; l++) {
                if (yy >= ridge[l][x]) {
                    float depth = (yy - ridge[l][x]) / PH;
                    uint32_t lc = mixc(sc->lc[l], 0xff000000, clampf(depth * 0.8f, 0, 0.35f));
                    if (!sc->dunes) lc = mixc(lc, rgb(255, 255, 255), clampf(0.06f - depth, 0, 0.06f) * 2.5f * (l == 0));
                    else lc = mixc(lc, sc->sun, clampf(sinf(x * 0.03f + l) * 0.15f, 0, 0.15f));
                    col = mixc(lc, sc->haze, (sc->nl - 1 - l) * 0.0f);
                }
            }
            if (sc->city) {
                int bw = 38 + (int)(rnd(x / 46, 0, seed) * 30);
                int b = x / bw;
                float bh = PH * (0.38f + rnd(b, 1, seed) * 0.32f);
                if (yy > PH - bh * 0.9f + 40) {
                    col = rgb(18, 18, 30);
                    if ((x % 12) > 4 && (yy % 18) > 9 && rnd(x / 12, yy / 18, seed) > 0.55f) col = rgb(255, 200, 90);
                }
            }
            if (sc->slabs) {
                static const uint32_t pal[5] = { C_ORANGE, C_LILAC, C_LIME, C_SKY, C_PAPER };
                int band = (int)((yy + 40 * sinf(x / 120.0f + seed)) / 170);
                col = pal[(band + seed) % 5] | 0xff000000;
                float cx = x - PW * 0.5f, cy = yy - PH * 0.55f;
                if (cx * cx + cy * cy < 150 * 150) col = C_INK;
            }
            if (refl) {
                float w = sinf(y * 0.25f + fbm(x / 60.0f, y / 8.0f, seed, 2) * 6) * 0.5f + 0.5f;
                col = mixc(col, rgb(10, 30, 60), 0.35f + 0.15f * w);
                if (sc->sunr > 0 && fabsf(x - sc->sunx * PW) < 60 * (y - horizon) / (PH - horizon) + 10 && w > 0.8f)
                    col = mixc(col, sc->sun, 0.6f);
            }
            float grain = (rnd(x, y, seed + 7) - 0.5f) * 10;   // film grain so it reads as a photo
            im->px[y * PW + x] = rgb(((col >> 16) & 255) + grain, ((col >> 8) & 255) + grain, (col & 255) + grain);
        }
    }
    return im;
}

static Image *thumb(Image *src) {   // 3x box downsample -> 200x266
    int tw = PW / 3, tht = PH / 3;
    Image *t = image_new(tw, tht);
    for (int y = 0; y < tht; y++)
        for (int x = 0; x < tw; x++) {
            unsigned r = 0, gg = 0, b = 0;
            for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) {
                uint32_t p = src->px[(y * 3 + j) * PW + x * 3 + i];
                r += (p >> 16) & 255; gg += (p >> 8) & 255; b += p & 255;
            }
            t->px[y * tw + x] = 0xff000000 | (r / 9) << 16 | (gg / 9) << 8 | (b / 9);
        }
    return t;
}

static void *gen_thread(void *a) {
    (void)a;
    Scene s[NPH] = {
        { rgb(30, 40, 90), rgb(250, 140, 80), rgb(255, 220, 160), 0.7f, 0.55f, 34, 4, { rgb(70, 60, 110), rgb(50, 45, 85), rgb(35, 30, 60), rgb(20, 18, 35) }, { 0.55f, 0.66f, 0.78f, 0.9f }, { 0.35f, 0.25f, 0.2f, 0.12f } },
        { rgb(70, 150, 230), rgb(190, 225, 250), rgb(255, 255, 240), 0.3f, 0.18f, 40, 0, {0}, {0}, {0}, .water = 1, .clouds = 1 },
        { rgb(240, 170, 110), rgb(255, 225, 170), rgb(255, 250, 220), 0.5f, 0.3f, 50, 3, { rgb(225, 140, 70), rgb(200, 110, 55), rgb(170, 85, 40) }, { 0.6f, 0.75f, 0.9f }, { 0.12f, 0.1f, 0.1f }, .dunes = 1 },
        { rgb(10, 12, 35), rgb(45, 30, 80), rgb(240, 240, 255), 0.8f, 0.15f, 26, 0, {0}, {0}, {0}, .stars = 1, .city = 1 },
        { rgb(5, 15, 30), rgb(15, 50, 70), 0, 0, 0, 0, 2, { rgb(10, 25, 35), rgb(5, 12, 18) }, { 0.72f, 0.86f }, { 0.25f, 0.15f }, .stars = 1, .aurora = 1 },
        { rgb(120, 180, 220), rgb(230, 230, 210), rgb(255, 250, 230), 0.25f, 0.3f, 30, 2, { rgb(40, 90, 60), rgb(25, 60, 40) }, { 0.55f, 0.62f }, { 0.25f, 0.15f }, .water = 1 },
        { rgb(2, 2, 10), rgb(15, 10, 35), 0, 0, 0, 0, 0, {0}, {0}, {0}, .stars = 1, .planet = 1 },
        { rgb(110, 170, 230), rgb(250, 210, 160), rgb(255, 240, 200), 0.2f, 0.2f, 30, 4, { rgb(200, 90, 50), rgb(170, 70, 40), rgb(140, 55, 35), rgb(100, 40, 30) }, { 0.45f, 0.6f, 0.75f, 0.9f }, { 0.15f, 0.12f, 0.1f, 0.08f } },
        { rgb(40, 110, 210), rgb(170, 210, 250), rgb(255, 255, 255), 0.75f, 0.12f, 30, 3, { rgb(240, 245, 250), rgb(120, 140, 170), rgb(60, 80, 110) }, { 0.5f, 0.72f, 0.88f }, { 0.4f, 0.2f, 0.12f } },
        { rgb(60, 40, 90), rgb(255, 120, 60), rgb(255, 190, 120), 0.5f, 0.7f, 60, 1, { rgb(30, 20, 30) }, { 0.92f }, { 0.08f }, .clouds = 1 },
        { rgb(150, 160, 230), rgb(250, 210, 220), rgb(255, 240, 230), 0.3f, 0.35f, 36, 4, { rgb(150, 120, 200), rgb(120, 90, 170), rgb(95, 70, 140), rgb(70, 50, 110) }, { 0.5f, 0.62f, 0.75f, 0.88f }, { 0.12f, 0.12f, 0.1f, 0.08f } },
        { rgb(8, 12, 30), rgb(30, 45, 80), rgb(240, 240, 220), 0.5f, 0.3f, 36, 1, { rgb(15, 20, 35) }, { 0.6f }, { 0.08f }, .water = 1, .stars = 1 },
        { 0, 0, 0, 0, 0, 0, 0, {0}, {0}, {0}, .slabs = 1 },
        { rgb(150, 200, 240), rgb(235, 245, 230), rgb(255, 255, 230), 0.8f, 0.2f, 30, 4, { rgb(120, 170, 80), rgb(80, 140, 60), rgb(50, 110, 50), rgb(30, 80, 40) }, { 0.5f, 0.62f, 0.75f, 0.88f }, { 0.2f, 0.15f, 0.12f, 0.1f }, .clouds = 1 },
    };
    double t0 = now();
    for (int i = 0; i < NPH; i++) {
        ph[i] = render(&s[i], i * 31 + 5);
        th[i] = thumb(ph[i]);
        ready = i + 1;
    }
    LOG("photos: generated %d photos in %.1f s", NPH, now() - t0);
    return 0;
}
void photos_init(void) { pthread_t t; pthread_create(&t, 0, gen_thread, 0); pthread_detach(t); }

// ---- UI ---------------------------------------------------------------------------------------
static Gest g;
static Scroll sc;
static int viewing = -1;
static Spring vt;             // viewer open 0..1
static Spring pan;            // horizontal page position (in photos)
static float vfrom[4];
static int chrome = 1;
static int vdrag;             // 1 horizontal, 2 vertical (dismiss)
static float dismiss;

static void grid_rect(int i, float *r) {
    float gx = P(4), gap = P(4), w = (SW - 2 * gx - 2 * gap) / 3;
    float top = P(176) + P(4) - sc.pos;
    r[0] = gx + (i % 3) * (w + gap); r[1] = top + (i / 3) * (w * 1.33f + gap); r[2] = w; r[3] = w * 1.33f;
}

static void open_(void) { viewing = -1; vt = (Spring){ 0 }; }

static void draw(Canvas *c, float dt) {
    g_clear(c, TH.bg);
    scroll_step(&sc, dt);
    float r[4];
    grid_rect(0, r);
    sc.max = ((NPH + 2) / 3) * (r[3] + P(4)) + P(40) - (SH - P(176));
    for (int i = 0; i < NPH; i++) {
        grid_rect(i, r);
        if (r[1] > SH || r[1] + r[3] < P(170)) continue;
        if (i < ready) g_image_cover(c, th[i], r[0], r[1], r[2], r[3], P(14), 255);
        else g_rrect(c, r[0], r[1], r[2], r[3], P(14), P(14), TH.line);
        if (pressed(0xE00 + i)) g_rrect(c, r[0], r[1], r[2], r[3], P(14), P(14), 0x40DFFF5E);
        hit(0xE00 + i, r[0], r[1], r[2], r[3]);
    }
    g_rect(c, 0, 0, SW, P(176), C_ORANGE);
    status_line(c, 0, C_INK, true, false);
    text(c, W800, P(52), -0.05f, P(18), P(140), C_INK, "Photos");
    char b[64];
    snprintf(b, sizeof b, ready < NPH ? "Developing %d of %d…" : "%d photos · all generated on device", ready, NPH);
    text(c, W600, P(14), 0, P(22), P(164), C_INK, b);
    // viewer
    if (viewing >= 0 || vt.x > 0.01f) {
        spring_step(&vt, dt, 2.6f, 0.86f);
        if (!vdrag) spring_step(&pan, dt, 2.8f, 0.9f);
        float u = clampf(vt.x, 0, 1.02f) * (1 - dismiss * 0.5f);
        g_rect(c, 0, 0, SW, SH, c_alpha(C_INK, clampf(u, 0, 1)));
        float fw = SW, fh = SW * PH / (float)PW, fy = (SH - fh) / 2 + dismiss * P(300);
        for (int k = -1; k <= 1; k++) {
            int i = (int)floorf(pan.x + 0.5f) + k;
            if (i < 0 || i >= NPH || i >= ready) continue;
            float off = (i - pan.x) * (SW + P(16));
            float x = lerpf(vfrom[0], off, u), y = lerpf(vfrom[1], fy, u), w = lerpf(vfrom[2], fw, u), h = lerpf(vfrom[3], fh, u);
            if (k != 0 && u < 0.9f) continue;
            g_image(c, ph[i], x, y, w, h, P(14) * (1 - u), 255);
        }
        if (chrome && u > 0.9f) {
            int i = (int)floorf(pan.x + 0.5f);
            if (i >= 0 && i < NPH) {
                g_rrect(c, 0, SH - P(150), SW, P(160), P(30), 0, C_PAPER);
                text(c, W800, P(30), -0.03f, P(22), SH - P(100), C_INK, names[i]);
                snprintf(b, sizeof b, "%d of %d  ·  %dx%d  ·  procedural", i + 1, NPH, PW, PH);
                text(c, W600, P(14), 0, P(22), SH - P(72), C_INK, b);
                home_bar(c, C_INK);
            }
            status_line(c, 0, C_PAPER, true, false);
            btn_icon(c, 0xE80, P(14), P(52), P(52), IC_BACK, C_PAPER, 0);
        }
        if (viewing < 0 && vt.x < 0.02f) vt.x = 0;
        return;
    }
    home_bar(c, TH.fg);
}

static void touch(InputEv *e) {
    int r = gest(&g, e);
    if (viewing >= 0) {
        if (r == G_DRAG_START) vdrag = g.vert && g.y > g.y0 ? 2 : 1;
        if ((r == G_DRAG_START || r == G_DRAG) && vdrag == 1) { pan.x -= (r == G_DRAG_START ? g.x - g.x0 : g.dx) / (SW + P(16)); pan.x = clampf(pan.x, -0.3f, NPH - 0.7f); pan.v = 0; }
        if ((r == G_DRAG_START || r == G_DRAG) && vdrag == 2) dismiss = clampf((g.y - g.y0) / P(500), 0, 1);
        if (r == G_DRAG_END) {
            if (vdrag == 1) {
                int cur = (int)floorf(pan.x + 0.5f);
                if (g.vx < -P(400)) cur = (int)floorf(pan.x) + 1;
                else if (g.vx > P(400)) cur = (int)ceilf(pan.x) - 1;
                pan.t = clampf(cur, 0, mini(ready, NPH) - 1);
                pan.v = -g.vx / (SW + P(16));
                viewing = (int)pan.t;
            } else if (vdrag == 2) {
                if (dismiss > 0.18f || g.vy > P(600)) { grid_rect(viewing, vfrom); viewing = -1; vt.t = 0; }
                dismiss = 0;
            }
            vdrag = 0;
        }
        if (r == G_TAP) {
            if (hit_at(e->x, e->y) == 0xE80) { grid_rect(viewing, vfrom); viewing = -1; vt.t = 0; }
            else chrome = !chrome;
        }
        return;
    }
    if (r == G_DRAG_START || r == G_DRAG) { if (g.vert) scroll_drag(&sc, r == G_DRAG_START ? g.y - g.y0 : g.dy); }
    else if (r == G_DRAG_END) { if (sc.held) scroll_fling(&sc, g.vy); }
    else if (r == G_TAP) {
        int id = hit_at(e->x, e->y);
        if (id >= 0xE00 && id < 0xE00 + NPH && id - 0xE00 < ready) {
            viewing = id - 0xE00;
            grid_rect(viewing, vfrom);
            // vfrom is relative to the page offset: express it for the target page
            pan.x = pan.t = viewing; pan.v = 0;
            vt = (Spring){ 0, 0, 1 };
            chrome = 1;
        }
    }
}
static void key(int k) {
    if (viewing < 0) return;
    if (k == K_RIGHT && viewing < ready - 1) pan.t = ++viewing;
    if (k == K_LEFT && viewing > 0) pan.t = --viewing;
    if (k == K_ESC || k == K_BACKSPACE) { grid_rect(viewing, vfrom); viewing = -1; vt.t = 0; }
}
App app_photos = { "Photos", IC_PHOTOS, open_, draw, touch, key, 0 };
