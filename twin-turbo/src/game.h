#pragma once
#include "gba.h"

enum { WHO_FRANCO, WHO_TRIPLESET };

typedef struct {
  int who;          /* the character you control */
  int orbs;         /* collected in the run stage, spent as boosts in the drive */
  int lives;
  int score;
  int run_time;     /* frames */
  int place;        /* drive result, 1 = first */
  int drive_time;
  int skip_run;     /* L+R held on the select screen: straight to the race */
} GameState;
extern GameState gs;

/* stages: return 0 = continue to next, 1 = back to title */
int title_screen(void);
int select_screen(void);
int run_stage(void);
int drive_stage(void);
void end_screen(void);
void gameover_screen(void);

/* fixed point helpers */
#define FX(v) ((s32)((v) * 256))
extern const s16 sin_lut[256];    /* Q12, 256 steps per turn */
INLINE s32 lu_sin(int a) { return sin_lut[a & 255]; }
INLINE s32 lu_cos(int a) { return sin_lut[(a + 64) & 255]; }
u32 rnd(void);
