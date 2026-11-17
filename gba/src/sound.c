#include "sound.h"

/* note frequencies (Hz * 16) for MIDI 0..11 at octave -1 -> computed by shifting */
static const u32 base_f16[12] = { 131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247 }; /* MIDI 0..11 * 16 ≈ Hz*16 */

static u32 note_hz16(int n) {             /* Hz * 16 */
  int oct = n / 12, k = n % 12;
  return base_f16[k] << oct;
}
static u16 sq_rate(int n) { u32 f = note_hz16(n); if (!f) return 0; s32 r = 2048 - (s32)(131072u * 16 / f); return r < 0 ? 0 : r; }
static u16 wv_rate(int n) { u32 f = note_hz16(n); if (!f) return 0; s32 r = 2048 - (s32)(65536u * 16 / f); return r < 0 ? 0 : r; }

/* ---- sfx scripts: {frames, note, duty|env byte, flags} ---- */
typedef struct { u8 frames, note, env, flags; } SfxStep;   /* env: vol<<4 | step ; flags: duty | 0x10 noise | 0x20 sweep-up | 0x40 sweep-down */
#define S(f, n, e, fl) { f, n, e, fl }
static const SfxStep sfx_pickup[] = { S(3, 72, 0xC1, 0x22), S(4, 79, 0xA2, 0x02), S(0, 0, 0, 0) };
static const SfxStep sfx_putback[] = { S(3, 76, 0xB1, 0x42), S(5, 67, 0x92, 0x02), S(0, 0, 0, 0) };
static const SfxStep sfx_rotl[] = { S(3, 81, 0x91, 0x01), S(0, 0, 0, 0) };
static const SfxStep sfx_rotr[] = { S(3, 84, 0x91, 0x01), S(0, 0, 0, 0) };
static const SfxStep sfx_place[] = { S(2, 0x34, 0xF2, 0x10), S(4, 55, 0xC2, 0x42), S(0, 0, 0, 0) };
static const SfxStep sfx_error[] = { S(5, 46, 0xB0, 0x02), S(3, 0, 0, 0), S(6, 46, 0xB2, 0x02), S(0, 0, 0, 0) };
static const SfxStep sfx_select[] = { S(2, 88, 0x81, 0x02), S(0, 0, 0, 0) };
static const SfxStep sfx_score[] = { S(4, 84, 0xA1, 0x02), S(6, 91, 0xA3, 0x02), S(0, 0, 0, 0) };
static const SfxStep sfx_move[] = { S(1, 96, 0x31, 0x00), S(0, 0, 0, 0) };
static const SfxStep sfx_gameover[] = { S(8, 72, 0xC2, 0x02), S(8, 67, 0xC2, 0x02), S(8, 63, 0xC2, 0x02), S(20, 60, 0xC4, 0x02), S(0, 0, 0, 0) };
static const SfxStep* const sfx_tab[] = { sfx_pickup, sfx_putback, sfx_rotl, sfx_rotr, sfx_place, sfx_error, sfx_select, sfx_score, sfx_gameover, sfx_move };

static const SfxStep* sfx_cur; static int sfx_t;
static int noise_busy;

typedef struct { const u8* d; u16 len, pos; s16 t; } Voice;
static const Song* song;
static Voice v_lead, v_harm, v_bass, v_drum;
static int frame_ctr;

static const u32 wave_bass[4] = { 0xEFCDAB89, 0x67452301, 0x10325476, 0x98BADCFE }; /* triangle */

void snd_init(void) {
  REG_SOUNDCNT_X = 0x80;
  REG_SOUNDCNT_L = 0xFF77;
  REG_SOUNDCNT_H = 0x0002;
  REG_SOUND1CNT_L = 0x0008;
  REG_SOUND3CNT_L = 0x0000;          /* select bank so we write bank 1 */
  for (int i = 0; i < 4; i++) WAVE_RAM[i] = wave_bass[i];
  REG_SOUND3CNT_L = 0x0040;          /* play bank 1, 32 samples, disabled */
  song = 0; sfx_cur = 0;
}

static void voice_start(Voice* v, const u8* d, u16 len) { v->d = d; v->len = len; v->pos = 0; v->t = 0; }

void snd_play_song(const Song* s) {
  song = s; frame_ctr = 0;
  voice_start(&v_lead, s->lead, s->lead_len);
  voice_start(&v_harm, s->harm, s->harm_len);
  voice_start(&v_bass, s->bass, s->bass_len);
  voice_start(&v_drum, s->drums, s->drums_len);
}
void snd_stop_song(void) {
  song = 0;
  REG_SOUND2CNT_L = 0; REG_SOUND2CNT_H = 0x8000;
  REG_SOUND3CNT_L = 0x0040;
  if (!noise_busy) { REG_SOUND4CNT_L = 0; REG_SOUND4CNT_H = 0x8000; }
  if (!sfx_cur) { REG_SOUND1CNT_H = 0; REG_SOUND1CNT_X = 0x8000; }
}

void snd_sfx(int id) { sfx_cur = sfx_tab[id]; sfx_t = 0; }

static void sfx_apply(const SfxStep* s) {
  if (s->flags & 0x10) {
    noise_busy = s->frames;
    REG_SOUND4CNT_L = (s->env >> 4) << 12 | (s->env & 7) << 8;
    REG_SOUND4CNT_H = 0x8000 | s->note;
    return;
  }
  if (!s->note) { REG_SOUND1CNT_H = 0; REG_SOUND1CNT_X = 0x8000; return; }
  u16 sweep = 0x0008;
  if (s->flags & 0x20) sweep = 0x0012;          /* up, step 2 */
  if (s->flags & 0x40) sweep = 0x001A;          /* down */
  REG_SOUND1CNT_L = sweep;
  REG_SOUND1CNT_H = ((s->flags & 3) << 6) | (s->env >> 4) << 12 | (s->env & 7) << 8;
  REG_SOUND1CNT_X = 0x8000 | sq_rate(s->note);
}

static void drum(int k) {
  if (noise_busy) return;
  switch (k) {
    case 1: REG_SOUND4CNT_L = 0xC100 | 0x00; REG_SOUND4CNT_H = 0x8000 | 0x61; break;   /* kick */
    case 2: REG_SOUND4CNT_L = 0xA200; REG_SOUND4CNT_H = 0x8000 | 0x32; break;          /* snare */
    case 3: REG_SOUND4CNT_L = 0x5100; REG_SOUND4CNT_H = 0x8000 | 0x08 | 0x10; break;   /* hat (7-bit) */
  }
}

/* returns new note when a note starts, -1 otherwise; 0 for rest */
static int voice_step(Voice* v) {
  if (!v->d || !v->len) return -1;
  if (v->t > 0) { v->t--; return -1; }
  if (v->pos >= v->len) v->pos = 0;
  int n = v->d[v->pos], dur = v->d[v->pos + 1];
  v->pos += 2;
  v->t = dur - 1;
  return n;
}

void snd_tick(void) {
  if (noise_busy) noise_busy--;
  if (sfx_cur) {
    const SfxStep* s = &sfx_cur[0];
    if (sfx_t == 0) sfx_apply(s);
    if (++sfx_t >= s->frames) {
      sfx_cur++; sfx_t = 0;
      if (!sfx_cur->frames) { sfx_cur = 0; REG_SOUND1CNT_H = 0; REG_SOUND1CNT_X = 0x8000; }
    }
  }
  if (!song) return;
  if (++frame_ctr < song->frames_per_tick) return;
  frame_ctr = 0;
  int n;
  if ((n = voice_step(&v_lead)) >= 0) {
    if (n) { REG_SOUND2CNT_L = song->lead_duty << 6 | song->lead_env << 8; REG_SOUND2CNT_H = 0x8000 | sq_rate(n); }
    else { REG_SOUND2CNT_L = 0; REG_SOUND2CNT_H = 0x8000; }
  }
  if ((n = voice_step(&v_harm)) >= 0 && !sfx_cur) {
    if (n) { REG_SOUND1CNT_L = 0x0008; REG_SOUND1CNT_H = (2 << 6) | 0x5000 | 0x0300; REG_SOUND1CNT_X = 0x8000 | sq_rate(n); }
    else { REG_SOUND1CNT_H = 0; REG_SOUND1CNT_X = 0x8000; }
  }
  if ((n = voice_step(&v_bass)) >= 0) {
    if (n) { REG_SOUND3CNT_L = 0x00C0; REG_SOUND3CNT_H = song->bass_vol << 13; REG_SOUND3CNT_X = 0x8000 | wv_rate(n); }
    else REG_SOUND3CNT_L = 0x0040;
  }
  if ((n = voice_step(&v_drum)) > 0) drum(n);
}
