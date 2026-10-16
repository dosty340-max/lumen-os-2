// Video: YouTube search via the Mac bridge, MJPEG + PCM playback decoded on the
// phone. Reader thread -> JPEG queue -> decoder thread -> frame ring; PCM goes to
// the audio mixer. The audio clock is the master: late frames are dropped.
#include "../net/net.h"
#include <ctype.h>
#include <unistd.h>

#define BRIDGE "http://10.0.2.2:8765"
typedef struct { char id[24], title[256], channel[128], uploaded[16]; int duration; long views; Image *thumb; volatile int tstate; } VItem;
static VItem res[20];
static volatile int nres, searching;
static char serr[160], query[256] = "";
static int qfocus, screen;           // screen 0 = browse, 1 = player
static Gest g;
static Scroll sc;
static Spring kb_s;

// ---- tiny JSON reader for the bridge's flat result objects ----------------------------------------
static const char *jstr(const char *p, char *out, int n) {   // p at opening quote
    int o = 0;
    for (p++; *p && *p != '"'; p++) {
        int c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            if (*p == 'u') {
                int cp = (int)strtol((char[]){ p[1], p[2], p[3], p[4], 0 }, 0, 16);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {   // surrogate pair
                    int lo = (int)strtol((char[]){ p[3], p[4], p[5], p[6], 0 }, 0, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                char b[4];
                int k = utf8_put(b, cp);
                for (int i = 0; i < k && o < n - 1; i++) out[o++] = b[i];
                continue;
            }
            c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
        }
        if (o < n - 1) out[o++] = c;
    }
    out[o] = 0;
    return *p ? p + 1 : p;
}
static int parse_results(const char *j) {
    int n = 0;
    const char *p = strchr(j, '[');
    while (p && *p && n < 20) {
        p = strchr(p, '{');
        if (!p) break;
        VItem *v = &res[n];
        memset(v, 0, sizeof *v);
        p++;
        while (*p && *p != '}') {
            while (*p && *p != '"' && *p != '}') p++;
            if (*p != '"') break;
            char key[32];
            p = jstr(p, key, sizeof key);
            while (*p == ' ' || *p == ':') p++;
            if (*p == '"') {
                char val[512];
                p = jstr(p, val, sizeof val);
                if (!strcmp(key, "id")) snprintf(v->id, sizeof v->id, "%s", val);
                else if (!strcmp(key, "title")) snprintf(v->title, sizeof v->title, "%s", val);
                else if (!strcmp(key, "channel")) snprintf(v->channel, sizeof v->channel, "%s", val);
                else if (!strcmp(key, "uploaded")) snprintf(v->uploaded, sizeof v->uploaded, "%s", val);
            } else {
                long x = strtol(p, (char **)&p, 10);
                if (!strcmp(key, "duration")) v->duration = x;
                else if (!strcmp(key, "views")) v->views = x;
                while (*p && *p != ',' && *p != '}') p++;
            }
            if (*p == ',') p++;
        }
        if (v->id[0]) n++;
        if (*p) p++;
    }
    return n;
}

static void fmt_time(int s, char *o, int n) {
    if (s < 0) s = 0;
    if (s >= 3600) snprintf(o, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(o, n, "%d:%02d", s / 60, s % 60);
}
static void fmt_views(long v, char *o, int n) {
    if (v >= 1000000) snprintf(o, n, "%.1fM views", v / 1e6);
    else if (v >= 1000) snprintf(o, n, "%.1fK views", v / 1e3);
    else snprintf(o, n, "%ld view%s", v, v == 1 ? "" : "s");
}

// ---- search & thumbnails (worker threads) ----------------------------------------------------------
static void *thumb_th(void *a) {
    VItem *v = a;
    char *u = strfmt(BRIDGE "/thumb?id=%s", v->id);
    HttpResp *r = http_get(u);
    Image *im = r->status == 200 ? image_decode(r->body, r->len) : 0;
    v->thumb = im;
    v->tstate = im ? 2 : 3;
    http_free(r);
    free(u);
    return 0;
}
static void load_thumb(VItem *v) {
    if (v->tstate || !v->id[0]) return;
    v->tstate = 1;
    pthread_t t; pthread_create(&t, 0, thumb_th, v); pthread_detach(t);
}
static void *search_th(void *a) {
    char *q = a, *enc = url_encode(q), *u = strfmt(BRIDGE "/search?q=%s", enc);
    LOG("video: search \"%s\"", q);
    HttpResp *r = http_get(u);
    if (r->status == 200 && r->body) {
        for (int i = 0; i < nres; i++) image_free(res[i].thumb);
        nres = 0;
        int n = parse_results(r->body);
        nres = n;
        LOG("video: %d results, first \"%s\" by %s", n, n ? res[0].title : "", n ? res[0].channel : "");
        serr[0] = 0;
        if (!n) snprintf(serr, sizeof serr, "No results for \"%s\"", q);
    } else if (r->status) snprintf(serr, sizeof serr, "The bridge couldn't search YouTube (HTTP %d).", r->status);
    else snprintf(serr, sizeof serr, "Can't reach the YouTube bridge at 10.0.2.2:8765.");
    http_free(r);
    free(u); free(enc); free(q);
    searching = 0;
    return 0;
}
static void do_search(void) {
    if (searching || !query[0]) return;
    searching = 1; serr[0] = 0; sc = (Scroll){ 0 };
    pthread_t t; pthread_create(&t, 0, search_th, xstrdup(query)); pthread_detach(t);
}

// ---- playback pipeline ---------------------------------------------------------------------------------
#define VQ 150
#define DQ 6
typedef struct { unsigned char *d; int len, pts; } Pkt;
typedef struct { Image *im; int pts; } Frame;
static Pkt vq[VQ];
static Frame dq[DQ];
static int vq_r, vq_w, dq_r, dq_w;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static volatile int gen, ended, reader_err;
static HttpStream *stream;
static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER;   // guards `stream` against abort/close races
static VItem cur;
static int pstate;                 // 0 none, 1 playing, 2 paused, 3 ended
static double base_t;              // stream start (seconds)
static int buffering, prebuf;
static Image *shown;
static int shown_pts = -1;
static long st_shown, st_dropped;
static double st_t, last_save;
static int scrubbing; static float scrub_f;
static Spring fs_s; static int fullscreen;
static double ctl_t;               // controls visible since
static Canvas *land;               // landscape layer for fullscreen

static int readn(HttpStream *s, void *buf, int n) {
    int o = 0;
    while (o < n) { int r = http_read(s, (char *)buf + o, n - o); if (r <= 0) return -1; o += r; }
    return 0;
}

typedef struct { int gen; double t; } StartArg;
static void *reader_th(void *a) {
    StartArg sa = *(StartArg *)a;
    free(a);
    char *u = strfmt(BRIDGE "/stream?id=%s&t=%.2f&h=360", cur.id, sa.t);
    int status = 0;
    char err[128] = "";
    HttpStream *s = http_open(u, &status, err, sizeof err);
    free(u);
    if (!s || status != 200) { LOG("video: stream failed (%d %s)", status, err); if (s) http_close(s); if (sa.gen == gen) reader_err = 1; return 0; }
    pthread_mutex_lock(&smu);
    if (sa.gen != gen) { pthread_mutex_unlock(&smu); http_close(s); return 0; }
    stream = s;
    pthread_mutex_unlock(&smu);
    char magic[4];
    unsigned char hdr[9];
    unsigned char *abuf = malloc(65536);
    long vbytes = 0;
    bool ok = !readn(s, magic, 4) && !memcmp(magic, "LMS1", 4);
    if (!ok && sa.gen == gen) reader_err = 1;
    while (ok && sa.gen == gen && !readn(s, hdr, 9)) {
        int type = hdr[0];
        uint32_t pts, len;
        memcpy(&pts, hdr + 1, 4); memcpy(&len, hdr + 5, 4);
        if (len > (8u << 20)) break;
        if (type == 'A' && len <= 65536) {
            if (readn(s, abuf, len)) break;
            int frames = len / 4, off = 0;
            while (off < frames && sa.gen == gen) {
                int k = audio_stream_push((int16_t *)abuf + off * 2, frames - off);
                off += k;
                if (off < frames) usleep(5000);   // PCM ring full: wait for the speaker
            }
        } else if (type == 'V') {
            unsigned char *d = malloc(len);
            if (readn(s, d, len)) { free(d); break; }
            vbytes += len;
            pthread_mutex_lock(&qmu);
            while (sa.gen == gen && (vq_w - vq_r) >= VQ) {
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 50000000;
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&qcv, &qmu, &ts);
            }
            if (sa.gen == gen) { vq[vq_w % VQ] = (Pkt){ d, (int)len, (int)pts }; vq_w++; pthread_cond_broadcast(&qcv); }
            else free(d);
            pthread_mutex_unlock(&qmu);
        } else if (type == 'E') { if (sa.gen == gen) ended = 1; break; }
        else {   // 'I' or unknown: skip payload
            char tmp[512];
            while (len) { int k = len > sizeof tmp ? (int)sizeof tmp : (int)len; if (readn(s, tmp, k)) break; len -= k; }
        }
    }
    LOG("video: reader done (gen %d, %ld KB video)", sa.gen, vbytes / 1024);
    free(abuf);
    pthread_mutex_lock(&smu);
    if (stream == s) stream = 0;
    pthread_mutex_unlock(&smu);
    http_close(s);
    return 0;
}

static void *decoder_th(void *a) {
    int my = (int)(intptr_t)a;
    double busy = 0, t0 = now();
    int n = 0;
    while (my == gen) {
        pthread_mutex_lock(&qmu);
        while (my == gen && (vq_w == vq_r || dq_w - dq_r >= DQ)) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 50000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&qcv, &qmu, &ts);
        }
        if (my != gen) { pthread_mutex_unlock(&qmu); break; }
        Pkt p = vq[vq_r % VQ];
        vq_r++;
        pthread_cond_broadcast(&qcv);
        pthread_mutex_unlock(&qmu);
        double d0 = now();
        Image *im = image_decode(p.d, p.len);
        busy += now() - d0;
        free(p.d);
        n++;
        if (!im) continue;
        pthread_mutex_lock(&qmu);
        if (my == gen) { dq[dq_w % DQ] = (Frame){ im, p.pts }; dq_w++; im = 0; }
        pthread_mutex_unlock(&qmu);
        image_free(im);
        if (now() - t0 > 10) { LOG("video: decoder %.1f fps, %.1f ms/frame", n / (now() - t0), busy * 1000 / n); t0 = now(); busy = 0; n = 0; }
    }
    return 0;
}

static void flush_queues(void) {
    pthread_mutex_lock(&qmu);
    while (vq_r < vq_w) { free(vq[vq_r % VQ].d); vq_r++; }
    while (dq_r < dq_w) { image_free(dq[dq_r % DQ].im); dq_r++; }
    pthread_cond_broadcast(&qcv);
    pthread_mutex_unlock(&qmu);
}
static void stop_stream(void) {
    pthread_mutex_lock(&smu);
    gen++;
    if (stream) http_abort(stream);
    stream = 0;
    pthread_mutex_unlock(&smu);
    flush_queues();
    audio_stream_reset();
}
static void start_stream(double t) {
    stop_stream();
    if (t < 0) t = 0;
    if (cur.duration && t > cur.duration - 1) t = cur.duration - 1;
    base_t = t;
    ended = 0; reader_err = 0; buffering = 0; prebuf = 1;
    audio_stream_pause(true);
    StartArg *sa = malloc(sizeof *sa);
    sa->gen = gen; sa->t = t;
    pthread_t th;
    pthread_create(&th, 0, reader_th, sa); pthread_detach(th);
    pthread_create(&th, 0, decoder_th, (void *)(intptr_t)gen); pthread_detach(th);
    LOG("video: play %s from %.1f s", cur.id, t);
}
static double clock_s(void) { return base_t + audio_stream_clock(); }

// ---- watch history -----------------------------------------------------------------------------------
static void save_history(double pos) {
    if (!cur.id[0]) return;
    const char *h = store_get("video.history");
    char *out = malloc(8192), *o = out;
    o += snprintf(o, 8192, "%s\t%s\t%s\t%d\t%d\n", cur.id, cur.title, cur.channel, cur.duration, (int)pos);
    int n = 1;
    for (const char *p = h ? h : ""; *p && n < 12;) {
        const char *e = strchr(p, '\n');
        if (!e) break;
        if (strncmp(p, cur.id, strlen(cur.id)) || p[strlen(cur.id)] != '\t') {
            if (o - out + (e - p) + 2 < 8192) { memcpy(o, p, e - p + 1); o += e - p + 1; n++; }
        }
        p = e + 1;
    }
    *o = 0;
    store_set("video.history", out);
    free(out);
}
static int history_get(int k, VItem *v, int *pos) {
    const char *h = store_get("video.history");
    for (const char *p = h ? h : ""; *p;) {
        const char *e = strchr(p, '\n');
        if (!e) break;
        if (k-- == 0) {
            char line[1024];
            snprintf(line, sizeof line, "%.*s", (int)(e - p), p);
            char *f[5] = { 0 }, *save = 0;
            int i = 0;
            for (char *t = strtok_r(line, "\t", &save); t && i < 5; t = strtok_r(0, "\t", &save)) f[i++] = t;
            if (i < 5) return 0;
            memset(v, 0, sizeof *v);
            snprintf(v->id, sizeof v->id, "%s", f[0]);
            snprintf(v->title, sizeof v->title, "%s", f[1]);
            snprintf(v->channel, sizeof v->channel, "%s", f[2]);
            v->duration = atoi(f[3]);
            *pos = atoi(f[4]);
            return 1;
        }
        p = e + 1;
    }
    return 0;
}
const char *video_continue_info(void) {
    static char b[64];
    VItem v; int pos;
    if (!history_get(0, &v, &pos)) return 0;
    int left = v.duration - pos;
    if (left < 30) snprintf(b, sizeof b, "Watched · %s", v.channel);
    else snprintf(b, sizeof b, "Continue · %d min left", (left + 59) / 60);
    return b;
}
bool video_playing(void) { return pstate == 1; }
const char *video_now_title(void) { return pstate == 1 || pstate == 2 ? cur.title : 0; }
void video_toggle_play(void) {
    if (pstate == 1) { pstate = 2; audio_stream_pause(true); save_history(clock_s()); }
    else if (pstate == 2) { pstate = 1; if (!prebuf && !buffering) audio_stream_pause(false); }
    else if (pstate == 3) { pstate = 1; start_stream(0); }
}

static void play(VItem *v, double t) {
    cur = *v;
    cur.thumb = 0; cur.tstate = 0;
    screen = 1; pstate = 1;
    fullscreen = 0; fs_s = (Spring){ 0 };
    if (shown) { image_free(shown); shown = 0; }
    shown_pts = -1;
    st_shown = st_dropped = 0; st_t = now();
    start_stream(t);
    save_history(t);
    last_save = now();
}
static void stop_player(void) {
    if (pstate) save_history(clock_s());
    stop_stream();
    pstate = 0; screen = 0; fullscreen = 0;
}

// ---- per-frame playback update (UI thread) ---------------------------------------------------------------
static void update(void) {
    if (!pstate) return;
    int buffered = audio_stream_buffered();
    if (pstate == 1) {
        if (prebuf) {   // start/seek: wait for ~0.4 s of audio and a decoded frame
            int have;
            pthread_mutex_lock(&qmu); have = dq_w - dq_r; pthread_mutex_unlock(&qmu);
            if ((buffered >= 48000 * 4 / 10 && have >= 2) || (ended && buffered > 0)) { prebuf = 0; audio_stream_pause(false); }
        } else if (!buffering && buffered < 48000 / 40 && !ended) { buffering = 1; audio_stream_pause(true); LOG("video: buffering at %.1f s", clock_s()); }
        else if (buffering && (buffered >= 48000 / 2 || ended)) { buffering = 0; audio_stream_pause(false); }
    }
    // pick the frame for the audio clock; drop late ones
    int clk = (int)(clock_s() * 1000);
    pthread_mutex_lock(&qmu);
    while (dq_w - dq_r > 0) {
        Frame *f = &dq[dq_r % DQ];
        bool has_next = dq_w - dq_r > 1;
        if (f->pts > clk + 5 && shown) break;              // not yet due
        if (has_next && dq[(dq_r + 1) % DQ].pts <= clk) {  // a newer frame is already due: this one is late
            image_free(f->im); dq_r++; st_dropped++;
            continue;
        }
        if (shown) image_free(shown);
        shown = f->im; shown_pts = f->pts; dq_r++; st_shown++;
        if (f->pts <= clk + 5 || !shown) break;
    }
    pthread_cond_broadcast(&qcv);
    int qd = dq_w - dq_r;
    pthread_mutex_unlock(&qmu);
    if (ended && buffered == 0 && qd == 0 && pstate == 1 && !prebuf) {
        pstate = 3; save_history(cur.duration); LOG("video: ended");
    }
    if (now() - st_t >= 10 && pstate == 1) {
        LOG("video: %.1f fps shown, %ld dropped, clock %.2f s, frame lag %d ms, audio buffered %d ms",
            st_shown / (now() - st_t), st_dropped, clock_s(), clk - shown_pts, buffered * 1000 / 48000);
        st_shown = st_dropped = 0; st_t = now();
    }
    if (now() - last_save > 5 && pstate == 1) { save_history(clock_s()); last_save = now(); }
}

// ---- drawing -----------------------------------------------------------------------------------------------
static void spinner(Canvas *c, float cx, float cy, float r, uint32_t col) {
    float a = (float)fmod(now() * 5, 2 * M_PI);
    g_arc(c, cx, cy, r, a, a + 4.4f, P(4), col);
}

static void draw_browse(Canvas *c, float dt) {
    float top = P(176);
    scroll_step(&sc, dt);
    int s[4] = { c->cx0, c->cy0, c->cx1, c->cy1 };
    g_clip(c, 0, top, SW, SH);
    float y = top + P(16) - sc.pos;
    float y0 = y;
    if (searching) {
        spinner(c, P(40), y + P(30), P(14), C_ORANGE);
        text(c, W800, P(22), -0.02f, P(66), y + P(38), TH.fg, "Searching YouTube…");
        y += P(70);
    } else if (serr[0]) {
        g_rrect(c, P(16), y, P(358), P(200), P(28), P(28), C_LILAC);
        text(c, W800, P(26), -0.03f, P(32), y + P(48), C_INK, strstr(serr, "reach") ? "Bridge offline" : "Hmm.");
        int st_[8], ln[8];
        int nl = text_wrap(W400, P(15), serr, P(320), st_, ln, 8);
        for (int i = 0; i < nl; i++) text_n(c, W400, P(15), 0, P(32), y + P(80) + i * P(21), C_INK, serr + st_[i], ln[i]);
        if (strstr(serr, "reach")) text(c, W400, P(13), 0, P(32), y + P(80) + nl * P(21) + P(4), C_INK, "Start it with ./run.sh on the Mac.");
        btn_text(c, 0x8001, P(32), y + P(136), P(140), P(48), "Retry", C_PAPER, C_INK);
        y += P(220);
    }
    if (nres && !searching) {
        text(c, W600, P(13), 0.12f, P(22), y + P(14), TH.sub, "RESULTS");
        y += P(26);
        for (int i = 0; i < nres; i++) {
            VItem *v = &res[i];
            float rh = P(112);
            if (y + rh > top && y < SH) {
                load_thumb(v);
                bool pr = pressed(0x8100 + i);
                if (pr) g_rrect(c, P(8), y - P(4), P(374), rh, P(20), P(20), TH.line);
                float tw = P(160), th = P(90);
                if (v->thumb) g_image_cover(c, v->thumb, P(16), y, tw, th, P(14), 255);
                else g_rrect(c, P(16), y, tw, th, P(14), P(14), TH.line);
                char d[16];
                fmt_time(v->duration, d, sizeof d);
                float dw = text_w(W600, P(12), 0, d) + P(12);
                g_rrect(c, P(16) + tw - dw - P(6), y + th - P(24), dw, P(18), P(6), P(6), C_INK);
                text(c, W600, P(12), 0, P(16) + tw - dw, y + th - P(10), C_PAPER, d);
                int st_[3], ln[3];
                int nl = text_wrap(W600, P(15), v->title, P(190), st_, ln, 3);
                if (nl > 2) nl = 2;
                for (int k = 0; k < nl; k++) text_n(c, W600, P(15), 0, P(186), y + P(17) + k * P(20), TH.fg, v->title + st_[k], ln[k]);
                char *ch = text_ellipsize(W400, P(13), v->channel, P(190));
                text(c, W400, P(13), 0, P(186), y + P(17) + nl * P(20) + P(4), TH.sub, ch);
                free(ch);
                char vw[32];
                fmt_views(v->views, vw, sizeof vw);
                text(c, W400, P(13), 0, P(186), y + P(17) + nl * P(20) + P(22), TH.sub, vw);
                hit(0x8100 + i, 0, y, SW, rh - P(8));
            }
            y += rh;
        }
    } else if (!searching && !serr[0]) {
        text(c, W600, P(13), 0.12f, P(22), y + P(14), TH.sub, "RECENTLY WATCHED");
        y += P(30);
        VItem v; int pos, k;
        for (k = 0; k < 8 && history_get(k, &v, &pos); k++) {
            bool pr = pressed(0x8200 + k);
            g_rrect(c, P(16), y, P(358), P(84), P(22), P(22), pr ? C_LIME : k == 0 ? C_ORANGE : C_PAPER);
            char *t = text_ellipsize(W800, P(17), v.title, P(320));
            text(c, W800, P(17), -0.01f, P(32), y + P(32), C_INK, t);
            free(t);
            char info[160], a[16], b[16];
            fmt_time(pos, a, sizeof a); fmt_time(v.duration, b, sizeof b);
            snprintf(info, sizeof info, "%s  ·  %s / %s", v.channel, a, b);
            char *ii = text_ellipsize(W400, P(13), info, P(320));
            text(c, W400, P(13), 0, P(32), y + P(54), C_INK, ii);
            free(ii);
            g_rrect(c, P(32), y + P(66), P(326), P(5), P(2.5f), P(2.5f), c_alpha(C_INK, 0.2f));
            g_rrect(c, P(32), y + P(66), P(326) * clampf(v.duration ? (float)pos / v.duration : 0, 0.02f, 1), P(5), P(2.5f), P(2.5f), C_INK);
            hit(0x8200 + k, P(16), y, P(358), P(84));
            y += P(96);
        }
        if (!k) {
            text(c, W800, P(34), -0.04f, P(20), y + P(40), TH.sub, "Nothing yet.");
            text(c, W400, P(15), 0, P(22), y + P(70), TH.sub, "Search YouTube above. Videos are decoded");
            text(c, W400, P(15), 0, P(22), y + P(90), TH.sub, "frame by frame right here on the phone.");
        }
    }
    sc.max = (y + sc.pos) - y0 - (SH - top) + P(60);
    c->cx0 = s[0]; c->cy0 = s[1]; c->cx1 = s[2]; c->cy1 = s[3];
    // header
    g_rect(c, 0, 0, SW, top, C_ORANGE);
    status_line(c, 0, C_INK, true, false);
    text(c, W800, P(52), -0.05f, P(18), P(100), C_INK, "Video");
    text(c, W600, P(13), 0, P(160), P(98), C_INK, "YouTube via the Mac bridge");
    field_draw(c, P(16), P(114), P(358), P(50), query, "Search YouTube", qfocus, C_PAPER, C_INK);
    if (!qfocus) icon(c, IC_SEARCH, P(338), P(127), P(24), C_INK);
    hit(0x8002, P(16), P(114), P(358), P(50));
    kb_s.t = qfocus;
    spring_step(&kb_s, dt, 3.2f, 0.9f);
    if (kb_s.x > 0.01f) kb_draw(c, SH - kb_height() * clampf(kb_s.x, 0, 1.05f), "search");
}

static void draw_video_into(Canvas *c, float x, float y, float w, float h) {
    g_rect(c, x, y, w, h, C_BLACK);
    if (shown) g_image(c, shown, x, y, w, h, 0, 255);
    if (pstate == 1 && (prebuf || buffering)) {
        g_circle(c, x + w / 2, y + h / 2, P(30), 0x88141414);
        spinner(c, x + w / 2, y + h / 2, P(18), C_PAPER);
    }
    if (reader_err) {
        g_rect(c, x, y, w, h, 0xCC141414);
        text_c(c, W800, P(18), 0, x + w / 2, y + h / 2, C_PAPER, "Stream failed. Tap play to retry.");
    }
}

static void scrub_bar(Canvas *c, int id, float x, float y, float w, float pos, float dur, uint32_t track, uint32_t fill) {
    float f = scrubbing && id == 0x8300 + fullscreen ? scrub_f : (dur > 0 ? clampf(pos / dur, 0, 1) : 0);
    g_rrect(c, x, y, w, P(10), P(5), P(5), track);
    g_rrect(c, x, y, fmaxf(P(10), w * f), P(10), P(5), P(5), fill);
    g_circle(c, x + w * f, y + P(5), scrubbing ? P(14) : P(10), fill);
    hit(id, x - P(10), y - P(18), w + P(20), P(46));
}

static int land_pts = -2;
static bool land_ctl;
static void draw_landscape(void) {
    Canvas *L = land;
    bool ctl = now() - ctl_t < 3.5 || pstate != 1 || scrubbing;
    // only redraw when a new frame arrived or the overlay is up (it shows a running clock)
    if (shown_pts == land_pts && !ctl && !land_ctl && !prebuf && !buffering) return;
    land_pts = shown_pts; land_ctl = ctl;
    g_rect(L, 0, 0, 140, 720, C_BLACK);
    g_rect(L, 1420, 0, 140, 720, C_BLACK);
    draw_video_into(L, (1560 - 1280) / 2, 0, 1280, 720);
    if (ctl) {
        g_rect(L, 0, 0, 1560, P(56), 0x99141414);
        g_rect(L, 0, 720 - P(86), 1560, P(86), 0x99141414);
        char *t = text_ellipsize(W800, P(18), cur.title, 1100);
        text(L, W800, P(18), -0.02f, P(24), P(36), C_PAPER, t);
        free(t);
        btn_icon(L, 0x8402, 1560 - P(64), P(6), P(44), IC_UNFULL, C_PAPER, 0);
        btn_icon(L, 0x8403, 780 - P(36), 360 - P(36), P(72), pstate == 1 ? IC_PAUSE : IC_PLAY_FILL, C_INK, C_ORANGE);
        double pos = scrubbing ? scrub_f * cur.duration : clock_s();
        char a[16], b[16];
        fmt_time((int)pos, a, sizeof a); fmt_time(cur.duration, b, sizeof b);
        text(L, W600, P(14), 0, P(24), 720 - P(26), C_PAPER, a);
        text_r(L, W600, P(14), 0, 1560 - P(24), 720 - P(26), C_PAPER, b);
        scrub_bar(L, 0x8301, P(90), 720 - P(36), 1560 - P(180), pos, cur.duration, 0x55F3F0E8, C_ORANGE);
    }
}

// screen -> landscape layer coordinates for the current rotation
static void fs_xform(float m[6], float *bb) {
    float u = clampf(fs_s.x, 0, 1);
    float th = u * (float)M_PI / 2, s0 = (float)SW / 1280, s = lerpf(s0, 1, u);
    float cx = lerpf(SW / 2.0f, SW / 2.0f, u), cy = lerpf(P(44) + SW * 9 / 32.0f, SH / 2.0f, u);
    float co = cosf(th), si = sinf(th);
    m[0] = co / s; m[1] = si / s; m[2] = 780 - (co * cx + si * cy) / s;
    m[3] = -si / s; m[4] = co / s; m[5] = 360 - (-si * cx + co * cy) / s;
    // screen bounding box of the rotated layer
    float xs[4] = { -780, 780, 780, -780 }, ys[4] = { -360, -360, 360, 360 };
    bb[0] = bb[1] = 1e9f; bb[2] = bb[3] = -1e9f;
    for (int i = 0; i < 4; i++) {
        float X = cx + s * (co * xs[i] - si * ys[i]), Y = cy + s * (si * xs[i] + co * ys[i]);
        bb[0] = fminf(bb[0], X); bb[1] = fminf(bb[1], Y); bb[2] = fmaxf(bb[2], X); bb[3] = fmaxf(bb[3], Y);
    }
}

static void draw_player(Canvas *c, float dt) {
    g_clear(c, TH.bg);
    float vy = P(44), vh = SW * 9 / 16.0f;
    g_rect(c, 0, 0, SW, vy, C_BLACK);
    status_line(c, 0, C_PAPER, false, false);
    btn_icon(c, 0x8401, P(10), P(2), P(40), IC_BACK, C_PAPER, 0);
    draw_video_into(c, 0, vy, SW, vh);
    hit(0x8405, 0, vy, SW, vh);
    float y = vy + vh + P(18);
    double pos = scrubbing && !fullscreen ? scrub_f * cur.duration : clock_s();
    scrub_bar(c, 0x8300, P(16), y, P(358), pos, cur.duration, TH.line, C_ORANGE);
    char a[16], b[16];
    fmt_time((int)pos, a, sizeof a); fmt_time(cur.duration, b, sizeof b);
    text(c, W600, P(13), 0, P(16), y + P(34), TH.sub, a);
    text_r(c, W600, P(13), 0, P(374), y + P(34), TH.sub, b);
    y += P(60);
    int st_[3], ln[3];
    int nl = text_wrap(W800, P(25), cur.title, P(358), st_, ln, 3);
    for (int i = 0; i < nl; i++) text_n(c, W800, P(25), -0.03f, P(16), y + P(24) + i * P(30), TH.fg, cur.title + st_[i], ln[i]);
    y += nl * P(30) + P(12);
    char vw[32], info[200];
    fmt_views(cur.views, vw, sizeof vw);
    snprintf(info, sizeof info, cur.views ? "%s  ·  %s" : "%s", cur.channel, vw);
    text(c, W600, P(15), 0, P(16), y + P(14), TH.sub, info);
    y += P(44);
    // controls slab
    g_rrect(c, P(16), y, P(358), P(110), P(30), P(30), C_ORANGE);
    btn_icon(c, 0x8406, P(36), y + P(29), P(52), IC_BACK, C_INK, 0);
    text_c(c, W800, P(11), 0, P(62), y + P(96), C_INK, "10 s");
    btn_icon(c, 0x8403, P(195) - P(40), y + P(15), P(80), pstate == 1 ? IC_PAUSE : IC_PLAY_FILL, C_ORANGE, C_INK);
    btn_icon(c, 0x8407, P(254), y + P(29), P(52), IC_FWD, C_INK, 0);
    text_c(c, W800, P(11), 0, P(280), y + P(96), C_INK, "10 s");
    btn_icon(c, 0x8404, P(312), y + P(29), P(52), IC_FULL, C_INK, 0);
    y += P(130);
    text(c, W600, P(12), 0.1f, P(16), y, TH.sub, "DECODED ON THE PHONE");
    text(c, W400, P(13), 0, P(16), y + P(20), TH.sub, "MJPEG 640x360 @ 30 fps + PCM 48 kHz from the Mac bridge.");
    text(c, W400, P(13), 0, P(16), y + P(38), TH.sub, "Audio clock is master; late frames are dropped.");
    home_bar(c, TH.fg);
    // fullscreen: our compositor rotates a landscape layer with an animation
    fs_s.t = fullscreen;
    spring_step(&fs_s, dt, 1.9f, 0.86f);
    if (fs_s.x > 0.005f) {
        if (!land) land = canvas_new(1560, 720);
        draw_landscape();
        g_rect(c, 0, 0, SW, SH, c_alpha(C_BLACK, clampf(fs_s.x * 1.5f, 0, 1)));
        if (fs_s.x > 0.999f && fabsf(fs_s.v) < 0.01f) g_rot90(c, land);   // at rest: exact quarter turn
        else {
            float m[6], bb[4];
            fs_xform(m, bb);
            g_layer_fast(c, land, m, bb[0], bb[1], bb[2], bb[3]);
        }
    }
}

static void open_(void) { qfocus = 0; if (pstate == 0) screen = 0; }

void video_tick(void) { update(); }   // called by the shell every frame, also in the background
static void draw(Canvas *c, float dt) {
    if (screen == 1) draw_player(c, dt);
    else { g_clear(c, TH.bg); draw_browse(c, dt); home_bar(c, TH.fg); }
}

static void seek_to(double t) {
    if (pstate == 3) pstate = 1;
    start_stream(t);
    if (pstate == 2) audio_stream_pause(true);
}

static void touch(InputEv *e) {
    float x = e->x, y = e->y;
    if (screen == 1 && fullscreen && fs_s.x > 0.9f) { float t = x; x = y; y = SW - t; }   // into landscape coordinates
    InputEv le = *e;
    le.x = x; le.y = y;
    int r = gest(&g, &le);
    if (screen == 1) {
        if (r == G_DOWN) {
            int id = hit_at(x, y);
            if (id == 0x8300 + fullscreen) { scrubbing = 1; ctl_t = now(); }
        }
        if (scrubbing) {
            float x0 = fullscreen ? P(90) : P(16), w = fullscreen ? 1560 - P(180) : P(358);
            scrub_f = clampf((x - x0) / w, 0, 1);
            if (r == G_DRAG_END || r == G_TAP) {
                scrubbing = 0;
                LOG("video: seek to %.1f s", scrub_f * cur.duration);
                seek_to(scrub_f * cur.duration);
            }
            return;
        }
        if (r != G_TAP) return;
        int id = hit_at(x, y);
        if (fullscreen) {
            bool vis = now() - ctl_t < 3.5;
            ctl_t = now();
            if (!vis && id != 0x8402) return;   // first tap only reveals the controls
        }
        switch (id) {
        case 0x8401: stop_player(); break;
        case 0x8402: fullscreen = 0; LOG("video: exit fullscreen"); break;
        case 0x8403: if (reader_err) seek_to(clock_s()); else video_toggle_play(); break;
        case 0x8404: fullscreen = 1; ctl_t = now(); LOG("video: fullscreen"); break;
        case 0x8406: seek_to(clock_s() - 10); break;
        case 0x8407: seek_to(clock_s() + 10); break;
        }
        return;
    }
    // browse screen
    if (qfocus && e->y > SH - kb_height()) {
        if (r == G_TAP) { int k = kb_tap(e->x, e->y); if (k) { if (field_key(query, sizeof query, k)) { qfocus = 0; do_search(); } } }
        return;
    }
    if (r == G_DRAG_START || r == G_DRAG) { if (g.vert) scroll_drag(&sc, r == G_DRAG_START ? g.y - g.y0 : g.dy); return; }
    if (r == G_DRAG_END) { if (sc.held) scroll_fling(&sc, g.vy); return; }
    if (r != G_TAP) return;
    int id = hit_at(e->x, e->y);
    if (id == 0x8002) { qfocus = 1; return; }
    qfocus = 0;
    if (id == 0x8001) do_search();
    else if (id >= 0x8100 && id < 0x8100 + nres) play(&res[id - 0x8100], 0);
    else if (id >= 0x8200 && id < 0x8210) {
        VItem v; int pos;
        if (history_get(id - 0x8200, &v, &pos)) play(&v, pos >= v.duration - 5 ? 0 : pos);
    }
}

static void key(int k) {
    if (screen == 0) {
        if (!qfocus && k > 32 && k < 127) qfocus = 1;
        if (qfocus && field_key(query, sizeof query, k)) { qfocus = 0; do_search(); }
        return;
    }
    if (k == ' ') video_toggle_play();
    else if (k == 'f') fullscreen = !fullscreen;
    else if (k == K_RIGHT) seek_to(clock_s() + 10);
    else if (k == K_LEFT) seek_to(clock_s() - 10);
    else if (k == K_ESC || k == K_BACKSPACE) { if (fullscreen) fullscreen = 0; else stop_player(); }
}

App app_video = { "Video", IC_VIDEO, open_, draw, touch, key, 0 };
