// Layout: block flow with collapsing sibling margins, inline formatting with
// word wrap, list markers, tables (content-sized columns), images and form
// controls. Produces a flat display list in device pixels.
#include "web.h"
#include <ctype.h>

typedef struct {
    DList *dl;
    Doc *doc;
    float sc, tsc;          // css px -> device px for boxes / text
    int link;               // current link index
    float first_base;       // first baseline in the current block (-1 unset)
} L;

static DItem *push(L *l, DItem it) {
    DList *d = l->dl;
    if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 1024; d->it = realloc(d->it, d->cap * sizeof *d->it); }
    d->it[d->n] = it;
    return &d->it[d->n++];
}
static int add_link(L *l, const char *href) {
    DList *d = l->dl;
    if (d->nlinks == d->capl) { d->capl = d->capl ? d->capl * 2 : 64; d->links = realloc(d->links, d->capl * sizeof(char *)); }
    d->links[d->nlinks] = url_resolve(l->doc->base, href);
    return d->nlinks++;
}

static bool skip(const Node *n) { return n->type == N_ELEM && (!n->st || n->st->display == D_NONE || n->st->hidden >= 1); }
static bool replaced(const Node *n) {
    const char *t = n->tag;
    return !strcmp(t, "img") || !strcmp(t, "input") || !strcmp(t, "button") || !strcmp(t, "select") || !strcmp(t, "textarea");
}
static bool is_block(const Node *n) {
    if (n->type != N_ELEM || !n->st || replaced(n)) return false;   // replaced elements are always atoms
    int d = n->st->display;
    return d == D_BLOCK || d == D_LIST_ITEM || d == D_TABLE || d == D_ROW || d == D_CELL || d == D_ROWGROUP || n->st->floated;
}
static int font_of(const Style *st) { return st->bold ? (st->fs >= 22 ? W800 : W600) : W400; }
static float fpx(L *l, const Style *st) { return st->fs * l->tsc; }
static float lenv(Len v, float base) { return v.unit == 1 ? v.v : v.unit == 2 ? base * v.v / 100 : 0; }

// ---- intrinsic widths (device px) --------------------------------------------------------------------
static void img_size(L *l, Node *n, float maxw, float *w, float *h);
static void meas(L *l, Node *n, float *mn, float *line, float *mx) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type == N_TEXT) {
            const Style *st = n->st;
            int f = font_of(st);
            float px = fpx(l, st), sp = text_w(f, px, 0, " ");
            const char *p = c->text, *e = c->text + c->tlen;
            while (p < e) {
                while (p < e && isspace((unsigned char)*p)) { if (*p == '\n' && st->ws == 1) { *mx = fmaxf(*mx, *line); *line = 0; } p++; }
                const char *ws = p;
                while (p < e && !isspace((unsigned char)*p)) p++;
                if (p > ws) {
                    float w = text_wn(f, px, 0, ws, p - ws);
                    if (st->ws != 2) *mn = fmaxf(*mn, w);
                    *line += w + sp;
                    if (st->ws == 2) *mn = fmaxf(*mn, *line);
                }
            }
            continue;
        }
        if (skip(c)) continue;
        const Style *st = c->st;
        if (!strcmp(c->tag, "br")) { *mx = fmaxf(*mx, *line); *line = 0; continue; }
        if (!strcmp(c->tag, "img")) {
            float w, h;
            img_size(l, c, 1e9f, &w, &h);
            *mn = fmaxf(*mn, w); *line += w;
            continue;
        }
        if (!strcmp(c->tag, "input") || !strcmp(c->tag, "select") || !strcmp(c->tag, "textarea") || !strcmp(c->tag, "button")) {
            float w = 120 * l->sc;
            *mn = fmaxf(*mn, w); *line += w;
            continue;
        }
        if (st->width.unit == 1 && !is_block(c)) { float w = st->width.v * l->sc; *mn = fmaxf(*mn, w); *line += w; continue; }
        float hp = (lenv(st->padding[1], 0) + lenv(st->padding[3], 0) + st->border[1] + st->border[3] + lenv(st->margin[1], 0) + lenv(st->margin[3], 0)) * l->sc;
        if (is_block(c)) {
            *mx = fmaxf(*mx, *line); *line = 0;
            float cmn = 0, cl = 0, cmx = 0;
            if (st->display == D_ROW || st->display == D_TABLE || st->display == D_ROWGROUP) {
                // a row is as wide as its cells side by side
                meas(l, c, &cmn, &cl, &cmx);
                if (st->display == D_ROW) {
                    float smn = 0, smx = 0;
                    for (Node *cell = c->first; cell; cell = cell->next) {
                        if (cell->type != N_ELEM || skip(cell)) continue;
                        float a = 0, b = 0, d = 0;
                        meas(l, cell, &a, &b, &d);
                        float cp = (lenv(cell->st->padding[1], 0) + lenv(cell->st->padding[3], 0)) * l->sc;
                        smn += a + cp; smx += fmaxf(d, b) + cp;
                    }
                    cmn = smn; cmx = smx; cl = 0;
                }
            } else meas(l, c, &cmn, &cl, &cmx);
            cmx = fmaxf(cmx, cl);
            if (st->width.unit == 1) cmn = cmx = st->width.v * l->sc;
            *mn = fmaxf(*mn, cmn + hp); *mx = fmaxf(*mx, cmx + hp);
        } else {
            *line += hp;
            meas(l, c, mn, line, mx);
        }
    }
}

// ---- images --------------------------------------------------------------------------------------------
static void img_size(L *l, Node *n, float maxw, float *w, float *h) {
    const Style *st = n->st;
    float cw = st->width.unit == 1 ? st->width.v : st->width.unit == 2 ? 0 : 0;
    float ch = st->height.unit == 1 ? st->height.v : 0;
    if (st->width.unit == 2) cw = maxw / l->sc * st->width.v / 100;
    float nw = n->img ? n->img->w : 0, nh = n->img ? n->img->h : 0;
    if (!cw && !ch) { cw = nw; ch = nh; }
    else if (cw && !ch) ch = nw ? cw * nh / nw : cw;
    else if (!cw && ch) cw = nh ? ch * nw / nh : ch;
    *w = cw * l->sc; *h = ch * l->sc;
    if (*w > maxw && *w > 0) { *h *= maxw / *w; *w = maxw; }
}

// ---- inline formatting ------------------------------------------------------------------------------------
enum { IT_WORD, IT_SPACE, IT_ATOM, IT_BR };
typedef struct {
    uint8_t kind, font;
    const char *s; int len;
    float w, h, px;
    const Style *st;
    int link;
    Node *node;
    uint32_t bg;
} II;
typedef struct { II *v; int n, cap; } IList;
static void ipush(IList *il, II it) {
    if (il->n == il->cap) { il->cap = il->cap ? il->cap * 2 : 256; il->v = realloc(il->v, il->cap * sizeof *il->v); }
    il->v[il->n++] = it;
}

static void collect_atom(L *l, Node *c, IList *il, float aw) {
    const char *t = c->tag;
        if (!strcmp(t, "img")) {
            float w, h;
            img_size(l, c, aw, &w, &h);
            if (w >= 2 * l->sc && h >= 2 * l->sc) ipush(il, (II){ IT_ATOM, 0, 0, 0, w, h, 0, c->st, l->link, c, 0 });
            else if (!c->img && attr(c, "alt") && *attr(c, "alt") && c->img_state == 3) {
                const char *alt = attr(c, "alt");
                ipush(il, (II){ IT_WORD, W400, alt, (int)strlen(alt), text_w(W400, fpx(l, c->st), 0, alt), fpx(l, c->st), fpx(l, c->st), c->st, l->link, 0, 0 });
            }
            return;
        }
        if (!strcmp(t, "input") || !strcmp(t, "textarea") || !strcmp(t, "select") || !strcmp(t, "button")) {
            const char *type = attr(c, "type");
            float px = fpx(l, c->st);
            if (!strcmp(t, "button") || (type && (!strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "reset")))) {
                char lab[128];
                if (!strcmp(t, "button")) text_content(c, lab, sizeof lab);
                else snprintf(lab, sizeof lab, "%s", attr(c, "value") ? attr(c, "value") : "Submit");
                if (!lab[0]) snprintf(lab, sizeof lab, "Go");
                char *s = ar_strndup(&l->doc->ar, lab, strlen(lab));
                float w = text_w(W600, px, 0, s) + 24 * l->sc;
                ipush(il, (II){ IT_ATOM, W600, s, (int)strlen(s), w, px * 2, px, c->st, l->link, c, 1 });
            } else if (type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) {
                ipush(il, (II){ IT_ATOM, 0, 0, 0, px, px, px, c->st, -1, c, 2 });
            } else if (type && (!strcasecmp(type, "image") || !strcasecmp(type, "file"))) {
            } else {
                float w = c->st->width.unit == 1 ? c->st->width.v * l->sc : c->st->width.unit == 2 ? aw * c->st->width.v / 100 : (attr(c, "size") ? atoi(attr(c, "size")) * 8.0f : 180) * l->sc;
                if (w > aw) w = aw;
                if (w < 60 * l->sc) w = fminf(aw, 60 * l->sc);
                float h = !strcmp(t, "textarea") ? px * 4.5f : px * 2.1f;
                if (!strcmp(t, "select") && !c->value) {   // shows the selected option
                    for (Node *o = c->first; o; o = o->next)
                        if (o->type == N_ELEM && !strcmp(o->tag, "option")) {
                            char lab[128]; text_content(o, lab, sizeof lab);
                            c->value = strdup(lab);
                            if (attr(o, "selected")) break;
                        }
                }
                ipush(il, (II){ IT_ATOM, 0, 0, 0, w, h, px, c->st, -1, c, 0 });
            }
            return;
        }
}

static void collect(L *l, Node *n, IList *il, float aw) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type == N_TEXT) {
            const Style *st = n->st;
            int f = font_of(st);
            float px = fpx(l, st);
            uint32_t bg = (st->display == D_INLINE && st->bg) ? st->bg : 0;
            const char *p = c->text, *e = p + c->tlen;
            while (p < e) {
                if (isspace((unsigned char)*p)) {
                    if (st->ws == 1 && *p == '\n') ipush(il, (II){ IT_BR, .st = st, .px = px });
                    else if (st->ws == 1) ipush(il, (II){ IT_SPACE, (uint8_t)f, " ", 1, text_w(f, px, 0, " ") * (*p == '\t' ? 4 : 1), px, px, st, l->link, 0, bg });
                    else if (!il->n || (il->v[il->n - 1].kind != IT_SPACE && il->v[il->n - 1].kind != IT_BR))
                        ipush(il, (II){ IT_SPACE, (uint8_t)f, " ", 1, text_w(f, px, 0, " "), px, px, st, l->link, 0, bg });
                    p++;
                    continue;
                }
                const char *ws = p;
                while (p < e && !isspace((unsigned char)*p)) p++;
                ipush(il, (II){ IT_WORD, (uint8_t)f, ws, (int)(p - ws), text_wn(f, px, 0, ws, p - ws), px, px, st, l->link, 0, bg });
            }
            continue;
        }
        if (skip(c)) continue;
        const char *t = c->tag;
        if (!strcmp(t, "br")) { ipush(il, (II){ IT_BR, .st = c->st, .px = fpx(l, c->st) }); continue; }
        if (replaced(c)) {   // block-level replaced elements sit on their own line
            bool blk = c->st->display == D_BLOCK || c->st->display == D_LIST_ITEM;
            if (blk) ipush(il, (II){ IT_BR, .st = c->st, .px = 0 });
            collect_atom(l, c, il, aw);
            if (blk) ipush(il, (II){ IT_BR, .st = c->st, .px = 0 });
            continue;
        }
        int saved = l->link;
        if (!strcmp(t, "a") && attr(c, "href") && strncasecmp(attr(c, "href"), "javascript:", 11)) l->link = add_link(l, attr(c, "href"));
        if (is_block(c)) {   // block inside inline content: break around it
            ipush(il, (II){ IT_BR, .st = c->st, .px = 0 });
            collect(l, c, il, aw);
            ipush(il, (II){ IT_BR, .st = c->st, .px = 0 });
        } else collect(l, c, il, aw);
        l->link = saved;
    }
}

static float asc_of(const II *it) {
    if (it->kind == IT_ATOM) return it->h;
    float lh = it->st->lh > 0 ? it->st->lh : 1.35f;
    return it->px * 0.8f + (lh - 1) * it->px / 2;
}
static float desc_of(const II *it) {
    if (it->kind == IT_ATOM) return it->node && !strcmp(it->node->tag, "img") ? 0 : it->px * 0.3f;
    float lh = it->st->lh > 0 ? it->st->lh : 1.35f;
    return it->px * 0.2f + (lh - 1) * it->px / 2;
}

static void emit_line(L *l, II *items, int n, float x, float y, float aw, int align, float *outh) {
    while (n && items[n - 1].kind == IT_SPACE) n--;
    int s0 = 0;
    while (s0 < n && items[s0].kind == IT_SPACE) s0++;
    float w = 0, A = 0, D = 0;
    for (int i = s0; i < n; i++) {
        if (items[i].kind == IT_BR) { A = fmaxf(A, asc_of(&items[i]) * (items[i].px > 0)); D = fmaxf(D, desc_of(&items[i]) * (items[i].px > 0)); continue; }
        w += items[i].w;
        A = fmaxf(A, asc_of(&items[i]));
        D = fmaxf(D, desc_of(&items[i]));
    }
    float cx = x + (align == 1 ? (aw - w) / 2 : align == 2 ? aw - w : 0);
    float base = y + A;
    if (l->first_base < 0 && n > s0) l->first_base = base;
    for (int i = s0; i < n; i++) {
        II *it = &items[i];
        if (it->kind == IT_BR) continue;
        if (it->bg && it->kind != IT_ATOM) push(l, (DItem){ DI_RECT, 0, cx, base - it->px * 0.85f, it->w, it->px * 1.15f, 0, it->bg, .link = -1 });
        if (it->kind == IT_WORD) {
            push(l, (DItem){ DI_TEXT, it->font, cx, base, it->w, it->px, it->px, it->st->color, it->s, it->len, 0, it->link });
            if (it->st->underline) {   // underline continues over a space only if the next word is underlined too
                bool cont = i + 2 < n && items[i + 1].kind == IT_SPACE && items[i + 2].kind == IT_WORD && items[i + 2].st->underline && items[i + 2].link == it->link;
                push(l, (DItem){ DI_LINE, 0, cx, base + it->px * 0.12f, it->w + (cont ? items[i + 1].w : 0), fmaxf(1, it->px * 0.06f), 0, it->st->color, .link = -1 });
            }
        } else if (it->kind == IT_SPACE && it->link >= 0) {
            push(l, (DItem){ DI_TEXT, it->font, cx, base, it->w, it->px, it->px, it->st->color, " ", 1, 0, it->link });
        } else if (it->kind == IT_ATOM) {
            Node *nd = it->node;
            int kind = !strcmp(nd->tag, "img") ? DI_IMAGE : it->bg == 1 ? DI_BUTTON : DI_INPUT;
            push(l, (DItem){ kind, it->font, cx, base - it->h, it->w, it->h, it->px, it->st->color, it->s, it->len, nd, it->link });
        }
        cx += it->w;
    }
    *outh = A + D;
}

// lays out an inline run; returns height
static float inline_run(L *l, IList *il, float x, float y, float aw, int align) {
    float cy = y, lw = 0;
    int start = 0;
    for (int i = 0; i < il->n; i++) {
        II *it = &il->v[i];
        if (it->kind == IT_BR) {
            float h;
            emit_line(l, il->v + start, i - start + 1, x, cy, aw, align, &h);
            if (h == 0 && it->px > 0) h = it->px * 1.3f;
            cy += h; start = i + 1; lw = 0;
            continue;
        }
        if (it->kind == IT_SPACE && lw == 0) continue;
        bool nowrap = it->st && it->st->ws == 2;
        if (lw + it->w > aw && lw > 0 && it->kind != IT_SPACE && !nowrap) {
            float h;
            emit_line(l, il->v + start, i - start, x, cy, aw, align, &h);
            cy += h; start = i; lw = 0;
        }
        if (it->kind == IT_WORD && it->w > aw && lw == 0) {   // a single overlong word: break it by characters
            const char *s = it->s;
            int n = it->len, k = 0;
            while (k < n && text_wn(it->font, it->px, 0, s, k + 1) <= aw) k++;
            if (k == 0) k = 1;
            while (k < n && ((unsigned char)s[k] & 0xc0) == 0x80) k++;
            if (k < n) {
                II head = *it, tail = *it;
                head.len = k; head.w = text_wn(it->font, it->px, 0, s, k);
                tail.s = s + k; tail.len = n - k; tail.w = text_wn(it->font, it->px, 0, tail.s, tail.len);
                float h;
                II tmp = head;
                emit_line(l, &tmp, 1, x, cy, aw, align, &h);
                cy += h;
                *it = tail;
                start = i; i--;
                continue;
            }
        }
        lw += it->w;
    }
    if (start < il->n) { float h; emit_line(l, il->v + start, il->n - start, x, cy, aw, align, &h); cy += h; }
    return cy - y;
}

// ---- blocks ------------------------------------------------------------------------------------------------
static float layout_block(L *l, Node *n, float x, float y, float aw, float *mb_out);
static float layout_table(L *l, Node *n, float x, float y, float aw);

static float flow(L *l, Node *n, float x, float y, float aw) {
    float cy = y, pm = 0;
    Node *c = n->first;
    while (c) {
        if (c->type == N_ELEM && skip(c)) { c = c->next; continue; }
        if (c->type == N_ELEM && is_block(c)) {
            float mt = lenv(c->st->margin[0], aw) * l->sc;
            cy += mt > pm ? mt - pm : 0;   // collapse with the previous sibling's bottom margin
            float mb = 0;
            cy += layout_block(l, c, x, cy, aw, &mb);
            cy += mb;
            pm = mb;
            c = c->next;
            continue;
        }
        // gather an inline run (anonymous block)
        IList il = { 0 };
        Node fake = *n;
        Node *first = c;
        while (c && !(c->type == N_ELEM && !skip(c) && is_block(c))) c = c->next;
        // collect over [first, c)
        Node tmp = { 0 };
        tmp.type = N_ELEM; tmp.tag = n->tag; tmp.st = n->st;
        tmp.first = first;
        Node *stop = c;
        // temporarily cut the sibling chain
        Node *last = first;
        while (last->next != stop) last = last->next;
        Node *save = last->next;
        last->next = 0;
        collect(l, &tmp, &il, aw);
        last->next = save;
        (void)fake;
        bool any = false;
        for (int i = 0; i < il.n; i++) if (il.v[i].kind == IT_WORD || il.v[i].kind == IT_ATOM) { any = true; break; }
        if (any) {
            float h = inline_run(l, &il, x, cy, aw, n->st->align);
            cy += h;
            pm = 0;
        }
        free(il.v);
    }
    return cy - y;
}

static float layout_block(L *l, Node *n, float x, float y, float aw, float *mb_out) {
    Style *st = n->st;
    float sc = l->sc;
    float ml = lenv(st->margin[3], aw) * sc, mr = lenv(st->margin[1], aw) * sc;
    *mb_out = lenv(st->margin[2], aw) * sc;
    float pl = lenv(st->padding[3], aw) * sc, pr = lenv(st->padding[1], aw) * sc, pt = lenv(st->padding[0], aw) * sc, pb = lenv(st->padding[2], aw) * sc;
    float bl = st->border[3] * sc, br = st->border[1] * sc, bt = st->border[0] * sc, bb = st->border[2] * sc;
    float bw = aw - ml - mr;   // border-box width
    if (st->width.unit == 1 || st->width.unit == 2) {
        float w = lenv(st->width, aw) * (st->width.unit == 1 ? sc : 1) + pl + pr + bl + br;
        if (w < bw) bw = w;
    }
    if (st->maxw.unit == 1 || st->maxw.unit == 2) {
        float w = lenv(st->maxw, aw) * (st->maxw.unit == 1 ? sc : 1) + pl + pr + bl + br;
        if (w < bw) bw = w;
    }
    if (bw < 8 * sc) bw = fminf(aw, 8 * sc);
    float bx = x + ml;
    if (st->margin[3].unit == 4 && st->margin[1].unit == 4) bx = x + (aw - bw) / 2;
    else if (st->margin[3].unit == 4) bx = x + aw - mr - bw;
    // background placeholder (patched once the height is known)
    int bg_i = -1;
    if (st->bg) { push(l, (DItem){ DI_RECT, 0, bx, y, bw, 0, 0, st->bg, .link = -1 }); bg_i = l->dl->n - 1; }
    float cx = bx + bl + pl, cw = bw - bl - br - pl - pr, cy = y + bt + pt;
    float ch;
    if (st->display == D_TABLE) ch = layout_table(l, n, cx, cy, cw);
    else {
        float fb = l->first_base;
        if (st->display == D_LIST_ITEM) l->first_base = -1;
        int saved = l->link;
        if (!strcmp(n->tag, "a") && attr(n, "href")) l->link = add_link(l, attr(n, "href"));
        ch = flow(l, n, cx, cy, cw);
        l->link = saved;
        if (st->display == D_LIST_ITEM && st->list) {
            float base = l->first_base >= 0 ? l->first_base : cy + st->fs * l->tsc;
            char mk[16];
            if (st->list == 2) {
                int idx = 1;
                const char *start = n->parent ? attr(n->parent, "start") : 0;
                if (start) idx = atoi(start);
                for (Node *s = n->parent ? n->parent->first : 0; s && s != n; s = s->next) if (s->type == N_ELEM && !strcmp(s->tag, "li")) idx++;
                snprintf(mk, sizeof mk, "%d.", idx);
            } else snprintf(mk, sizeof mk, "•");
            char *m = ar_strndup(&l->doc->ar, mk, strlen(mk));
            float px = fpx(l, st), w = text_w(W400, px, 0, m);
            push(l, (DItem){ DI_TEXT, W400, cx - w - 6 * sc, base, w, px, px, st->color, m, (int)strlen(m), 0, -1 });
        }
        if (st->display == D_LIST_ITEM) l->first_base = fb;
    }
    if (st->height.unit == 1 && n->tag[0] != 'i' && st->height.v * l->sc > ch && st->height.v < 2000) ch = st->height.v * l->sc;
    float h = bt + pt + ch + pb + bb;
    if (bg_i >= 0) l->dl->it[bg_i].h = h;
    uint32_t *bc = st->bcolor;
    if (bt > 0) push(l, (DItem){ DI_RECT, 0, bx, y, bw, bt, 0, bc[0] ? bc[0] : st->color, .link = -1 });
    if (bb > 0) push(l, (DItem){ DI_RECT, 0, bx, y + h - bb, bw, bb, 0, bc[2] ? bc[2] : st->color, .link = -1 });
    if (bl > 0) push(l, (DItem){ DI_RECT, 0, bx, y, bl, h, 0, bc[3] ? bc[3] : st->color, .link = -1 });
    if (br > 0) push(l, (DItem){ DI_RECT, 0, bx + bw - br, y, br, h, 0, bc[1] ? bc[1] : st->color, .link = -1 });
    return h;
}

// ---- tables -------------------------------------------------------------------------------------------------
#define MAXC 32
static int collect_rows(Node *n, Node **rows, int max, int k) {
    for (Node *c = n->first; c && k < max; c = c->next) {
        if (c->type != N_ELEM || skip(c)) continue;
        if (c->st->display == D_ROW || !strcmp(c->tag, "tr")) rows[k++] = c;
        else if (c->st->display == D_ROWGROUP || !strcmp(c->tag, "tbody") || !strcmp(c->tag, "thead") || !strcmp(c->tag, "tfoot")) k = collect_rows(c, rows, max, k);
    }
    return k;
}
static float layout_table(L *l, Node *n, float x, float y, float aw) {
    Node **rows = malloc(2000 * sizeof *rows);   // per call: tables nest
    int nr = collect_rows(n, rows, 2000, 0);
    float sc = l->sc;
    const char *cs = attr(n, "cellspacing");
    float sp = (cs ? atoi(cs) : 2) * sc;
    float mn[MAXC] = { 0 }, mx[MAXC] = { 0 }, pref[MAXC] = { 0 };
    int ncols = 0;
    for (int r = 0; r < nr; r++) {
        int col = 0;
        for (Node *c = rows[r]->first; c && col < MAXC; c = c->next) {
            if (c->type != N_ELEM || skip(c)) continue;
            int span = attr(c, "colspan") ? maxi(1, atoi(attr(c, "colspan"))) : 1;
            float a = 0, b = 0, d = 0;
            meas(l, c, &a, &b, &d);
            d = fmaxf(d, b);
            float pad = (lenv(c->st->padding[1], 0) + lenv(c->st->padding[3], 0) + c->st->border[1] + c->st->border[3]) * sc;
            a += pad; d += pad;
            if (c->st->width.unit == 1) { pref[col] = fmaxf(pref[col], c->st->width.v * sc); d = fmaxf(a, c->st->width.v * sc); }
            if (span == 1) { mn[col] = fmaxf(mn[col], a); mx[col] = fmaxf(mx[col], d); }
            else {
                for (int k = 0; k < span && col + k < MAXC; k++) { mn[col + k] = fmaxf(mn[col + k], a / span); mx[col + k] = fmaxf(mx[col + k], d / span); }
            }
            col += span;
        }
        ncols = maxi(ncols, mini(col, MAXC));
    }
    if (!ncols) { free(rows); return 0; }
    float avail = aw - sp * (ncols + 1);
    float tw = n->st->width.unit ? lenv(n->st->width, aw) * (n->st->width.unit == 1 ? sc : 1) - sp * (ncols + 1) : 0;
    if (tw > avail || tw <= 0) tw = tw > avail ? avail : 0;
    float smn = 0, smx = 0;
    for (int i = 0; i < ncols; i++) { mx[i] = fmaxf(mx[i], mn[i]); smn += mn[i]; smx += mx[i]; }
    float cw[MAXC];
    float target = tw > 0 ? tw : fminf(avail, smx);
    if (smx <= target) for (int i = 0; i < ncols; i++) cw[i] = mx[i] + (smx > 0 ? (target - smx) * mx[i] / smx : (target / ncols));
    else if (smn >= target) for (int i = 0; i < ncols; i++) cw[i] = smn > 0 ? target * mn[i] / smn : target / ncols;
    else for (int i = 0; i < ncols; i++) cw[i] = mn[i] + (target - smn) * (smx - smn > 0 ? (mx[i] - mn[i]) / (smx - smn) : 0);
    float cy = y + sp;
    float tw_total = sp;
    for (int i = 0; i < ncols; i++) tw_total += cw[i] + sp;
    float tx = x;
    if (n->st->margin[1].unit == 4 && n->st->margin[3].unit == 4 && tw_total < aw) tx = x + (aw - tw_total) / 2;
    for (int r = 0; r < nr; r++) {
        Node *row = rows[r];
        int row_bg = row->st->bg ? (push(l, (DItem){ DI_RECT, 0, tx, cy, tw_total, 0, 0, row->st->bg, .link = -1 }), l->dl->n - 1) : -1;
        float rh = 0, cx = tx + sp;
        int col = 0;
        int cell_bg[MAXC], ncell = 0;
        float cell_x[MAXC], cell_w[MAXC];
        for (Node *c = row->first; c && col < ncols; c = c->next) {
            if (c->type != N_ELEM || skip(c)) continue;
            int span = attr(c, "colspan") ? maxi(1, atoi(attr(c, "colspan"))) : 1;
            float w = 0;
            for (int k = 0; k < span && col + k < ncols; k++) w += cw[col + k] + (k ? sp : 0);
            Style *st = c->st;
            cell_bg[ncell] = st->bg ? (push(l, (DItem){ DI_RECT, 0, cx, cy, w, 0, 0, st->bg, .link = -1 }), l->dl->n - 1) : -1;
            cell_x[ncell] = cx; cell_w[ncell] = w; ncell++;
            float pl = lenv(st->padding[3], w) * sc, pr = lenv(st->padding[1], w) * sc, pt = lenv(st->padding[0], w) * sc, pb = lenv(st->padding[2], w) * sc;
            float b = st->border[0] * sc;
            float h = flow(l, c, cx + pl + b, cy + pt + b, w - pl - pr - 2 * b) + pt + pb + 2 * b;
            if (b > 0) {
                uint32_t bc = st->bcolor[0] ? st->bcolor[0] : 0xffaaaaaa;
                push(l, (DItem){ DI_RECT, 0, cx, cy, w, b, 0, bc, .link = -1 });
                push(l, (DItem){ DI_RECT, 0, cx, cy, b, h, 0, bc, .link = -1 });
                push(l, (DItem){ DI_RECT, 0, cx + w - b, cy, b, h, 0, bc, .link = -1 });
                push(l, (DItem){ DI_RECT, 0, cx, cy + h - b, w, b, 0, bc, .link = -1 });
            }
            rh = fmaxf(rh, h);
            cx += w + sp;
            col += span;
        }
        for (int k = 0; k < ncell; k++) if (cell_bg[k] >= 0) { l->dl->it[cell_bg[k]].h = rh; (void)cell_x; (void)cell_w; }
        if (row_bg >= 0) l->dl->it[row_bg].h = rh;
        cy += rh + sp;
    }
    free(rows);
    return cy - y;
}

DList *layout_doc(Doc *d, float width, float scale) {
    DList *dl = calloc(1, sizeof *dl);
    L l = { dl, d, S, S * scale, -1, -1 };
    Node *body = d->body;
    uint32_t bg = body && body->st && body->st->bg ? body->st->bg : d->root->st && d->root->st->bg ? d->root->st->bg : 0xffffffff;
    dl->page_bg = bg;
    float mb;
    float h = 0;
    if (body && body->st) h = layout_block(&l, body, 0, 0, width, &mb) + mb;
    dl->height = h;
    return dl;
}

void dlist_free(DList *l) {
    if (!l) return;
    for (int i = 0; i < l->nlinks; i++) free(l->links[i]);
    free(l->links); free(l->it); free(l);
}
