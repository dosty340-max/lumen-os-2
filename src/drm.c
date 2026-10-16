// Display: raw DRM/KMS ioctls, two dumb buffers, page flips with events.
#include "lumen.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

static int fd = -1;
static uint32_t crtc_id, conn_id, fb_id[2];
static struct drm_mode_modeinfo mode;
static uint32_t *map[2];
static int back, can_flip = 1, flip_pending;
static Canvas screen;

static int xioctl(unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

Canvas *drm_init(void) {
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { LOG("drm: no card0 (%s)", strerror(errno)); return NULL; }
    xioctl(DRM_IOCTL_SET_MASTER, 0);

    struct drm_mode_card_res res = {0};
    xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res);
    uint32_t conns[16] = {0}, crtcs[16] = {0}, encs[16] = {0};
    res.count_fbs = 0;
    if (res.count_connectors > 16) res.count_connectors = 16;
    if (res.count_crtcs > 16) res.count_crtcs = 16;
    if (res.count_encoders > 16) res.count_encoders = 16;
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res);

    for (unsigned i = 0; i < res.count_connectors && !conn_id; i++) {
        struct drm_mode_get_connector c = { .connector_id = conns[i] };
        xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c);
        if (c.connection != 1 || !c.count_modes) continue;
        struct drm_mode_modeinfo modes[64];
        uint32_t ce[16]; uint32_t props[64]; uint64_t pv[64];
        if (c.count_modes > 64) c.count_modes = 64;
        if (c.count_encoders > 16) c.count_encoders = 16;
        if (c.count_props > 64) c.count_props = 64;
        c.modes_ptr = (uintptr_t)modes; c.encoders_ptr = (uintptr_t)ce;
        c.props_ptr = (uintptr_t)props; c.prop_values_ptr = (uintptr_t)pv;
        xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c);
        int best = 0;   // prefer the mode matching our panel size, else preferred
        for (unsigned m = 0; m < c.count_modes; m++)
            if (modes[m].hdisplay == SW && modes[m].vdisplay == SH) { best = m; break; }
        mode = modes[best];
        conn_id = c.connector_id;
        if (c.encoder_id) {
            struct drm_mode_get_encoder e = { .encoder_id = c.encoder_id };
            if (!xioctl(DRM_IOCTL_MODE_GETENCODER, &e)) crtc_id = e.crtc_id;
        }
    }
    if (!conn_id) { LOG("drm: no connected connector"); return NULL; }
    if (!crtc_id) crtc_id = crtcs[0];
    LOG("drm: mode %dx%d@%d conn %u crtc %u", mode.hdisplay, mode.vdisplay, mode.vrefresh, conn_id, crtc_id);

    for (int i = 0; i < 2; i++) {
        struct drm_mode_create_dumb cd = { .width = SW, .height = SH, .bpp = 32 };
        if (xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { LOG("drm: create dumb failed"); return NULL; }
        struct drm_mode_fb_cmd fb = { .width = SW, .height = SH, .pitch = cd.pitch, .bpp = 32, .depth = 24, .handle = cd.handle };
        if (xioctl(DRM_IOCTL_MODE_ADDFB, &fb)) { LOG("drm: addfb failed"); return NULL; }
        fb_id[i] = fb.fb_id;
        struct drm_mode_map_dumb md = { .handle = cd.handle };
        xioctl(DRM_IOCTL_MODE_MAP_DUMB, &md);
        map[i] = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
        if (map[i] == MAP_FAILED) { LOG("drm: mmap failed"); return NULL; }
        memset(map[i], 0, cd.size);
        if (cd.pitch != SW * 4) LOG("drm: unexpected pitch %u", cd.pitch);
    }
    struct drm_mode_crtc sc = { .crtc_id = crtc_id, .fb_id = fb_id[0], .set_connectors_ptr = (uintptr_t)&conn_id,
                                .count_connectors = 1, .mode = mode, .mode_valid = 1 };
    if (xioctl(DRM_IOCTL_MODE_SETCRTC, &sc)) { LOG("drm: setcrtc failed %s", strerror(errno)); return NULL; }
    back = 1;
    screen.w = SW; screen.h = SH; screen.px = map[back];
    g_noclip(&screen);
    return &screen;
}

static void wait_flip(void) {
    while (flip_pending) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0) { flip_pending = 0; break; }   // never hang the UI on a lost event
        char buf[256];
        int n = read(fd, buf, sizeof buf);
        for (int o = 0; o + (int)sizeof(struct drm_event) <= n; ) {
            struct drm_event *e = (struct drm_event *)(buf + o);
            if (e->type == DRM_EVENT_FLIP_COMPLETE) flip_pending = 0;
            o += e->length;
        }
    }
}

// Present the back buffer and hand out the other one for the next frame.
Canvas *drm_flip(void) {
    if (can_flip) {
        struct drm_mode_crtc_page_flip pf = { .crtc_id = crtc_id, .fb_id = fb_id[back], .flags = DRM_MODE_PAGE_FLIP_EVENT };
        if (xioctl(DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0) flip_pending = 1;
        else { LOG("drm: page flip unsupported (%s), using setcrtc", strerror(errno)); can_flip = 0; }
    }
    if (!can_flip) {
        struct drm_mode_crtc sc = { .crtc_id = crtc_id, .fb_id = fb_id[back], .set_connectors_ptr = (uintptr_t)&conn_id,
                                    .count_connectors = 1, .mode = mode, .mode_valid = 1 };
        xioctl(DRM_IOCTL_MODE_SETCRTC, &sc);
    }
    wait_flip();
    back ^= 1;
    screen.px = map[back];
    return &screen;
}
