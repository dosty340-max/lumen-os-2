// Text: stb_truetype rasterises Bricolage Grotesque glyphs into a cache.
#include "lumen.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static stbtt_fontinfo fi[3];
static unsigned char *fdata[3];

typedef struct {
    int f, cp, sz;           // sz = pixel size * 4
    int glyph, w, h, xo, yo;
    float adv;
    unsigned char *bm;
} Glyph;
#define GC 8192
static Glyph cache[GC];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;   // web layout measures text off-thread

void font_init(void) {
    const char *names[3] = { "/fonts/B400.ttf", "/fonts/B600.ttf", "/fonts/B800.ttf" };
    for (int i = 0; i < 3; i++) {
        FILE *f = fopen(names[i], "rb");
        if (!f) { LOG("font: missing %s", names[i]); exit(1); }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        fdata[i] = malloc(n);
        fread(fdata[i], 1, n, f);
        fclose(f);
        stbtt_InitFont(&fi[i], fdata[i], 0);
    }
}

int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p;
    if (!c) return 0;
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (n == 1) { *s += 1; return c < 0x80 ? c : 0xFFFD; }
    int cp = c & (0x3f >> (n - 1));
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) { *s += i; return 0xFFFD; }
        cp = cp << 6 | (p[i] & 0x3f);
    }
    *s += n;
    return cp;
}
int utf8_put(char *o, int cp) {
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xc0 | cp >> 6; o[1] = 0x80 | (cp & 63); return 2; }
    if (cp < 0x10000) { o[0] = 0xe0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 63); o[2] = 0x80 | (cp & 63); return 3; }
    o[0] = 0xf0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 63); o[2] = 0x80 | (cp >> 6 & 63); o[3] = 0x80 | (cp & 63);
    return 4;
}

static Glyph *glyph(int f, float px, int cp) {
    int sz = (int)(px * 4 + 0.5f);
    unsigned hsh = ((unsigned)cp * 2654435761u) ^ ((unsigned)sz * 40503u) ^ ((unsigned)f * 97u);
    Glyph *g = &cache[hsh % GC];
    if (g->bm && g->f == f && g->cp == cp && g->sz == sz) return g;
    free(g->bm);
    memset(g, 0, sizeof *g);
    float scale = stbtt_ScaleForPixelHeight(&fi[f], sz / 4.0f);
    int gi = stbtt_FindGlyphIndex(&fi[f], cp);
    if (!gi && cp > 32) {   // map common typographic chars the font lacks
        int alt = cp == 0x2019 || cp == 0x2018 ? '\'' : cp == 0x201C || cp == 0x201D ? '"' : cp == 0xA0 ? ' ' : '?';
        gi = stbtt_FindGlyphIndex(&fi[f], alt);
    }
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&fi[f], gi, &adv, &lsb);
    g->f = f; g->cp = cp; g->sz = sz; g->glyph = gi;
    g->adv = adv * scale;
    g->bm = stbtt_GetGlyphBitmap(&fi[f], scale, scale, gi, &g->w, &g->h, &g->xo, &g->yo);
    if (!g->bm) g->bm = malloc(1);
    return g;
}

static float kern(int f, float px, int g1, int g2) {
    if (!g1 || !g2) return 0;
    return stbtt_GetGlyphKernAdvance(&fi[f], g1, g2) * stbtt_ScaleForPixelHeight(&fi[f], px);
}

float text_wn(int f, float px, float track, const char *s, int n) {
    pthread_mutex_lock(&mu);
    float x = 0;
    int prev = 0;
    const char *e = n < 0 ? 0 : s + n;
    while (*s && (!e || s < e)) {
        int cp = utf8_next(&s);
        Glyph *g = glyph(f, px, cp);
        x += kern(f, px, prev, g->glyph) + g->adv + track * px;
        prev = g->glyph;
    }
    pthread_mutex_unlock(&mu);
    return x - (x > 0 ? track * px : 0);
}
float text_w(int f, float px, float track, const char *s) { return text_wn(f, px, track, s, -1); }

float text_n(Canvas *c, int f, float px, float track, float x, float base, uint32_t col, const char *s, int n) {
    pthread_mutex_lock(&mu);
    unsigned ca = (col >> 24) + ((col >> 24) >> 7);
    int prev = 0;
    const char *e = n < 0 ? 0 : s + n;
    while (*s && (!e || s < e)) {
        int cp = utf8_next(&s);
        Glyph *g = glyph(f, px, cp);
        x += kern(f, px, prev, g->glyph);
        prev = g->glyph;
        int gx = (int)floorf(x + 0.5f) + g->xo, gy = (int)floorf(base + 0.5f) + g->yo;
        if (gx < c->cx1 && gy < c->cy1 && gx + g->w > c->cx0 && gy + g->h > c->cy0) {
            int y0 = maxi(gy, c->cy0), y1 = mini(gy + g->h, c->cy1);
            int x0 = maxi(gx, c->cx0), x1 = mini(gx + g->w, c->cx1);
            for (int y = y0; y < y1; y++) {
                const unsigned char *src = g->bm + (y - gy) * g->w;
                uint32_t *d = c->px + (size_t)y * c->w;
                for (int xx = x0; xx < x1; xx++) {
                    unsigned a = src[xx - gx];
                    if (!a) continue;
                    a = ((a + (a >> 7)) * ca) >> 8;
                    uint32_t dv = d[xx];
                    uint32_t rb = (((col & 0xff00ff) * a + (dv & 0xff00ff) * (256 - a)) >> 8) & 0xff00ff;
                    uint32_t gg = (((col & 0xff00) * a + (dv & 0xff00) * (256 - a)) >> 8) & 0xff00;
                    d[xx] = 0xff000000 | rb | gg;
                }
            }
        }
        x += g->adv + track * px;
    }
    pthread_mutex_unlock(&mu);
    return x;
}
float text(Canvas *c, int f, float px, float track, float x, float base, uint32_t col, const char *s) {
    return text_n(c, f, px, track, x, base, col, s, -1);
}
void text_r(Canvas *c, int f, float px, float track, float xr, float base, uint32_t col, const char *s) {
    text(c, f, px, track, xr - text_w(f, px, track, s), base, col, s);
}
void text_c(Canvas *c, int f, float px, float track, float xc, float base, uint32_t col, const char *s) {
    text(c, f, px, track, xc - text_w(f, px, track, s) / 2, base, col, s);
}
float font_asc(int f, float px) {
    int a, d, l;
    stbtt_GetFontVMetrics(&fi[f], &a, &d, &l);
    return a * stbtt_ScaleForPixelHeight(&fi[f], px);
}
float font_cap(int f, float px) {
    int x0, y0, x1, y1;
    if (!stbtt_GetCodepointBox(&fi[f], 'H', &x0, &y0, &x1, &y1)) return px * 0.7f;
    return y1 * stbtt_ScaleForPixelHeight(&fi[f], px);
}

char *text_ellipsize(int f, float px, const char *s, float maxw) {
    if (text_w(f, px, 0, s) <= maxw) return xstrdup(s);
    int n = strlen(s);
    float ew = text_w(f, px, 0, "…");
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
        if (text_wn(f, px, 0, s, n) + ew <= maxw) break;
    }
    while (n > 0 && s[n - 1] == ' ') n--;
    return strfmt("%.*s…", n, s);
}

// Greedy word wrap. Fills line start offsets / byte lengths; returns line count.
// A trailing newline yields a final empty line (so a caret can sit there).
int text_wrap(int f, float px, const char *s, float maxw, int *st, int *ln, int max) {
    int n = 0;
    const char *p = s;
    while (n < max) {
        const char *ls = p, *q = p, *brk = 0;
        float w = 0;
        int prevg = 0;
        while (*q && *q != '\n') {
            const char *nx = q;
            int cp = utf8_next(&nx);
            pthread_mutex_lock(&mu);
            Glyph *g = glyph(f, px, cp);
            float adv = g->adv + kern(f, px, prevg, g->glyph);
            prevg = g->glyph;
            pthread_mutex_unlock(&mu);
            if (w + adv > maxw && q > ls) break;
            w += adv;
            if (*q == ' ') brk = q;
            q = nx;
        }
        if (!*q || *q == '\n') {
            st[n] = ls - s; ln[n] = q - ls; n++;
            if (!*q) break;
            p = q + 1;
            if (!*p) { if (n < max) { st[n] = p - s; ln[n] = 0; n++; } break; }
            continue;
        }
        if (brk && brk > ls) { st[n] = ls - s; ln[n] = brk - ls; n++; p = brk + 1; }
        else { st[n] = ls - s; ln[n] = q - ls; n++; p = q; }
    }
    return n;
}
