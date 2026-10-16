// Persistence without a filesystem: a key/value store serialised into one of
// two slots on the raw data disk (/dev/vda). Each save goes to the older slot,
// so a crash mid-write always leaves the previous state intact.
#include "ui.h"
#include <fcntl.h>
#include <unistd.h>

#define SLOT_SIZE (4 << 20)
#define MAGIC "LUMENKV1"
typedef struct { char *k, *v; } KV;
static KV *kv;
static int nkv, cap;
static unsigned seq;
static int dirty;
static double dirty_t;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static uint32_t crc32(const unsigned char *p, size_t n) {
    uint32_t c = 0xffffffff;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320 & -(c & 1)); }
    return ~c;
}

static int load_slot(int fd, int slot, unsigned *sq) {
    unsigned char hdr[20];
    if (pread(fd, hdr, 20, (off_t)slot * SLOT_SIZE) != 20 || memcmp(hdr, MAGIC, 8)) return -1;
    uint32_t s, len, crc;
    memcpy(&s, hdr + 8, 4); memcpy(&len, hdr + 12, 4); memcpy(&crc, hdr + 16, 4);
    if (len > SLOT_SIZE - 20) return -1;
    unsigned char *b = malloc(len + 1);
    if (pread(fd, b, len, (off_t)slot * SLOT_SIZE + 20) != (ssize_t)len || crc32(b, len) != crc) { free(b); return -1; }
    *sq = s;
    free(b);
    return 0;
}

static void parse_slot(int fd, int slot) {
    unsigned char hdr[20];
    pread(fd, hdr, 20, (off_t)slot * SLOT_SIZE);
    uint32_t len;
    memcpy(&len, hdr + 12, 4);
    unsigned char *b = malloc(len + 1);
    pread(fd, b, len, (off_t)slot * SLOT_SIZE + 20);
    for (size_t o = 0; o + 6 <= len;) {
        uint16_t kl; uint32_t vl;
        memcpy(&kl, b + o, 2); memcpy(&vl, b + o + 2, 4);
        o += 6;
        if (o + kl + vl > len) break;
        char *k = strndup((char *)b + o, kl), *v = strndup((char *)b + o + kl, vl);
        o += kl + vl;
        store_set(k, v);
        free(k); free(v);
    }
    free(b);
}

static void save_now(void) {
    pthread_mutex_lock(&mu);
    size_t len = 0;
    for (int i = 0; i < nkv; i++) len += 6 + strlen(kv[i].k) + strlen(kv[i].v);
    unsigned char *b = malloc(20 + len);
    size_t o = 20;
    for (int i = 0; i < nkv; i++) {
        uint16_t kl = strlen(kv[i].k); uint32_t vl = strlen(kv[i].v);
        memcpy(b + o, &kl, 2); memcpy(b + o + 2, &vl, 4); o += 6;
        memcpy(b + o, kv[i].k, kl); o += kl;
        memcpy(b + o, kv[i].v, vl); o += vl;
    }
    seq++;
    unsigned s = seq;
    dirty = 0;
    pthread_mutex_unlock(&mu);
    uint32_t l32 = len, crc = crc32(b + 20, len);
    memcpy(b, MAGIC, 8); memcpy(b + 8, &s, 4); memcpy(b + 12, &l32, 4); memcpy(b + 16, &crc, 4);
    int fd = open("/dev/vda", O_RDWR | O_CLOEXEC);
    if (fd < 0) { free(b); return; }
    if (len + 20 <= SLOT_SIZE) {
        pwrite(fd, b, len + 20, (off_t)(s & 1) * SLOT_SIZE);
        fsync(fd);
    }
    close(fd);
    free(b);
}

static void *saver(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!dirty) pthread_cond_wait(&cv, &mu);
        pthread_mutex_unlock(&mu);
        usleep(300000);   // debounce bursts of changes
        save_now();
    }
    return 0;
}

void store_init(void) {
    int fd = open("/dev/vda", O_RDONLY | O_CLOEXEC);
    if (fd < 0) LOG("store: no data disk");
    else {
        unsigned s0 = 0, s1 = 0;
        int ok0 = load_slot(fd, 0, &s0) == 0, ok1 = load_slot(fd, 1, &s1) == 0;
        int slot = ok0 && ok1 ? (s1 > s0 ? 1 : 0) : ok1 ? 1 : ok0 ? 0 : -1;
        if (slot >= 0) { parse_slot(fd, slot); seq = slot ? s1 : s0; }
        LOG("store: %d keys loaded (slot %d, seq %u)", nkv, slot, seq);
        close(fd);
    }
    dirty = 0;
    pthread_t th;
    pthread_create(&th, 0, saver, 0);
}

const char *store_get(const char *key) {
    const char *r = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nkv; i++) if (!strcmp(kv[i].k, key)) { r = kv[i].v; break; }
    pthread_mutex_unlock(&mu);
    return r;   // ponytail: pointer stays valid until the key is set again (UI thread only writes)
}
void store_set(const char *key, const char *val) {
    pthread_mutex_lock(&mu);
    int i;
    for (i = 0; i < nkv; i++) if (!strcmp(kv[i].k, key)) break;
    if (i == nkv) {
        if (nkv == cap) { cap = cap ? cap * 2 : 64; kv = realloc(kv, cap * sizeof *kv); }
        kv[nkv++] = (KV){ strdup(key), strdup(val ? val : "") };
    } else if (strcmp(kv[i].v, val ? val : "")) {
        free(kv[i].v);
        kv[i].v = strdup(val ? val : "");
    } else { pthread_mutex_unlock(&mu); return; }
    dirty = 1;
    dirty_t = now();
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}
int store_geti(const char *key, int def) { const char *v = store_get(key); return v ? atoi(v) : def; }
void store_seti(const char *key, int v) { char b[16]; snprintf(b, sizeof b, "%d", v); store_set(key, b); }
void store_flush(void) { if (dirty) save_now(); }

void settings_load(void) {
    SET.accent = store_geti("set.accent", 0) % NACCENT;
    SET.dark = store_geti("set.dark", 1);
    SET.bright = store_geti("set.bright", 100);
    SET.vol = store_geti("set.vol", 70);
    SET.textsize = store_geti("set.textsize", 1);
    SET.muted = store_geti("set.muted", 0);
    SET.wifi = store_geti("set.wifi", 1);
    SET.airplane = store_geti("set.airplane", 0);
    theme_apply();
}
void settings_save(void) {
    store_seti("set.accent", SET.accent);
    store_seti("set.dark", SET.dark);
    store_seti("set.bright", SET.bright);
    store_seti("set.vol", SET.vol);
    store_seti("set.textsize", SET.textsize);
    store_seti("set.muted", SET.muted);
    store_seti("set.wifi", SET.wifi);
    store_seti("set.airplane", SET.airplane);
}
