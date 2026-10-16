// HTTP/1.1 client: keep-alive pool, chunked transfer, Content-Length, redirects,
// in-memory cookie jar, gzip fallback, HTTPS through BearSSL with the Mozilla CA
// bundle compiled in as trust anchors.
#include "net.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "bearssl.h"

extern const br_x509_trust_anchor *lumen_tas;
extern const size_t lumen_tas_num;
#define UA "Mozilla/5.0 (Linux; Lumen OS 2; Mobile) LumenWeb/2.0"

// ---- URLs ------------------------------------------------------------------------------------
int url_parse(const char *s, Url *u) {
    memset(u, 0, sizeof *u);
    while (*s == ' ') s++;
    const char *p = strstr(s, "://");
    if (!p) return -1;
    snprintf(u->scheme, sizeof u->scheme, "%.*s", (int)(p - s), s);
    for (char *c = u->scheme; *c; c++) *c = tolower(*c);
    p += 3;
    const char *hend = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', hend - p);
    if (at) p = at + 1;
    const char *colon = memchr(p, ':', hend - p);
    snprintf(u->host, sizeof u->host, "%.*s", (int)((colon ? colon : hend) - p), p);
    for (char *c = u->host; *c; c++) *c = tolower(*c);
    u->port = colon ? atoi(colon + 1) : !strcmp(u->scheme, "https") ? 443 : 80;
    const char *path = hend;
    int pl = strcspn(path, "#");
    snprintf(u->path, sizeof u->path, "%s%.*s", *path == '/' ? "" : "/", pl, path);
    if (strcmp(u->scheme, "http") && strcmp(u->scheme, "https")) return -1;
    return u->host[0] ? 0 : -1;
}

static void normalize_path(char *path) {   // resolve ./ and ../ segments
    char *q = strchr(path, '?');
    char query[4096] = "";
    if (q) { snprintf(query, sizeof query, "%s", q); *q = 0; }
    char *seg[256];
    int n = 0;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    int trailing = buf[0] && buf[strlen(buf) - 1] == '/';
    for (char *t = strtok(buf, "/"); t; t = strtok(0, "/")) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (n) n--; continue; }
        if (n < 256) seg[n++] = t;
    }
    char out[4096] = "";
    int o = 0;
    for (int i = 0; i < n; i++) o += snprintf(out + o, sizeof out - o, "/%s", seg[i]);
    if (!n || trailing) o += snprintf(out + o, sizeof out - o, "/");
    snprintf(path, 4096, "%s%s", out, query);
}

char *url_resolve(const char *base, const char *rel) {
    while (*rel == ' ' || *rel == '\n' || *rel == '\t') rel++;
    if (strstr(rel, "://") && strcspn(rel, ":") < strcspn(rel, "/?#")) return xstrdup(rel);
    Url b;
    if (url_parse(base, &b)) return xstrdup(rel);
    char port[16] = "";
    if (!((b.port == 80 && !strcmp(b.scheme, "http")) || (b.port == 443 && !strcmp(b.scheme, "https")))) snprintf(port, sizeof port, ":%d", b.port);
    if (rel[0] == '/' && rel[1] == '/') return strfmt("%s:%s", b.scheme, rel);
    char path[4096];
    if (rel[0] == '/') snprintf(path, sizeof path, "%s", rel);
    else if (rel[0] == '?') { snprintf(path, sizeof path, "%.*s%s", (int)strcspn(b.path, "?"), b.path, rel); }
    else if (rel[0] == '#' || !rel[0]) snprintf(path, sizeof path, "%s", b.path);
    else {
        int dir = strcspn(b.path, "?");
        while (dir > 0 && b.path[dir - 1] != '/') dir--;
        snprintf(path, sizeof path, "%.*s%s", dir, b.path, rel);
    }
    char *h = strchr(path, '#');
    if (h) *h = 0;
    normalize_path(path);
    return strfmt("%s://%s%s%s", b.scheme, b.host, port, path);
}

char *url_encode(const char *s) {
    char *o = malloc(strlen(s) * 3 + 1), *p = o;
    for (; *s; s++) {
        unsigned char c = *s;
        if (isalnum(c) || strchr("-_.~", c)) *p++ = c;
        else if (c == ' ') *p++ = '+';
        else p += sprintf(p, "%%%02X", c);
    }
    *p = 0;
    return o;
}

// ---- cookies ------------------------------------------------------------------------------------
typedef struct { char domain[128], path[128], name[128]; char *value; } Cookie;
static Cookie jar[256];
static int njar;
static pthread_mutex_t jar_mu = PTHREAD_MUTEX_INITIALIZER;

static void cookie_set(const char *host, const char *hdr) {
    char name[128] = "", domain[128], path[128] = "/";
    const char *eq = strchr(hdr, '='), *semi = strchr(hdr, ';');
    if (!eq || (semi && semi < eq)) return;
    snprintf(name, sizeof name, "%.*s", (int)(eq - hdr), hdr);
    int vl = semi ? semi - eq - 1 : (int)strlen(eq + 1);
    snprintf(domain, sizeof domain, "%s", host);
    bool del = false;
    for (const char *a = semi; a && *a; a = strchr(a + 1, ';')) {
        const char *k = a + 1;
        while (*k == ' ') k++;
        if (!strncasecmp(k, "domain=", 7)) { k += 7; if (*k == '.') k++; snprintf(domain, sizeof domain, "%.*s", (int)strcspn(k, ";"), k); }
        else if (!strncasecmp(k, "path=", 5)) snprintf(path, sizeof path, "%.*s", (int)strcspn(k + 5, ";"), k + 5);
        else if (!strncasecmp(k, "max-age=", 8) && atoi(k + 8) <= 0) del = true;
    }
    pthread_mutex_lock(&jar_mu);
    int i;
    for (i = 0; i < njar; i++) if (!strcmp(jar[i].name, name) && !strcmp(jar[i].domain, domain) && !strcmp(jar[i].path, path)) break;
    if (i == njar && njar < 256) njar++;
    if (i < 256) {
        free(jar[i].value);
        snprintf(jar[i].domain, sizeof jar[i].domain, "%s", domain);
        snprintf(jar[i].path, sizeof jar[i].path, "%s", path);
        snprintf(jar[i].name, sizeof jar[i].name, "%s", name);
        jar[i].value = del ? xstrdup("") : strndup(eq + 1, vl);
        if (del) jar[i].name[0] = 0;
    }
    pthread_mutex_unlock(&jar_mu);
}
static void cookie_header(const Url *u, char *out, int n) {
    int o = 0;
    out[0] = 0;
    pthread_mutex_lock(&jar_mu);
    for (int i = 0; i < njar; i++) {
        Cookie *c = &jar[i];
        if (!c->name[0]) continue;
        int hl = strlen(u->host), dl = strlen(c->domain);
        bool dm = !strcmp(u->host, c->domain) || (hl > dl && !strcmp(u->host + hl - dl, c->domain) && u->host[hl - dl - 1] == '.');
        if (!dm || strncmp(u->path, c->path, strlen(c->path))) continue;
        o += snprintf(out + o, n - o, "%s%s=%s", o ? "; " : "", c->name, c->value);
        if (o >= n) { out[n - 1] = 0; break; }
    }
    pthread_mutex_unlock(&jar_mu);
}

// ---- connections (plain or TLS), with a keep-alive pool ----------------------------------------------------
typedef struct Conn {
    int fd, tls, port;
    char host[256];
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    unsigned char rb[16384];
    int rpos, rlen;
    double idle_since;
} Conn;
static Conn *pool[8];
static pthread_mutex_t pool_mu = PTHREAD_MUTEX_INITIALIZER;

static int sock_rd(void *ctx, unsigned char *b, size_t n) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t r = read(fd, b, n);
        if (r <= 0 && errno == EINTR) continue;
        return r <= 0 ? -1 : (int)r;
    }
}
static int sock_wr(void *ctx, const unsigned char *b, size_t n) {
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t r = write(fd, b, n);
        if (r <= 0 && errno == EINTR) continue;
        return r <= 0 ? -1 : (int)r;
    }
}

static void conn_free(Conn *c) { if (c) { if (c->fd >= 0) close(c->fd); free(c); } }

static Conn *conn_open(const Url *u, char *err, int errn) {
    uint32_t ip;
    if (net_state() != NET_UP) { snprintf(err, errn, "No network connection"); return 0; }
    if (dns_resolve(u->host, &ip)) { snprintf(err, errn, "Can't find %s", u->host); return 0; }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(u->port), .sin_addr.s_addr = ip };
    if (connect(fd, (struct sockaddr *)&a, sizeof a) && errno != EINPROGRESS) { close(fd); snprintf(err, errn, "Connect failed"); return 0; }
    struct pollfd pf = { fd, POLLOUT, 0 };
    int soerr = 0;
    socklen_t sl = sizeof soerr;
    if (poll(&pf, 1, 10000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) || soerr) {
        close(fd);
        snprintf(err, errn, "Can't connect to %s (%s)", u->host, soerr ? strerror(soerr) : "timeout");
        return 0;
    }
    fcntl(fd, F_SETFL, 0);   // back to blocking with timeouts
    struct timeval tv = { 20, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    Conn *c = calloc(1, sizeof *c);
    c->fd = fd;
    c->port = u->port;
    c->tls = !strcmp(u->scheme, "https");
    snprintf(c->host, sizeof c->host, "%s", u->host);
    if (c->tls) {
        br_ssl_client_init_full(&c->sc, &c->xc, lumen_tas, lumen_tas_num);
        br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
        br_ssl_client_reset(&c->sc, u->host, 0);
        br_sslio_init(&c->io, &c->sc.eng, sock_rd, &c->fd, sock_wr, &c->fd);
        // drive the handshake now so certificate errors are reported clearly
        if (br_sslio_flush(&c->io) < 0 || (br_ssl_engine_current_state(&c->sc.eng) & BR_SSL_CLOSED)) {
            int e = br_ssl_engine_last_error(&c->sc.eng);
            snprintf(err, errn, e >= 32 && e < 64 ? "Certificate not trusted (BearSSL %d)" : "TLS handshake failed (BearSSL %d)", e);
            LOG("tls: %s: %s", u->host, err);
            conn_free(c);
            return 0;
        }
    }
    return c;
}

static Conn *conn_get(const Url *u, char *err, int errn, int *reused) {
    int tls = !strcmp(u->scheme, "https");
    pthread_mutex_lock(&pool_mu);
    for (int i = 0; i < 8; i++) {
        Conn *c = pool[i];
        if (c && c->tls == tls && c->port == u->port && !strcmp(c->host, u->host)) {
            pool[i] = 0;
            pthread_mutex_unlock(&pool_mu);
            if (now() - c->idle_since > 30) { conn_free(c); break; }
            *reused = 1;
            return c;
        }
    }
    pthread_mutex_unlock(&pool_mu);
    *reused = 0;
    return conn_open(u, err, errn);
}
static void conn_put(Conn *c) {
    c->idle_since = now();
    pthread_mutex_lock(&pool_mu);
    int slot = -1;
    for (int i = 0; i < 8; i++) if (!pool[i]) { slot = i; break; }
    if (slot < 0) { slot = 0; conn_free(pool[0]); }
    pool[slot] = c;
    pthread_mutex_unlock(&pool_mu);
}

static int conn_write(Conn *c, const void *b, size_t n) {
    if (c->tls) {
        if (br_sslio_write_all(&c->io, b, n) < 0) return -1;
        return br_sslio_flush(&c->io);
    }
    const char *p = b;
    while (n) {
        ssize_t r = write(c->fd, p, n);
        if (r <= 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= r;
    }
    return 0;
}
static int conn_fill(Conn *c) {
    c->rpos = 0;
    c->rlen = c->tls ? br_sslio_read(&c->io, c->rb, sizeof c->rb) : (int)read(c->fd, c->rb, sizeof c->rb);
    if (c->rlen < 0) c->rlen = 0;
    return c->rlen;
}
static int conn_read(Conn *c, void *buf, int n) {
    if (c->rpos >= c->rlen && conn_fill(c) <= 0) return 0;
    int k = mini(n, c->rlen - c->rpos);
    memcpy(buf, c->rb + c->rpos, k);
    c->rpos += k;
    return k;
}
static int conn_line(Conn *c, char *out, int n) {   // reads a CRLF line
    int o = 0;
    for (;;) {
        if (c->rpos >= c->rlen && conn_fill(c) <= 0) return o ? o : -1;
        char ch = c->rb[c->rpos++];
        if (ch == '\n') break;
        if (ch != '\r' && o < n - 1) out[o++] = ch;
    }
    out[o] = 0;
    return o;
}

// ---- requests ---------------------------------------------------------------------------------------------
struct HttpStream {
    Conn *c;
    int chunked, keep;
    long remaining;    // content-length or chunk remaining; -1 = until close
    int done;
    Url u;
};

static __thread const char *ua_override;
void http_set_ua(const char *ua) { ua_override = ua; }
static __thread http_progress_fn prog_fn;
static __thread void *prog_ctx;
void http_set_progress(http_progress_fn fn, void *ctx) { prog_fn = fn; prog_ctx = ctx; }

typedef struct { int status; char ctype[128], location[2048], encoding[32]; long clen; int chunked, close; } Head;

// send one request and read the response head; returns stream positioned at the body
static HttpStream *send_req(const char *method, const Url *u, const char *body, const char *ctype, Head *h, char *err, int errn) {
    for (int attempt = 0; attempt < 2; attempt++) {
        int reused = 0;
        Conn *c = conn_get(u, err, errn, &reused);
        if (!c) return 0;
        char cookies[4096];
        cookie_header(u, cookies, sizeof cookies);
        char *req = strfmt("%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
                           "Accept-Language: en-US,en;q=0.9\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n%s%s%s%s%s%s\r\n",
                           method, u->path, u->host, ua_override ? ua_override : UA,
                           cookies[0] ? "Cookie: " : "", cookies, cookies[0] ? "\r\n" : "",
                           ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "");
        if (body) { char *r2 = strfmt("%.*sContent-Length: %zu\r\n\r\n%s", (int)strlen(req) - 2, req, strlen(body), body); free(req); req = r2; }
        int w = conn_write(c, req, strlen(req));
        free(req);
        char line[8192];
        if (w < 0 || conn_line(c, line, sizeof line) < 0 || strncmp(line, "HTTP/", 5)) {
            conn_free(c);
            if (reused) continue;   // stale keep-alive connection: retry on a fresh one
            snprintf(err, errn, "No response from %s", u->host);
            return 0;
        }
        memset(h, 0, sizeof *h);
        h->status = atoi(strchr(line, ' ') ? strchr(line, ' ') + 1 : "0");
        h->clen = -1;
        if (!strncmp(line, "HTTP/1.0", 8)) h->close = 1;
        while (conn_line(c, line, sizeof line) > 0) {
            char *v = strchr(line, ':');
            if (!v) continue;
            *v++ = 0;
            while (*v == ' ') v++;
            if (!strcasecmp(line, "content-length")) h->clen = atol(v);
            else if (!strcasecmp(line, "transfer-encoding") && strcasestr(v, "chunked")) h->chunked = 1;
            else if (!strcasecmp(line, "content-type")) snprintf(h->ctype, sizeof h->ctype, "%s", v);
            else if (!strcasecmp(line, "location")) snprintf(h->location, sizeof h->location, "%s", v);
            else if (!strcasecmp(line, "content-encoding")) snprintf(h->encoding, sizeof h->encoding, "%s", v);
            else if (!strcasecmp(line, "connection") && strcasestr(v, "close")) h->close = 1;
            else if (!strcasecmp(line, "set-cookie")) cookie_set(u->host, v);
        }
        HttpStream *s = calloc(1, sizeof *s);
        s->c = c;
        s->u = *u;
        s->chunked = h->chunked;
        s->keep = !h->close;
        s->remaining = h->chunked ? 0 : h->clen;
        if (!strcmp(method, "HEAD") || h->status == 204 || h->status == 304 || (h->status >= 100 && h->status < 200)) { s->remaining = 0; s->done = 1; }
        else if (!h->chunked && h->clen < 0) s->keep = 0;
        if (!h->chunked && h->clen == 0) s->done = 1;
        return s;
    }
    return 0;
}

int http_read(HttpStream *s, void *buf, int n) {
    if (s->done) return 0;
    if (s->chunked && s->remaining == 0) {
        char line[64];
        int l;
        do l = conn_line(s->c, line, sizeof line); while (l == 0);   // CRLF after the previous chunk
        if (l < 0) { s->done = 1; s->keep = 0; return -1; }
        s->remaining = strtol(line, 0, 16);
        if (s->remaining <= 0) {   // last chunk: skip trailers
            while (conn_line(s->c, line, sizeof line) > 0) {}
            s->done = 1;
            return 0;
        }
    }
    if (s->remaining > 0 && n > s->remaining) n = s->remaining;
    int r = conn_read(s->c, buf, n);
    if (r <= 0) { s->done = 1; s->keep = 0; return s->remaining < 0 ? 0 : -1; }
    if (s->remaining > 0) {
        s->remaining -= r;
        if (s->remaining == 0 && !s->chunked) s->done = 1;
    }
    return r;
}

void http_abort(HttpStream *s) { if (s) shutdown(s->c->fd, SHUT_RDWR); }   // unblocks a reader on another thread
void http_close(HttpStream *s) {
    if (!s) return;
    if (s->done && s->keep) conn_put(s->c);
    else conn_free(s->c);
    free(s);
}

HttpResp *http_request(const char *method, const char *url, const char *body, const char *ctype, size_t max_body) {
    HttpResp *r = calloc(1, sizeof *r);
    double t0 = now();
    char *cur = xstrdup(url);
    char *m = xstrdup(method);
    const char *b = body;
    for (int hop = 0; hop <= 5; hop++) {
        Url u;
        if (url_parse(cur, &u)) { snprintf(r->err, sizeof r->err, "Bad address"); break; }
        Head h;
        HttpStream *s = send_req(m, &u, b, b ? ctype : 0, &h, r->err, sizeof r->err);
        if (!s) break;
        r->status = h.status;
        if (h.status >= 300 && h.status < 400 && h.location[0] && hop < 5) {
            char tmp[1024];
            while (http_read(s, tmp, sizeof tmp) > 0) {}
            http_close(s);
            char *next = url_resolve(cur, h.location);
            LOG("http: %d %s -> %s", h.status, cur, next);
            free(cur);
            cur = next;
            if (h.status == 303 || ((h.status == 301 || h.status == 302) && strcmp(m, "GET"))) { free(m); m = xstrdup("GET"); b = 0; }
            continue;
        }
        size_t cap = h.clen > 0 ? (size_t)h.clen + 1 : 65536, len = 0;
        char *buf = malloc(cap);
        for (;;) {
            if (len + 16384 + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
            int k = http_read(s, buf + len, 16384);
            if (k <= 0) break;
            len += k;
            if (prog_fn) prog_fn(prog_ctx, len, h.clen > 0 ? (size_t)h.clen : 0);
            if (max_body && len >= max_body) { s->keep = 0; break; }
        }
        http_close(s);
        buf[len] = 0;
        if (h.encoding[0] && (strcasestr(h.encoding, "gzip") || strcasestr(h.encoding, "deflate"))) {
            size_t on;
            unsigned char *dec = inflate_any((unsigned char *)buf, len, &on);
            LOG("http: inflated %zu -> %zu bytes (%s)", len, on, h.encoding);
            free(buf);
            buf = realloc(dec, on + 1);
            buf[on] = 0;
            len = on;
        }
        r->body = buf;
        r->len = len;
        r->content_type = xstrdup(h.ctype);
        break;
    }
    r->url = cur;
    free(m);
    r->ms = (now() - t0) * 1000;
    LOG("http: %s %s -> %d (%zu bytes, %.0f ms)%s%s", method, r->url, r->status, r->len, r->ms, r->err[0] ? " " : "", r->err);
    return r;
}
HttpResp *http_get(const char *url) { return http_request("GET", url, 0, 0, 0); }
void http_free(HttpResp *r) {
    if (!r) return;
    free(r->body); free(r->content_type); free(r->url); free(r);
}

HttpStream *http_open(const char *url, int *status, char *err, int errn) {
    char *cur = xstrdup(url);
    for (int hop = 0; hop <= 5; hop++) {
        Url u;
        if (url_parse(cur, &u)) break;
        Head h;
        HttpStream *s = send_req("GET", &u, 0, 0, &h, err, errn);
        if (!s) break;
        *status = h.status;
        if (h.status >= 300 && h.status < 400 && h.location[0]) {
            http_close(s);
            char *n = url_resolve(cur, h.location);
            free(cur);
            cur = n;
            continue;
        }
        free(cur);
        s->keep = 0;   // streams are not returned to the pool
        return s;
    }
    free(cur);
    return 0;
}
