/* Twin Turbo: a GBA game starring That Franco and tripleset. */
#include "game.h"
#include "video.h"
#include "audio.h"

GameState gs;
static u32 rng_state = 0x1996BEEF;
u32 rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

int main(void) {
  REG_WAITCNT = 0x4317;            /* faster ROM access + prefetch */
  REG_DISPSTAT = 0x0008;           /* VBlank IRQ */
  REG_IE = 0x0001;
  REG_IME = 1;
  audio_init();
  gs.who = WHO_FRANCO;
  for (;;) {
    if (title_screen()) continue;
    if (select_screen()) continue;
    /* hold L+R while confirming on the select screen to jump straight to the race */
    if (gs.skip_run) { gs.orbs = 40; gs.lives = 3; }
    else if (run_stage()) { gameover_screen(); continue; }
    if (drive_stage()) continue;
    end_screen();
  }
}
