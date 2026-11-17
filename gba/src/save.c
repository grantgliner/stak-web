#include "save.h"
#define SAVE_MAGIC  0x4B415453u   /* "STAK" */
#define SAVE_MAGIC2 0x5345444Du   /* "MDES": per-mode bests */
#define SAVE_MAGIC3 0x43524141u   /* "AARC": arcade best */
SaveData save;
void save_load(void) {
  volatile u8* sram = (volatile u8*)MEM_SRAM;
  u8* d = (u8*)&save;
  for (unsigned i = 0; i < sizeof save; i++) d[i] = sram[i];
  if (save.magic != SAVE_MAGIC) { save.magic = SAVE_MAGIC; save.best_blocks = 0; save.best_score = 0; save.music_on = 1; save.magic2 = 0; }
  if (save.magic2 != SAVE_MAGIC2) {
    save.magic2 = SAVE_MAGIC2; save.best[0] = 0; save.best[1] = save.best_blocks; save.best[2] = 0; save.mode = 1; save.pad3 = 0;
    save_write();
  }
  if (save.magic3 != SAVE_MAGIC3) { save.magic3 = SAVE_MAGIC3; save.best_arcade = 0; save.pad4 = 0; save_write(); }
  if (save.mode > 3) save.mode = 1;
}
void save_write(void) {
  volatile u8* sram = (volatile u8*)MEM_SRAM;
  const u8* d = (const u8*)&save;
  for (unsigned i = 0; i < sizeof save; i++) sram[i] = d[i];
}
