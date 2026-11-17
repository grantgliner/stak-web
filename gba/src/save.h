#pragma once
#include "gba.h"
/* best_blocks is the classic best (the layout of the first version); the
   second part was added with the relaxed and expert modes */
typedef struct {
  u32 magic; u16 best_blocks; u16 pad; u32 best_score; u8 music_on, pad2[3];
  u32 magic2; u16 best[3];             /* per mode: relaxed, classic, expert */
  u8 mode, pad3;                       /* last mode played */
} SaveData;
extern SaveData save;
void save_load(void);
void save_write(void);
