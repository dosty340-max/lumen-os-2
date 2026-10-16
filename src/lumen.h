// Lumen OS 2 — shared declarations for every module.
#pragma once
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- screen & design units ------------------------------------------------
#define SW 720
#define SH 1560
#define S (720.0f / 390.0f)          // logical point -> physical pixel
#define P(v) ((float)(v) * S)
#define LW 390                       // logical width
#define LH 845                       // logical height (1560 / S)

// Slab tokens
#define C_ORANGE 0xFFFF6A2Bu
#define C_LILAC  0xFFC8B8FFu
#define C_LIME   0xFFDFFF5Eu
#define C_SKY    0xFF7FD4FFu
#define C_PAPER  0xFFF3F0E8u
#define C_INK    0xFF141414u
#define C_MUTED  0xFFC9C5BCu
#define C_BLACK  0xFF000000u

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline int mini(int a, int b) { return a < b ? a : b; }
static inline int maxi(int a, int b) { return a > b ? a : b; }

// ---- util (sys.c) -----------------------------------------------------------
double now(void);                       // monotonic seconds
void LOG(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
char *xstrdup(const char *s);
char *strfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  // malloc'd
void request_reboot(void);

// ---- graphics (gfx.c) -------------------------------------------------------
typedef struct { int w, h; uint32_t *px; int cx0, cy0, cx1, cy1; } Canvas;
typedef struct { int w, h; uint32_t *px; } Image;   // ARGB, straight alpha

Canvas *canvas_new(int w, int h);
void g_clip(Canvas *c, float x0, float y0, float x1, float y1);  // intersects current clip
void g_noclip(Canvas *c);
void g_clear(Canvas *c, uint32_t col);
void g_rect(Canvas *c, float x, float y, float w, float h, uint32_t col);
void g_rrect(Canvas *c, float x, float y, float w, float h, float rtop, float rbot, uint32_t col);
void g_rrect_line(Canvas *c, float x, float y, float w, float h, float r, float lw, uint32_t col);
void g_circle(Canvas *c, float cx, float cy, float r, uint32_t col);
void g_ring(Canvas *c, float cx, float cy, float r, float lw, uint32_t col);
void g_line(Canvas *c, float x0, float y0, float x1, float y1, float w, uint32_t col);
void g_arc(Canvas *c, float cx, float cy, float r, float a0, float a1, float w, uint32_t col);
void g_image(Canvas *c, const Image *im, float x, float y, float w, float h, float radius, int alpha);
void g_image_cover(Canvas *c, const Image *im, float x, float y, float w, float h, float radius, int alpha);
// draw src transformed: dst point (x,y) samples src at (m0*x+m1*y+m2, m3*x+m4*y+m5)
void g_layer(Canvas *dst, const Canvas *src, const float inv[6], float bx0, float by0, float bx1, float by1, int alpha);
void g_layer_rect(Canvas *dst, const Canvas *src, float x, float y, float w, float h, float radius, int alpha);
void g_dim(Canvas *c, float k);       // multiply all pixels by k (brightness)
uint32_t c_alpha(uint32_t col, float a);
uint32_t c_mix(uint32_t a, uint32_t b, float t);
Image *image_decode(const void *data, int len);      // stb_image -> Image (NULL on fail)
Image *image_new(int w, int h);
void image_free(Image *im);

// ---- text (font.c) ----------------------------------------------------------
enum { W400, W600, W800 };
void font_init(void);
float text_w(int f, float px, float track, const char *s);              // track in em
float text(Canvas *c, int f, float px, float track, float x, float base, uint32_t col, const char *s);
float text_n(Canvas *c, int f, float px, float track, float x, float base, uint32_t col, const char *s, int n);
float text_wn(int f, float px, float track, const char *s, int n);
void text_r(Canvas *c, int f, float px, float track, float xr, float base, uint32_t col, const char *s);
void text_c(Canvas *c, int f, float px, float track, float xc, float base, uint32_t col, const char *s);
float font_asc(int f, float px);       // ascent (positive) in px
float font_cap(int f, float px);       // cap height in px
int utf8_next(const char **s);         // decode one codepoint, advance
int utf8_put(char *o, int cp);
// fit text into width with an ellipsis; returns malloc'd string
char *text_ellipsize(int f, float px, const char *s, float maxw);

// ---- display & input --------------------------------------------------------
Canvas *drm_init(void);
Canvas *drm_flip(void);

enum { EV_DOWN = 1, EV_MOVE, EV_UP, EV_KEY };
enum { K_BACKSPACE = 8, K_ENTER = 10, K_ESC = 27, K_LEFT = 0x110000, K_RIGHT, K_UP, K_DOWN, K_TAB };
typedef struct { int type; float x, y; int key; } InputEv;   // key: codepoint or K_*
void input_init(void);
int input_poll(InputEv *out, int max);
extern float touch_x, touch_y; extern bool touch_down;
