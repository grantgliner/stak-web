#include "save.h"
#define SAVE_MAGIC 0x4B415453u   /* "STAK" */
SaveData save;
void save_load(void) {
  volatile u8* sram = (volatile u8*)MEM_SRAM;
  u8* d = (u8*)&save;
  for (unsigned i = 0; i < sizeof save; i++) d[i] = sram[i];
  if (save.magic != SAVE_MAGIC) { save.magic = SAVE_MAGIC; save.best_blocks = 0; save.best_score = 0; save.music_on = 1; save_write(); }
}
void save_write(void) {
  volatile u8* sram = (volatile u8*)MEM_SRAM;
  const u8* d = (const u8*)&save;
  for (unsigned i = 0; i < sizeof save; i++) sram[i] = d[i];
}
