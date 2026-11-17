#pragma once
#include "gba.h"

/* PSG-only audio: music on channels 2 (lead), 3 (wave bass) and 4 (drums);
 * sound effects on channel 1 (and briefly 4). No CPU mixing needed. */

typedef struct {
  const u8* lead; u16 lead_len;      /* (note, ticks) pairs, note 0 = rest */
  const u8* harm; u16 harm_len;      /* optional second voice on ch1 when no sfx */
  const u8* bass; u16 bass_len;
  const u8* drums; u16 drums_len;    /* (drum, ticks): 1 kick 2 snare 3 hat */
  u8 frames_per_tick;
  u8 lead_duty, lead_env, bass_vol;
} Song;

enum { SFX_PICKUP, SFX_PUTBACK, SFX_ROTATE_L, SFX_ROTATE_R, SFX_PLACE, SFX_ERROR, SFX_SELECT, SFX_SCORE, SFX_GAMEOVER, SFX_MOVE };

void snd_init(void);
void snd_play_song(const Song* s);
void snd_stop_song(void);
void snd_sfx(int id);
void snd_tick(void);     /* call once per frame */
