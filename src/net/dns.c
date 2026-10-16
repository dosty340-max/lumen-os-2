// DNS resolver: UDP A-record queries to the DHCP-provided server, with a TTL
// cache and timeout/retry.
#include "net.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

char dns_server[20] = "10.0.2.3";
typedef struct { char host[256]; uint32_t ip; double exp; } Entry;
static Entry cache[64];
static int ncache;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

// skip a (possibly compressed) name; returns new offset or -1
static int skip_name(const unsigned char *m, int n, int o) {
    while (o < n) {
        int l = m[o];
        if (l == 0) return o + 1;
        if ((l & 0xc0) == 0xc0) return o + 2;
        o += l + 1;
    }
    return -1;
}

static int query(const char *host, uint32_t *ip, uint32_t *ttl) {
    unsigned char q[512], r[1500];
    uint16_t id;
    getentropy(&id, 2);
    memset(q, 0, 12);
    memcpy(q, &id, 2);
    q[2] = 0x01;            // recursion desired
    q[5] = 1;               // one question
    int o = 12;
    for (const char *p = host; *p;) {
        const char *dot = strchr(p, '.');
        int l = dot ? dot - p : (int)strlen(p);
        if (l == 0 || l > 63 || o + l + 6 > (int)sizeof q) return -1;
        q[o++] = l;
        memcpy(q + o, p, l);
        o += l;
        p += l + (dot ? 1 : 0);
    }
    q[o++] = 0;
    q[o++] = 0; q[o++] = 1;   // type A
    q[o++] = 0; q[o++] = 1;   // class IN
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(53) };
    inet_aton(dns_server, &to.sin_addr);
    int result = -1;
    for (int attempt = 0; attempt < 3 && result < 0; attempt++) {
        sendto(s, q, o, 0, (struct sockaddr *)&to, sizeof to);
        struct pollfd pf = { s, POLLIN, 0 };
        double deadline = now() + 2;
        while (now() < deadline && result < 0) {
            if (poll(&pf, 1, (int)((deadline - now()) * 1000) + 1) <= 0) break;
            int n = recv(s, r, sizeof r, 0);
            if (n < 12 || memcmp(r, &id, 2)) continue;
            if ((r[3] & 15) != 0) { result = -2; break; }   // NXDOMAIN etc
            int qd = r[4] << 8 | r[5], an = r[6] << 8 | r[7];
            int p = 12;
            for (int i = 0; i < qd && p > 0; i++) { p = skip_name(r, n, p); if (p > 0) p += 4; }
            for (int i = 0; i < an && p > 0 && p < n; i++) {   // CNAMEs come first; take the first A
                p = skip_name(r, n, p);
                if (p < 0 || p + 10 > n) break;
                int type = r[p] << 8 | r[p + 1];
                uint32_t t = (uint32_t)r[p + 4] << 24 | r[p + 5] << 16 | r[p + 6] << 8 | r[p + 7];
                int rl = r[p + 8] << 8 | r[p + 9];
                p += 10;
                if (type == 1 && rl == 4 && p + 4 <= n) { memcpy(ip, r + p, 4); *ttl = t; result = 0; break; }
                p += rl;
            }
            if (result < 0) result = -3;
        }
    }
    close(s);
    return result;
}

int dns_resolve(const char *host, uint32_t *ip) {
    struct in_addr a;
    if (inet_aton(host, &a)) { *ip = a.s_addr; return 0; }
    double t = now();
    pthread_mutex_lock(&mu);
    for (int i = 0; i < ncache; i++)
        if (!strcasecmp(cache[i].host, host) && cache[i].exp > t) { *ip = cache[i].ip; pthread_mutex_unlock(&mu); return 0; }
    pthread_mutex_unlock(&mu);
    uint32_t ttl = 60;
    int r = query(host, ip, &ttl);
    if (r == 0) {
        pthread_mutex_lock(&mu);
        Entry *e = ncache < 64 ? &cache[ncache++] : &cache[(unsigned)t % 64];
        snprintf(e->host, sizeof e->host, "%s", host);
        e->ip = *ip;
        e->exp = t + (ttl < 30 ? 30 : ttl > 3600 ? 3600 : ttl);
        pthread_mutex_unlock(&mu);
        a.s_addr = *ip;
        LOG("dns: %s -> %s (ttl %u, %.0f ms)", host, inet_ntoa(a), ttl, (now() - t) * 1000);
    } else LOG("dns: %s failed (%d)", host, r);
    return r;
}
