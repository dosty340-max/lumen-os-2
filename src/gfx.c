// Software rasteriser: AA rounded rects (signed distance), lines, arcs,
// bilinear image/layer blits, alpha blending. Everything is ARGB8888.
#include "lumen.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

Canvas *canvas_new(int w, int h) {
    Canvas *c = calloc(1, sizeof *c);
    c->w = w; c->h = h;
    c->px = calloc((size_t)w * h, 4);
    g_noclip(c);
    return c;
}
void g_noclip(Canvas *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }
void g_clip(Canvas *c, float x0, float y0, float x1, float y1) {
    c->cx0 = maxi(c->cx0, (int)floorf(x0)); c->cy0 = maxi(c->cy0, (int)floorf(y0));
    c->cx1 = mini(c->cx1, (int)ceilf(x1)); c->cy1 = mini(c->cy1, (int)ceilf(y1));
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
}

uint32_t c_alpha(uint32_t col, float a) {
    int al = (int)((col >> 24) * clampf(a, 0, 1) + 0.5f);
    return (col & 0xffffff) | ((uint32_t)al << 24);
}
uint32_t c_mix(uint32_t a, uint32_t b, float t) {
    t = clampf(t, 0, 1);
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        int ca = (a >> s) & 255, cb = (b >> s) & 255;
        r |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return r;
}

// a: 0..256
static inline uint32_t blend(uint32_t d, uint32_t s, unsigned a) {
    uint32_t rb = (((s & 0xff00ff) * a + (d & 0xff00ff) * (256 - a)) >> 8) & 0xff00ff;
    uint32_t g = (((s & 0x00ff00) * a + (d & 0x00ff00) * (256 - a)) >> 8) & 0x00ff00;
    return 0xff000000 | rb | g;
}
static inline unsigned a256(unsigned a255) { return a255 + (a255 >> 7); }

static void span(Canvas *c, int y, int x0, int x1, uint32_t col, unsigned a /*0..256*/) {
    if (y < c->cy0 || y >= c->cy1) return;
    if (x0 < c->cx0) x0 = c->cx0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (x0 >= x1 || a == 0) return;
    uint32_t *p = c->px + (size_t)y * c->w + x0, *e = p + (x1 - x0);
    if (a >= 256) { uint32_t v = col | 0xff000000; while (p < e) *p++ = v; }
    else while (p < e) { *p = blend(*p, col, a); p++; }
}
static inline void plot(Canvas *c, int x, int y, uint32_t col, unsigned a) {
    if (x < c->cx0 || x >= c->cx1 || y < c->cy0 || y >= c->cy1 || a == 0) return;
    uint32_t *p = c->px + (size_t)y * c->w + x;
    *p = a >= 256 ? (col | 0xff000000) : blend(*p, col, a);
}

void g_clear(Canvas *c, uint32_t col) {
    for (int y = c->cy0; y < c->cy1; y++) span(c, y, c->cx0, c->cx1, col, 256);
}

// ---- rounded rect geometry ---------------------------------------------------
typedef struct { float x0, y0, x1, y1, rt, rb, cx, cy, hw, hh; } RR;
static RR rr_make(float x, float y, float w, float h, float rt, float rb) {
    float m = fminf(w, h) * 0.5f;
    RR g = { x, y, x + w, y + h, fminf(fmaxf(rt, 0), m), fminf(fmaxf(rb, 0), m), x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f };
    return g;
}
static inline float rr_sdf(const RR *g, float px, float py) {
    float r = py < g->cy ? g->rt : g->rb;
    float qx = fabsf(px - g->cx) - g->hw + r, qy = fabsf(py - g->cy) - g->hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    if (in > 0) in = 0;
    return sqrtf(ox * ox + oy * oy) + in - r;
}
static inline float rr_cov(const RR *g, float px, float py) { return clampf(0.5f - rr_sdf(g, px, py), 0, 1); }
// range of pixel indices in row with full coverage (may be empty: *a > *b)
static void rr_full(const RR *g, float fy, int *a, int *b) {
    *a = 1; *b = 0;
    if (fy < g->y0 + 0.5f || fy > g->y1 - 0.5f) return;
    int ia = (int)ceilf(g->x0), ib = (int)floorf(g->x1) - 1;
    float r = fy < g->cy ? g->rt : g->rb;
    float cyc = clampf(fy, g->y0 + r, g->y1 - r), dy = fy - cyc;
    if (dy != 0) {
        float in = r - 0.5f;
        if (in <= 0 || fabsf(dy) >= in) return;
        float h = sqrtf(in * in - dy * dy);
        ia = maxi(ia, (int)ceilf(g->x0 + r - h - 0.5f));
        ib = mini(ib, (int)floorf(g->x1 - r + h - 0.5f));
    }
    *a = ia; *b = ib;
}

void g_rrect(Canvas *c, float x, float y, float w, float h, float rtop, float rbot, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    RR g = rr_make(x, y, w, h, rtop, rbot);
    unsigned ca = a256(col >> 24);
    int ya = maxi(c->cy0, (int)floorf(g.y0)), yb = mini(c->cy1, (int)ceilf(g.y1));
    int xa = maxi(c->cx0, (int)floorf(g.x0)), xb = mini(c->cx1, (int)ceilf(g.x1));
    for (int py = ya; py < yb; py++) {
        float fy = py + 0.5f;
        int a, b;
        rr_full(&g, fy, &a, &b);
        if (a <= b) {
            span(c, py, a, b + 1, col, ca);
            for (int px = xa; px < a && px < xb; px++) plot(c, px, py, col, (unsigned)(ca * rr_cov(&g, px + 0.5f, fy)));
            for (int px = maxi(b + 1, xa); px < xb; px++) plot(c, px, py, col, (unsigned)(ca * rr_cov(&g, px + 0.5f, fy)));
        } else {
            for (int px = xa; px < xb; px++) plot(c, px, py, col, (unsigned)(ca * rr_cov(&g, px + 0.5f, fy)));
        }
    }
}
void g_rect(Canvas *c, float x, float y, float w, float h, uint32_t col) {
    // integer-aligned fast path (most slabs)
    if (x == floorf(x) && y == floorf(y) && w == floorf(w) && h == floorf(h)) {
        int y0 = maxi((int)y, c->cy0), y1 = mini((int)(y + h), c->cy1);
        unsigned a = a256(col >> 24);
        for (int py = y0; py < y1; py++) span(c, py, (int)x, (int)(x + w), col, a);
        return;
    }
    g_rrect(c, x, y, w, h, 0, 0, col);
}

void g_rrect_line(Canvas *c, float x, float y, float w, float h, float r, float lw, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    RR o = rr_make(x, y, w, h, r, r);
    RR i = rr_make(x + lw, y + lw, w - 2 * lw, h - 2 * lw, fmaxf(r - lw, 0), fmaxf(r - lw, 0));
    unsigned ca = a256(col >> 24);
    int ya = maxi(c->cy0, (int)floorf(o.y0)), yb = mini(c->cy1, (int)ceilf(o.y1));
    int xa = maxi(c->cx0, (int)floorf(o.x0)), xb = mini(c->cx1, (int)ceilf(o.x1));
    for (int py = ya; py < yb; py++) {
        float fy = py + 0.5f;
        int oa, ob, ia, ib;
        rr_full(&o, fy, &oa, &ob);
        rr_full(&i, fy, &ia, &ib);
        for (int px = xa; px < xb; px++) {
            if (ia <= ib && px >= ia && px <= ib) { px = ib; continue; }
            float co = (px >= oa && px <= ob) ? 1 : rr_cov(&o, px + 0.5f, fy);
            float cv = co - rr_cov(&i, px + 0.5f, fy);
            if (cv > 0) plot(c, px, py, col, (unsigned)(ca * clampf(cv, 0, 1)));
        }
    }
}
void g_circle(Canvas *c, float cx, float cy, float r, uint32_t col) { g_rrect(c, cx - r, cy - r, 2 * r, 2 * r, r, r, col); }
void g_ring(Canvas *c, float cx, float cy, float r, float lw, uint32_t col) { g_rrect_line(c, cx - r, cy - r, 2 * r, 2 * r, r, lw, col); }

void g_line(Canvas *c, float x0, float y0, float x1, float y1, float w, uint32_t col) {
    float hw = w * 0.5f;
    int xa = maxi(c->cx0, (int)floorf(fminf(x0, x1) - hw - 1)), xb = mini(c->cx1, (int)ceilf(fmaxf(x0, x1) + hw + 1));
    int ya = maxi(c->cy0, (int)floorf(fminf(y0, y1) - hw - 1)), yb = mini(c->cy1, (int)ceilf(fmaxf(y0, y1) + hw + 1));
    float dx = x1 - x0, dy = y1 - y0, ll = dx * dx + dy * dy;
    unsigned ca = a256(col >> 24);
    for (int py = ya; py < yb; py++)
        for (int px = xa; px < xb; px++) {
            float fx = px + 0.5f - x0, fy = py + 0.5f - y0;
            float t = ll > 0 ? clampf((fx * dx + fy * dy) / ll, 0, 1) : 0;
            float ex = fx - t * dx, ey = fy - t * dy;
            float cov = clampf(hw + 0.5f - sqrtf(ex * ex + ey * ey), 0, 1);
            if (cov > 0) plot(c, px, py, col, (unsigned)(ca * cov));
        }
}

// angles in radians, 0 = +x, increasing clockwise (screen coords), a0 < a1
void g_arc(Canvas *c, float cx, float cy, float r, float a0, float a1, float w, uint32_t col) {
    float hw = w * 0.5f, R = r + hw + 1;
    int xa = maxi(c->cx0, (int)floorf(cx - R)), xb = mini(c->cx1, (int)ceilf(cx + R));
    int ya = maxi(c->cy0, (int)floorf(cy - R)), yb = mini(c->cy1, (int)ceilf(cy + R));
    float e0x = cx + r * cosf(a0), e0y = cy + r * sinf(a0), e1x = cx + r * cosf(a1), e1y = cy + r * sinf(a1);
    unsigned ca = a256(col >> 24);
    float span_ = a1 - a0;
    for (int py = ya; py < yb; py++)
        for (int px = xa; px < xb; px++) {
            float fx = px + 0.5f - cx, fy = py + 0.5f - cy;
            float d = sqrtf(fx * fx + fy * fy);
            if (fabsf(d - r) > hw + 1) continue;
            float ang = atan2f(fy, fx) - a0;
            while (ang < 0) ang += 2 * (float)M_PI;
            float dd;
            if (ang <= span_) dd = fabsf(d - r);
            else {
                float ax = px + 0.5f - e0x, ay = py + 0.5f - e0y, bx = px + 0.5f - e1x, by = py + 0.5f - e1y;
                dd = sqrtf(fminf(ax * ax + ay * ay, bx * bx + by * by));
            }
            float cov = clampf(hw + 0.5f - dd, 0, 1);
            if (cov > 0) plot(c, px, py, col, (unsigned)(ca * cov));
        }
}

// ---- images --------------------------------------------------------------------
static inline uint32_t bilerp(const uint32_t *px, int w, int h, int fx, int fy /*16.16*/) {
    int x = fx >> 16, y = fy >> 16;
    unsigned ax = (fx >> 8) & 255, ay = (fy >> 8) & 255;
    if (x < 0) { x = 0; ax = 0; }
    if (y < 0) { y = 0; ay = 0; }
    if (x >= w - 1) { x = w - 1; ax = 0; }
    if (y >= h - 1) { y = h - 1; ay = 0; }
    const uint32_t *p = px + (size_t)y * w + x;
    uint32_t a = p[0], b = ax ? p[1] : a, c = ay ? p[w] : a, d = (ax && ay) ? p[w + 1] : c;
    if (!ax) d = c;
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        unsigned top = ((a >> s) & 255) * (256 - ax) + ((b >> s) & 255) * ax;
        unsigned bot = ((c >> s) & 255) * (256 - ax) + ((d >> s) & 255) * ax;
        r |= ((top * (256 - ay) + bot * ay) >> 16) << s;
    }
    return r;
}

static void blit(Canvas *c, const Image *im, float x, float y, float w, float h, float radius, int alpha,
                 float sx, float sy, float sw, float sh) {
    if (w <= 0 || h <= 0 || !im || !im->px) return;
    RR g = rr_make(x, y, w, h, radius, radius);
    int ya = maxi(c->cy0, (int)floorf(y)), yb = mini(c->cy1, (int)ceilf(y + h));
    int xa = maxi(c->cx0, (int)floorf(x)), xb = mini(c->cx1, (int)ceilf(x + w));
    int stepx = (int)(sw / w * 65536), stepy = (int)(sh / h * 65536);
    bool exact = (stepx == 65536 && stepy == 65536 && x == floorf(x) && y == floorf(y));
    unsigned A = a256(alpha);
    for (int py = ya; py < yb; py++) {
        float fy = py + 0.5f;
        int fa, fb;
        rr_full(&g, fy, &fa, &fb);
        if (radius <= 0) { fa = xa; fb = xb - 1; }
        int sfy = (int)((sy + (fy - y) * sh / h - 0.5f) * 65536);
        uint32_t *d = c->px + (size_t)py * c->w;
        int sfx = (int)((sx + (xa + 0.5f - x) * sw / w - 0.5f) * 65536);
        const uint32_t *srow = exact ? im->px + (size_t)((int)sy + py - (int)y) * im->w + (int)sx : 0;
        for (int px = xa; px < xb; px++, sfx += stepx) {
            uint32_t s = exact ? srow[px - (int)x] : bilerp(im->px, im->w, im->h, sfx, sfy);
            unsigned a = (a256(s >> 24) * A) >> 8;
            if (px < fa || px > fb) a = (unsigned)(a * rr_cov(&g, px + 0.5f, fy));
            if (a >= 256) d[px] = s | 0xff000000;
            else if (a) d[px] = blend(d[px], s, a);
        }
    }
}
void g_image(Canvas *c, const Image *im, float x, float y, float w, float h, float radius, int alpha) {
    if (im) blit(c, im, x, y, w, h, radius, alpha, 0, 0, im->w, im->h);
}
void g_image_cover(Canvas *c, const Image *im, float x, float y, float w, float h, float radius, int alpha) {
    if (!im) return;
    float ar = w / h, iar = (float)im->w / im->h;
    if (iar > ar) { float sw = im->h * ar; blit(c, im, x, y, w, h, radius, alpha, (im->w - sw) / 2, 0, sw, im->h); }
    else { float sh = im->w / ar; blit(c, im, x, y, w, h, radius, alpha, 0, (im->h - sh) / 2, im->w, sh); }
}
void g_layer_rect(Canvas *dst, const Canvas *src, float x, float y, float w, float h, float radius, int alpha) {
    blit(dst, (const Image *)src, x, y, w, h, radius, alpha, 0, 0, src->w, src->h);
}

void g_layer(Canvas *dst, const Canvas *src, const float m[6], float bx0, float by0, float bx1, float by1, int alpha) {
    int ya = maxi(dst->cy0, (int)floorf(by0)), yb = mini(dst->cy1, (int)ceilf(by1));
    int xa = maxi(dst->cx0, (int)floorf(bx0)), xb = mini(dst->cx1, (int)ceilf(bx1));
    unsigned A = a256(alpha);
    int dux = (int)(m[0] * 65536), dvx = (int)(m[3] * 65536);
    for (int py = ya; py < yb; py++) {
        float fy = py + 0.5f, fx = xa + 0.5f;
        float u = m[0] * fx + m[1] * fy + m[2] - 0.5f, v = m[3] * fx + m[4] * fy + m[5] - 0.5f;
        int iu = (int)(u * 65536), iv = (int)(v * 65536);
        uint32_t *d = dst->px + (size_t)py * dst->w;
        for (int px = xa; px < xb; px++, iu += dux, iv += dvx) {
            if (iu < -32768 || iv < -32768 || (iu >> 16) >= src->w || (iv >> 16) >= src->h) continue;
            uint32_t s = bilerp(src->px, src->w, src->h, iu, iv);
            d[px] = A >= 256 ? s | 0xff000000 : blend(d[px], s, A);
        }
    }
}

void g_dim(Canvas *c, float k) {
    unsigned m = (unsigned)(clampf(k, 0, 1) * 256);
    size_t n = (size_t)c->w * c->h;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = c->px[i];
        c->px[i] = 0xff000000 | ((((p & 0xff00ff) * m) >> 8) & 0xff00ff) | ((((p & 0xff00) * m) >> 8) & 0xff00);
    }
}

Image *image_new(int w, int h) {
    Image *im = calloc(1, sizeof *im);
    im->w = w; im->h = h;
    im->px = calloc((size_t)w * h, 4);
    return im;
}
void image_free(Image *im) { if (im) { free(im->px); free(im); } }

Image *image_decode(const void *data, int len) {
    int w, h, n;
    unsigned char *rgba = stbi_load_from_memory(data, len, &w, &h, &n, 4);
    if (!rgba) return NULL;
    Image *im = malloc(sizeof *im);
    im->w = w; im->h = h;
    im->px = (uint32_t *)rgba;   // convert RGBA bytes -> ARGB words in place
    for (size_t i = 0; i < (size_t)w * h; i++) {
        unsigned char *p = rgba + i * 4;
        im->px[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    return im;
}
