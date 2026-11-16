// Experimental fbdev backend for dreamlte. Only maps the current visible mode.
#include "display.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static uint8_t *scanout;
static Canvas *screen;

static bool channel(struct fb_bitfield channel, unsigned offset, unsigned length) {
    return channel.offset == offset && channel.length == length && !channel.msb_right;
}

static bool supported_mode(void) {
    return fix.type == FB_TYPE_PACKED_PIXELS && fix.visual == FB_VISUAL_TRUECOLOR &&
        var.bits_per_pixel == 32 && !var.grayscale && !var.nonstd && !var.rotate &&
        !(var.vmode & FB_VMODE_YWRAP) &&
        channel(var.red, 16, 8) && channel(var.green, 8, 8) && channel(var.blue, 0, 8) &&
        (!var.transp.length || channel(var.transp, 24, 8)) &&
        var.xres && var.yres && fix.line_length % 4 == 0 &&
        (uint64_t)var.xoffset + var.xres <= var.xres_virtual &&
        (uint64_t)var.yoffset + var.yres <= var.yres_virtual &&
        ((uint64_t)var.xoffset + var.xres) * 4 <= fix.line_length &&
        (uint64_t)var.yoffset + var.yres <= fix.smem_len / fix.line_length;
}

static Canvas *fbdev_init(void) {
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) fd = open("/dev/graphics/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG("fbdev: cannot open fb0 (%s)", strerror(errno));
        return NULL;
    }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        LOG("fbdev: cannot query mode (%s)", strerror(errno));
        close(fd);
        return NULL;
    }
    if (!supported_mode()) {
        LOG("fbdev: unsupported mode %ux%u, %u bpp, stride %u",
            var.xres, var.yres, var.bits_per_pixel, fix.line_length);
        close(fd);
        return NULL;
    }
    scanout = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int error = errno;
    close(fd);
    if (scanout == MAP_FAILED) {
        LOG("fbdev: mmap failed (%s)", strerror(error));
        return NULL;
    }
    screen = canvas_new(SW, SH);
    LOG("fbdev: %ux%u, stride %u", var.xres, var.yres, fix.line_length);
    return screen;
}

static Canvas *fbdev_present(void) {
    uint8_t *first = scanout + (size_t)var.yoffset * fix.line_length + (size_t)var.xoffset * 4;
    for (uint32_t y = 0; y < var.yres; y++) {
        const uint32_t *src = screen->px + (uint64_t)y * SH / var.yres * SW;
        uint32_t *dst = (uint32_t *)(first + (size_t)y * fix.line_length);
        // Incremental nearest-neighbor scaling across the scanline.
        uint64_t phase = 0;
        unsigned sx = 0;
        for (uint32_t x = 0; x < var.xres; x++) {
            dst[x] = src[sx] | 0xff000000u;
            phase += SW;
            if (phase >= var.xres) {
                sx += phase / var.xres;
                phase %= var.xres;
            }
        }
    }
    return screen;
}

const DisplayBackend display_backend = { .init = fbdev_init, .present = fbdev_present };
