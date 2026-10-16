// Calculator: expression entry with operator precedence, press animations.
#include "../ui.h"

#define MAXT 64
static char tok[MAXT][32];      // alternating numbers and operators
static int ntok;
static char cur[32] = "0";      // number being typed
static char shown_expr[256];
static int just_eval, err;
static Gest g;
static Spring kscale[20];        // per-key press spring (1 = rest)

static const char *KEYS[20] = { "C", "±", "%", "÷", "7", "8", "9", "×", "4", "5", "6", "−", "1", "2", "3", "+", "0", "", ".", "=" };

static int prec(const char *op) { return (!strcmp(op, "×") || !strcmp(op, "÷")) ? 2 : 1; }
static double apply(double a, double b, const char *op, int *e) {
    if (!strcmp(op, "+")) return a + b;
    if (!strcmp(op, "−")) return a - b;
    if (!strcmp(op, "×")) return a * b;
    if (b == 0) { *e = 1; return 0; }
    return a / b;
}
// shunting-yard over tok[] + cur
static double evaluate(int *e) {
    double vs[MAXT]; const char *os[MAXT];
    int nv = 0, no = 0;
    *e = 0;
    for (int i = 0; i <= ntok; i++) {
        const char *t = i < ntok ? tok[i] : cur;
        if (i % 2 == 0) { vs[nv++] = strtod(t, 0); continue; }
        while (no && prec(os[no - 1]) >= prec(t)) { double b = vs[--nv], a = vs[--nv]; vs[nv++] = apply(a, b, os[--no], e); }
        os[no++] = t;
    }
    while (no) { double b = vs[--nv], a = vs[--nv]; vs[nv++] = apply(a, b, os[--no], e); }
    return vs[0];
}
static void fmt(double v, char *o, int n) {
    if (!isfinite(v)) { snprintf(o, n, "Error"); return; }
    snprintf(o, n, "%.12g", v);
    if (strchr(o, 'e')) snprintf(o, n, "%.6g", v);
    if (!strcmp(o, "-0")) snprintf(o, n, "0");
}
static void rebuild_expr(void) {
    shown_expr[0] = 0;
    for (int i = 0; i < ntok; i++) { strcat(shown_expr, tok[i]); strcat(shown_expr, " "); }
}

static void press(int k) {
    const char *key = KEYS[k];
    if (err && k != 0) return;
    if (k == 0) { ntok = 0; strcpy(cur, "0"); err = 0; just_eval = 0; shown_expr[0] = 0; return; }
    if (k == 1) { if (cur[0] == '-') memmove(cur, cur + 1, strlen(cur)); else if (strcmp(cur, "0")) { memmove(cur + 1, cur, strlen(cur) + 1); cur[0] = '-'; } return; }
    if (k == 2) { fmt(strtod(cur, 0) / 100, cur, sizeof cur); return; }
    if (k == 19) {   // =
        if (!ntok) return;
        int e;
        double v = evaluate(&e);
        rebuild_expr();
        strcat(shown_expr, cur); strcat(shown_expr, " =");
        if (e) { strcpy(cur, "Error"); err = 1; } else fmt(v, cur, sizeof cur);
        ntok = 0; just_eval = 1;
        LOG("calc: %s %s", shown_expr, cur);
        return;
    }
    if (k == 3 || k == 7 || k == 11 || k == 15) {   // operator
        if (ntok + 2 >= MAXT) return;
        strcpy(tok[ntok++], cur);
        strcpy(tok[ntok++], key);
        strcpy(cur, "0");
        just_eval = 0;
        rebuild_expr();
        return;
    }
    // digit or point
    if (just_eval) { strcpy(cur, "0"); just_eval = 0; shown_expr[0] = 0; }
    if (strlen(cur) >= 15) return;
    if (k == 18) { if (!strchr(cur, '.')) strcat(cur, "."); return; }
    if (!strcmp(cur, "0")) strcpy(cur, key);
    else if (!strcmp(cur, "-0")) strcpy(cur + 1, key);
    else strcat(cur, key);
}

static void open_(void) { }

static void draw(Canvas *c, float dt) {
    g_clear(c, TH.bg);
    // display slab
    float dh = P(330);
    g_rect(c, 0, 0, SW, dh, C_LIME);
    status_line(c, 0, C_INK, true, false);
    text(c, W800, P(20), -0.02f, P(20), P(70), C_INK, "Calculator");
    char *ex = text_ellipsize(W600, P(18), shown_expr, P(350));
    text_r(c, W600, P(18), 0, P(372), P(200), c_alpha(C_INK, 0.6f), ex);
    free(ex);
    char disp[40];
    snprintf(disp, sizeof disp, "%s", cur);
    float fs = P(110);
    while (fs > P(30) && text_w(W800, fs, -0.06f, disp) > P(356)) fs *= 0.92f;
    text_r(c, W800, fs, -0.06f, P(374), dh - P(28), C_INK, disp);
    // keypad
    float gx = P(16), gap = P(10), kw = (SW - 2 * gx - 3 * gap) / 4, kh = P(82);
    float y0 = dh + P(18);
    for (int k = 0; k < 20; k++) {
        if (k == 17) continue;
        int r = k / 4, col = k % 4;
        float w = k == 16 ? kw * 2 + gap : kw;
        float x = gx + col * (kw + gap), y = y0 + r * (kh + gap);
        Spring *s = &kscale[k];
        s->t = pressed(0x900 + k) ? 0.9f : 1;
        if (s->x == 0) s->x = 1;
        spring_step(s, dt, 5, 0.5f);
        float sc = s->x, cw = w * sc, ch = kh * sc;
        float cx = x + (w - cw) / 2, cy = y + (kh - ch) / 2;
        bool op = col == 3, fn = r == 0 && col < 3, eq = k == 19;
        bool pr = pressed(0x900 + k);
        uint32_t fill = pr ? C_LIME : eq ? C_ORANGE : op ? C_LILAC : fn ? C_PAPER : 0;
        uint32_t fg = fill ? C_INK : TH.fg;
        if (fill) g_rrect(c, cx, cy, cw, ch, P(26) * sc, P(26) * sc, fill);
        else g_rrect_line(c, cx, cy, cw, ch, P(26) * sc, P(2), TH.fg);
        const char *lab = (k == 0 && (strcmp(cur, "0") || ntok)) ? "AC" : KEYS[k];
        if (k == 0) lab = ntok || strcmp(cur, "0") ? "C" : "AC";
        float ls = P(op || eq ? 34 : 30) * sc;
        if (k == 16) text(c, W800, ls, 0, cx + P(30), cy + ch / 2 + ls * 0.36f, fg, lab);
        else text_c(c, W800, ls, 0, cx + cw / 2, cy + ch / 2 + ls * 0.36f, fg, lab);
        hit(0x900 + k, x, y, w, kh);
    }
    home_bar(c, TH.fg);
}

static void touch(InputEv *e) {
    int r = gest(&g, e);
    if (r == G_TAP) {
        int id = hit_at(e->x, e->y);
        if (id >= 0x900 && id < 0x914) { press(id - 0x900); kscale[id - 0x900].v = -3; }
    }
}
static void key(int k) {
    const char *map = "0123456789";
    const char *p = k < 128 && k ? strchr(map, k) : 0;
    static const int dk[10] = { 16, 12, 13, 14, 8, 9, 10, 4, 5, 6 };
    if (p) press(dk[p - map]);
    else if (k == '+') press(15);
    else if (k == '-') press(11);
    else if (k == '*' || k == 'x') press(7);
    else if (k == '/') press(3);
    else if (k == '.') press(18);
    else if (k == '=' || k == K_ENTER) press(19);
    else if (k == K_BACKSPACE || k == 'c') press(0);
}
App app_calc = { "Calculator", IC_CALC, open_, draw, touch, key, 0 };
