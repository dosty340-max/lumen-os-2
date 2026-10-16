// CSS: a practical subset. Selectors (tag, .class, #id, [attr], descendant,
// child, :root/:first-child), specificity cascade with !important, @media
// evaluated at phone width, var() with fallbacks, computed styles.
#include "web.h"
#include <ctype.h>

#define VIEW_W 390.0f
typedef struct { const char *tag, *id, *attr, *attrv; const char *cls[6]; int ncls, never, first, last; } Comp;
typedef struct { Comp c[8]; char comb[8]; int n; int spec; } Sel;
typedef struct { char *prop, *val; int imp; } Decl;
typedef struct { Sel sel; Decl *d; int nd; int order; } Rule;

typedef struct Bucket { char *key; int *idx; int n, cap; struct Bucket *next; } Bucket;
#define NB 2048
struct Sheet {
    Rule *r; int n, cap;
    Arena ar;
    Bucket *b[NB];       // index by rightmost id/class/tag
    int *uni; int nuni, capuni;
    char **vk, **vv; int nv, capv;   // custom properties (--x) from :root/html/body
    int indexed;
};

static unsigned hstr(const char *s, char kind) { unsigned h = kind * 31u; while (*s) h = h * 131 + (unsigned char)*s++; return h % NB; }
static void bucket_add(Sheet *s, char kind, const char *key, int ri) {
    unsigned h = hstr(key, kind);
    Bucket *b;
    for (b = s->b[h]; b; b = b->next) if (b->key[0] == kind && !strcmp(b->key + 1, key)) break;
    if (!b) {
        b = ar_alloc(&s->ar, sizeof *b);
        b->key = ar_alloc(&s->ar, strlen(key) + 2);
        b->key[0] = kind; strcpy(b->key + 1, key);
        b->next = s->b[h]; s->b[h] = b;
    }
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 8; b->idx = realloc(b->idx, b->cap * sizeof(int)); }
    b->idx[b->n++] = ri;
}
static Bucket *bucket_get(Sheet *s, char kind, const char *key) {
    for (Bucket *b = s->b[hstr(key, kind)]; b; b = b->next) if (b->key[0] == kind && !strcmp(b->key + 1, key)) return b;
    return 0;
}

// ---- selector parsing ------------------------------------------------------------------------------
static bool ident_ch(char c) { return isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80 || c == '\\'; }
static const char *read_ident(Arena *a, const char *p, const char **out) {
    const char *s = p;
    while (ident_ch(*p)) p += *p == '\\' && p[1] ? 2 : 1;
    char *o = ar_strndup(a, s, p - s), *w = o;
    for (char *r = o; *r; r++) if (*r != '\\') *w++ = *r;   // drop escapes (e.g. .md\:flex)
    *w = 0;
    *out = o;
    return p;
}
static int parse_sel(Arena *a, const char *s, Sel *sel) {
    memset(sel, 0, sizeof *sel);
    const char *p = s;
    char comb = 0;
    while (*p) {
        while (isspace((unsigned char)*p)) { p++; if (!comb) comb = ' '; }
        if (!*p) break;
        if (*p == '>' || *p == '+' || *p == '~') {
            comb = *p++;
            while (isspace((unsigned char)*p)) p++;
        }
        if (sel->n == 8) return -1;
        Comp *c = &sel->c[sel->n];
        if (comb == '+' || comb == '~') c->never = 1;   // sibling combinators: unsupported, never match
        sel->comb[sel->n] = sel->n ? (comb == '>' ? '>' : ' ') : 0;
        comb = 0;
        int any = 0;
        while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '+' && *p != '~') {
            if (*p == '*') { p++; any = 1; }
            else if (*p == '#') { p = read_ident(a, p + 1, &c->id); sel->spec += 10000; any = 1; }
            else if (*p == '.') {
                const char *cl;
                p = read_ident(a, p + 1, &cl);
                if (c->ncls < 6) c->cls[c->ncls++] = cl;
                sel->spec += 100; any = 1;
            } else if (*p == '[') {
                const char *e = strchr(p, ']');
                if (!e) return -1;
                char buf[128];
                snprintf(buf, sizeof buf, "%.*s", (int)(e - p - 1), p + 1);
                char *eq = strchr(buf, '=');
                if (eq) {
                    if (eq > buf && strchr("~|^$*", eq[-1])) c->never = 1;
                    *eq = 0;
                    char *v = eq + 1;
                    if (*v == '"' || *v == '\'') { v++; v[strcspn(v, "\"'")] = 0; }
                    c->attrv = ar_strndup(a, v, strlen(v));
                }
                for (char *k = buf; *k; k++) *k = tolower((unsigned char)*k);
                c->attr = ar_strndup(a, buf, strlen(buf));
                sel->spec += 100; any = 1;
                p = e + 1;
            } else if (*p == ':') {
                p++;
                bool el = *p == ':';
                if (el) p++;
                const char *nm;
                p = read_ident(a, p, &nm);
                if (*p == '(') {   // skip arguments
                    int depth = 0;
                    do { if (*p == '(') depth++; else if (*p == ')') depth--; p++; } while (*p && depth);
                }
                if (!strcmp(nm, "root")) { c->tag = "html"; }
                else if (!strcmp(nm, "first-child")) c->first = 1;
                else if (!strcmp(nm, "last-child")) c->last = 1;
                else if (!strcmp(nm, "link") || !strcmp(nm, "any-link")) c->attr = "href";
                else c->never = 1;   // :hover, ::before, :not() ...
                sel->spec += 100; any = 1;
            } else if (ident_ch(*p)) {
                const char *t;
                p = read_ident(a, p, &t);
                for (char *k = (char *)t; *k; k++) *k = tolower((unsigned char)*k);
                c->tag = t; sel->spec += 1; any = 1;
            } else return -1;
        }
        if (!any) return -1;
        sel->n++;
    }
    return sel->n ? 0 : -1;
}

static bool comp_match(const Comp *c, const Node *n) {
    if (c->never || n->type != N_ELEM) return false;
    if (c->tag && strcmp(c->tag, n->tag)) return false;
    if (c->id) { const char *id = attr(n, "id"); if (!id || strcmp(id, c->id)) return false; }
    for (int i = 0; i < c->ncls; i++) if (!has_class(n, c->cls[i])) return false;
    if (c->attr) {
        const char *v = attr(n, c->attr);
        if (!v || (c->attrv && strcmp(v, c->attrv))) return false;
    }
    if (c->first) { for (Node *s = n->parent ? n->parent->first : 0; s && s != n; s = s->next) if (s->type == N_ELEM) return false; }
    if (c->last) { for (const Node *s = n->next; s; s = s->next) if (s->type == N_ELEM) return false; }
    return true;
}
static bool sel_match_from(const Sel *s, int i, const Node *n) {
    if (!comp_match(&s->c[i], n)) return false;
    if (i == 0) return true;
    if (s->comb[i] == '>') return n->parent && sel_match_from(s, i - 1, n->parent);
    for (const Node *a = n->parent; a; a = a->parent)
        if (sel_match_from(s, i - 1, a)) return true;
    return false;
}

// ---- media queries ------------------------------------------------------------------------------------
static float len_px(const char *v) {
    float x = strtof(v, 0);
    if (strstr(v, "em")) x *= 16;
    return x;
}
static bool media_ok(const char *q) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", q);
    for (char *k = buf; *k; k++) *k = tolower((unsigned char)*k);
    char *save = 0;
    for (char *part = strtok_r(buf, ",", &save); part; part = strtok_r(0, ",", &save)) {
        bool ok = true, neg = false;
        if (strstr(part, "not ")) neg = true;
        if (strstr(part, "print") || strstr(part, "speech")) ok = false;
        for (char *f = strchr(part, '('); f && ok; f = strchr(f + 1, '(')) {
            char *colon = strchr(f, ':'), *close = strchr(f, ')');
            if (!close) break;
            char feat[64] = "", val[64] = "";
            if (colon && colon < close) {
                snprintf(feat, sizeof feat, "%.*s", (int)(colon - f - 1), f + 1);
                snprintf(val, sizeof val, "%.*s", (int)(close - colon - 1), colon + 1);
            } else snprintf(feat, sizeof feat, "%.*s", (int)(close - f - 1), f + 1);
            char *fe = feat; while (*fe == ' ') fe++;
            char *ve = val; while (*ve == ' ') ve++;
            if (strstr(fe, "max-width")) ok = VIEW_W <= len_px(ve);
            else if (strstr(fe, "min-width")) ok = VIEW_W >= len_px(ve);
            else if (strstr(fe, "orientation")) ok = strstr(ve, "portrait") != 0;
            else if (strstr(fe, "prefers-color-scheme")) ok = strstr(ve, "light") != 0;
            else if (strstr(fe, "prefers-reduced-motion")) ok = strstr(ve, "no-preference") != 0;
            else if (strstr(fe, "hover") || strstr(fe, "pointer")) ok = strstr(ve, "none") || strstr(ve, "coarse");
            else if (strstr(fe, "max-height")) ok = 845 <= len_px(ve);
            else if (strstr(fe, "min-height")) ok = 845 >= len_px(ve);
            else if (strstr(fe, "resolution") || strstr(fe, "pixel-ratio")) ok = true;
            else if (strstr(fe, "width") || strstr(fe, "height")) ok = true;
            else ok = false;
        }
        if (ok != neg) return true;
    }
    return false;
}

// ---- stylesheet parsing --------------------------------------------------------------------------------
static const char *skip_ws_comments(const char *p) {
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (p[0] == '/' && p[1] == '*') { const char *e = strstr(p + 2, "*/"); p = e ? e + 2 : p + strlen(p); continue; }
        if (p[0] == '<' && p[1] == '!' && p[2] == '-' && p[3] == '-') { p += 4; continue; }
        if (p[0] == '-' && p[1] == '-' && p[2] == '>') { p += 3; continue; }
        return p;
    }
}
static const char *block_end(const char *p) {   // p after '{' -> pointer at matching '}'
    int depth = 1;
    while (*p) {
        if (*p == '"' || *p == '\'') { char q = *p++; while (*p && *p != q) { if (*p == '\\' && p[1]) p++; p++; } if (*p) p++; continue; }
        if (p[0] == '/' && p[1] == '*') { const char *e = strstr(p + 2, "*/"); p = e ? e + 2 : p + strlen(p); continue; }
        if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p;
        p++;
    }
    return p;
}

static int parse_decls(Arena *a, const char *s, size_t n, Decl **out) {
    int cap = 8, nd = 0;
    Decl *d = ar_alloc(a, cap * sizeof *d);
    size_t i = 0;
    while (i < n) {
        size_t st = i;
        int paren = 0;
        while (i < n && (s[i] != ';' || paren)) {
            if (s[i] == '(') paren++;
            else if (s[i] == ')') paren--;
            else if (s[i] == '"' || s[i] == '\'') { char q = s[i++]; while (i < n && s[i] != q) i++; }
            i++;
        }
        const char *colon = memchr(s + st, ':', i - st);
        if (colon) {
            const char *ps = s + st, *pe = colon;
            while (ps < pe && isspace((unsigned char)*ps)) ps++;
            while (pe > ps && isspace((unsigned char)pe[-1])) pe--;
            const char *vs = colon + 1, *ve = s + i;
            while (vs < ve && isspace((unsigned char)*vs)) vs++;
            while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
            int imp = 0;
            if (ve - vs >= 10 && !strncasecmp(ve - 10, "!important", 10)) { imp = 1; ve -= 10; while (ve > vs && isspace((unsigned char)ve[-1])) ve--; }
            if (pe > ps && ve >= vs) {
                if (nd == cap) { Decl *nd2 = ar_alloc(a, cap * 2 * sizeof *d); memcpy(nd2, d, nd * sizeof *d); d = nd2; cap *= 2; }
                char *prop = ar_strndup(a, ps, pe - ps);
                if (prop[0] != '-' || prop[1] != '-') for (char *k = prop; *k; k++) *k = tolower((unsigned char)*k);
                d[nd++] = (Decl){ prop, ar_strndup(a, vs, ve - vs), imp };
            }
        }
        i++;
    }
    *out = d;
    return nd;
}

static void add_var(Sheet *s, const char *k, const char *v) {
    for (int i = 0; i < s->nv; i++) if (!strcmp(s->vk[i], k)) { s->vv[i] = (char *)v; return; }
    if (s->nv == s->capv) { s->capv = s->capv ? s->capv * 2 : 64; s->vk = realloc(s->vk, s->capv * sizeof(char *)); s->vv = realloc(s->vv, s->capv * sizeof(char *)); }
    s->vk[s->nv] = (char *)k; s->vv[s->nv] = (char *)v; s->nv++;
}

static void parse_rules(Sheet *s, const char *p, const char *end) {
    while (p < end) {
        p = skip_ws_comments(p);
        if (p >= end || !*p) break;
        if (*p == '}') { p++; continue; }
        if (*p == '@') {
            const char *brace = p, *semi = p;
            while (brace < end && *brace != '{' && *brace != ';') brace++;
            semi = brace;
            if (semi >= end || *semi == ';') { p = semi + 1; continue; }   // @import/@charset ...
            const char *be = block_end(brace + 1);
            char pre[512];
            snprintf(pre, sizeof pre, "%.*s", (int)mini(brace - p, 511), p);
            if (!strncasecmp(pre, "@media", 6)) { if (media_ok(pre + 6)) parse_rules(s, brace + 1, be); }
            else if (!strncasecmp(pre, "@supports", 9) || !strncasecmp(pre, "@layer", 6) || !strncasecmp(pre, "@container", 10)) parse_rules(s, brace + 1, be);
            p = *be ? be + 1 : be;
            continue;
        }
        const char *brace = p;
        while (brace < end && *brace != '{') brace++;
        if (brace >= end) break;
        const char *be = block_end(brace + 1);
        Decl *d;
        int nd = parse_decls(&s->ar, brace + 1, be - brace - 1, &d);
        // selector list
        char *sl = ar_strndup(&s->ar, p, brace - p);
        char *save = 0;
        for (char *one = strtok_r(sl, ",", &save); one; one = strtok_r(0, ",", &save)) {
            Sel sel;
            if (parse_sel(&s->ar, one, &sel)) continue;
            Comp *last = &sel.c[sel.n - 1];
            if (sel.n == 1 && last->tag && (!strcmp(last->tag, "html") || !strcmp(last->tag, "body")) && !last->ncls && !last->id)
                for (int k = 0; k < nd; k++) if (d[k].prop[0] == '-' && d[k].prop[1] == '-') add_var(s, d[k].prop, d[k].val);
            if (last->never) continue;
            if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->r = realloc(s->r, s->cap * sizeof *s->r); }
            s->r[s->n] = (Rule){ sel, d, nd, s->n };
            s->n++;
        }
        p = *be ? be + 1 : be;
    }
}

Sheet *css_parse(const char *src, Sheet *into) {
    Sheet *s = into ? into : calloc(1, sizeof *s);
    char *copy = ar_strndup(&s->ar, src, strlen(src));
    parse_rules(s, copy, copy + strlen(copy));
    s->indexed = 0;
    return s;
}
void css_free(Sheet *s) {
    if (!s) return;
    for (int i = 0; i < NB; i++) for (Bucket *b = s->b[i]; b; b = b->next) free(b->idx);
    free(s->r); free(s->uni); free(s->vk); free(s->vv);
    ar_free(&s->ar);
    free(s);
}
static void build_index(Sheet *s) {
    for (int i = 0; i < NB; i++) for (Bucket *b = s->b[i]; b; b = b->next) b->n = 0;
    s->nuni = 0;
    for (int i = 0; i < s->n; i++) {
        Comp *c = &s->r[i].sel.c[s->r[i].sel.n - 1];
        if (c->id) bucket_add(s, '#', c->id, i);
        else if (c->ncls) bucket_add(s, '.', c->cls[0], i);
        else if (c->tag) bucket_add(s, 't', c->tag, i);
        else {
            if (s->nuni == s->capuni) { s->capuni = s->capuni ? s->capuni * 2 : 64; s->uni = realloc(s->uni, s->capuni * sizeof(int)); }
            s->uni[s->nuni++] = i;
        }
    }
    s->indexed = 1;
}

// ---- values ---------------------------------------------------------------------------------------------
static const struct { const char *n; uint32_t c; } NAMED[] = {
    { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x008000 }, { "blue", 0x0000ff },
    { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xc0c0c0 }, { "navy", 0x000080 }, { "maroon", 0x800000 },
    { "purple", 0x800080 }, { "teal", 0x008080 }, { "olive", 0x808000 }, { "orange", 0xffa500 }, { "yellow", 0xffff00 },
    { "lime", 0x00ff00 }, { "aqua", 0x00ffff }, { "fuchsia", 0xff00ff }, { "darkgray", 0xa9a9a9 }, { "darkgrey", 0xa9a9a9 },
    { "lightgray", 0xd3d3d3 }, { "lightgrey", 0xd3d3d3 }, { "whitesmoke", 0xf5f5f5 }, { "gainsboro", 0xdcdcdc },
    { "dimgray", 0x696969 }, { "darkblue", 0x00008b }, { "darkred", 0x8b0000 }, { "darkgreen", 0x006400 },
    { "lightblue", 0xadd8e6 }, { "beige", 0xf5f5dc }, { "ivory", 0xfffff0 }, { "linen", 0xfaf0e6 }, { "brown", 0xa52a2a },
    { "pink", 0xffc0cb }, { "gold", 0xffd700 }, { "crimson", 0xdc143c }, { "steelblue", 0x4682b4 }, { "slategray", 0x708090 },
    { "royalblue", 0x4169e1 }, { "firebrick", 0xb22222 }, { "darkorange", 0xff8c00 }, { "tomato", 0xff6347 },
    { "lightyellow", 0xffffe0 }, { "aliceblue", 0xf0f8ff }, { "ghostwhite", 0xf8f8ff }, { "snow", 0xfffafa },
};
uint32_t css_color(const char *v, bool *ok) {
    *ok = true;
    while (*v == ' ') v++;
    if (!strncasecmp(v, "light-dark(", 11)) v += 11;   // phone is in light colour scheme for web content
    if (*v == '#') {
        unsigned x = 0;
        int n = 0;
        for (const char *p = v + 1; isxdigit((unsigned char)*p) && n < 8; p++, n++) x = x * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10));
        if (n == 3 || n == 4) {
            if (n == 4) { if ((x & 15) == 0) return 0; x >>= 4; }
            unsigned r = (x >> 8) & 15, g = (x >> 4) & 15, b = x & 15;
            return 0xff000000 | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
        }
        if (n == 6) return 0xff000000 | x;
        if (n == 8) return ((x & 255) < 40 ? 0 : 0xff000000) | (x >> 8);
        *ok = false;
        return 0;
    }
    if (!strncasecmp(v, "rgb", 3)) {
        const char *p = strchr(v, '(');
        if (!p) { *ok = false; return 0; }
        float c[4] = { 0, 0, 0, 1 };
        int n = 0;
        p++;
        while (*p && *p != ')' && n < 4) {
            while (*p == ' ' || *p == ',' || *p == '/') p++;
            if (!*p || *p == ')') break;
            char *e;
            c[n] = strtof(p, &e);
            if (e == p) break;
            if (*e == '%') { c[n] = n < 3 ? c[n] * 2.55f : c[n] / 100; e++; }
            n++;
            p = e;
        }
        if (c[3] < 0.15f) return 0;   // mostly transparent: treat as none
        return 0xff000000 | (uint32_t)clampf(c[0], 0, 255) << 16 | (uint32_t)clampf(c[1], 0, 255) << 8 | (uint32_t)clampf(c[2], 0, 255);
    }
    if (!strncasecmp(v, "transparent", 11)) return 0;
    for (size_t i = 0; i < sizeof NAMED / sizeof NAMED[0]; i++)
        if (!strncasecmp(v, NAMED[i].n, strlen(NAMED[i].n)) && !isalnum((unsigned char)v[strlen(NAMED[i].n)])) return 0xff000000 | NAMED[i].c;
    *ok = false;
    return 0;
}

// resolves var(--x, fallback) using the sheet's custom properties
static void subst_vars(Sheet *s, const char *v, char *out, int cap, int depth) {
    int o = 0;
    while (*v && o < cap - 1) {
        if (!strncmp(v, "var(", 4)) {
            const char *p = v + 4;
            while (*p == ' ') p++;
            int depth2 = 1;
            const char *e = p;
            while (*e && depth2) { if (*e == '(') depth2++; else if (*e == ')') depth2--; if (depth2) e++; }
            const char *comma = memchr(p, ',', e - p);
            char name[96];
            snprintf(name, sizeof name, "%.*s", (int)((comma ? comma : e) - p), p);
            for (char *k = name + strlen(name); k > name && k[-1] == ' '; ) *--k = 0;
            const char *val = 0;
            for (int i = 0; i < s->nv; i++) if (!strcmp(s->vk[i], name)) { val = s->vv[i]; break; }
            char fb[256] = "";
            if (!val && comma) { snprintf(fb, sizeof fb, "%.*s", (int)(e - comma - 1), comma + 1); val = fb; }
            if (val && depth < 4) {
                char tmp[256];
                subst_vars(s, val, tmp, sizeof tmp, depth + 1);
                o += snprintf(out + o, cap - o, "%s", tmp);
                if (o >= cap) o = cap - 1;
            }
            v = *e ? e + 1 : e;
            continue;
        }
        out[o++] = *v++;
    }
    out[o] = 0;
}

static Len parse_len(const char *v, float fs) {
    while (*v == ' ') v++;
    if (!strncmp(v, "auto", 4)) return (Len){ 0, 4 };
    if (!strncmp(v, "none", 4)) return (Len){ 0, 0 };
    if (!strncmp(v, "calc(", 5)) {   // take the first percentage/length term
        v += 5;
        while (*v == ' ' || *v == '(') v++;
    }
    char *e;
    float x = strtof(v, &e);
    if (e == v) return (Len){ 0, 0 };
    if (*e == '%') return (Len){ x, 2 };
    if (!strncmp(e, "em", 2)) return (Len){ x * fs, 1 };
    if (!strncmp(e, "rem", 3)) return (Len){ x * 16, 1 };
    if (!strncmp(e, "pt", 2)) return (Len){ x * 1.333f, 1 };
    if (!strncmp(e, "vw", 2)) return (Len){ x * VIEW_W / 100, 1 };
    if (!strncmp(e, "vh", 2)) return (Len){ x * 8.45f, 1 };
    if (!strncmp(e, "ex", 2) || !strncmp(e, "ch", 2)) return (Len){ x * fs * 0.5f, 1 };
    return (Len){ x, 1 };
}
static int split_vals(char *v, char **parts, int max) {
    int n = 0;
    char *p = v;
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        parts[n++] = p;
        int paren = 0;
        while (*p && (*p != ' ' || paren)) { if (*p == '(') paren++; else if (*p == ')') paren--; p++; }
        if (*p) *p++ = 0;
    }
    return n;
}
static void box4(Len *dst, char *v, float fs) {
    char *pp[4];
    int n = split_vals(v, pp, 4);
    if (!n) return;
    Len l[4];
    for (int i = 0; i < n; i++) l[i] = parse_len(pp[i], fs);
    if (n == 1) l[1] = l[2] = l[3] = l[0];
    else if (n == 2) { l[2] = l[0]; l[3] = l[1]; }
    else if (n == 3) l[3] = l[1];
    for (int i = 0; i < 4; i++) dst[i] = l[i];
}
static void border_side(Style *st, int side, char *v) {
    char *pp[6];
    int n = split_vals(v, pp, 6);
    bool none = false;
    for (int i = 0; i < n; i++) {
        bool ok;
        uint32_t c = css_color(pp[i], &ok);
        if (ok) { st->bcolor[side] = c; continue; }
        if (!strcmp(pp[i], "none") || !strcmp(pp[i], "hidden")) none = true;
        else if (isdigit((unsigned char)pp[i][0]) || pp[i][0] == '.') st->border[side] = parse_len(pp[i], st->fs).v;
        else if (!strcmp(pp[i], "thin")) st->border[side] = 1;
        else if (!strcmp(pp[i], "medium")) st->border[side] = 3;
        else if (!strcmp(pp[i], "thick")) st->border[side] = 5;
        else if (strcmp(pp[i], "solid") && strcmp(pp[i], "dotted") && strcmp(pp[i], "dashed") && strcmp(pp[i], "double")) {}
        else if (!st->border[side]) st->border[side] = 3;
    }
    if (none) st->border[side] = 0;
    if (!st->bcolor[side]) st->bcolor[side] = st->color;
}

static float font_size_val(const char *v, float parent) {
    static const struct { const char *k; float s; } KW[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 }, { "x-large", 24 }, { "xx-large", 32 },
    };
    for (size_t i = 0; i < sizeof KW / sizeof KW[0]; i++) if (!strcmp(v, KW[i].k)) return KW[i].s;
    if (!strcmp(v, "smaller")) return parent * 0.83f;
    if (!strcmp(v, "larger")) return parent * 1.2f;
    Len l = parse_len(v, parent);
    if (l.unit == 2) return parent * l.v / 100;
    if (l.unit == 1 && l.v > 0) return l.v;
    return parent;
}

static void apply(Sheet *sh, Style *st, const Style *par, const char *prop, const char *val0) {
    char val[512];
    if (strstr(val0, "var(")) subst_vars(sh, val0, val, sizeof val, 0);
    else snprintf(val, sizeof val, "%s", val0);
    for (char *k = val; *k; k++) if (*k == '\n' || *k == '\t') *k = ' ';
    bool inherit = !strcmp(val, "inherit");
    bool ok;
    const char *p = prop;
    if (!strcmp(p, "display")) {
        static const struct { const char *k; int d; } D[] = {
            { "none", D_NONE }, { "inline-block", D_INLINE_BLOCK }, { "inline-flex", D_INLINE_BLOCK }, { "inline-grid", D_INLINE_BLOCK },
            { "inline-table", D_INLINE_BLOCK }, { "inline", D_INLINE }, { "block", D_BLOCK }, { "flex", D_BLOCK }, { "grid", D_BLOCK },
            { "list-item", D_LIST_ITEM }, { "table-row-group", D_ROWGROUP }, { "table-header-group", D_ROWGROUP },
            { "table-footer-group", D_ROWGROUP }, { "table-row", D_ROW }, { "table-cell", D_CELL }, { "table-caption", D_BLOCK },
            { "table", D_TABLE }, { "contents", D_INLINE }, { "flow-root", D_BLOCK },
        };
        for (size_t i = 0; i < sizeof D / sizeof D[0]; i++) if (!strncmp(val, D[i].k, strlen(D[i].k))) { st->display = D[i].d; break; }
    } else if (!strcmp(p, "color")) { uint32_t c = css_color(val, &ok); if (ok && c) st->color = c; else if (inherit) st->color = par->color; }
    else if (!strcmp(p, "background-color")) { uint32_t c = css_color(val, &ok); if (ok) st->bg = c; }
    else if (!strcmp(p, "background")) {
        char *pp[8];
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", val);
        int n = split_vals(tmp, pp, 8);
        for (int i = 0; i < n; i++) { uint32_t c = css_color(pp[i], &ok); if (ok) { st->bg = c; break; } }
        if (!strcmp(val, "none")) st->bg = 0;
    } else if (!strcmp(p, "font-size")) st->fs = font_size_val(val, par->fs);
    else if (!strcmp(p, "font-weight")) st->bold = !strcmp(val, "bold") || !strcmp(val, "bolder") || atoi(val) >= 600;
    else if (!strcmp(p, "font-style")) st->italic = !strcmp(val, "italic") || !strcmp(val, "oblique");
    else if (!strcmp(p, "font")) {
        char tmp[512], *pp[10];
        snprintf(tmp, sizeof tmp, "%s", val);
        int n = split_vals(tmp, pp, 10);
        for (int i = 0; i < n; i++) {
            if (!strcmp(pp[i], "bold") || atoi(pp[i]) >= 600) st->bold = 1;
            else if (!strcmp(pp[i], "normal")) {}
            else if (isdigit((unsigned char)pp[i][0]) || pp[i][0] == '.') {
                char *slash = strchr(pp[i], '/');
                if (slash) { *slash = 0; float l = strtof(slash + 1, 0); st->lh = strchr(slash + 1, 'p') ? l / font_size_val(pp[i], par->fs) : l; }
                if (atoi(pp[i]) < 100 || strchr(pp[i], 'p') || strchr(pp[i], 'e') || strchr(pp[i], '%')) st->fs = font_size_val(pp[i], par->fs);
            }
        }
    } else if (!strcmp(p, "text-align")) {
        st->align = strstr(val, "center") ? 1 : (strstr(val, "right") || !strcmp(val, "end")) ? 2 : 0;
    } else if (!strcmp(p, "margin")) box4(st->margin, val, st->fs);
    else if (!strcmp(p, "padding")) box4(st->padding, val, st->fs);
    else if (!strncmp(p, "margin-", 7) || !strncmp(p, "padding-", 8)) {
        Len *b = p[0] == 'm' ? st->margin : st->padding;
        const char *side = strchr(p, '-') + 1;
        int i = !strcmp(side, "top") || !strcmp(side, "block-start") ? 0 : !strcmp(side, "right") || !strcmp(side, "inline-end") ? 1
              : !strcmp(side, "bottom") || !strcmp(side, "block-end") ? 2 : !strcmp(side, "left") || !strcmp(side, "inline-start") ? 3 : -1;
        if (i >= 0) b[i] = parse_len(val, st->fs);
    } else if (!strcmp(p, "border")) { for (int i = 0; i < 4; i++) { char t[512]; snprintf(t, sizeof t, "%s", val); border_side(st, i, t); } }
    else if (!strcmp(p, "border-top") || !strcmp(p, "border-right") || !strcmp(p, "border-bottom") || !strcmp(p, "border-left")) {
        int i = p[7] == 't' ? 0 : p[7] == 'r' ? 1 : p[7] == 'b' ? 2 : 3;
        char t[512]; snprintf(t, sizeof t, "%s", val);
        border_side(st, i, t);
    } else if (!strcmp(p, "border-width")) { Len l[4]; memset(l, 0, sizeof l); char t[512]; snprintf(t, sizeof t, "%s", val); box4(l, t, st->fs); for (int i = 0; i < 4; i++) st->border[i] = l[i].v; }
    else if (!strcmp(p, "border-color")) { uint32_t c = css_color(val, &ok); if (ok) for (int i = 0; i < 4; i++) st->bcolor[i] = c; }
    else if (!strcmp(p, "border-style") && (!strcmp(val, "none") || !strcmp(val, "hidden"))) { for (int i = 0; i < 4; i++) st->border[i] = 0; }
    else if (!strcmp(p, "width")) st->width = parse_len(val, st->fs);
    else if (!strcmp(p, "max-width")) st->maxw = parse_len(val, st->fs);
    else if (!strcmp(p, "height")) st->height = parse_len(val, st->fs);
    else if (!strcmp(p, "white-space")) st->ws = !strncmp(val, "pre", 3) ? 1 : !strcmp(val, "nowrap") ? 2 : 0;
    else if (!strcmp(p, "text-decoration") || !strcmp(p, "text-decoration-line")) st->underline = strstr(val, "underline") != 0;
    else if (!strcmp(p, "list-style-type") || !strcmp(p, "list-style")) {
        st->list = strstr(val, "none") ? 0 : (strstr(val, "decimal") || strstr(val, "alpha") || strstr(val, "roman")) ? 2 : 1;
    } else if (!strcmp(p, "line-height")) {
        if (!strcmp(val, "normal")) st->lh = 1.3f;
        else { Len l = parse_len(val, st->fs); st->lh = l.unit == 2 ? l.v / 100 : (strstr(val, "px") || strstr(val, "em")) ? l.v / st->fs : strtof(val, 0); }
        if (st->lh < 0.8f || st->lh > 3) st->lh = 1.35f;
    } else if (!strcmp(p, "visibility")) st->hidden = !strcmp(val, "hidden") || !strcmp(val, "collapse");
    else if (!strcmp(p, "float")) st->floated = strcmp(val, "none") != 0;
    else if (!strcmp(p, "position")) st->pos_abs = !strcmp(val, "absolute") || !strcmp(val, "fixed");
    else if (!strcmp(p, "left")) st->left = parse_len(val, st->fs);
    else if (!strcmp(p, "top")) st->top = parse_len(val, st->fs);
    else if ((!strcmp(p, "clip") && strstr(val, "rect")) || (!strcmp(p, "clip-path") && strstr(val, "inset(50%"))) st->hidden = 2;
}

const char *UA_CSS =
    "html,body,div,p,ul,ol,dl,dt,dd,h1,h2,h3,h4,h5,h6,form,header,footer,nav,section,article,aside,main,blockquote,pre,"
    "figure,figcaption,address,fieldset,hr,center,details,summary,legend,caption,hgroup,menu,dialog{display:block}"
    "head,script,style,title,meta,link,template,svg,math,iframe,video,audio,canvas,object,embed,map,datalist,noembed,"
    "input[type=hidden],dialog,area,param,source,track{display:none}"
    "li{display:list-item}table{display:table;text-align:left}tr{display:table-row}td,th{display:table-cell;padding:1px}"
    "thead,tbody,tfoot{display:table-row-group}img,input,button,select,textarea{display:inline-block}"
    "body{margin:8px;color:#111;line-height:1.35}p{margin:1em 0}"
    "h1{font-size:2em;margin:.67em 0;font-weight:bold}h2{font-size:1.5em;margin:.75em 0;font-weight:bold}"
    "h3{font-size:1.17em;margin:.83em 0;font-weight:bold}h4{margin:1.1em 0;font-weight:bold}"
    "h5{font-size:.83em;font-weight:bold}h6{font-size:.75em;font-weight:bold}"
    "ul,ol,menu{margin:1em 0;padding-left:30px}ul{list-style-type:disc}ol{list-style-type:decimal}"
    "li ul,li ol{margin:0}dd{margin-left:30px}blockquote{margin:1em 20px}figure{margin:1em 0}"
    "a{color:#0b57d0;text-decoration:underline}b,strong,th,dt{font-weight:bold}i,em,cite,var,dfn{font-style:italic}"
    "pre{white-space:pre;margin:1em 0}small,sub,sup{font-size:smaller}big{font-size:larger}"
    "th{text-align:center}center{text-align:center}hr{border-top:1px solid #aaa;margin:.5em 0}"
    "code,kbd,samp,tt{color:#333}mark{background:#ff0}u,ins{text-decoration:underline}"
    "button,input[type=submit]{background:#e6e6e6;border:1px solid #888;padding:3px 8px}";

typedef struct { int ri, spec, order, imp; } Match;
static int match_cmp(const void *a, const void *b) {
    const Match *x = a, *y = b;
    if (x->imp != y->imp) return x->imp - y->imp;
    if (x->spec != y->spec) return x->spec - y->spec;
    return x->order - y->order;
}

static void presentational(Node *n, Style *st) {
    const char *v;
    bool ok;
    if ((v = attr(n, "bgcolor"))) { uint32_t c = css_color(v, &ok); if (ok) st->bg = c; }
    if (!strcmp(n->tag, "font") && (v = attr(n, "color"))) { uint32_t c = css_color(v, &ok); if (ok) st->color = c; }
    if (!strcmp(n->tag, "font") && (v = attr(n, "size"))) { int s = atoi(v); if (v[0] == '+' || v[0] == '-') s += 3; static const float fz[] = { 10, 10, 13, 16, 18, 24, 32, 48 }; st->fs = fz[s < 1 ? 1 : s > 7 ? 7 : s]; }
    if ((v = attr(n, "align"))) {
        if (!strcmp(n->tag, "table") || !strcmp(n->tag, "img")) {   // align on a table positions the box, not its text
            if (!strcasecmp(v, "center")) st->margin[1] = st->margin[3] = (Len){ 0, 4 };
        } else st->align = !strcasecmp(v, "center") ? 1 : !strcasecmp(v, "right") ? 2 : 0;
    }
    if ((v = attr(n, "width"))) st->width = parse_len(v, st->fs);
    if ((v = attr(n, "height")) && !strcmp(n->tag, "img")) st->height = parse_len(v, st->fs);
    if (!strcmp(n->tag, "table") && (v = attr(n, "border")) && atoi(v) > 0) for (int i = 0; i < 4; i++) { st->border[i] = 1; st->bcolor[i] = 0xff888888; }
    if ((!strcmp(n->tag, "td") || !strcmp(n->tag, "th")) && n->parent) {
        for (Node *t = n->parent; t; t = t->parent)
            if (!strcmp(t->tag, "table")) {
                if ((v = attr(t, "cellpadding"))) for (int i = 0; i < 4; i++) st->padding[i] = (Len){ atoi(v), 1 };
                if ((v = attr(t, "border")) && atoi(v) > 0) for (int i = 0; i < 4; i++) { st->border[i] = 1; st->bcolor[i] = 0xffaaaaaa; }
                break;
            }
    }
    if (!strcmp(n->tag, "ol") && (v = attr(n, "type"))) st->list = 2;
    if (attr(n, "hidden")) st->display = D_NONE;
}

static void style_node(Sheet *s, Node *n, const Style *par, Arena *a, Match *mb, int mcap) {
    if (n->type != N_ELEM) return;
    Style *st = ar_alloc(a, sizeof *st);
    // inherited properties
    st->color = par->color; st->fs = par->fs; st->bold = par->bold; st->italic = par->italic; st->align = par->align;
    st->ws = par->ws; st->list = par->list; st->lh = par->lh; st->hidden = par->hidden == 1 ? 1 : 0; st->underline = par->underline;
    st->display = D_INLINE;
    presentational(n, st);
    // collect matching rules via the index
    int nm = 0;
    Bucket *cand[16];
    int nc = 0;
    const char *id = attr(n, "id");
    if (id && (cand[nc] = bucket_get(s, '#', id))) nc++;
    const char *cls = attr(n, "class");
    if (cls) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", cls);
        char *save = 0;
        for (char *c = strtok_r(buf, " \t\n", &save); c && nc < 14; c = strtok_r(0, " \t\n", &save)) {
            Bucket *b = bucket_get(s, '.', c);
            bool dup = false;
            for (int k = 0; k < nc; k++) if (cand[k] == b) dup = true;
            if (b && !dup) cand[nc++] = b;
        }
    }
    if ((cand[nc] = bucket_get(s, 't', n->tag))) nc++;
    for (int k = 0; k <= nc; k++) {
        int cnt = k < nc ? cand[k]->n : s->nuni;
        int *idx = k < nc ? cand[k]->idx : s->uni;
        for (int j = 0; j < cnt; j++) {
            Rule *r = &s->r[idx[j]];
            if (!sel_match_from(&r->sel, r->sel.n - 1, n)) continue;
            if (nm + 2 > mcap) break;
            mb[nm++] = (Match){ idx[j], r->sel.spec, r->order, 0 };
            bool imp = false;
            for (int d = 0; d < r->nd; d++) if (r->d[d].imp) imp = true;
            if (imp) mb[nm++] = (Match){ idx[j], r->sel.spec, r->order, 1 };
        }
    }
    qsort(mb, nm, sizeof *mb, match_cmp);
    const char *inl = attr(n, "style");
    Decl *id_ = 0;
    int nid = inl ? parse_decls(a, inl, strlen(inl), &id_) : 0;
    bool inline_done = false;
    // font-size first so em units in the same rule resolve against it
    for (int pass = 0; pass < 2; pass++) {
        inline_done = false;
        for (int m = 0; m < nm; m++) {
            if (mb[m].imp && !inline_done) {
                for (int d = 0; d < nid; d++) if (!id_[d].imp && ((pass == 0) == !strcmp(id_[d].prop, "font-size"))) apply(s, st, par, id_[d].prop, id_[d].val);
                inline_done = true;
            }
            Rule *r = &s->r[mb[m].ri];
            for (int d = 0; d < r->nd; d++)
                if (r->d[d].imp == mb[m].imp && ((pass == 0) == !strcmp(r->d[d].prop, "font-size"))) apply(s, st, par, r->d[d].prop, r->d[d].val);
        }
        if (!inline_done) for (int d = 0; d < nid; d++) if (!id_[d].imp && ((pass == 0) == !strcmp(id_[d].prop, "font-size"))) apply(s, st, par, id_[d].prop, id_[d].val);
        for (int d = 0; d < nid; d++) if (id_[d].imp && ((pass == 0) == !strcmp(id_[d].prop, "font-size"))) apply(s, st, par, id_[d].prop, id_[d].val);
    }
    // out-of-flow boxes are laid out in flow, except off-screen / screen-reader-only ones
    if (st->pos_abs && !st->hidden && ((st->left.unit == 1 && st->left.v < -300) || (st->top.unit == 1 && st->top.v < -300) ||
                                       (st->width.unit == 1 && st->width.v <= 2) || (st->height.unit == 1 && st->height.v <= 2)))
        st->hidden = 3;
    n->st = st;
    for (Node *c = n->first; c; c = c->next) style_node(s, c, st, a, mb, mcap);
}

void css_style_doc(Doc *d, Sheet *s, float base_px) {
    if (!s->indexed) build_index(s);
    Style root = { .color = 0xff111111, .fs = base_px, .lh = 1.35f, .list = 1 };
    Match *mb = malloc(4096 * sizeof *mb);
    style_node(s, d->root, &root, &d->ar, mb, 4096);
    free(mb);
}
