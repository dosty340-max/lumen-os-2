// Web: the browser app. One worker thread per tab fetches, parses, styles and
// lays out pages; image loaders decode off the UI thread; the UI only paints
// the published display list. No JavaScript.
#include "../web/web.h"
#include <ctype.h>
#include <unistd.h>

#define MAXTAB 6
#define HDR 112          // header height (pt)
#define BAR 70           // bottom toolbar height (pt)
typedef struct Tab {
    char url[2048], title[256];
    Doc *doc; DList *dl; Sheet *sheet;
    Scroll sc;
    volatile float progress;
    volatile int loading, relayout, nimg_pending;
    char err[200];
    char *hist[64]; float hscroll[64]; int nhist, hpos;
    pthread_mutex_t mu;           // guards doc/dl swap and painting
    pthread_mutex_t jmu; pthread_cond_t jcv;
    char *job_url, *job_body; int job, job_hist;   // job: 1 load
    int gen, tsize;
    Node *focus;
    float restore_scroll;
} Tab;
static Tab *tabs[MAXTAB];
static int ntab, cur;
static int editing;                 // address bar focused
static int addr_sel;                // whole address selected: next key replaces it
static char addr[2048];
static Gest g;
static Spring kb_s, prog_a, switcher;
static int show_switcher;
static double flash_t; static int flash_link = -1;
static pthread_mutex_t img_mu = PTHREAD_MUTEX_INITIALIZER;
static int img_threads;

static float tscale(void) { static const float f[3] = { 0.9f, 1.0f, 1.18f }; return f[SET.textsize % 3]; }
static float page_top(void) { return P(HDR); }
static float page_bot(void) { return SH - P(BAR); }

// ---- bookmarks & history (persisted) ---------------------------------------------------------------
static bool is_bookmarked(const char *url) {
    const char *b = store_get("web.bookmarks");
    if (!b || !url[0]) return false;
    char needle[2100];
    snprintf(needle, sizeof needle, "\t%s\n", url);
    return strstr(b, needle) != 0;
}
static void toggle_bookmark(Tab *t) {
    if (!t->url[0]) return;
    const char *b = store_get("web.bookmarks");
    char *old = xstrdup(b), line[2400];
    snprintf(line, sizeof line, "%s\t%s\n", t->title[0] ? t->title : t->url, t->url);
    if (is_bookmarked(t->url)) {   // remove the line containing the url
        char needle[2100];
        snprintf(needle, sizeof needle, "\t%s\n", t->url);
        char *p = strstr(old, needle), *ls = p;
        while (ls > old && ls[-1] != '\n') ls--;
        memmove(ls, p + strlen(needle), strlen(p + strlen(needle)) + 1);
        store_set("web.bookmarks", old);
    } else {
        char *n = strfmt("%s%s", old, line);
        store_set("web.bookmarks", n);
        free(n);
        notify("Web", "Bookmark added");
    }
    free(old);
}
static void add_history(const char *title, const char *url) {
    const char *h = store_get("web.history");
    char *out = malloc(16384), *o = out;
    o += snprintf(o, 16384, "%s\t%s\n", title[0] ? title : url, url);
    int lines = 1;
    if (h) {
        const char *p = h;
        while (*p && lines < 30) {
            const char *e = strchr(p, '\n');
            if (!e) break;
            const char *tab = memchr(p, '\t', e - p);
            if (!(tab && (size_t)(e - tab - 1) == strlen(url) && !strncmp(tab + 1, url, e - tab - 1)) && o - out + (e - p) + 2 < 16384) {
                memcpy(o, p, e - p + 1); o += e - p + 1; lines++;
            }
            p = e + 1;
        }
    }
    *o = 0;
    store_set("web.history", out);
    free(out);
}

// ---- loading pipeline (worker thread) --------------------------------------------------------------
typedef struct { Tab *t; Node *n; char *url; int gen; } ImgJob;
static void *img_thread(void *a) {
    ImgJob *j = a;
    HttpResp *r = http_request("GET", j->url, 0, 0, 6 << 20);
    Image *im = r->status == 200 && r->body ? image_decode(r->body, r->len) : 0;
    http_free(r);
    Tab *t = j->t;
    pthread_mutex_lock(&t->mu);
    if (t->gen == j->gen) {   // page is still the same document
        if (im && (im->w > 4096 || im->h > 4096)) { image_free(im); im = 0; }
        j->n->img = im;
        j->n->img_state = im ? 2 : 3;
        t->relayout = 1;
    } else image_free(im);
    t->nimg_pending--;
    pthread_mutex_unlock(&t->mu);
    free(j->url); free(j);
    pthread_mutex_lock(&img_mu); img_threads--; pthread_mutex_unlock(&img_mu);
    return 0;
}
static void collect_imgs(Node *n, Node **out, int *k, int max) {
    for (Node *c = n->first; c && *k < max; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->st && (c->st->display == D_NONE || c->st->hidden >= 1)) continue;
        if (!strcmp(c->tag, "img") && attr(c, "src") && strncmp(attr(c, "src"), "data:", 5)) out[(*k)++] = c;
        collect_imgs(c, out, k, max);
    }
}
static void start_images(Tab *t, Doc *d, int gen) {
    Node *imgs[80];
    int k = 0;
    collect_imgs(d->root, imgs, &k, 80);
    for (int i = 0; i < k; i++) {
        imgs[i]->img_state = 1;
        ImgJob *j = calloc(1, sizeof *j);
        j->t = t; j->n = imgs[i]; j->gen = gen;
        j->url = url_resolve(d->base, attr(imgs[i], "src"));
        // throttle: at most 4 decoders in flight
        for (;;) {
            pthread_mutex_lock(&img_mu);
            if (img_threads < 4) { img_threads++; pthread_mutex_unlock(&img_mu); break; }
            pthread_mutex_unlock(&img_mu);
            usleep(20000);
            if (t->gen != gen) { free(j->url); free(j); return; }
        }
        pthread_mutex_lock(&t->mu); t->nimg_pending++; pthread_mutex_unlock(&t->mu);
        pthread_t th;
        pthread_create(&th, 0, img_thread, j);
        pthread_detach(th);
    }
}

static void progress_cb(void *ctx, size_t got, size_t total) {
    Tab *t = ctx;
    float f = total ? (float)got / total : 1 - 1 / (1 + got / 60000.0f);
    t->progress = 0.12f + 0.5f * clampf(f, 0, 1);
}

static char *css_cache_url[24], *css_cache_txt[24];
static int css_cache_n;
static pthread_mutex_t css_mu = PTHREAD_MUTEX_INITIALIZER;
static char *fetch_css(const char *url) {
    pthread_mutex_lock(&css_mu);
    for (int i = 0; i < css_cache_n; i++) if (!strcmp(css_cache_url[i], url)) { char *r = xstrdup(css_cache_txt[i]); pthread_mutex_unlock(&css_mu); return r; }
    pthread_mutex_unlock(&css_mu);
    HttpResp *r = http_request("GET", url, 0, 0, 3 << 20);
    char *txt = r->status == 200 && r->body ? xstrdup(r->body) : 0;
    http_free(r);
    if (txt) {
        pthread_mutex_lock(&css_mu);
        int i = css_cache_n < 24 ? css_cache_n++ : (int)(now() * 10) % 24;
        if (i < css_cache_n && css_cache_url[i] && i != css_cache_n - 1) { free(css_cache_url[i]); free(css_cache_txt[i]); }
        css_cache_url[i] = xstrdup(url); css_cache_txt[i] = xstrdup(txt);
        pthread_mutex_unlock(&css_mu);
    }
    return txt;
}
static void gather_css(Node *n, Doc *d, Sheet *sh, int *nlink) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (!strcmp(c->tag, "style")) {
            const char *m = attr(c, "media");
            if (!(m && strstr(m, "print") && !strstr(m, "screen")) && c->first && c->first->type == N_TEXT) css_parse(c->first->text, sh);
        } else if (!strcmp(c->tag, "link") && attr(c, "href") && attr(c, "rel") && strcasestr(attr(c, "rel"), "stylesheet") && !strcasestr(attr(c, "rel"), "alternate")) {
            const char *m = attr(c, "media");
            if (m && strstr(m, "print") && !strstr(m, "screen")) continue;
            if (*nlink >= 8) continue;
            (*nlink)++;
            char *u = url_resolve(d->base, attr(c, "href"));
            char *txt = fetch_css(u);
            LOG("web: stylesheet %s (%zu bytes)", u, txt ? strlen(txt) : 0);
            if (txt) { css_parse(txt, sh); free(txt); }
            free(u);
        }
        gather_css(c, d, sh, nlink);
    }
}

static char *error_page(const char *url, const char *err) {
    return strfmt("<html><body style='margin:24px;background:#F3F0E8;color:#141414'><h1 style='font-size:40px'>Can't open this page</h1>"
                  "<p style='font-size:18px'>%s</p><p style='color:#666'>%s</p></body></html>", err, url);
}

static void do_load(Tab *t, char *url, char *body, int hist_mode) {
    int gen = ++t->gen;
    t->loading = 1; t->progress = 0.06f; t->err[0] = 0;
    double t0 = now();
    // DuckDuckGo result links are redirect wrappers: follow the real target directly
    char *real = 0;
    const char *ud = strstr(url, "duckduckgo.com/l/?");
    if (ud && (ud = strstr(url, "uddg="))) {
        ud += 5;
        real = malloc(strlen(ud) + 1);
        char *o = real;
        for (const char *p = ud; *p && *p != '&'; p++) {
            if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) { char h[3] = { p[1], p[2], 0 }; *o++ = strtol(h, 0, 16); p += 2; }
            else *o++ = *p == '+' ? ' ' : *p;
        }
        *o = 0;
    }
    const char *fetch_url = real ? real : url;
    http_set_progress(progress_cb, t);
    HttpResp *r = http_request(body ? "POST" : "GET", fetch_url, body, body ? "application/x-www-form-urlencoded" : 0, 12 << 20);
    http_set_progress(0, 0);
    if (gen != t->gen) { http_free(r); free(real); return; }
    t->progress = 0.65f;
    char *html;
    size_t hlen;
    const char *final = r->url ? r->url : fetch_url;
    if (r->status == 0 || !r->body) { html = error_page(fetch_url, r->err[0] ? r->err : "No response"); hlen = strlen(html); }
    else if (r->content_type && !strncmp(r->content_type, "image/", 6)) { html = strfmt("<html><body style='margin:0;background:#141414'><img src='%s' style='width:100%%'></body></html>", final); hlen = strlen(html); }
    else if (r->content_type && strstr(r->content_type, "text/plain")) {
        html = malloc(r->len + 64);
        strcpy(html, "<pre style='white-space:pre-wrap'>");
        size_t o = strlen(html);
        for (size_t i = 0; i < r->len; i++) {
            if (r->body[i] == '<') { memcpy(html + o, "&lt;", 4); o += 4; html = realloc(html, r->len * 4 + 64); }
            else html[o++] = r->body[i];
        }
        html[o] = 0;
        hlen = o;
    } else { html = r->body; r->body = 0; hlen = r->len; }
    Doc *d = html_parse(html, hlen, final);
    free(html);
    t->progress = 0.72f;
    Sheet *sh = css_parse(UA_CSS, 0);
    int nlink = 0;
    gather_css(d->root, d, sh, &nlink);
    t->progress = 0.85f;
    css_style_doc(d, sh, 16);
    DList *dl = layout_doc(d, SW, tscale());
    double ms = (now() - t0) * 1000;
    LOG("web: loaded %s -> %d \"%s\" (%zu bytes html, %d display items, height %.0f, %.0f ms)", final, r->status, d->title ? d->title : "", hlen, dl->n, dl->height, ms);
    if (gen != t->gen) { doc_free(d); dlist_free(dl); css_free(sh); http_free(r); free(real); return; }
    pthread_mutex_lock(&t->mu);
    Doc *od = t->doc; DList *odl = t->dl; Sheet *osh = t->sheet;
    t->doc = d; t->dl = dl; t->sheet = sh;
    t->focus = 0;
    snprintf(t->url, sizeof t->url, "%s", final);
    snprintf(t->title, sizeof t->title, "%s", d->title && d->title[0] ? d->title : final);
    t->sc = (Scroll){ 0 };
    if (hist_mode == 0) {   // new navigation: truncate forward history
        for (int i = t->hpos + 1; i < t->nhist; i++) free(t->hist[i]);
        t->nhist = t->hpos + 1;
        if (t->nhist >= 64) { free(t->hist[0]); memmove(t->hist, t->hist + 1, 63 * sizeof(char *)); memmove(t->hscroll, t->hscroll + 1, 63 * sizeof(float)); t->nhist--; }
        t->hist[t->nhist] = xstrdup(final);
        t->hscroll[t->nhist] = 0;
        t->hpos = t->nhist++;
    } else {
        free(t->hist[t->hpos]);
        t->hist[t->hpos] = xstrdup(final);
        t->sc.pos = t->restore_scroll;
    }
    t->tsize = SET.textsize;
    t->progress = 1; t->loading = 0;
    pthread_mutex_unlock(&t->mu);
    doc_free(od); dlist_free(odl); css_free(osh);
    if (r->status) add_history(t->title, final);
    char *refresh = d->refresh ? url_resolve(d->base, d->refresh) : 0;
    http_free(r);
    free(real);
    if (refresh) {   // meta refresh: queue a follow-up load
        pthread_mutex_lock(&t->jmu);
        if (!t->job) { t->job = 1; t->job_url = refresh; t->job_body = 0; t->job_hist = 0; pthread_cond_signal(&t->jcv); }
        else free(refresh);
        pthread_mutex_unlock(&t->jmu);
    }
    start_images(t, d, gen);
}

static void *tab_thread(void *a) {
    Tab *t = a;
    for (;;) {
        pthread_mutex_lock(&t->jmu);
        while (!t->job) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 250000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&t->jcv, &t->jmu, &ts);
            if (!t->job && t->doc && (t->relayout || t->tsize != SET.textsize)) break;
        }
        int job = t->job, hm = t->job_hist;
        char *url = t->job_url, *body = t->job_body;
        t->job = 0; t->job_url = 0; t->job_body = 0;
        pthread_mutex_unlock(&t->jmu);
        if (job == 1) { do_load(t, url, body, hm); free(url); free(body); continue; }
        // relayout after images arrived or the text size changed
        t->relayout = 0;
        int gen = t->gen;
        t->tsize = SET.textsize;
        DList *dl = layout_doc(t->doc, SW, tscale());
        pthread_mutex_lock(&t->mu);
        if (gen == t->gen) { DList *o = t->dl; t->dl = dl; dl = o; }
        pthread_mutex_unlock(&t->mu);
        dlist_free(dl);
    }
    return 0;
}

static Tab *new_tab(void) {
    if (ntab >= MAXTAB) return tabs[cur];
    Tab *t = calloc(1, sizeof *t);
    pthread_mutex_init(&t->mu, 0);
    pthread_mutex_init(&t->jmu, 0);
    pthread_cond_init(&t->jcv, 0);
    t->hpos = -1;
    pthread_t th;
    pthread_create(&th, 0, tab_thread, t);
    pthread_detach(th);
    tabs[ntab] = t;
    cur = ntab++;
    return t;
}
static void navigate(Tab *t, const char *url, const char *body, int hist_mode) {
    pthread_mutex_lock(&t->jmu);
    free(t->job_url); free(t->job_body);
    t->job = 1; t->job_url = xstrdup(url); t->job_body = body ? xstrdup(body) : 0; t->job_hist = hist_mode;
    pthread_cond_signal(&t->jcv);
    pthread_mutex_unlock(&t->jmu);
    t->loading = 1; t->progress = 0.03f;
    snprintf(t->url, sizeof t->url, "%s", url);
    LOG("web: navigate %s%s", url, body ? " (POST)" : "");
}
static void go(const char *input) {
    while (*input == ' ') input++;
    if (!*input) return;
    char *url;
    bool has_scheme = strstr(input, "://") != 0;
    bool looks_url = !strchr(input, ' ') && (strchr(input, '.') || !strncmp(input, "localhost", 9));
    if (has_scheme) url = xstrdup(input);
    else if (looks_url) url = strfmt("https://%s", input);
    else { char *q = url_encode(input); url = strfmt("https://html.duckduckgo.com/html/?q=%s", q); free(q); }
    navigate(tabs[cur], url, 0, 0);
    free(url);
}
static void history_go(int dir) {
    Tab *t = tabs[cur];
    int np = t->hpos + dir;
    if (np < 0 || np >= t->nhist) return;
    t->hscroll[t->hpos] = t->sc.pos;
    t->hpos = np;
    t->restore_scroll = t->hscroll[np];
    navigate(t, t->hist[np], 0, 1);
}

// ---- forms ------------------------------------------------------------------------------------------
static const char *input_value(Node *n) { return n->value ? n->value : attr(n, "value") ? attr(n, "value") : ""; }
static void form_walk(Node *n, Node *submitter, char *q, int cap) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        const char *name = attr(c, "name");
        if (name && *name) {
            const char *type = attr(c, "type") ? attr(c, "type") : "text";
            const char *val = 0;
            if (!strcmp(c->tag, "input")) {
                if (!strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "image")) val = c == submitter ? input_value(c) : 0;
                else if (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")) val = (c->checked || attr(c, "checked")) ? (attr(c, "value") ? attr(c, "value") : "on") : 0;
                else val = input_value(c);
            } else if (!strcmp(c->tag, "textarea")) val = c->value ? c->value : "";
            else if (!strcmp(c->tag, "select")) {
                for (Node *o = c->first; o; o = o->next)
                    if (o->type == N_ELEM && !strcmp(o->tag, "option")) { val = attr(o, "value") ? attr(o, "value") : ""; if (attr(o, "selected")) break; }
            } else if (!strcmp(c->tag, "button") && c == submitter) val = attr(c, "value") ? attr(c, "value") : "";
            if (val) {
                char *k = url_encode(name), *v = url_encode(val);
                int l = strlen(q);
                snprintf(q + l, cap - l, "%s%s=%s", l ? "&" : "", k, v);
                free(k); free(v);
            }
        }
        form_walk(c, submitter, q, cap);
    }
}
static void submit_form(Tab *t, Node *from) {
    Node *f = from;
    while (f && !(f->type == N_ELEM && !strcmp(f->tag, "form"))) f = f->parent;
    if (!f) return;
    char *q = calloc(1, 8192);
    form_walk(f, from, q, 8192);
    const char *action = attr(f, "action");
    char *base = url_resolve(t->doc->base, action && *action ? action : t->url);
    const char *method = attr(f, "method");
    LOG("web: submit form %s %s ? %s", method ? method : "get", base, q);
    if (method && !strcasecmp(method, "post")) navigate(t, base, q, 0);
    else {
        char *qs = strchr(base, '?');
        if (qs) *qs = 0;
        char *u = strfmt("%s?%s", base, q);
        navigate(t, u, 0, 0);
        free(u);
    }
    free(base); free(q);
    t->focus = 0;
}

// ---- drawing ------------------------------------------------------------------------------------------
static void draw_page(Canvas *c, Tab *t, float dt) {
    float top = page_top(), bot = page_bot() - (editing || t->focus ? 0 : 0);
    int sc[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
    g_clip(c, 0, top, SW, bot);
    pthread_mutex_lock(&t->mu);
    DList *dl = t->dl;
    float view = bot - top;
    if (dl) {
        t->sc.max = dl->height - view + P(20);
        scroll_step(&t->sc, dt);
        g_rect(c, 0, top, SW, view, dl->page_bg | 0xff000000);
        float off = top - t->sc.pos;
        for (int i = 0; i < dl->n; i++) {
            DItem *it = &dl->it[i];
            float y = it->y + off;
            if (it->kind == DI_TEXT) { if (y < top - 4 || y - it->size > bot) continue; }
            else if (y > bot || y + it->h < top) continue;
            switch (it->kind) {
            case DI_RECT: case DI_LINE: g_rect(c, it->x, y, it->w, it->h, it->color); break;
            case DI_TEXT:
                if (it->link >= 0 && it->link == flash_link && now() - flash_t < 0.35) g_rect(c, it->x, y - it->size * 0.9f, it->w, it->size * 1.2f, C_LIME);
                text_n(c, it->font, it->size, 0, it->x, y, it->color, it->text, it->len);
                break;
            case DI_IMAGE:
                if (it->node->img) g_image(c, it->node->img, it->x, y, it->w, it->h, 0, 255);
                else g_rect(c, it->x, y, it->w, it->h, 0xffe8e6e1);
                break;
            case DI_INPUT: {
                Node *n = it->node;
                const char *type = attr(n, "type");
                if (type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) {
                    g_rrect_line(c, it->x, y, it->w, it->h, P(3), P(1.5f), 0xff555555);
                    if (n->checked || attr(n, "checked")) g_rect(c, it->x + it->w * 0.25f, y + it->h * 0.25f, it->w / 2, it->h / 2, 0xff333333);
                    break;
                }
                bool foc = t->focus == n;
                g_rrect(c, it->x, y, it->w, it->h, P(6), P(6), 0xffffffff);
                g_rrect_line(c, it->x, y, it->w, it->h, P(6), foc ? P(2.5f) : P(1.2f), foc ? C_ORANGE : 0xff8a8a8a);
                const char *v = input_value(n);
                bool ph = !*v;
                if (ph) v = attr(n, "placeholder") ? attr(n, "placeholder") : "";
                char masked[256];
                if (!ph && type && !strcasecmp(type, "password")) { int k = mini(strlen(v), 255); memset(masked, '*', k); masked[k] = 0; v = masked; }
                int s2[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
                g_clip(c, it->x + P(4), y, it->x + it->w - P(4), y + it->h);
                float tw = text_w(W400, it->size, 0, v), tx = it->x + P(8);
                if (!ph && tw > it->w - P(20)) tx = it->x + it->w - P(12) - tw;
                text(c, W400, it->size, 0, tx, y + it->h / 2 + it->size * 0.35f, ph ? 0xff9a9a9a : 0xff111111, v);
                if (foc && fmod(now(), 1) < 0.6) g_rect(c, tx + (ph ? 0 : tw) + P(1), y + it->h * 0.2f, P(2), it->h * 0.6f, C_ORANGE);
                c->cx0 = s2[0]; c->cy0 = s2[1]; c->cx1 = s2[2]; c->cy1 = s2[3];
                break;
            }
            case DI_BUTTON:
                g_rrect(c, it->x, y, it->w, it->h, P(6), P(6), pressed(0x7000 + i) ? C_LIME : 0xffe4e2dc);
                g_rrect_line(c, it->x, y, it->w, it->h, P(6), P(1.2f), 0xff8a8a8a);
                text_c(c, W600, it->size, 0, it->x + it->w / 2, y + it->h / 2 + it->size * 0.35f, 0xff111111, it->text);
                break;
            }
        }
        // scroll indicator
        if (dl->height > view) {
            float h = fmaxf(P(40), view * view / dl->height), yy = top + (view - h) * clampf(t->sc.pos / fmaxf(1, dl->height - view), 0, 1);
            g_rrect(c, SW - P(6), yy, P(3), h, P(1.5f), P(1.5f), 0x66141414);
        }
    } else {
        g_rect(c, 0, top, SW, view, TH.bg);
    }
    pthread_mutex_unlock(&t->mu);
    c->cx0 = sc[0]; c->cy0 = sc[1]; c->cx1 = sc[2]; c->cy1 = sc[3];
}

static void draw_start(Canvas *c, Tab *t) {
    (void)t;
    float top = page_top();
    g_rect(c, 0, top, SW, page_bot() - top, TH.bg);
    float y = top + P(24);
    text(c, W800, P(48), -0.05f, P(18), y + P(44), TH.fg, "Web");
    text(c, W600, P(14), 0, P(20), y + P(70), TH.sub, "HTML + CSS, rendered on the phone. No JavaScript.");
    y += P(100);
    text(c, W600, P(13), 0.12f, P(22), y, TH.sub, "BOOKMARKS");
    y += P(14);
    const char *b = store_get("web.bookmarks");
    static const char *def = "Wikipedia\thttps://en.m.wikipedia.org/wiki/Main_Page\nHacker News\thttps://news.ycombinator.com/\nCNN Lite\thttps://lite.cnn.com/\nNPR Text\thttps://text.npr.org/\nexample.com\thttps://example.com/\n";
    if (!b || !*b) b = def;
    int k = 0;
    static const uint32_t cols[] = { C_LIME, C_SKY, C_LILAC, C_ORANGE, C_PAPER };
    for (const char *p = b; *p && k < 8;) {
        const char *e = strchr(p, '\n');
        if (!e) break;
        const char *tb = memchr(p, '\t', e - p);
        if (tb) {
            float x = P(16) + (k % 2) * P(184), yy = y + (k / 2) * P(84);
            g_rrect(c, x, yy, P(174), P(74), P(22), P(22), pressed(0x7400 + k) ? C_INK : cols[k % 5]);
            char title[64];
            snprintf(title, sizeof title, "%.*s", (int)(tb - p), p);
            char *tt = text_ellipsize(W800, P(17), title, P(148));
            text(c, W800, P(17), -0.02f, x + P(14), yy + P(32), pressed(0x7400 + k) ? C_PAPER : C_INK, tt);
            free(tt);
            char host[96];
            Url u;
            char ub[2048];
            snprintf(ub, sizeof ub, "%.*s", (int)(e - tb - 1), tb + 1);
            snprintf(host, sizeof host, "%s", url_parse(ub, &u) ? ub : u.host);
            char *hh = text_ellipsize(W400, P(12.5f), host, P(148));
            text(c, W400, P(12.5f), 0, x + P(14), yy + P(56), pressed(0x7400 + k) ? C_PAPER : C_INK, hh);
            free(hh);
            hit(0x7400 + k, x, yy, P(174), P(74));
            k++;
        }
        p = e + 1;
    }
    y += ((k + 1) / 2) * P(84) + P(20);
    text(c, W600, P(13), 0.12f, P(22), y, TH.sub, "RECENT");
    y += P(10);
    const char *h = store_get("web.history");
    int n = 0;
    for (const char *p = h ? h : ""; *p && n < 6 && y < page_bot() - P(50);) {
        const char *e = strchr(p, '\n');
        if (!e) break;
        const char *tb = memchr(p, '\t', e - p);
        if (tb) {
            g_rect(c, P(16), y, P(358), P(1.5f), TH.line);
            char title[256];
            snprintf(title, sizeof title, "%.*s", (int)(tb - p), p);
            char *tt = text_ellipsize(W600, P(15), title, P(330));
            text(c, W600, P(15), 0, P(22), y + P(30), pressed(0x7500 + n) ? C_ORANGE : TH.fg, tt);
            free(tt);
            hit(0x7500 + n, 0, y, SW, P(46));
            y += P(46);
            n++;
        }
        p = e + 1;
    }
}

static const char *nth_line_url(const char *list, int k, char *out, int n) {
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, '\n');
        if (!e) break;
        const char *tb = memchr(p, '\t', e - p);
        if (tb && k-- == 0) { snprintf(out, n, "%.*s", (int)(e - tb - 1), tb + 1); return out; }
        p = e + 1;
    }
    return 0;
}

static void draw_switcher(Canvas *c) {
    float u = clampf(switcher.x, 0, 1);
    g_rect(c, 0, 0, SW, SH, c_alpha(C_INK, 0.92f * u));
    float off = (1 - u) * SH * 0.3f;
    text(c, W800, P(44), -0.04f, P(18), P(110) + off, c_alpha(C_PAPER, u), "Tabs");
    static const uint32_t cols[] = { C_ORANGE, C_LILAC, C_LIME, C_SKY, C_PAPER, C_ORANGE };
    for (int i = 0; i < ntab; i++) {
        float y = P(140) + i * P(96) + off;
        bool sel = i == cur;
        g_rrect(c, P(16), y, P(358), P(84), P(26), P(26), cols[i % 6]);
        if (sel) g_rrect_line(c, P(10), y - P(6), P(370), P(96), P(30), P(3), C_PAPER);
        char *tt = text_ellipsize(W800, P(18), tabs[i]->title[0] ? tabs[i]->title : "New tab", P(270));
        text(c, W800, P(18), -0.02f, P(32), y + P(36), C_INK, tt);
        free(tt);
        Url uu;
        char *hh = text_ellipsize(W400, P(13), url_parse(tabs[i]->url, &uu) ? "Start page" : uu.host, P(270));
        text(c, W400, P(13), 0, P(32), y + P(60), C_INK, hh);
        free(hh);
        hit(0x7600 + i, P(16), y, P(300), P(84));
        btn_icon(c, 0x7700 + i, P(318), y + P(18), P(48), IC_CLOSE, C_INK, 0);
    }
    float by = P(140) + ntab * P(96) + P(10) + off;
    if (ntab < MAXTAB) btn_text(c, 0x7800, P(16), by, P(170), P(58), "New tab", C_INK, C_LIME);
    btn_text(c, 0x7801, P(204), by, P(170), P(58), "Done", C_PAPER, 0);
}

static void open_(void) {
    if (!ntab) new_tab();
    const char *arg = shell_launch_arg();
    if (arg && !strcmp(arg, "search")) { editing = 1; addr[0] = 0; }
    else editing = 0;
    show_switcher = 0; switcher.x = switcher.t = 0;
}

static void draw(Canvas *c, float dt) {
    if (!ntab) new_tab();
    Tab *t = tabs[cur];
    g_clear(c, TH.bg);
    if (t->url[0] && (t->dl || t->loading)) draw_page(c, t, dt);
    else draw_start(c, t);
    // header slab
    float hb = P(HDR);
    g_rect(c, 0, 0, SW, hb, C_ORANGE);
    status_line(c, 0, C_INK, true, false);
    float ax = P(16), ay = P(50), aw = P(358), ah = P(48);
    g_rrect(c, ax, ay, aw, ah, P(18), P(18), editing ? C_PAPER : 0xFFFFE3D6);
    int s[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
    g_clip(c, ax + P(10), ay, ax + aw - P(10), ay + ah);
    if (editing) {
        if (addr_sel) {   // selected: lime highlight, typing replaces it
            float w = fminf(text_w(W400, P(16), 0, addr), aw - P(40));
            g_rrect(c, ax, ay, aw, ah, P(18), P(18), C_PAPER);
            g_rect(c, ax + P(14), ay + P(10), w + P(4), ah - P(20), C_LIME);
            text(c, W400, P(16), 0, ax + P(16), ay + ah / 2 + P(5.5f), C_INK, addr);
        } else field_draw(c, ax, ay, aw, ah, addr, "Search or type a URL", true, C_PAPER, C_INK);
    } else {
        Url u;
        bool ok = !url_parse(t->url, &u);
        const char *shown = ok ? u.host : "Search or type a URL";
        if (!strncmp(shown, "www.", 4)) shown += 4;
        float ix = ax + P(16);
        if (ok && !strcmp(u.scheme, "https")) { icon(c, IC_LOCK, ix, ay + P(13), P(20), C_INK); ix += P(26); }
        else icon(c, IC_SEARCH, ix, ay + P(13), P(20), c_alpha(C_INK, ok ? 1 : 0.5f)), ix += P(28);
        text(c, ok ? W600 : W400, P(16), 0, ix, ay + P(30), c_alpha(C_INK, ok ? 1 : 0.55f), shown);
        if (t->loading) { float a = (float)now() * 6; g_arc(c, ax + aw - P(24), ay + ah / 2, P(8), a, a + 4.2f, P(2.5f), C_INK); }
        else if (ok) icon(c, IC_REFRESH, ax + aw - P(34), ay + P(14), P(20), C_INK);
    }
    c->cx0 = s[0]; c->cy0 = s[1]; c->cx1 = s[2]; c->cy1 = s[3];
    hit(0x7101, ax, ay, aw - P(48), ah);
    hit(0x7102, ax + aw - P(48), ay, P(48), ah);
    // progress bar in the Slab accent
    prog_a.t = t->loading ? 1 : 0;
    spring_step(&prog_a, dt, 2.5f, 1);
    if (prog_a.x > 0.01f) {
        g_rect(c, 0, hb - P(5), SW * (t->loading ? t->progress : 1), P(5), c_alpha(C_LIME, clampf(prog_a.x, 0, 1)));
    }
    // bottom toolbar
    float ty = page_bot();
    g_rrect(c, 0, ty, SW, SH - ty, P(26), 0, C_INK);
    int ics[5] = { IC_BACK, IC_FWD, is_bookmarked(t->url) ? IC_BOOKMARK_FILL : IC_BOOKMARK, IC_TABS, IC_GLOBE_SMALL };
    bool en[5] = { t->hpos > 0, t->hpos < t->nhist - 1, t->url[0] != 0, true, true };
    for (int i = 0; i < 5; i++) {
        float bx = P(16) + i * P(74);
        uint32_t col = en[i] ? C_PAPER : 0x55F3F0E8;
        if (pressed(0x7200 + i)) g_rrect(c, bx, ty + P(8), P(62), P(42), P(16), P(16), C_LIME), col = C_INK;
        icon(c, ics[i], bx + P(19), ty + P(17), P(24), col);
        if (i == 3) { char nb[4]; snprintf(nb, 4, "%d", ntab); text_c(c, W800, P(10), 0, bx + P(30) + P(0.5f), ty + P(34), col, nb); }
        hit(0x7200 + i, bx, ty, P(62), P(60));
    }
    home_bar(c, C_PAPER);
    // keyboard for the address bar or a page form field
    kb_s.t = editing || t->focus ? 1 : 0;
    spring_step(&kb_s, dt, 3.2f, 0.9f);
    if (kb_s.x > 0.01f) kb_draw(c, SH - kb_height() * clampf(kb_s.x, 0, 1.05f), "go");
    // tab switcher
    switcher.t = show_switcher;
    spring_step(&switcher, dt, 3, 0.85f);
    if (switcher.x > 0.01f) draw_switcher(c);
}

static void type_key(int k) {
    Tab *t = tabs[cur];
    if (editing) {
        if (addr_sel && k != K_ENTER) { addr[0] = 0; addr_sel = 0; if (k == K_BACKSPACE) return; }
        addr_sel = 0;
        if (field_key(addr, sizeof addr, k)) { editing = 0; go(addr); }
        return;
    }
    if (t->focus) {
        Node *n = t->focus;
        if (!n->value) { n->value = calloc(1, 1024); snprintf(n->value, 1024, "%s", attr(n, "value") ? attr(n, "value") : ""); }
        if (field_key(n->value, 1024, k)) submit_form(t, n);
    }
}

static void touch(InputEv *e) {
    Tab *t = tabs[cur];
    int r = gest(&g, e);
    if (switcher.t > 0.5f) {
        if (r != G_TAP) return;
        int id = hit_at(e->x, e->y);
        if (id >= 0x7600 && id < 0x7600 + ntab) { cur = id - 0x7600; show_switcher = 0; }
        else if (id >= 0x7700 && id < 0x7700 + ntab && ntab > 1) {
            int i = id - 0x7700;
            tabs[i]->gen++;   // let the worker idle; ponytail: tab memory is not reclaimed until reboot
            memmove(&tabs[i], &tabs[i + 1], (ntab - i - 1) * sizeof(Tab *));
            ntab--;
            if (cur >= ntab) cur = ntab - 1;
        } else if (id == 0x7800) { new_tab(); show_switcher = 0; editing = 1; addr[0] = 0; }
        else if (id == 0x7801) show_switcher = 0;
        return;
    }
    bool kb = editing || t->focus;
    if (kb && e->y > SH - kb_height()) {
        if (r == G_TAP) { int k = kb_tap(e->x, e->y); if (k) type_key(k); }
        return;
    }
    if (r == G_DRAG_START || r == G_DRAG) {
        if (g.vert && g.y0 > page_top() && g.y0 < page_bot()) scroll_drag(&t->sc, r == G_DRAG_START ? g.y - g.y0 : g.dy);
        return;
    }
    if (r == G_DRAG_END) { if (t->sc.held) scroll_fling(&t->sc, g.vy); return; }
    if (r != G_TAP) return;
    int id = hit_at(e->x, e->y);
    if (id == 0x7101) { editing = 1; addr_sel = t->url[0] != 0; snprintf(addr, sizeof addr, "%s", t->url); t->focus = 0; return; }
    if (id == 0x7102) { if (t->loading) t->gen++, t->loading = 0; else if (t->url[0]) navigate(t, t->url, 0, 1); return; }
    if (id >= 0x7200 && id < 0x7205) {
        editing = 0; t->focus = 0;
        switch (id - 0x7200) {
        case 0: history_go(-1); break;
        case 1: history_go(1); break;
        case 2: toggle_bookmark(t); break;
        case 3: show_switcher = 1; break;
        case 4: t->url[0] = 0; pthread_mutex_lock(&t->mu); t->gen++; t->loading = 0; pthread_mutex_unlock(&t->mu); break;
        }
        return;
    }
    if (id >= 0x7400 && id < 0x7408) {
        const char *b = store_get("web.bookmarks");
        static const char *def = "Wikipedia\thttps://en.m.wikipedia.org/wiki/Main_Page\nHacker News\thttps://news.ycombinator.com/\nCNN Lite\thttps://lite.cnn.com/\nNPR Text\thttps://text.npr.org/\nexample.com\thttps://example.com/\n";
        char u[2048];
        if (nth_line_url(b && *b ? b : def, id - 0x7400, u, sizeof u)) navigate(t, u, 0, 0);
        return;
    }
    if (id >= 0x7500 && id < 0x7510) { char u[2048]; if (nth_line_url(store_get("web.history"), id - 0x7500, u, sizeof u)) navigate(t, u, 0, 0); return; }
    if (editing) { editing = 0; return; }
    // tap inside the page: links, inputs, buttons
    if (e->y < page_top() || e->y > page_bot()) return;
    pthread_mutex_lock(&t->mu);
    DList *dl = t->dl;
    char *target = 0;
    Node *focus = 0, *button = 0;
    if (dl) {
        float py = e->y - page_top() + t->sc.pos;
        float slop = P(6);
        for (int i = dl->n - 1; i >= 0; i--) {
            DItem *it = &dl->it[i];
            float x0 = it->x - slop, x1 = it->x + it->w + slop, y0, y1;
            if (it->kind == DI_TEXT) { y0 = it->y - it->size * 0.95f - slop; y1 = it->y + it->size * 0.3f + slop; }
            else { y0 = it->y - slop; y1 = it->y + it->h + slop; }
            if (e->x < x0 || e->x > x1 || py < y0 || py > y1) continue;
            if (it->kind == DI_INPUT) {
                const char *type = attr(it->node, "type");
                if (type && (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) { it->node->checked = !(it->node->checked || attr(it->node, "checked")); break; }
                focus = it->node; break;
            }
            if (it->kind == DI_BUTTON) { button = it->node; break; }
            if (it->link >= 0) { target = xstrdup(dl->links[it->link]); flash_link = it->link; flash_t = now(); break; }
        }
    }
    pthread_mutex_unlock(&t->mu);
    if (focus) { t->focus = focus; return; }
    t->focus = 0;
    if (button) { submit_form(t, button); return; }
    if (target) {
        if (!strncasecmp(target, "http://", 7) || !strncasecmp(target, "https://", 8)) {
            char *h = strchr(target, '#');
            bool same = h && !strncmp(target, t->url, h - target) && strlen(t->url) == (size_t)(h - target);
            if (!same) navigate(t, target, 0, 0);
        }
        free(target);
    }
}

static void key(int k) {
    Tab *t = tabs[cur];
    if (editing || t->focus) { type_key(k); return; }
    if (k == K_DOWN) t->sc.vel = P(1400);
    else if (k == K_UP) t->sc.vel = -P(1400);
    else if (k == ' ') t->sc.vel = P(3000);
    else if (k == K_BACKSPACE) history_go(-1);
    else if (k > 32 && k < 127) { editing = 1; addr[0] = 0; field_key(addr, sizeof addr, k); }
}

uint32_t web_status_col(void) { return C_INK; }
App app_web = { "Web", IC_WEB, open_, draw, touch, key, web_status_col };
