// Notes: list + editor, saved to the data disk, own on-screen keyboard.
#include "../ui.h"

typedef struct { long ts; char *text; } Note;
static Note *notes;
static int nnotes, loaded;
static int editing = -1;        // index being edited
static Gest g;
static Scroll list_sc, ed_sc;
static Spring kb_y;             // keyboard slide (0 hidden .. 1 shown)
static int kb_on;
#define CAP 8192

static float fsize(void) { static const float s[3] = { 15, 17, 20 }; return P(s[SET.textsize % 3]); }

static void load(void) {
    if (loaded) return;
    loaded = 1;
    const char *s = store_get("notes");
    if (!s || !*s) return;
    char *dup = xstrdup(s), *save = 0;
    for (char *r = strtok_r(dup, "\x1e", &save); r; r = strtok_r(0, "\x1e", &save)) {
        char *sep = strchr(r, '\x1f');
        if (!sep) continue;
        notes = realloc(notes, (nnotes + 1) * sizeof *notes);
        notes[nnotes].ts = atol(r);
        notes[nnotes].text = malloc(CAP);
        snprintf(notes[nnotes].text, CAP, "%s", sep + 1);
        nnotes++;
    }
    free(dup);
}
static void save(void) {
    size_t len = 1;
    for (int i = 0; i < nnotes; i++) len += strlen(notes[i].text) + 24;
    char *b = malloc(len), *o = b;
    *o = 0;
    for (int i = 0; i < nnotes; i++) {
        if (!notes[i].text[0]) continue;   // empty notes are not kept
        o += sprintf(o, "%s%ld\x1f%s", o == b ? "" : "\x1e", notes[i].ts, notes[i].text);
    }
    store_set("notes", b);
    int n = 0;
    for (int i = 0; i < nnotes; i++) n += notes[i].text[0] != 0;
    store_seti("notes.count", n);
    free(b);
}
static void close_editor(void) {
    if (editing >= 0 && !notes[editing].text[0]) {   // drop empty note
        free(notes[editing].text);
        memmove(&notes[editing], &notes[editing + 1], (nnotes - editing - 1) * sizeof *notes);
        nnotes--;
    }
    editing = -1; kb_on = 0;
    save();
}

static void open_(void) { load(); editing = -1; kb_on = 0; kb_y.x = 0; }

static void first_line(const char *t, char *o, int n) {
    int i = 0;
    while (t[i] && t[i] != '\n' && i < n - 1) { o[i] = t[i]; i++; }
    o[i] = 0;
}

static void draw_list(Canvas *c, float dt) {
    float top = P(176);
    scroll_step(&list_sc, dt);
    float rh = P(104), y = top + P(16) - list_sc.pos;
    list_sc.max = nnotes * (rh + P(10)) + P(60) - (SH - top);
    int sc[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
    g_clip(c, 0, top, SW, SH);
    if (!nnotes) {
        text(c, W800, P(40), -0.04f, P(20), top + P(90), TH.sub, "No notes yet");
        text(c, W600, P(16), 0, P(22), top + P(124), TH.sub, "Tap + to write one.");
    }
    for (int i = nnotes - 1; i >= 0; i--, y += rh + P(10)) {
        if (y > SH || y + rh < top) continue;
        int id = 0xB00 + i;
        bool pr = pressed(id);
        if (pr) g_rrect(c, P(16), y, P(358), rh, P(22), P(22), C_LIME);
        else g_rrect_line(c, P(16), y, P(358), rh, P(22), P(2), TH.fg);
        uint32_t fg = pr ? C_INK : TH.fg;
        char t[160];
        first_line(notes[i].text, t, sizeof t);
        char *tt = text_ellipsize(W800, P(20), t[0] ? t : "Untitled", P(310));
        text(c, W800, P(20), -0.02f, P(34), y + P(38), fg, tt);
        free(tt);
        const char *rest = strchr(notes[i].text, '\n');
        char pv[200];
        first_line(rest ? rest + 1 : "", pv, sizeof pv);
        char *pp = text_ellipsize(W400, P(14), pv[0] ? pv : "No additional text", P(310));
        text(c, W400, P(14), 0, P(34), y + P(64), pr ? C_INK : TH.sub, pp);
        free(pp);
        char ds[48];
        time_t ts = notes[i].ts;
        struct tm lt;
        gmtime_r(&ts, &lt);
        strftime(ds, sizeof ds, "%d %b · %H:%M", &lt);
        text(c, W600, P(12), 0.04f, P(34), y + P(88), pr ? C_INK : TH.sub, ds);
        hit(id, P(16), y, P(358), rh);
    }
    c->cx0 = sc[0]; c->cy0 = sc[1]; c->cx1 = sc[2]; c->cy1 = sc[3];
    // header on top of the scrolled list
    g_rect(c, 0, 0, SW, top, C_LIME);
    status_line(c, 0, C_INK, true, false);
    text(c, W800, P(52), -0.05f, P(18), P(140), C_INK, "Notes");
    char cnt[32];
    snprintf(cnt, sizeof cnt, "%d note%s", nnotes, nnotes == 1 ? "" : "s");
    text(c, W600, P(14), 0, P(22), P(164), C_INK, cnt);
    btn_icon(c, 0xA01, P(312), P(100), P(56), IC_PLUS, C_LIME, C_INK);
}

static void draw_editor(Canvas *c, float dt) {
    Note *n = &notes[editing];
    float top = P(132);
    kb_y.t = kb_on;
    spring_step(&kb_y, dt, 3.2f, 0.9f);
    float kbt = SH - kb_height() * clampf(kb_y.x, 0, 1.05f);
    float fs = fsize(), lh = fs * 1.45f;
    static int st[2048], ln[2048];
    int nl = text_wrap(W400, fs, n->text, P(350), st, ln, 2048);
    float view = kbt - top - P(20);
    ed_sc.max = nl * lh + P(40) - view;
    if (kb_on && !ed_sc.held) ed_sc.pos = fmaxf(ed_sc.max, 0);   // keep the caret in view while typing
    scroll_step(&ed_sc, dt);
    int sc[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
    g_clip(c, 0, top, SW, kbt);
    float y = top + P(24) - ed_sc.pos;
    for (int i = 0; i < nl; i++, y += lh) {
        if (y < top - lh || y > kbt + lh) continue;
        bool title = i == 0;
        text_n(c, title ? W800 : W400, title ? fs * 1.3f : fs, 0, P(20), y + fs, TH.fg, n->text + st[i], ln[i]);
        if (title) y += fs * 0.35f;
    }
    if (!n->text[0]) text(c, W800, fs * 1.3f, 0, P(20), top + P(24) + fs, TH.sub, "Start typing…");
    // caret at the end
    if (fmod(now(), 1.0) < 0.6) {
        int li = nl - 1;
        bool title = li == 0;
        float cy = top + P(24) - ed_sc.pos + li * lh + (li > 0 ? fs * 0.35f : 0);
        float cx = P(20) + text_wn(title ? W800 : W400, title ? fs * 1.3f : fs, 0, n->text + st[li], ln[li]);
        g_rect(c, cx + P(1), cy - P(2), P(2.5f), (title ? fs * 1.3f : fs) * 1.25f, C_ORANGE);
    }
    hit(0xA02, 0, top, SW, kbt - top);
    c->cx0 = sc[0]; c->cy0 = sc[1]; c->cx1 = sc[2]; c->cy1 = sc[3];
    // header
    g_rect(c, 0, 0, SW, top, C_LIME);
    status_line(c, 0, C_INK, true, false);
    btn_icon(c, 0xA03, P(14), P(62), P(52), IC_BACK, C_INK, 0);
    char ds[48];
    time_t ts = n->ts;
    struct tm lt;
    gmtime_r(&ts, &lt);
    strftime(ds, sizeof ds, "%a %d %b, %H:%M", &lt);
    text(c, W800, P(22), -0.02f, P(78), P(86), C_INK, "Note");
    text(c, W600, P(13), 0, P(78), P(106), C_INK, ds);
    btn_icon(c, 0xA05, P(258), P(62), P(52), IC_KEYBOARD, C_INK, 0);
    btn_icon(c, 0xA04, P(320), P(62), P(52), IC_TRASH, C_INK, 0);
    if (kb_y.x > 0.01f) kb_draw(c, kbt, "return");
}

static void draw(Canvas *c, float dt) {
    load();
    g_clear(c, TH.bg);
    if (editing >= 0) draw_editor(c, dt);
    else draw_list(c, dt);
    if (!(editing >= 0 && kb_y.x > 0.5f)) home_bar(c, TH.fg);
}

static void type_key(int k) {
    if (editing < 0) return;
    char *t = notes[editing].text;
    if (k == K_ENTER) { int n = strlen(t); if (n < CAP - 2) { t[n] = '\n'; t[n + 1] = 0; } }
    else field_key(t, CAP, k);
    notes[editing].ts = time(0);
    save();
}

static void touch(InputEv *e) {
    int r = gest(&g, e);
    Scroll *s = editing >= 0 ? &ed_sc : &list_sc;
    if (r == G_DRAG_START || r == G_DRAG) {
        if (g.vert && !(editing >= 0 && kb_on && g.y0 > SH - kb_height())) scroll_drag(s, r == G_DRAG_START ? g.y - g.y0 : g.dy);
    } else if (r == G_DRAG_END) {
        if (s->held) scroll_fling(s, g.vy);
    } else if (r == G_TAP) {
        int id = hit_at(e->x, e->y);
        if (editing >= 0 && kb_on && kb_hit(id)) { int k = kb_tap(e->x, e->y); if (k) type_key(k); return; }
        if (id == 0xA01) {   // new note
            notes = realloc(notes, (nnotes + 1) * sizeof *notes);
            notes[nnotes].ts = time(0);
            notes[nnotes].text = calloc(1, CAP);
            editing = nnotes++;
            kb_on = 1;
            ed_sc = (Scroll){ 0 };
        } else if (id >= 0xB00 && id < 0xB00 + nnotes && editing < 0) {
            editing = id - 0xB00; kb_on = 0; ed_sc = (Scroll){ 0 };
        } else if (id == 0xA02) kb_on = 1;
        else if (id == 0xA03) close_editor();
        else if (id == 0xA05) kb_on = !kb_on;
        else if (id == 0xA04 && editing >= 0) {
            free(notes[editing].text);
            memmove(&notes[editing], &notes[editing + 1], (nnotes - editing - 1) * sizeof *notes);
            nnotes--; editing = -1; kb_on = 0;
            save();
        }
    }
}
static void key(int k) {
    if (editing >= 0) type_key(k);
}
App app_notes = { "Notes", IC_NOTES, open_, draw, touch, key, 0 };
