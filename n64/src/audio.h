#pragma once
enum { SFX_PICKUP, SFX_PLACE, SFX_PUTBACK, SFX_ROTATE_L, SFX_ROTATE_R, SFX_SELECT, SFX_ERROR, SFX_GAMEOVER, SFX_SCORE, SFX_COUNT };
void snd_init(void);
void snd_update(void);
void snd_sfx(int id);
void snd_render(short* out, int n);   /* mix n samples (stereo pairs) */
