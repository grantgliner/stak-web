/* Title, character select, ending and game over (Mode 4 bitmaps + sprites). */
#include "game.h"
#include "video.h"
#include "audio.h"
#include "gen/title_gen.h"
#include "gen/run_gen.h"
#include "gen/music_gen.h"

#define T_A 512            /* Mode 4: OBJ tiles start at 512 */
#define T_B (512 + 24)
#define T_CAR (512 + 48)
#define T_FONT (512 + 112)
#define T_ORB (512 + 208)
#define P_HUD 8

static void load_bitmap(const u32* bmp, const u16* pal) {
  REG_DISPCNT = 0x0080;
  dma3_copy32((void*)MEM_VRAM, bmp, 240 * 160 / 4);
  dma3_copy32(PAL_BG, pal, 128);
  dma3_copy32(PAL_OBJ, run_obj_pal, sizeof(run_obj_pal) / 4);
  dma3_copy32(OBJ_TILES + T_CAR * 16, hero_side_tiles, 32 * 8 * 2);
  dma3_copy32(OBJ_TILES + T_FONT * 16, font_tiles, sizeof(font_tiles) / 4);
  dma3_copy32(OBJ_TILES + T_ORB * 16, orb_tiles, sizeof(orb_tiles) / 4);
  REG_BG2PA = 256; REG_BG2PB = 0; REG_BG2PC = 0; REG_BG2PD = 256; REG_BG2X = 0; REG_BG2Y = 0;
  REG_BGCNT[2] = 0;
}

static const u32* char_src(int who) { return who == WHO_FRANCO ? char_franco : char_tripleset; }

static void set_char(int slot_tile, int who, int frame) {
  dma3_copy32(OBJ_TILES + slot_tile * 16, char_src(who) + frame * 192, 192);
}

static void put_char(int tile, int who, int x, int y, int hflip) {
  ObjAttr* o = oam_add();
  o->attr0 = ATTR0_Y(y) | ATTR0_SQUARE;
  o->attr1 = ATTR1_X(x) | ATTR1_SIZE_32 | (hflip ? ATTR1_HFLIP : 0);
  o->attr2 = ATTR2_TILE(tile) | ATTR2_PAL(who) | ATTR2_PRIO(0);
  o = oam_add();
  o->attr0 = ATTR0_Y(y + 32) | ATTR0_WIDE;
  o->attr1 = ATTR1_X(x) | ATTR1_SIZE_32 | (hflip ? ATTR1_HFLIP : 0);
  o->attr2 = ATTR2_TILE(tile + 16) | ATTR2_PAL(who) | ATTR2_PRIO(0);
}

static void zoom(int z) {   /* z: Q8 scale of the screen around its centre (256 = 1x) */
  s32 inv = 65536 / z;      /* texture step per pixel */
  REG_BG2PA = inv; REG_BG2PD = inv; REG_BG2PB = 0; REG_BG2PC = 0;
  REG_BG2X = (120 << 8) - 120 * inv;
  REG_BG2Y = (80 << 8) - 80 * inv;
}

int title_screen(void) {
  video_off();
  load_bitmap(title_bitmap, title_pal);
  set_fade(16);
  zoom(1400);
  REG_DISPCNT = 4 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D;
  audio_play(&song_title);
  int t = 0;
  s32 car_x = -800;
  for (;;) {
    t++;
    /* logo slam: zoom from far in, settle with a little overshoot */
    if (t <= 24) { int k = 24 - t; zoom(256 + k * k * 2); set_fade(16 - t * 16 / 24); }
    else if (t <= 34) { int k = t - 24; zoom(256 - (k < 5 ? k : 10 - k) * 4); }
    else zoom(256);
    set_char(T_A, WHO_FRANCO, (t >> 4) & 1 ? CF_IDLE1 : CF_IDLE0);
    set_char(T_B, WHO_TRIPLESET, (t >> 4) & 1 ? CF_IDLE0 : CF_IDLE1);
    oam_begin();
    if (t > 40 && ((t >> 5) & 1)) obj_text(76, 140, "PRESS START", T_FONT, P_HUD);
    put_char(T_A, WHO_FRANCO, 6, 108, 0);
    put_char(T_B, WHO_TRIPLESET, 202, 108, 1);
    /* the twin turbo car streaks across now and then */
    car_x += 9;
    if (car_x > 2400) car_x = -900;
    if (car_x > -64 && car_x < 240) {
      ObjAttr* o = oam_add();
      o->attr0 = ATTR0_Y(122) | ATTR0_WIDE;
      o->attr1 = ATTR1_X(car_x) | ATTR1_SIZE_64;
      o->attr2 = ATTR2_TILE(T_CAR + ((t >> 2) & 1) * 32) | ATTR2_PAL(7) | ATTR2_PRIO(1);
    }
    oam_hide_rest();
    video_wait();
    if (t > 30 && (keys_hit & (KEY_START | KEY_A))) { sfx(SFX_CONFIRM); break; }
  }
  return 0;
}

int select_screen(void) {
  int sel = gs.who;
  REG_BLDCNT = 0x00C4;     /* darken BG2 only */
  REG_BLDY = 9;
  for (int t = 0;; t++) {
    if (keys_hit & (KEY_LEFT | KEY_RIGHT)) { sel ^= 1; sfx(SFX_SELECT); }
    if (keys_hit & KEY_B) { REG_BLDCNT = 0; return 1; }
    set_char(T_A, WHO_FRANCO, sel == WHO_FRANCO ? CF_RUN0 + ((t >> 3) % 6) : CF_IDLE0);
    set_char(T_B, WHO_TRIPLESET, sel == WHO_TRIPLESET ? CF_RUN0 + ((t >> 3) % 6) : CF_IDLE0);
    oam_begin();
    obj_text(48, 16, "CHOOSE YOUR RUNNER", T_FONT, P_HUD);
    obj_text(28, 128, "THAT FRANCO", T_FONT, P_HUD);
    obj_text(140, 128, "TRIPLESET", T_FONT, P_HUD);
    int ax = sel == WHO_FRANCO ? 66 : 172;
    if ((t >> 3) & 1) obj_text(ax, 46, "V", T_FONT, P_HUD);
    put_char(T_A, WHO_FRANCO, 50, 68, 0);
    put_char(T_B, WHO_TRIPLESET, 156, 68, 0);
    obj_text(32, 146, "SELECT SWAPS MID-RUN", T_FONT, P_HUD);
    oam_hide_rest();
    video_wait();
    if (keys_hit & (KEY_A | KEY_START)) { sfx(SFX_CONFIRM); break; }
  }
  gs.skip_run = (keys & (KEY_L | KEY_R)) == (KEY_L | KEY_R);
  gs.who = sel;
  gs.score = 0;
  audio_fade(30);
  fade_to(-16, 30);
  return 0;
}

static void num_text(int x, int y, const char* label, int v) {
  char b[24]; int n = 0;
  while (label[n]) { b[n] = label[n]; n++; }
  fmt_int(b + n, v);
  obj_text(x, y, b, T_FONT, P_HUD);
}

void end_screen(void) {
  video_off();
  load_bitmap(ending_bitmap, ending_pal);
  set_fade(16);
  REG_DISPCNT = 4 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D;
  audio_play(&song_title);
  for (int t = 0;; t++) {
    if (t < 32) set_fade(16 - t / 2);
    set_char(T_A, WHO_FRANCO, (t >> 3) & 1 ? CF_CHEER1 : CF_CHEER0);
    set_char(T_B, WHO_TRIPLESET, (t >> 3) & 1 ? CF_CHEER0 : CF_CHEER1);
    oam_begin();
    num_text(70, 58, "PLACE  ", gs.place);
    num_text(70, 70, "ORBS   ", gs.orbs);
    num_text(70, 82, "SCORE  ", gs.score);
    if (t > 120 && ((t >> 5) & 1)) obj_text(76, 100, "PRESS START", T_FONT, P_HUD);
    put_char(T_A, WHO_FRANCO, 14, 64, 0);
    put_char(T_B, WHO_TRIPLESET, 194, 64, 1);
    oam_hide_rest();
    video_wait();
    if (t > 120 && (keys_hit & (KEY_START | KEY_A))) break;
  }
  audio_fade(40);
  fade_to(16, 40);
}

void gameover_screen(void) {
  video_off();
  load_bitmap(ending_bitmap, ending_pal);
  REG_DISPCNT = 4 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D;
  REG_BLDCNT = 0x00C4; REG_BLDY = 12;
  for (int t = 0; t < 240; t++) {
    set_char(T_A, gs.who, CF_HURT);
    oam_begin();
    obj_text(84, 60, "GAME OVER", T_FONT, P_HUD);
    obj_text(56, 80, "THE ROAD WAS TOO HOT", T_FONT, P_HUD);
    put_char(T_A, gs.who, 104, 96, 0);
    oam_hide_rest();
    video_wait();
    if (t > 60 && (keys_hit & (KEY_START | KEY_A))) break;
  }
  fade_to(-16, 30);
  REG_BLDCNT = 0;
}
