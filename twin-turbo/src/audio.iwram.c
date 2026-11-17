/* Sample mixer + sequencer (ARM, IWRAM). See audio.h. */
#include "audio.h"
#include "gen/music_gen.h"

typedef struct {
  const s8* data;
  u32 pos, inc, end, loop_len;   /* Q12 */
  s32 env; u8 stage, vel, on, chv;
  const Instrument* ins;
} Voice;

enum { ST_OFF, ST_ATK, ST_DEC, ST_SUS, ST_REL };

static s8 mixbuf[MIX_LEN * 2] __attribute__((aligned(4)));
static s32 acc[MIX_LEN];
static Voice voice[NVOICE];
static volatile int playing_half;

static const Song* song;
static u32 ev_pos;
static s32 wait_ticks;
static s32 to_tick_q8;           /* samples until the next tick, Q8 */
static int master = 16, fade_len, fade_t;
static u8 ch_vol[12];

/* Q16 ratio 2^(k/12) for k = 0..11 */
static const u32 semi_q16[12] = { 65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116772, 123715 };

static u32 note_inc(u32 inc60, int note) {
  int d = note - 60, oct = 0;
  while (d < 0) { d += 12; oct--; }
  while (d >= 12) { d -= 12; oct++; }
  u32 r = (u32)(((u64)inc60 * semi_q16[d]) >> 16);
  return oct >= 0 ? r << oct : r >> -oct;
}

/* ---------------------------------------------------------------- PSG */
/* GB-style square rate for MIDI note n: 2048 - 131072 / f. dist_tab is
 * 131072 / f for MIDI 36..47; each octave up halves it. */
static const u16 dist_tab[12] = { 2004, 1891, 1785, 1685, 1591, 1501, 1417, 1337, 1262, 1192, 1125, 1062 };
static u16 sq_rate(int n) {
  if (n < 36) n = 36;
  int oct = (n - 36) / 12, k = (n - 36) % 12;
  return 2048 - (dist_tab[k] >> oct);
}

static int sfx_noise_busy, sfx_busy;

static void noise_hit(int preset, int vel) {
  if (sfx_noise_busy) return;
  int v = (vel * 15 + 64) >> 7;
  if (v > 15) v = 15;
  v = (v * master) >> 4;
  switch (preset) {
    default:
    case 0: REG_SOUND4CNT_L = (v << 12) | 0x0100; REG_SOUND4CNT_H = 0x8000 | 0x00; break;          /* closed hat: shortest decay, top noise */
    case 1: REG_SOUND4CNT_L = (v << 12) | 0x0300; REG_SOUND4CNT_H = 0x8000 | 0x01; break;          /* open hat */
    case 2: REG_SOUND4CNT_L = ((v * 3 / 4) << 12) | 0x0100; REG_SOUND4CNT_H = 0x8000 | 0x10 | 0x02; break; /* shaker (7-bit, slightly tonal) */
    case 3: REG_SOUND4CNT_L = (v << 12) | 0x0600; REG_SOUND4CNT_H = 0x8000 | 0x13; break;          /* crash/ride wash */
  }
}

static void square_note(int note, int vel) {
  if (!note) { REG_SOUND2CNT_L = 0; REG_SOUND2CNT_H = 0x8000; return; }
  int v = ((song->sq_env >> 4) * vel + 64) >> 7;
  v = (v * master) >> 4;
  REG_SOUND2CNT_L = (song->sq_duty << 6) | (v << 12) | ((song->sq_env & 7) << 8);
  REG_SOUND2CNT_H = 0x8000 | sq_rate(note);
}

/* ---------------------------------------------------------------- sequencer */
static void note_on(int ch, int note, int vel) {
  int kind = song->kind[ch];
  if (kind == CH_NOISE) { if (vel) noise_hit(note, vel); return; }
  if (kind == CH_SQUARE) { square_note(vel ? note : 0, vel); return; }
  if (ch >= NVOICE) return;
  Voice* v = &voice[ch];
  if (!vel) { if (v->stage != ST_OFF) v->stage = ST_REL; return; }
  const Instrument* ins = &instruments[song->inst[ch] + (kind == CH_KIT ? note : 0)];
  v->ins = ins;
  v->data = ins->data;
  v->pos = 0;
  v->inc = ins->fixed || kind == CH_KIT ? ins->inc60 : note_inc(ins->inc60, note);
  v->end = ins->len << 12;
  v->loop_len = (ins->len - ins->loop) << 12;
  v->vel = vel;
  v->env = ins->atk >= 65535 ? 65535 : 0;
  v->stage = ins->atk >= 65535 ? ST_DEC : ST_ATK;
}

static void env_tick(void) {
  for (int i = 0; i < NVOICE; i++) {
    Voice* v = &voice[i];
    const Instrument* ins = v->ins;
    switch (v->stage) {
      case ST_ATK: v->env += ins->atk; if (v->env >= 65535) { v->env = 65535; v->stage = ST_DEC; } break;
      case ST_DEC: v->env -= ins->dec; if (v->env <= ins->sus) { v->env = ins->sus; v->stage = ST_SUS; } break;
      case ST_SUS: break;
      case ST_REL: v->env -= ins->rel; if (v->env <= 0) { v->env = 0; v->stage = ST_OFF; } break;
    }
    if (v->stage == ST_SUS && v->env <= 0) v->stage = ST_OFF;
  }
}

static void seq_tick(void) {
  env_tick();
  if (!song) return;
  if (wait_ticks > 0) { if (--wait_ticks > 0) return; }
  const u8* e = song->ev;
  for (;;) {
    if (ev_pos >= song->len) { ev_pos = song->loop_at; continue; }
    u8 b = e[ev_pos];
    if (b < 0x80) { ev_pos++; wait_ticks = b; if (b) return; continue; }
    if (b == 0xFE) { wait_ticks = e[ev_pos + 1] | (e[ev_pos + 2] << 8); ev_pos += 3; if (wait_ticks) return; continue; }
    if (b == 0xFF) { ev_pos = song->loop_at; continue; }
    note_on(b & 0x0F, e[ev_pos + 1], e[ev_pos + 2]);
    ev_pos += 3;
  }
}

/* ---------------------------------------------------------------- mixer */
static void mix_voice(Voice* v, s32* out, int n) {
  int vol = ((v->env >> 8) * v->vel * v->ins->vol) >> 15;    /* 0..63 */
  vol = (vol * master) >> 4;
  if (fade_len) vol = vol * (fade_len - fade_t) / fade_len;
  if (vol <= 0) {
    /* still advance so loops stay in phase */
    u32 adv = v->inc * n;
    v->pos += adv;
    if (v->pos >= v->end) {
      if (v->loop_len) { while (v->pos >= v->end) v->pos -= v->loop_len; }
      else v->stage = ST_OFF;
    }
    return;
  }
  const s8* d = v->data;
  u32 pos = v->pos, inc = v->inc, end = v->end;
  while (n > 0) {
    u32 left = (end - pos + inc - 1) / inc;
    int c = (u32)n < left ? n : (int)left;
    n -= c;
    while (c >= 4) {
      out[0] += d[pos >> 12] * vol; pos += inc;
      out[1] += d[pos >> 12] * vol; pos += inc;
      out[2] += d[pos >> 12] * vol; pos += inc;
      out[3] += d[pos >> 12] * vol; pos += inc;
      out += 4; c -= 4;
    }
    while (c-- > 0) { *out++ += d[pos >> 12] * vol; pos += inc; }
    if (pos >= end) {
      if (v->loop_len) pos -= v->loop_len;
      else { v->stage = ST_OFF; break; }
    }
  }
  v->pos = pos;
}

static void mix_segment(int start, int n) {
  for (int i = 0; i < NVOICE; i++)
    if (voice[i].stage != ST_OFF) mix_voice(&voice[i], acc + start, n);
}

void audio_frame(void) {
  s8* out = mixbuf + (playing_half ? 0 : MIX_LEN);   /* fill the half that isn't playing */
  for (int i = 0; i < MIX_LEN; i++) acc[i] = 0;
  int pos = 0;
  while (pos < MIX_LEN) {
    int until = (to_tick_q8 + 255) >> 8;
    if (until <= 0) until = 1;
    int n = MIX_LEN - pos;
    if (n > until) n = until;
    mix_segment(pos, n);
    pos += n;
    to_tick_q8 -= n << 8;
    if (to_tick_q8 <= 0) {
      seq_tick();
      to_tick_q8 += song ? song->spt_q8 : (183 << 8);
    }
  }
  for (int i = 0; i < MIX_LEN; i++) {
    s32 s = acc[i] >> 7;
    out[i] = s > 127 ? 127 : s < -128 ? -128 : s;
  }
  if (fade_len && fade_t < fade_len && ++fade_t >= fade_len) { audio_stop(); }
  if (sfx_noise_busy) sfx_noise_busy--;
}

void audio_vblank(void) {
  if (playing_half == 1) {
    REG_DMA1CNT = 0;
    REG_DMA1SAD = (u32)mixbuf;
    REG_DMA1CNT = DMA_ENABLE | DMA_SPECIAL | DMA_REPEAT | DMA_32 | DMA_DST_FIXED;
    playing_half = 0;
  } else playing_half = 1;
}

void audio_init(void) {
  REG_SOUNDCNT_X = 0x80;
  REG_SOUNDCNT_L = 0xFF77;              /* PSG: all channels both sides, full volume */
  REG_SOUNDCNT_H = 0x0B06;              /* PSG 100%, DSA 100%, DSA L+R, timer 0, reset FIFO */
  REG_SOUNDBIAS = (REG_SOUNDBIAS & 0x3FF) | 0x4000;   /* 8-bit / 65 kHz PWM: best for 18 kHz output */
  for (int i = 0; i < MIX_LEN * 2; i++) mixbuf[i] = 0;
  REG_TM0CNT_H = 0;
  REG_TM0CNT_L = 65536 - 924;           /* 16.78 MHz / 924 = 18157 Hz */
  REG_DMA1CNT = 0;
  REG_DMA1SAD = (u32)mixbuf;
  REG_DMA1DAD = (u32)&REG_FIFO_A;
  REG_DMA1CNT = DMA_ENABLE | DMA_SPECIAL | DMA_REPEAT | DMA_32 | DMA_DST_FIXED;
  REG_TM0CNT_H = 0x0080;
  playing_half = 0;
  REG_SOUND1CNT_L = 0x0008;
  song = 0;
}

void audio_play(const Song* s) {
  for (int i = 0; i < NVOICE; i++) voice[i].stage = ST_OFF;
  fade_len = fade_t = 0;
  wait_ticks = 0; ev_pos = 0; to_tick_q8 = 0;
  song = s;
  REG_SOUND2CNT_L = 0; REG_SOUND2CNT_H = 0x8000;
}
void audio_stop(void) {
  song = 0; fade_len = fade_t = 0;
  for (int i = 0; i < NVOICE; i++) if (voice[i].stage != ST_OFF) voice[i].stage = ST_REL;
  REG_SOUND2CNT_L = 0; REG_SOUND2CNT_H = 0x8000;
}
void audio_fade(int frames) { fade_len = frames; fade_t = 0; }
void audio_set_master(int v) { master = v; }

/* ---------------------------------------------------------------- SFX (PSG 1 + noise) */
typedef struct { u8 frames, note, env, flags; } SfxStep;   /* env: vol<<4|step; flags: duty | 0x10 noise | 0x20 sweep up | 0x40 sweep down */
#define S(f, n, e, fl) { f, n, e, fl }
static const SfxStep s_jump[] = { S(2, 67, 0xA1, 0x22), S(6, 74, 0x82, 0x22), S(0, 0, 0, 0) };
static const SfxStep s_land[] = { S(2, 0x45, 0x61, 0x10), S(0, 0, 0, 0) };
static const SfxStep s_orb[] = { S(3, 88, 0xA1, 0x02), S(3, 93, 0xA1, 0x02), S(8, 100, 0x92, 0x02), S(0, 0, 0, 0) };
static const SfxStep s_hurt[] = { S(3, 0x55, 0xF2, 0x10), S(4, 60, 0xC1, 0x41), S(8, 52, 0xA2, 0x41), S(0, 0, 0, 0) };
static const SfxStep s_select[] = { S(2, 84, 0x81, 0x02), S(0, 0, 0, 0) };
static const SfxStep s_confirm[] = { S(4, 76, 0xB1, 0x02), S(4, 83, 0xB1, 0x02), S(10, 88, 0xB3, 0x02), S(0, 0, 0, 0) };
static const SfxStep s_whoosh[] = { S(20, 0x24, 0x84, 0x10), S(0, 0, 0, 0) };
static const SfxStep s_boost[] = { S(4, 0x34, 0xC1, 0x10), S(6, 60, 0xB2, 0x23), S(10, 72, 0x93, 0x23), S(0, 0, 0, 0) };
static const SfxStep s_door[] = { S(3, 0x46, 0xB1, 0x10), S(3, 0, 0, 0), S(4, 0x56, 0x91, 0x10), S(0, 0, 0, 0) };
static const SfxStep s_count[] = { S(10, 69, 0xC2, 0x02), S(0, 0, 0, 0) };
static const SfxStep s_go[] = { S(24, 81, 0xF3, 0x02), S(0, 0, 0, 0) };
static const SfxStep s_bump[] = { S(3, 0x63, 0xD1, 0x10), S(4, 50, 0xB1, 0x41), S(0, 0, 0, 0) };
static const SfxStep* const sfx_tab[] = { s_jump, s_land, s_orb, s_hurt, s_select, s_confirm, s_whoosh, s_boost, s_door, s_count, s_go, s_bump };
static const SfxStep* sfx_cur; static int sfx_t;

static void sfx_apply(const SfxStep* s) {
  if (s->flags & 0x10) {
    sfx_noise_busy = s->frames + 2;
    REG_SOUND4CNT_L = (s->env >> 4) << 12 | (s->env & 7) << 8;
    REG_SOUND4CNT_H = 0x8000 | s->note;
    return;
  }
  if (!s->note) { REG_SOUND1CNT_H = 0; REG_SOUND1CNT_X = 0x8000; return; }
  u16 sweep = 0x0008;
  if (s->flags & 0x20) sweep = 0x0013;
  if (s->flags & 0x40) sweep = 0x001B;
  REG_SOUND1CNT_L = sweep;
  REG_SOUND1CNT_H = ((s->flags & 3) << 6) | (s->env >> 4) << 12 | (s->env & 7) << 8;
  REG_SOUND1CNT_X = 0x8000 | sq_rate(s->note);
}

void sfx(int id) { sfx_cur = sfx_tab[id]; sfx_t = 0; sfx_busy = 1; }

void sfx_frame(void) {
  if (!sfx_cur) return;
  const SfxStep* s = sfx_cur;
  if (sfx_t == 0) sfx_apply(s);
  if (++sfx_t >= s->frames) {
    sfx_cur++; sfx_t = 0;
    if (!sfx_cur->frames) { sfx_cur = 0; sfx_busy = 0; REG_SOUND1CNT_H = 0; REG_SOUND1CNT_X = 0x8000; }
  }
}
