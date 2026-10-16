// Small utilities shared by everything.
#include "lumen.h"
#include <time.h>
#include <unistd.h>

double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
void LOG(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    pthread_mutex_lock(&log_mu);
    // timestamp in seconds since boot so the serial log reads like a timeline
    char ts[32];
    int m = snprintf(ts, sizeof ts, "[%8.3f] ", now());
    write(2, ts, m);
    write(2, buf, n);
    pthread_mutex_unlock(&log_mu);
}

char *xstrdup(const char *s) { return s ? strdup(s) : strdup(""); }

char *strfmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *r = 0;
    if (vasprintf(&r, fmt, ap) < 0) r = 0;
    va_end(ap);
    return r;
}
