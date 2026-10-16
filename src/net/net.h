// Lumen net library: DNS, HTTP/1.1 (+TLS via BearSSL), URLs, inflate.
// Used by the Web and Video apps. All calls are blocking: run them off the UI thread.
#pragma once
#include "../ui.h"

typedef struct { char scheme[8]; char host[256]; int port; char path[4096]; } Url;
int url_parse(const char *s, Url *u);                 // 0 ok
char *url_resolve(const char *base, const char *rel); // malloc'd absolute URL
char *url_encode(const char *s);                      // form/query encoding, malloc'd

int dns_resolve(const char *host, uint32_t *ip);      // ip in network order; 0 ok
extern char dns_server[20];

typedef struct {
    int status;
    char *body; size_t len;
    char *content_type;
    char *url;          // final URL after redirects
    char err[128];
    double ms;
} HttpResp;
// method "GET"/"POST"; body/ctype may be NULL. Follows up to 5 redirects.
HttpResp *http_request(const char *method, const char *url, const char *body, const char *ctype, size_t max_body);
HttpResp *http_get(const char *url);
void http_free(HttpResp *r);
// progress of the current request on this thread's last call (0..1, -1 unknown)
typedef void (*http_progress_fn)(void *ctx, size_t got, size_t total);
void http_set_progress(http_progress_fn fn, void *ctx);
void http_set_ua(const char *ua);   // per-thread User-Agent override (NULL = default)

typedef struct HttpStream HttpStream;
HttpStream *http_open(const char *url, int *status, char *err, int errn);   // streaming GET body
int http_read(HttpStream *s, void *buf, int n);      // >0 bytes, 0 EOF, <0 error
void http_close(HttpStream *s);
void http_abort(HttpStream *s);   // thread-safe: makes a blocked http_read return

// inflate (RFC 1951) with zlib/gzip wrappers; returns malloc'd output
unsigned char *inflate_any(const unsigned char *in, size_t n, size_t *outn);
