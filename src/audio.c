// Audio: raw ALSA PCM ioctls on /dev/snd/pcmC0D0p (virtio-sound).
// One mixer thread writes continuously: video PCM ring + UI tones, volume applied.
#include "ui.h"
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>

#define RATE 48000
#define BLOCK 960                       // 20 ms per write
#define RING (RATE * 40)                // video PCM ring: 40 s
static int fd = -1;
static unsigned long buf_frames = 8192;
static int16_t ring[RING * 2];
static volatile long ring_rd, ring_wr;  // frame counters
static volatile int paused;
static volatile int gen;                // bumps on reset
volatile long audio_frames_total;       // frames written to the device (for tests)

// block records map device position -> stream position (audio clock)
typedef struct { long dev0; long s0; int n; int gen; } Rec;
static Rec recs[128];
static int rec_i;
static long dev_written;
static volatile long audible_dev;       // device frame audible at time audible_t
static volatile double audible_t;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long stream_consumed;            // stream frames taken since reset

// UI tones
typedef struct { float f, amp; long left, total; double ph; } Tone;
static Tone tones[8];

static void p_any(struct snd_pcm_hw_params *p) {
    memset(p, 0, sizeof *p);
    for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
        memset(&p->masks[i], 0xff, sizeof p->masks[i]);
    for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++) {
        p->intervals[i].min = 0; p->intervals[i].max = UINT_MAX;
    }
    p->rmask = ~0u; p->info = ~0u;
}
static void p_mask(struct snd_pcm_hw_params *p, int param, unsigned v) {
    struct snd_mask *m = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof *m);
    m->bits[v >> 5] |= 1u << (v & 31);
}
static void p_range(struct snd_pcm_hw_params *p, int param, unsigned lo, unsigned hi) {
    struct snd_interval *iv = &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    iv->min = lo; iv->max = hi; iv->integer = 1;
}
static unsigned p_get(struct snd_pcm_hw_params *p, int param) {
    return p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
}

static int pcm_open(void) {
    fd = open("/dev/snd/pcmC0D0p", O_RDWR | O_CLOEXEC);
    if (fd < 0) { LOG("audio: no pcm device (%s)", strerror(errno)); return -1; }
    struct snd_pcm_hw_params hw;
    for (int attempt = 0; attempt < 2; attempt++) {
        p_any(&hw);
        p_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        p_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
        p_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
        p_range(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2, 2);
        p_range(&hw, SNDRV_PCM_HW_PARAM_RATE, RATE, RATE);
        if (attempt == 0) {
            p_range(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 960, 1024);
            p_range(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 4096, 8192);
        }
        if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) == 0) break;
        LOG("audio: hw_params attempt %d failed (%s)", attempt, strerror(errno));
        if (attempt == 1) { close(fd); fd = -1; return -1; }
    }
    unsigned period = p_get(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    buf_frames = p_get(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
    struct snd_pcm_sw_params sw = {0};
    unsigned long boundary = buf_frames;
    while (boundary * 2 <= LONG_MAX - buf_frames) boundary *= 2;
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = period;
    sw.start_threshold = period * 2;
    sw.stop_threshold = buf_frames;
    sw.boundary = boundary;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw)) LOG("audio: sw_params failed (%s)", strerror(errno));
    ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
    LOG("audio: pcmC0D0p open, 48000 Hz s16 stereo, period %u, buffer %lu", period, buf_frames);
    return 0;
}

static void mix_block(int16_t *out) {
    static float acc[BLOCK * 2];
    memset(acc, 0, sizeof acc);
    // video stream
    int g = gen;
    long rd = ring_rd, avail = ring_wr - rd;
    int n = paused ? 0 : (int)(avail < BLOCK ? avail : BLOCK);
    for (int i = 0; i < n; i++) {
        long k = (rd + i) % RING;
        acc[i * 2] = ring[k * 2];
        acc[i * 2 + 1] = ring[k * 2 + 1];
    }
    if (g == gen) ring_rd = rd + n;
    pthread_mutex_lock(&mu);
    if (g == gen) {
        recs[rec_i] = (Rec){ dev_written, stream_consumed, n, g };
        rec_i = (rec_i + 1) % 128;
        stream_consumed += n;
    }
    pthread_mutex_unlock(&mu);
    // tones (soft sine with attack/release envelope)
    for (int t = 0; t < 8; t++) {
        Tone *o = &tones[t];
        for (int i = 0; i < BLOCK && o->left > 0; i++, o->left--) {
            long p = o->total - o->left;
            float env = fminf(1, fminf(p / 240.0f, o->left / 2400.0f));
            float v = (float)sin(o->ph) * o->amp * env * 32767 * 0.5f;
            v += (float)sin(o->ph * 2) * o->amp * env * 32767 * 0.12f;
            o->ph += 2 * M_PI * o->f / RATE;
            acc[i * 2] += v; acc[i * 2 + 1] += v;
        }
    }
    float vol = SET.muted ? 0 : (SET.vol / 100.0f) * (SET.vol / 100.0f);
    for (int i = 0; i < BLOCK * 2; i++) {
        float v = acc[i] * vol;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

static void *mixer(void *arg) {
    (void)arg;
    static int16_t out[BLOCK * 2];
    for (;;) {
        mix_block(out);
        struct snd_xferi x = { .buf = out, .frames = BLOCK };
        int off = 0;
        while (off < BLOCK) {
            x.buf = out + off * 2; x.frames = BLOCK - off;
            if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
                if (errno == EPIPE || errno == ESTRPIPE) { ioctl(fd, SNDRV_PCM_IOCTL_PREPARE); continue; }
                if (errno == EINTR || errno == EAGAIN) continue;
                static int nerr;
                if (nerr++ < 5) LOG("audio: write failed (%s), re-preparing", strerror(errno));
                ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);   // host side hiccup (e.g. CoreAudio starting): recover like an xrun
                usleep(20000);
                break;
            }
            off += x.result;
        }
        dev_written += BLOCK;
        audio_frames_total += BLOCK;
        snd_pcm_sframes_t delay = 0;
        if (ioctl(fd, SNDRV_PCM_IOCTL_DELAY, &delay) < 0) delay = buf_frames / 2;
        pthread_mutex_lock(&mu);
        audible_dev = dev_written - delay;
        audible_t = now();
        pthread_mutex_unlock(&mu);
    }
    return 0;
}

void audio_init(void) {
    if (pcm_open() < 0) return;
    pthread_t th;
    pthread_create(&th, 0, mixer, 0);
}

void audio_tone(float freq, float secs, float vol) {
    pthread_mutex_lock(&mu);
    for (int t = 0; t < 8; t++)
        if (tones[t].left <= 0) {
            tones[t] = (Tone){ freq, vol, (long)(secs * RATE), (long)(secs * RATE), 0 };
            break;
        }
    pthread_mutex_unlock(&mu);
}
// tones are scheduled by delaying the start inside a helper thread
static void *alarm_th(void *a) {
    (void)a;
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 3; i++) { audio_tone(1320, 0.12f, 0.8f); usleep(170000); }
        usleep(450000);
    }
    return 0;
}
void audio_alarm(void) { pthread_t th; pthread_create(&th, 0, alarm_th, 0); pthread_detach(th); }

int audio_stream_space(void) { return RING - (int)(ring_wr - ring_rd) - 1; }
int audio_stream_buffered(void) { return (int)(ring_wr - ring_rd); }
int audio_stream_push(const int16_t *pcm, int frames) {
    int space = audio_stream_space();
    if (frames > space) frames = space;
    long w = ring_wr;
    for (int i = 0; i < frames; i++) {
        long k = (w + i) % RING;
        ring[k * 2] = pcm[i * 2];
        ring[k * 2 + 1] = pcm[i * 2 + 1];
    }
    __sync_synchronize();
    ring_wr = w + frames;
    return frames;
}
void audio_stream_reset(void) {
    pthread_mutex_lock(&mu);
    gen++;
    ring_rd = ring_wr;
    stream_consumed = 0;
    pthread_mutex_unlock(&mu);
}
void audio_stream_pause(bool p) { paused = p; }

// seconds of stream audio that have actually come out of the speaker since reset
double audio_stream_clock(void) {
    pthread_mutex_lock(&mu);
    double t = now();
    long d = audible_dev + (long)((t - audible_t) * RATE);
    if (d > dev_written) d = dev_written;
    long pos = -1;
    for (int k = 0; k < 128; k++) {
        Rec *r = &recs[(rec_i - 1 - k + 256) % 128];
        if (r->gen != gen) break;
        if (d >= r->dev0) { pos = r->s0 + mini((int)(d - r->dev0), r->n); break; }
        pos = r->s0;   // older than everything we know: earliest stream position
    }
    pthread_mutex_unlock(&mu);
    if (pos < 0) pos = 0;
    return pos / (double)RATE;
}
