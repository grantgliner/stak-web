/* Sound effects: a tiny CPU synth (square, triangle and noise voices with pitch
 * slides and decaying envelopes) mixed into libdragon's audio buffers. Short
 * chirpy sounds in the spirit of the web version's, built from notes so the
 * ROM carries no samples. */
#include <stdint.h>
#include <string.h>
#include "audio.h"
#ifdef N64
#include <libdragon.h>
#endif

#define RATE 22050
#define BUFLEN_MAX 1024
enum { SQ, TRI, NOISE, SQ25 };
typedef struct { uint16_t delay, dur; uint16_t f0, f1; uint8_t wave, vol; } Note;   /* ms, ms, Hz, Hz, wave, 0..255 */
typedef struct { const Note* n; int count; } Sfx;

static const Note n_pickup[] = { { 0, 55, 520, 1040, SQ25, 70 }, { 45, 60, 1040, 1560, TRI, 150 } };
static const Note n_place[] = { { 0, 90, 240, 90, TRI, 230 }, { 0, 25, 1800, 400, NOISE, 60 }, { 60, 80, 784, 784, SQ25, 34 } };
static const Note n_putback[] = { { 0, 90, 700, 330, TRI, 170 }, { 0, 70, 700, 350, SQ25, 30 } };
static const Note n_rot_l[] = { { 0, 32, 880, 660, SQ25, 50 } };
static const Note n_rot_r[] = { { 0, 32, 660, 880, SQ25, 50 } };
static const Note n_select[] = { { 0, 28, 1320, 1320, TRI, 110 } };
static const Note n_error[] = { { 0, 70, 150, 140, SQ, 70 }, { 95, 90, 150, 130, SQ, 70 } };
static const Note n_gameover[] = { { 0, 150, 523, 523, TRI, 190 }, { 150, 150, 392, 392, TRI, 190 }, { 300, 150, 330, 330, TRI, 190 },
                                   { 450, 420, 262, 247, TRI, 210 }, { 450, 420, 131, 124, SQ25, 34 } };
static const Note n_score[] = { { 0, 60, 784, 784, TRI, 140 }, { 60, 60, 988, 988, TRI, 140 }, { 120, 140, 1175, 1175, TRI, 150 } };
static const Sfx sfx[SFX_COUNT] = {
#define S(a) { a, sizeof a / sizeof a[0] }
    S(n_pickup), S(n_place), S(n_putback), S(n_rot_l), S(n_rot_r), S(n_select), S(n_error), S(n_gameover), S(n_score),
#undef S
};

#define NVOICE 8
typedef struct {
    int active, wave;
    int32_t wait, len, t;      /* samples */
    uint32_t phase, inc, inc1; int32_t dinc;
    int vol;
} Voice;
static Voice voice[NVOICE];
static uint32_t noise = 0x12345;

static uint32_t hz_inc(int hz) { return (uint32_t)(((uint64_t)hz << 32) / RATE); }

void snd_sfx(int id) {
    if (id < 0 || id >= SFX_COUNT) return;
    const Sfx* s = &sfx[id];
    for (int k = 0; k < s->count; k++) {
        const Note* n = &s->n[k];
        /* take a free voice, or the one nearest its end */
        int best = 0; int32_t left = 0x7FFFFFFF;
        for (int v = 0; v < NVOICE; v++) {
            if (!voice[v].active) { best = v; left = -1; break; }
            int32_t l = voice[v].wait + voice[v].len - voice[v].t;
            if (l < left) { left = l; best = v; }
        }
        Voice* v = &voice[best];
        v->active = 1; v->wave = n->wave;
        v->wait = n->delay * RATE / 1000; v->len = n->dur * RATE / 1000; v->t = 0;
        if (v->len < 1) v->len = 1;
        v->phase = 0; v->inc = hz_inc(n->f0); v->inc1 = hz_inc(n->f1);
        v->dinc = (int32_t)(((int64_t)v->inc1 - v->inc) / v->len);
        v->vol = n->vol;
    }
}

/* mix n mono samples into out (stereo interleaved) */
void snd_render(short* out, int n) {
    static int32_t mix[BUFLEN_MAX];
    if (n > BUFLEN_MAX) n = BUFLEN_MAX;
    memset(mix, 0, n * sizeof mix[0]);
    for (int k = 0; k < NVOICE; k++) {
        Voice* v = &voice[k];
        if (!v->active) continue;
        for (int i = 0; i < n; i++) {
            if (v->wait > 0) { v->wait--; continue; }
            if (v->t >= v->len) { v->active = 0; break; }
            /* envelope: 3 ms attack, then a linear fall to 0 */
            int32_t a = RATE * 3 / 1000;
            int32_t env = v->t < a ? v->t * 256 / a : (v->len - v->t) * 256 / (v->len - a + 1);
            int32_t s;
            uint32_t p = v->phase;
            switch (v->wave) {
            case SQ: s = (p >> 31) ? 64 : -64; break;
            case SQ25: s = (p >> 30) == 0 ? 64 : -64; break;
            case TRI: { int32_t x = (int32_t)(p >> 24); s = x < 128 ? x - 64 : 191 - x; break; }
            default:
                if ((v->phase + v->inc) < p || v->t == 0) { noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5; }
                s = (noise & 128) ? 48 : -48; break;
            }
            mix[i] += s * env * v->vol >> 8;
            v->phase += v->inc;
            v->inc += v->dinc;
            v->t++;
        }
    }
    for (int i = 0; i < n; i++) {
        int32_t x = mix[i] * 2;
        if (x > 32767) x = 32767; if (x < -32768) x = -32768;
        out[2 * i] = out[2 * i + 1] = (short)x;
    }
}

#ifdef N64
/* A polled AI driver: no interrupts, the AI's two-entry DMA queue is topped up
   once a frame. (libdragon's interrupt-driven driver hangs under the browser
   emulator this ROM targets.) Buffers of 1024 samples: two queued cover 93 ms,
   enough for frames down to ~12 fps. */
#define AI_BASE   0xA4500000
#define AI_REG(o) (*(volatile uint32_t*)(AI_BASE + (o)))
#define AI_DRAM   0x00
#define AI_LEN    0x04
#define AI_CTRL   0x08
#define AI_STATUS 0x0C
#define AI_DACRATE 0x10
#define AI_BITRATE 0x14
#define NBUF 3
#define BUFLEN 1024
static short ai_buf[NBUF][BUFLEN * 2] __attribute__((aligned(16)));
static int ai_next;

void snd_init(void) {
    int clock = get_tv_type() == TV_PAL ? 49656530 : get_tv_type() == TV_MPAL ? 48628316 : 48681812;
    int dacrate = ((2 * clock / RATE) + 1) / 2 - 1;
    int bitrate = dacrate / 66; if (bitrate > 16) bitrate = 16;
    AI_REG(AI_DACRATE) = dacrate;
    AI_REG(AI_BITRATE) = bitrate - 1;
    AI_REG(AI_CTRL) = 1;
}

void snd_update(void) {
    for (int k = 0; k < 2 && !(AI_REG(AI_STATUS) & 0x80000000); k++) {
        short* b = ai_buf[ai_next];
        snd_render(b, BUFLEN);
        data_cache_hit_writeback(b, sizeof ai_buf[0]);
        AI_REG(AI_DRAM) = PhysicalAddr(b);
        AI_REG(AI_LEN) = sizeof ai_buf[0];
        ai_next = (ai_next + 1) % NBUF;
    }
}
#endif
