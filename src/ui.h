// Slab UI toolkit + cross-module APIs used by the shell and apps.
#pragma once
#include "lumen.h"
#include <time.h>

// ---- theme & settings ----------------------------------------------------------
typedef struct { uint32_t bg, fg, sub, line, strong; } Theme;
extern Theme TH;
typedef struct { int accent, dark, bright, vol, textsize, muted, wifi, airplane; } Settings;
extern Settings SET;
void theme_apply(void);
#define NACCENT 5
extern const uint32_t ACCENTS[NACCENT][3];
extern const char *ACCENT_NAMES[NACCENT];
void settings_save(void);
void settings_load(void);

// ---- springs -------------------------------------------------------------------------
typedef struct { float x, v, t; } Spring;   // position, velocity, target
void spring_step(Spring *s, float dt, float freq, float zeta);
bool spring_rest(Spring *s, float eps);

// ---- hits & gestures -----------------------------------------------------------------
void hit_frame(void);
void hit(int id, float x, float y, float w, float h);
int hit_at(float x, float y);
bool pressed(int id);
extern int press_id;
enum { G_NONE, G_DOWN, G_TAP, G_DRAG_START, G_DRAG, G_DRAG_END };
typedef struct { float x0, y0, x, y, dx, dy, vx, vy; double t; int down, drag, vert; } Gest;
int gest(Gest *g, InputEv *e);

typedef struct { float pos, vel, max; int held; } Scroll;
void scroll_drag(Scroll *s, float dy);
void scroll_fling(Scroll *s, float vy);
void scroll_step(Scroll *s, float dt);

// ---- icons ---------------------------------------------------------------------------------
enum {
    IC_NONE, IC_VIDEO, IC_WEB, IC_PHOTOS, IC_NOTES, IC_CLOCK, IC_CALC, IC_GAME, IC_SETTINGS, IC_SEARCH,
    IC_WIFI, IC_WIFI_OFF, IC_AIRPLANE, IC_MOON, IC_SUN, IC_SPEAKER, IC_MUTE, IC_PLAY, IC_PLAY_FILL, IC_PAUSE,
    IC_FULL, IC_UNFULL, IC_BACK, IC_FWD, IC_PLUS, IC_TRASH, IC_CLOSE, IC_UP, IC_REFRESH, IC_BOOKMARK,
    IC_BOOKMARK_FILL, IC_TABS, IC_CHECK, IC_TIMER, IC_STOPWATCH, IC_CHEVRON, IC_BATTERY, IC_LOCK, IC_CLOUD,
    IC_NET_CONN, IC_KEYBOARD, IC_SHIFT, IC_BKSP, IC_RETURN, IC_GLOBE_SMALL, IC_HISTORY, IC_SWIPE,
};
void icon(Canvas *c, int id, float x, float y, float size, uint32_t col);

// ---- chrome ----------------------------------------------------------------------------------
void status_line(Canvas *c, float y, uint32_t col, bool show_time, bool show_brand);
void home_bar(Canvas *c, uint32_t col);
float app_header(Canvas *c, uint32_t col, const char *title, float h);
void btn_icon(Canvas *c, int id, float x, float y, float d, int ic, uint32_t fg, uint32_t fill);
void btn_text(Canvas *c, int id, float x, float y, float w, float h, const char *label, uint32_t fg, uint32_t fill);

// ---- keyboard & fields ------------------------------------------------------------------------
float kb_height(void);
void kb_draw(Canvas *c, float y, const char *ret_label);
int kb_tap(float x, float y);
bool kb_hit(int id);
int field_key(char *buf, int cap, int k);
void field_draw(Canvas *c, float x, float y, float w, float h, const char *buf, const char *ph, bool focus, uint32_t bg, uint32_t fg);

// ---- apps ----------------------------------------------------------------------------------------
typedef struct App {
    const char *name;
    int icon;
    void (*open)(void);                 // called when the app is launched
    void (*draw)(Canvas *c, float dt);  // full-screen draw into the app layer
    void (*touch)(InputEv *e);          // finger events (already outside system edges)
    void (*key)(int k);                 // keyboard
    uint32_t (*status_col)(void);       // colour of status line text over the app
} App;
extern App app_video, app_web, app_photos, app_notes, app_clock, app_calc, app_game, app_settings;
void shell_launch(App *a, const char *arg);   // open an app (e.g. Web with a query)
void shell_go_home(void);
const char *shell_launch_arg(void);
void notify(const char *sender, const char *text);
extern float shell_app_alpha;                 // app content fade (for apps that care)

// ---- persistence (store.c) -------------------------------------------------------------------------
void store_init(void);
const char *store_get(const char *key);
void store_set(const char *key, const char *val);
int store_geti(const char *key, int def);
void store_seti(const char *key, int v);
void store_flush(void);

// ---- audio (audio.c) -----------------------------------------------------------------------------
void audio_init(void);
void audio_tone(float freq, float secs, float vol);      // UI sounds
void audio_alarm(void);
// PCM stream for video: push interleaved s16 stereo 48k
int audio_stream_push(const int16_t *pcm, int frames);    // returns frames accepted
int audio_stream_space(void);
void audio_stream_reset(void);
double audio_stream_clock(void);  // seconds of stream audio actually played
int audio_stream_buffered(void);  // frames queued (not yet played)
void audio_stream_pause(bool p);
extern volatile long audio_frames_total;

// ---- network (net/) ------------------------------------------------------------------------------
enum { NET_DOWN, NET_CONNECTING, NET_UP };
int net_state(void);
void net_init(void);
void net_set_enabled(bool on);
typedef struct { char ip[20], gw[20], dns[20], mac[20]; } NetInfo;
void net_info(NetInfo *ni);

// ---- clock app timer (shown on home) ---------------------------------------------------------------
float clock_timer_left(void);    // <0 if not running
void clock_tick(float dt);
int game_best(void);
const char *video_continue_info(void);   // "Continue · 12 min left" or NULL
bool video_playing(void);
const char *video_now_title(void);
void video_toggle_play(void);
const char *weather_text(void);
void photos_init(void);
