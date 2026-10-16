// Network bring-up: rtnetlink (link/address/route) and a DHCP client over a raw
// AF_PACKET socket that builds Ethernet/IPv4/UDP/BOOTP frames by hand.
#include "net.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static char ifname[IFNAMSIZ] = "eth0";
static int ifindex;
static unsigned char mac[6];
static volatile int state = NET_DOWN, enabled = 1;
static uint32_t my_ip, my_mask, my_gw, my_dns;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static char weather[64] = "—";

int net_state(void) { return state; }
const char *weather_text(void) { return weather; }

void net_info(NetInfo *ni) {
    memset(ni, 0, sizeof *ni);
    struct in_addr a;
    if (state == NET_UP) {
        a.s_addr = my_ip; snprintf(ni->ip, sizeof ni->ip, "%s", inet_ntoa(a));
        a.s_addr = my_gw; snprintf(ni->gw, sizeof ni->gw, "%s", inet_ntoa(a));
        a.s_addr = my_dns; snprintf(ni->dns, sizeof ni->dns, "%s", inet_ntoa(a));
    }
    snprintf(ni->mac, sizeof ni->mac, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---- rtnetlink -------------------------------------------------------------------------------
static int nl_talk(struct nlmsghdr *h) {
    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (s < 0) return -1;
    struct sockaddr_nl k = { .nl_family = AF_NETLINK };
    h->nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
    h->nlmsg_seq = (uint32_t)time(0);
    int r = -1;
    if (sendto(s, h, h->nlmsg_len, 0, (struct sockaddr *)&k, sizeof k) >= 0) {
        char buf[4096];
        int n = recv(s, buf, sizeof buf, 0);
        struct nlmsghdr *a = (struct nlmsghdr *)buf;
        if (n > 0 && NLMSG_OK(a, (unsigned)n) && a->nlmsg_type == NLMSG_ERROR) {
            r = ((struct nlmsgerr *)NLMSG_DATA(a))->error;
            if (r == -EEXIST) r = 0;
        }
    }
    close(s);
    return r;
}
static void rta_add(struct nlmsghdr *h, int type, const void *d, int len) {
    struct rtattr *a = (struct rtattr *)((char *)h + NLMSG_ALIGN(h->nlmsg_len));
    a->rta_type = type;
    a->rta_len = RTA_LENGTH(len);
    memcpy(RTA_DATA(a), d, len);
    h->nlmsg_len = NLMSG_ALIGN(h->nlmsg_len) + RTA_ALIGN(a->rta_len);
}
static int link_set(int up) {
    struct { struct nlmsghdr h; struct ifinfomsg i; char pad[64]; } m = {0};
    m.h.nlmsg_len = NLMSG_LENGTH(sizeof m.i);
    m.h.nlmsg_type = RTM_NEWLINK;
    m.i.ifi_family = AF_UNSPEC;
    m.i.ifi_index = ifindex;
    m.i.ifi_change = IFF_UP;
    m.i.ifi_flags = up ? IFF_UP : 0;
    return nl_talk(&m.h);
}
static int addr_op(int type, uint32_t ip, int prefix) {
    struct { struct nlmsghdr h; struct ifaddrmsg a; char pad[128]; } m = {0};
    m.h.nlmsg_len = NLMSG_LENGTH(sizeof m.a);
    m.h.nlmsg_type = type;
    if (type == RTM_NEWADDR) m.h.nlmsg_flags = NLM_F_CREATE | NLM_F_REPLACE;
    m.a.ifa_family = AF_INET;
    m.a.ifa_prefixlen = prefix;
    m.a.ifa_scope = RT_SCOPE_UNIVERSE;
    m.a.ifa_index = ifindex;
    rta_add(&m.h, IFA_LOCAL, &ip, 4);
    rta_add(&m.h, IFA_ADDRESS, &ip, 4);
    return nl_talk(&m.h);
}
static int route_add(uint32_t gw) {
    struct { struct nlmsghdr h; struct rtmsg r; char pad[128]; } m = {0};
    m.h.nlmsg_len = NLMSG_LENGTH(sizeof m.r);
    m.h.nlmsg_type = RTM_NEWROUTE;
    m.h.nlmsg_flags = NLM_F_CREATE | NLM_F_REPLACE;
    m.r.rtm_family = AF_INET;
    m.r.rtm_table = RT_TABLE_MAIN;
    m.r.rtm_protocol = RTPROT_BOOT;
    m.r.rtm_scope = RT_SCOPE_UNIVERSE;
    m.r.rtm_type = RTN_UNICAST;
    rta_add(&m.h, RTA_GATEWAY, &gw, 4);
    rta_add(&m.h, RTA_OIF, &ifindex, 4);
    return nl_talk(&m.h);
}

static int find_iface(void) {
    DIR *d = opendir("/sys/class/net");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.' && strcmp(e->d_name, "lo")) { snprintf(ifname, sizeof ifname, "%s", e->d_name); break; }
    closedir(d);
    ifindex = if_nametoindex(ifname);
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0) memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    close(s);
    return ifindex ? 0 : -1;
}

// ---- DHCP --------------------------------------------------------------------------------------
static uint16_t csum(const void *p, int n) {
    const uint8_t *b = p;
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2) s += b[i] << 8 | b[i + 1];
    if (n & 1) s += b[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return htons(~s);
}

static int dhcp_send(int s, uint32_t xid, int type, uint32_t req_ip, uint32_t server) {
    unsigned char f[600] = {0};
    unsigned char *eth = f, *ip = f + 14, *udp = ip + 20, *bp = udp + 8;
    memset(eth, 0xff, 6); memcpy(eth + 6, mac, 6); eth[12] = 0x08; eth[13] = 0x00;
    // BOOTP
    bp[0] = 1; bp[1] = 1; bp[2] = 6;
    memcpy(bp + 4, &xid, 4);
    bp[10] = 0x80;                     // broadcast flag: we have no address yet
    memcpy(bp + 28, mac, 6);
    unsigned char *o = bp + 236;
    *o++ = 99; *o++ = 130; *o++ = 83; *o++ = 99;     // magic cookie
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, mac, 6); o += 6;
    *o++ = 12; *o++ = 5; memcpy(o, "lumen", 5); o += 5;
    if (req_ip) { *o++ = 50; *o++ = 4; memcpy(o, &req_ip, 4); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; memcpy(o, &server, 4); o += 4; }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    *o++ = 255;
    int bl = o - bp;
    if (bl < 300) bl = 300;
    // UDP 68 -> 67 (checksum 0 = none, allowed for IPv4)
    udp[0] = 0; udp[1] = 68; udp[2] = 0; udp[3] = 67;
    udp[4] = (8 + bl) >> 8; udp[5] = (8 + bl) & 255;
    // IPv4 0.0.0.0 -> 255.255.255.255
    ip[0] = 0x45; ip[2] = (20 + 8 + bl) >> 8; ip[3] = (20 + 8 + bl) & 255;
    ip[8] = 64; ip[9] = 17;
    memset(ip + 16, 0xff, 4);
    uint16_t c = csum(ip, 20);
    memcpy(ip + 10, &c, 2);
    struct sockaddr_ll to = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_IP), .sll_ifindex = ifindex, .sll_halen = 6 };
    memset(to.sll_addr, 0xff, 6);
    return sendto(s, f, 14 + 20 + 8 + bl, 0, (struct sockaddr *)&to, sizeof to) > 0 ? 0 : -1;
}

typedef struct { int type; uint32_t yiaddr, mask, router, dns, server, lease; } Offer;
static int dhcp_recv(int s, uint32_t xid, Offer *of, double deadline) {
    unsigned char f[1600];
    while (now() < deadline) {
        struct timeval tv = { 0, 200000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        int n = recv(s, f, sizeof f, 0);
        if (n < 14 + 20 + 8 + 240) continue;
        unsigned char *ip = f + 14;
        if (f[12] != 0x08 || f[13] != 0 || ip[9] != 17) continue;
        int ihl = (ip[0] & 15) * 4;
        unsigned char *udp = ip + ihl, *bp = udp + 8;
        if (udp[3] != 68 || bp[0] != 2 || memcmp(bp + 4, &xid, 4)) continue;
        memset(of, 0, sizeof *of);
        memcpy(&of->yiaddr, bp + 16, 4);
        unsigned char *o = bp + 240, *end = f + n;
        while (o < end && *o != 255) {
            if (*o == 0) { o++; continue; }
            int t = o[0], l = o[1];
            unsigned char *v = o + 2;
            if (v + l > end) break;
            if (t == 53) of->type = v[0];
            else if (t == 1 && l >= 4) memcpy(&of->mask, v, 4);
            else if (t == 3 && l >= 4) memcpy(&of->router, v, 4);
            else if (t == 6 && l >= 4) memcpy(&of->dns, v, 4);
            else if (t == 54 && l >= 4) memcpy(&of->server, v, 4);
            else if (t == 51 && l >= 4) of->lease = ntohl(*(uint32_t *)v);
            o += 2 + l;
        }
        return 0;
    }
    return -1;
}

static int dhcp(void) {
    int s = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_IP));
    if (s < 0) { LOG("dhcp: AF_PACKET socket failed (%s)", strerror(errno)); return -1; }
    struct sockaddr_ll b = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_IP), .sll_ifindex = ifindex };
    bind(s, (struct sockaddr *)&b, sizeof b);
    uint32_t xid;
    getentropy(&xid, 4);
    Offer of, ack;
    int r = -1;
    for (int tries = 0; tries < 4 && enabled; tries++) {
        LOG("dhcp: DISCOVER (xid %08x)", xid);
        dhcp_send(s, xid, 1, 0, 0);
        if (dhcp_recv(s, xid, &of, now() + 2 + tries) || of.type != 2) continue;
        struct in_addr a = { of.yiaddr };
        LOG("dhcp: OFFER %s", inet_ntoa(a));
        dhcp_send(s, xid, 3, of.yiaddr, of.server);
        LOG("dhcp: REQUEST");
        if (dhcp_recv(s, xid, &ack, now() + 3) || ack.type != 5) { LOG("dhcp: no ACK"); continue; }
        my_ip = ack.yiaddr ? ack.yiaddr : of.yiaddr;
        my_mask = ack.mask ? ack.mask : of.mask;
        my_gw = ack.router ? ack.router : of.router;
        my_dns = ack.dns ? ack.dns : of.dns;
        r = 0;
        break;
    }
    close(s);
    if (r) return -1;
    int prefix = __builtin_popcount(my_mask);
    char ips[20], gws[20];
    struct in_addr a = { my_ip };
    snprintf(ips, sizeof ips, "%s", inet_ntoa(a));
    a.s_addr = my_gw;
    snprintf(gws, sizeof gws, "%s", inet_ntoa(a));
    a.s_addr = my_dns;
    snprintf(dns_server, sizeof dns_server, "%s", inet_ntoa(a));
    int e1 = addr_op(RTM_NEWADDR, my_ip, prefix), e2 = route_add(my_gw);
    LOG("dhcp: ACK %s/%d gw %s dns %s (addr %d, route %d)", ips, prefix, gws, dns_server, e1, e2);
    return e1 ? -1 : 0;
}

// ---- state machine -----------------------------------------------------------------------------
static void *weather_th(void *a) {
    (void)a;
    http_set_ua("Lumen/2.0");   // wttr.in serves plain text to non-browser agents
    HttpResp *r = http_get("https://wttr.in/?format=%25t%7C%25C");
    if (r->status == 200 && r->body && r->len < 120 && strchr(r->body, '|')) {
        char t[32] = "", cnd[64] = "";
        sscanf(r->body, "%31[^|]|%63[^\n]", t, cnd);
        char *p = t[0] == '+' ? t + 1 : t;
        char *cc = strstr(p, "°C");
        if (cc) cc[2] = 0;   // keep "12°"
        if (cc) cc[2] = 0;
        snprintf(weather, sizeof weather, "%s %s", p, cnd);
        // keep it short for the lock screen
        if (strlen(weather) > 22) weather[22] = 0;
        for (int k = strlen(weather) - 1; k > 0 && weather[k] == ' '; k--) weather[k] = 0;
        LOG("net: weather \"%s\"", weather);
    } else LOG("net: weather unavailable (%d %s)", r->status, r->err);
    http_free(r);
    return 0;
}

static void *net_thread(void *arg) {
    (void)arg;
    if (find_iface()) { LOG("net: no interface"); return 0; }
    LOG("net: interface %s index %d", ifname, ifindex);
    static int first = 1;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!enabled) {
            if (state != NET_DOWN) {
                addr_op(RTM_DELADDR, my_ip, __builtin_popcount(my_mask));
                link_set(0);
                state = NET_DOWN;
                LOG("net: interface down");
            }
            pthread_cond_wait(&cv, &mu);
        }
        pthread_mutex_unlock(&mu);
        if (state == NET_UP) {   // connected: idle until something changes
            pthread_mutex_lock(&mu);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;
            pthread_cond_timedwait(&cv, &mu, &ts);
            pthread_mutex_unlock(&mu);
            continue;
        }
        state = NET_CONNECTING;
        link_set(1);
        usleep(200000);
        double t0 = now();
        if (dhcp() == 0 && enabled) {
            state = NET_UP;
            NetInfo ni;
            net_info(&ni);
            LOG("net: connected in %.0f ms", (now() - t0) * 1000);
            char msg[64];
            snprintf(msg, sizeof msg, "Connected · %s", ni.ip);
            notify("Network", msg);
            if (first) {   // M6 check: fetch example.com over HTTPS and log it
                first = 0;
                HttpResp *r = http_get("https://example.com/");
                char *t = r->body ? strstr(r->body, "<title>") : 0;
                LOG("net: GET https://example.com/ -> %d, %zu bytes, %.0f ms, title \"%.*s\" %s", r->status, r->len, r->ms,
                    t ? (int)(strcspn(t + 7, "<")) : 0, t ? t + 7 : "", r->err);
                http_free(r);
                pthread_t w;
                pthread_create(&w, 0, weather_th, 0);
                pthread_detach(w);
            }
        } else if (enabled) {
            LOG("net: DHCP failed, retrying");
            state = NET_DOWN;
            sleep(2);
        }
    }
    return 0;
}

void net_init(void) {
    enabled = SET.wifi && !SET.airplane;
    pthread_t t;
    pthread_create(&t, 0, net_thread, 0);
    pthread_detach(t);
}

void net_set_enabled(bool on) {
    pthread_mutex_lock(&mu);
    enabled = on;
    if (on && state == NET_DOWN) state = NET_CONNECTING;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    LOG("net: %s", on ? "enable" : "disable");
}

void net_test(char *out, int n) {
    if (state != NET_UP) { snprintf(out, n, "FAIL · no network"); return; }
    HttpResp *r = http_get("https://example.com/");
    if (r->status == 200) snprintf(out, n, "OK · HTTPS %d · %zu bytes · %.0f ms", r->status, r->len, r->ms);
    else snprintf(out, n, "FAIL · %s", r->err[0] ? r->err : "bad status");
    LOG("net: test connection: %s", out);
    http_free(r);
}
