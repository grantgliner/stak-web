#pragma once
#include "gba.h"
typedef struct { u32 magic; u16 best_blocks; u16 pad; u32 best_score; u8 music_on, pad2[3]; } SaveData;
extern SaveData save;
void save_load(void);
void save_write(void);
