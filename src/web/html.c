// HTML: tolerant tokenizer + tree builder (implied html/head/body, auto-closing
// p/li/dt/dd/tr/td/option, void elements, raw text, entities, UTF-8/latin-1).
#include "web.h"
#include <ctype.h>

// ---- arena -------------------------------------------------------------------------------------
typedef struct ArenaBlk { struct ArenaBlk *next; size_t used, cap; char data[]; } ArenaBlk;
void *ar_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->blk || a->blk->used + n > a->blk->cap) {
        size_t cap = n > 65536 ? n : 65536;
        ArenaBlk *b = malloc(sizeof *b + cap);
        b->next = a->blk; b->used = 0; b->cap = cap;
        a->blk = b;
    }
    void *p = a->blk->data + a->blk->used;
    a->blk->used += n;
    memset(p, 0, n);
    return p;
}
char *ar_strndup(Arena *a, const char *s, size_t n) {
    char *p = ar_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
void ar_free(Arena *a) {
    for (ArenaBlk *b = a->blk, *nx; b; b = nx) { nx = b->next; free(b); }
    a->blk = 0;
}

// ---- entities ------------------------------------------------------------------------------------
static const struct { const char *n; int cp; } ENT[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
    { "hellip", 0x2026 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "laquo", 0xAB }, { "raquo", 0xBB }, { "middot", 0xB7 }, { "bull", 0x2022 }, { "deg", 0xB0 },
    { "times", 0xD7 }, { "divide", 0xF7 }, { "euro", 0x20AC }, { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 },
    { "sect", 0xA7 }, { "para", 0xB6 }, { "plusmn", 0xB1 }, { "frac12", 0xBD }, { "frac14", 0xBC }, { "sup2", 0xB2 },
    { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "aacute", 0xE1 }, { "agrave", 0xE0 }, { "iacute", 0xED }, { "oacute", 0xF3 },
    { "uacute", 0xFA }, { "ntilde", 0xF1 }, { "ouml", 0xF6 }, { "uuml", 0xFC }, { "auml", 0xE4 }, { "szlig", 0xDF },
    { "ccedil", 0xE7 }, { "Eacute", 0xC9 }, { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 },
    { "thinsp", 0x2009 }, { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "zwj", 0x200D }, { "zwnj", 0x200C }, { "shy", 0xAD },
    { "lrm", 0x200E }, { "rlm", 0x200F }, { "prime", 0x2032 }, { "minus", 0x2212 }, { "hearts", 0x2665 }, { "check", 0x2713 },
};

// decode entities in s[0..n) into the arena
static char *decode(Arena *a, const char *s, size_t n) {
    char *o = ar_alloc(a, n * 2 + 8), *p = o;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { *p++ = s[i]; continue; }
        size_t j = i + 1;
        int cp = -1;
        if (j < n && s[j] == '#') {
            j++;
            bool hex = j < n && (s[j] == 'x' || s[j] == 'X');
            if (hex) j++;
            long v = 0;
            size_t st = j;
            while (j < n && (hex ? isxdigit((unsigned char)s[j]) : isdigit((unsigned char)s[j]))) {
                v = v * (hex ? 16 : 10) + (isdigit((unsigned char)s[j]) ? s[j] - '0' : (tolower(s[j]) - 'a' + 10));
                if (v > 0x10FFFF) v = 0xFFFD;
                j++;
            }
            if (j > st) cp = v ? (int)v : 0xFFFD;
        } else {
            size_t st = j;
            while (j < n && isalnum((unsigned char)s[j]) && j - st < 10) j++;
            for (size_t k = 0; k < sizeof ENT / sizeof ENT[0]; k++)
                if (strlen(ENT[k].n) == j - st && !memcmp(ENT[k].n, s + st, j - st)) { cp = ENT[k].cp; break; }
        }
        if (cp < 0) { *p++ = '&'; continue; }
        if (j < n && s[j] == ';') j++;
        if (cp == 0xAD || cp == 0x200B || cp == 0x200E || cp == 0x200F || cp == 0x200D || cp == 0x200C) { i = j - 1; continue; }
        p += utf8_put(p, cp);
        i = j - 1;
    }
    *p = 0;
    return o;
}

// ---- DOM helpers ------------------------------------------------------------------------------------
const char *attr(const Node *n, const char *name) {
    for (int i = 0; i < n->nattr; i++) if (!strcmp(n->attr[i * 2], name)) return n->attr[i * 2 + 1];
    return 0;
}
bool has_class(const Node *n, const char *cls) {
    const char *c = attr(n, "class");
    if (!c) return false;
    size_t l = strlen(cls);
    for (const char *p = c; (p = strstr(p, cls)); p += l)
        if ((p == c || isspace((unsigned char)p[-1])) && (!p[l] || isspace((unsigned char)p[l]))) return true;
    return false;
}
static void append(Node *parent, Node *n) {
    n->parent = parent;
    if (parent->last) parent->last->next = n; else parent->first = n;
    parent->last = n;
}
static void tc(const Node *n, char *out, int cap, int *o) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type == N_TEXT) {
            for (int i = 0; i < c->tlen && *o < cap - 1; i++) {
                char ch = c->text[i];
                if (isspace((unsigned char)ch)) { if (*o && out[*o - 1] != ' ') out[(*o)++] = ' '; }
                else out[(*o)++] = ch;
            }
        } else tc(c, out, cap, o);
    }
}
void text_content(const Node *n, char *out, int cap) {
    int o = 0;
    tc(n, out, cap, &o);
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

// ---- tree builder -----------------------------------------------------------------------------------
static const char *VOID[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr", "keygen", 0 };
static const char *RAW[] = { "script", "style", "textarea", "title", "xmp", "noembed", "iframe", 0 };
static const char *HEADONLY[] = { "title", "meta", "link", "style", "base", "script", 0 };
static const char *CLOSES_P[] = { "address", "article", "aside", "blockquote", "div", "dl", "fieldset", "footer", "form",
    "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "menu", "nav", "ol", "p", "pre", "section", "table", "ul",
    "figure", "main", "details", "center", "dd", "dt", "li", "figcaption", "hgroup", "summary", 0 };
static bool in(const char *t, const char **set) { for (; *set; set++) if (!strcmp(t, *set)) return true; return false; }

typedef struct { Doc *d; Node *stack[256]; int sp; } TB;
static Node *cur(TB *t) { return t->stack[t->sp - 1]; }
static void pop_to(TB *t, int i) { if (i >= 1) t->sp = i; }   // keep stack[0..i-1]
// index of innermost open element named tag, not crossing any boundary tag
static int find_open(TB *t, const char *tag, const char **boundary) {
    for (int i = t->sp - 1; i >= 1; i--) {
        if (!strcmp(t->stack[i]->tag, tag)) return i;
        if (boundary && in(t->stack[i]->tag, boundary)) return -1;
    }
    return -1;
}
static void ensure_body(TB *t) {
    if (t->d->body) {
        // if we are still in head, move to body
        if (t->sp >= 2 && t->stack[1] == t->d->head) { t->sp = 1; t->stack[t->sp++] = t->d->body; }
        return;
    }
    Node *b = ar_alloc(&t->d->ar, sizeof *b);
    b->type = N_ELEM; b->tag = "body";
    append(t->d->root, b);
    t->d->body = b;
    t->sp = 1;
    t->stack[t->sp++] = b;
}

static void open_elem(TB *t, Node *n, bool selfclose) {
    const char *tag = n->tag;
    static const char *TBL[] = { "table", 0 }, *LISTB[] = { "ul", "ol", "table", 0 }, *DLB[] = { "dl", "table", 0 },
                      *ROWB[] = { "table", 0 }, *CELLB[] = { "tr", "table", 0 }, *SELB[] = { "select", "datalist", 0 },
                      *PB[] = { "table", "td", "th", "button", "li", "dd", "dt", 0 };
    if (!strcmp(tag, "html")) {   // merge attributes into root
        if (!t->d->root->nattr) { t->d->root->attr = n->attr; t->d->root->nattr = n->nattr; }
        return;
    }
    if (!strcmp(tag, "head")) return;
    if (!strcmp(tag, "body")) {
        if (!t->d->body) { ensure_body(t); t->d->body->attr = n->attr; t->d->body->nattr = n->nattr; }
        return;
    }
    if (!strcmp(tag, "noscript") && !t->d->body) return;   // its <link>/<style> children land in head
    if (in(tag, HEADONLY) && !t->d->body) {   // stays in head
        append(t->d->head, n);
        return;
    }
    ensure_body(t);
    int i;
    if (in(tag, CLOSES_P) && (i = find_open(t, "p", PB)) > 0) pop_to(t, i);
    if (!strcmp(tag, "li") && (i = find_open(t, "li", LISTB)) > 0) pop_to(t, i);
    if ((!strcmp(tag, "dt") || !strcmp(tag, "dd"))) {
        if ((i = find_open(t, "dd", DLB)) > 0) pop_to(t, i);
        if ((i = find_open(t, "dt", DLB)) > 0) pop_to(t, i);
    }
    if (!strcmp(tag, "tr")) {
        if ((i = find_open(t, "tr", ROWB)) > 0) pop_to(t, i);
    }
    if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
        if ((i = find_open(t, "td", CELLB)) > 0) pop_to(t, i);
        if ((i = find_open(t, "th", CELLB)) > 0) pop_to(t, i);
    }
    if (!strcmp(tag, "thead") || !strcmp(tag, "tbody") || !strcmp(tag, "tfoot")) {
        static const char *G[] = { "thead", "tbody", "tfoot", 0 };
        for (int k = t->sp - 1; k >= 1; k--) {
            if (!strcmp(t->stack[k]->tag, "table")) break;
            if (in(t->stack[k]->tag, G)) { pop_to(t, k); break; }
        }
    }
    if (!strcmp(tag, "option") && (i = find_open(t, "option", SELB)) > 0) pop_to(t, i);
    if (!strcmp(tag, "a") && (i = find_open(t, "a", TBL)) > 0) pop_to(t, i);
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]) {
        Node *c = cur(t);
        if (c->tag[0] == 'h' && c->tag[1] >= '1' && c->tag[1] <= '6' && !c->tag[2]) t->sp--;
    }
    append(cur(t), n);
    if (!selfclose && !in(tag, VOID) && t->sp < 255) t->stack[t->sp++] = n;
}

static void close_elem(TB *t, const char *tag) {
    if (!strcmp(tag, "body") || !strcmp(tag, "html") || !strcmp(tag, "head")) return;
    if (!strcmp(tag, "br")) {   // </br> acts like <br>
        Node *n = ar_alloc(&t->d->ar, sizeof *n);
        n->type = N_ELEM; n->tag = "br";
        ensure_body(t);
        append(cur(t), n);
        return;
    }
    static const char *B[] = { "table", 0 };
    bool cell = !strcmp(tag, "td") || !strcmp(tag, "th") || !strcmp(tag, "tr");
    int i = find_open(t, tag, cell ? B : 0);
    if (i > 0) pop_to(t, i);
}

static void add_text(TB *t, const char *s, size_t n, bool raw) {
    if (!n) return;
    bool ws = true;
    for (size_t i = 0; i < n && ws; i++) ws = isspace((unsigned char)s[i]);
    if (!t->d->body && ws) return;          // whitespace before <body>
    if (!t->d->body && !raw) ensure_body(t);
    Node *p = t->d->body ? cur(t) : t->d->head;
    char *txt = raw ? ar_strndup(&t->d->ar, s, n) : decode(&t->d->ar, s, n);
    Node *last = p->last;
    if (last && last->type == N_TEXT) {   // merge adjacent text
        size_t l1 = last->tlen, l2 = strlen(txt);
        char *m = ar_alloc(&t->d->ar, l1 + l2 + 1);
        memcpy(m, last->text, l1); memcpy(m + l1, txt, l2 + 1);
        last->text = m; last->tlen = l1 + l2;
        return;
    }
    Node *tn = ar_alloc(&t->d->ar, sizeof *tn);
    tn->type = N_TEXT;
    tn->text = txt;
    tn->tlen = strlen(txt);
    append(p, tn);
}

// Latin-1 -> UTF-8 if the input is not valid UTF-8
static char *fix_charset(const char *src, size_t len, size_t *outlen) {
    size_t bad = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = src[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > len) { bad++; i++; continue; }
        for (int k = 1; k < n; k++) if ((src[i + k] & 0xc0) != 0x80) { bad++; n = 1; break; }
        i += n;
    }
    if (bad == 0 || bad < len / 2000) { *outlen = len; return 0; }
    char *o = malloc(len * 2 + 1), *p = o;
    for (size_t i = 0; i < len; i++) p += utf8_put(p, (unsigned char)src[i]);
    *outlen = p - o;
    return o;
}

Doc *html_parse(const char *src0, size_t len0, const char *url) {
    size_t len;
    char *conv = fix_charset(src0, len0, &len);
    const char *src = conv ? conv : src0;
    Doc *d = calloc(1, sizeof *d);
    d->base = xstrdup(url);
    Node *root = ar_alloc(&d->ar, sizeof *root);
    root->type = N_ELEM; root->tag = "html";
    Node *head = ar_alloc(&d->ar, sizeof *head);
    head->type = N_ELEM; head->tag = "head";
    append(root, head);
    d->root = root; d->head = head;
    TB t = { d, { root }, 1 };
    size_t i = 0, text_start = 0;
    while (i < len) {
        if (src[i] != '<') { i++; continue; }
        // comment / doctype / bogus
        if (i + 1 < len && src[i + 1] == '!') {
            add_text(&t, src + text_start, i - text_start, false);
            const char *e;
            if (i + 3 < len && src[i + 2] == '-' && src[i + 3] == '-') {
                e = memmem(src + i + 4, len - i - 4, "-->", 3);
                i = e ? (size_t)(e - src) + 3 : len;
            } else {
                e = memchr(src + i, '>', len - i);
                i = e ? (size_t)(e - src) + 1 : len;
            }
            text_start = i;
            continue;
        }
        bool end = i + 1 < len && src[i + 1] == '/';
        size_t j = i + 1 + end;
        if (j >= len || !isalpha((unsigned char)src[j])) { i++; continue; }   // a lone '<' is text
        add_text(&t, src + text_start, i - text_start, false);
        size_t ns = j;
        while (j < len && !isspace((unsigned char)src[j]) && src[j] != '>' && src[j] != '/') j++;
        char tagname[32];
        int tl = mini(j - ns, 31);
        for (int k = 0; k < tl; k++) tagname[k] = tolower((unsigned char)src[ns + k]);
        tagname[tl] = 0;
        // attributes
        char *names[64], *vals[64];
        int na = 0;
        bool selfclose = false;
        while (j < len && src[j] != '>') {
            while (j < len && (isspace((unsigned char)src[j]))) j++;
            if (j < len && src[j] == '/') { selfclose = true; j++; continue; }
            if (j >= len || src[j] == '>') break;
            size_t as = j;
            while (j < len && !isspace((unsigned char)src[j]) && src[j] != '=' && src[j] != '>' && !(src[j] == '/' && src[j + 1] == '>')) j++;
            size_t ae = j;
            while (j < len && isspace((unsigned char)src[j])) j++;
            const char *vs = "";
            size_t vl = 0;
            if (j < len && src[j] == '=') {
                j++;
                while (j < len && isspace((unsigned char)src[j])) j++;
                if (j < len && (src[j] == '"' || src[j] == '\'')) {
                    char q = src[j++];
                    vs = src + j;
                    const char *e = memchr(src + j, q, len - j);
                    vl = e ? (size_t)(e - vs) : len - j;
                    j += vl + 1;
                } else {
                    vs = src + j;
                    while (j < len && !isspace((unsigned char)src[j]) && src[j] != '>') j++;
                    vl = src + j - vs;
                }
            }
            if (ae > as && na < 64 && !end) {
                char *nm = ar_strndup(&d->ar, src + as, ae - as);
                for (char *c = nm; *c; c++) *c = tolower((unsigned char)*c);
                names[na] = nm;
                vals[na] = decode(&d->ar, vs, vl);
                na++;
            }
        }
        i = j < len ? j + 1 : len;
        text_start = i;
        if (end) { close_elem(&t, tagname); continue; }
        Node *n = ar_alloc(&d->ar, sizeof *n);
        n->type = N_ELEM;
        n->tag = ar_strndup(&d->ar, tagname, strlen(tagname));
        if (na) {
            n->attr = ar_alloc(&d->ar, sizeof(char *) * na * 2);
            for (int k = 0; k < na; k++) { n->attr[k * 2] = names[k]; n->attr[k * 2 + 1] = vals[k]; }
            n->nattr = na;
        }
        if (!strcmp(tagname, "meta")) {
            const char *he = attr(n, "http-equiv"), *ct = attr(n, "content");
            if (he && ct && !strcasecmp(he, "refresh") && !d->refresh) {
                const char *u = strcasestr(ct, "url=");
                if (u) {
                    u += 4;
                    while (*u == '\'' || *u == '"' || *u == ' ') u++;
                    char *r = xstrdup(u);
                    r[strcspn(r, "'\"")] = 0;
                    d->refresh = r;
                }
            }
        }
        if (!strcmp(tagname, "base") && attr(n, "href")) { char *b = url_resolve(url, attr(n, "href")); free(d->base); d->base = b; }
        open_elem(&t, n, selfclose);
        if (in(tagname, RAW) && !selfclose) {
            // raw text until the matching end tag
            char close[40];
            snprintf(close, sizeof close, "</%s", tagname);
            const char *e = strcasestr(src + i, close);
            size_t stop = e ? (size_t)(e - src) : len;
            if (!strcmp(tagname, "title")) {
                char *tt = decode(&d->ar, src + i, stop - i);
                if (!d->title) d->title = xstrdup(tt);
            } else if (!strcmp(tagname, "textarea")) {
                n->value = strndup(src + i, stop - i);
            } else if (stop > i) {
                Node *tn = ar_alloc(&d->ar, sizeof *tn);
                tn->type = N_TEXT;
                tn->text = ar_strndup(&d->ar, src + i, stop - i);
                tn->tlen = stop - i;
                append(n, tn);
            }
            if (!strcmp(n->tag, "title") && n->parent == d->head) {}
            const char *gt = e ? memchr(e, '>', len - (e - src)) : 0;
            i = gt ? (size_t)(gt - src) + 1 : len;
            text_start = i;
            close_elem(&t, tagname);
        }
    }
    add_text(&t, src + text_start, len - text_start, false);
    ensure_body(&t);
    free(conv);
    if (d->title) {   // collapse whitespace in the title
        char *o = d->title, *p = d->title;
        bool sp = true;
        for (; *p; p++) {
            if (isspace((unsigned char)*p)) { if (!sp) *o++ = ' '; sp = true; }
            else { *o++ = *p; sp = false; }
        }
        while (o > d->title && o[-1] == ' ') o--;
        *o = 0;
    }
    return d;
}

static void free_values(Node *n) {
    for (Node *c = n->first; c; c = c->next) {
        free(c->value);
        if (c->img) image_free(c->img);
        free_values(c);
    }
}
void doc_free(Doc *d) {
    if (!d) return;
    free_values(d->root);
    ar_free(&d->ar);
    free(d->title); free(d->base); free(d->refresh);
    free(d);
}
