#pragma once
#include "gba.h"

/* PSG-only audio: music on channels 2 (lead), 1 (harmony, yields to sound
 * effects), 3 (wave bass) and 4 (drums). No CPU mixing needed. */

typedef struct {
  const u8* lead; u16 lead_len;      /* (note, ticks) pairs, note 0 = rest */
  const u8* harm; u16 harm_len;      /* optional second voice on ch1 when no sfx */
  const u8* bass; u16 bass_len;
  const u8* drums; u16 drums_len;    /* (drum, ticks): 1 kick 2 snare 3 hat 4 open hat */
  u8 frames_per_tick;
  u8 lead_duty, lead_env, bass_vol;  /* env: volume << 4 | decay step (0 = hold) */
  u8 harm_duty, harm_env;
  u8 vib_delay, vib_depth;           /* lead vibrato: frames before it starts, depth in 1/16 semitone */
  u8 bass_wave;                      /* 0 triangle, 1 soft saw */
} Song;

enum { SFX_PICKUP, SFX_PUTBACK, SFX_ROTATE_L, SFX_ROTATE_R, SFX_PLACE, SFX_ERROR, SFX_SELECT, SFX_SCORE, SFX_GAMEOVER, SFX_MOVE };

void snd_init(void);
void snd_play_song(const Song* s);
void snd_stop_song(void);
void snd_sfx(int id);
void snd_tick(void);     /* once per frame, from the VBlank interrupt (video.c) */
