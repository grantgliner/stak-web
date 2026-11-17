/* Run stage: side-scrolling dash to the car. You lead, your partner follows a
 * few frames behind (replaying your path). Hover cars come at you from the
 * right: jump them, bounce off their roofs, or slide under the tall ones. */
#include "game.h"
#include "video.h"
#include "audio.h"
#include "gen/run_gen.h"
#include "gen/level_gen.h"
#include "gen/music_gen.h"

#define SBB_ROAD 24
#define SBB_MID  26
#define SBB_HUD  28
#define SBB_FAR  29
#define FONT_BG  448

#define T_PLAYER 0
#define T_PARTNER 24
#define T_CARS 48
#define T_HERO 304
#define T_ORB 368
#define T_FX 384
#define T_WARN 392
#define T_BANNER 400
#define T_FONT 416

#define P_CAR 2
#define P_ORB 6
#define P_HERO 7
#define P_HUD 8

/* physics, Q8 */
#define ACC 22
#define AIR_ACC 14
#define FRIC 12
#define BRAKE 56
#define MAXV 940
#define JUMP_V (-1400)
#define JUMP_CUT (-640)
#define GRAV 66
#define MAXFALL 1800
#define SLIDE_FRAMES 34

enum { CAR_BOLT, CAR_ORCA, CAR_WASP, CAR_MANTA };
enum { AN_RUN, AN_IDLE, AN_JUMP, AN_FALL, AN_HURT, AN_SLIDE, AN_CHEER };

typedef struct { s32 x, y; s32 vx, vy; int on_ground, slide, inv, anim_t, frame, hurt, jump_held, land; } Player;
typedef struct { int active, type, warn; s32 x, y, vx; int y0, phase, bonked; s32 vy; } Car;
typedef struct { s32 x, y, vx, vy; int t, active; } Loose;
typedef struct { s16 x, y; u8 frame; } Hist;

static Player pl;
static Car cars[6];
static Loose loose[8];
static u8 orb_taken[NUM_ORBS];
static Hist hist[64];
static int hist_pos;
static s32 cam_x;
static int next_car, last_col, partner_who;
static int fx_t[4]; static s32 fx_x[4], fx_y[4]; static int fx_kind[4];
static int checkpoint;
static int ko_timer;

static const u32* char_src(int who) { return who == WHO_FRANCO ? char_franco : char_tripleset; }

static int ground_at(s32 xpx, s32 feet_prev, s32 feet_now) {
  /* landing height for one-way hover pads; returns GROUND_Y if none */
  for (int i = 0; i < NUM_PADS; i++) {
    int px = lvl_pad[i * 3], py = lvl_pad[i * 3 + 1], w = lvl_pad[i * 3 + 2] * 8;
    if (px > xpx + 8) break;
    if (xpx >= px - 4 && xpx < px + w + 4 && feet_prev <= py && feet_now >= py) return py;
  }
  return GROUND_Y;
}

static int on_pad(s32 xpx, s32 feet) {
  for (int i = 0; i < NUM_PADS; i++) {
    int px = lvl_pad[i * 3], py = lvl_pad[i * 3 + 1], w = lvl_pad[i * 3 + 2] * 8;
    if (px > xpx + 8) break;
    if (xpx >= px - 4 && xpx < px + w + 4 && feet == py) return 1;
  }
  return 0;
}

/* ---- BG streaming: write map column c (world tile column) */
static void write_column(int c) {
  vu16* m = SCREENBLOCK(SBB_ROAD + ((c & 63) >> 5));
  int x = c & 31;
  for (int r = 0; r < 15; r++) m[r * 32 + x] = 0;
  for (int r = 0; r < 5; r++) m[(15 + r) * 32 + x] = road_pattern[r * 8 + (c & 7)] | (2 << 12);
  int wx = c * 8;
  for (int i = 0; i < NUM_PADS; i++) {
    int px = lvl_pad[i * 3], py = lvl_pad[i * 3 + 1], w = lvl_pad[i * 3 + 2];
    if (px > wx) break;
    int k = (wx - px) / 8;
    if (k < w) {
      int part = k == 0 ? 0 : k == w - 1 ? 3 : 1 + (k & 1);
      int r = py / 8;
      m[r * 32 + x] = pad_ids[part] | (2 << 12);
      m[(r + 1) * 32 + x] = pad_ids[4 + part] | (2 << 12);
    }
  }
}

static void stream_bg(void) {
  int want = (cam_x >> 3) + 31;
  while (last_col < want) write_column(++last_col);
}

static void setup_video(void) {
  REG_DISPCNT = 0x0080;
  dma3_copy32(PAL_BG, run_bg_pal, 48 / 2);
  dma3_copy32(PAL_BG + 15 * 16, hud_pal, 8);
  dma3_copy32(PAL_OBJ, run_obj_pal, sizeof(run_obj_pal) / 4);
  dma3_copy32(CHARBLOCK(0), far_tiles, sizeof(far_tiles) / 4);
  dma3_copy32(CHARBLOCK(0) + FONT_BG * 16, font_tiles, sizeof(font_tiles) / 4);
  dma3_copy32(CHARBLOCK(1), road_tiles, sizeof(road_tiles) / 4);
  dma3_copy32(CHARBLOCK(2), mid_tiles, sizeof(mid_tiles) / 4);
  dma3_fill32(SCREENBLOCK(SBB_FAR), 0, 512);
  for (int y = 0; y < 20; y++) for (int x = 0; x < 32; x++) SCREENBLOCK(SBB_FAR)[y * 32 + x] = far_map[y * 32 + x];
  for (int i = 0; i < 2048; i++) SCREENBLOCK(SBB_MID)[i] = 0;
  for (int b = 0; b < 2; b++)
    for (int i = 0; i < 640; i++) SCREENBLOCK(SBB_MID + b)[i] = mid_map[b * 640 + i] | (1 << 12);
  for (int i = 0; i < 2048; i++) SCREENBLOCK(SBB_ROAD)[i] = 0;
  dma3_copy32(OBJ_TILES + T_CARS * 16, car_tiles, sizeof(car_tiles) / 4);
  dma3_copy32(OBJ_TILES + T_HERO * 16, hero_side_tiles, sizeof(hero_side_tiles) / 4);
  dma3_copy32(OBJ_TILES + T_ORB * 16, orb_tiles, sizeof(orb_tiles) / 4);
  dma3_copy32(OBJ_TILES + T_FX * 16, fx_tiles, sizeof(fx_tiles) / 4);
  dma3_copy32(OBJ_TILES + T_WARN * 16, warn_tiles, sizeof(warn_tiles) / 4);
  dma3_copy32(OBJ_TILES + T_BANNER * 16, banner_tiles, sizeof(banner_tiles) / 4);
  dma3_copy32(OBJ_TILES + T_FONT * 16, font_tiles, sizeof(font_tiles) / 4);
  REG_BGCNT[0] = BG_PRIO(0) | BG_CBB(0) | BG_SBB(SBB_HUD);
  REG_BGCNT[1] = BG_PRIO(1) | BG_CBB(1) | BG_SBB(SBB_ROAD) | BG_SIZE_64x32;
  REG_BGCNT[2] = BG_PRIO(2) | BG_CBB(2) | BG_SBB(SBB_MID) | BG_SIZE_64x32;
  REG_BGCNT[3] = BG_PRIO(3) | BG_CBB(0) | BG_SBB(SBB_FAR);
  txt_setup(0, 0, SBB_HUD, FONT_BG, 15);
  video_hblank_dma(run_sky, &PAL_BG[0], -1);
}

static void reset_at(int cp) {
  checkpoint = cp;
  pl.x = (lvl_check[cp] + 40) << 8; pl.y = GROUND_Y << 8;
  pl.vx = 0; pl.vy = 0; pl.on_ground = 1; pl.slide = 0; pl.inv = 90; pl.hurt = 0; pl.jump_held = 0; pl.land = 0;
  for (int i = 0; i < 6; i++) cars[i].active = 0;
  for (int i = 0; i < 8; i++) loose[i].active = 0;
  next_car = 0;
  while (next_car < NUM_CARS && lvl_car[next_car * 4] < (pl.x >> 8) + 120) next_car++;
  for (int i = 0; i < 64; i++) { hist[i].x = (pl.x >> 8) - 18; hist[i].y = GROUND_Y; hist[i].frame = CF_IDLE0; }
  cam_x = (pl.x >> 8) - 72; if (cam_x < 0) cam_x = 0;
  last_col = (cam_x >> 3) - 1;
  for (int i = 0; i < 32; i++) write_column(++last_col);
  last_col = (cam_x >> 3) + 31;
  ko_timer = 0;
}

static void spawn_fx(int kind, s32 x, s32 y) {
  for (int i = 0; i < 4; i++) if (!fx_t[i]) { fx_t[i] = 1; fx_kind[i] = kind; fx_x[i] = x; fx_y[i] = y; return; }
}

static void hud(void) {
  txt_put(1, 1, "ORBS");
  txt_num(6, 1, gs.orbs, 3);
  txt_put(22, 1, "LIVES");
  txt_num(28, 1, gs.lives, 1);
  /* progress: a strip of 12 dashes and a marker */
  int p = (pl.x >> 8) * 12 / LEVEL_LEN; if (p > 11) p = 11; if (p < 0) p = 0;
  for (int i = 0; i < 12; i++) txt_putc(10 + i, 1, i == p ? '>' : i < p ? '=' : '-', 15);
}

static void hit_player(void) {
  if (pl.inv || pl.hurt) return;
  if (gs.orbs > 0) {
    int n = gs.orbs > 8 ? 8 : gs.orbs;
    for (int i = 0; i < n; i++) {
      loose[i].active = 1; loose[i].t = 0;
      loose[i].x = pl.x; loose[i].y = pl.y - (20 << 8);
      loose[i].vx = ((i & 1) ? 1 : -1) * (180 + i * 70);
      loose[i].vy = -(500 + (i & 3) * 120);
    }
    gs.orbs = 0;
    pl.hurt = 30; pl.inv = 120;
    pl.vx = -400; pl.vy = -700; pl.on_ground = 0; pl.slide = 0;
    sfx(SFX_HURT);
  } else {
    /* knocked out */
    pl.hurt = 999; ko_timer = 1;
    pl.vx = -300; pl.vy = -1300; pl.on_ground = 0;
    sfx(SFX_HURT);
  }
}

static void update_player(void) {
  s32 feet_prev = pl.y >> 8;
  int ctl = !pl.hurt;
  int right = ctl && (keys & KEY_RIGHT), left = ctl && (keys & KEY_LEFT);
  if (pl.hurt && pl.hurt < 999) pl.hurt--;
  if (pl.inv) pl.inv--;
  if (pl.on_ground) {
    if (right) { pl.vx += ACC; }
    else if (left) { pl.vx -= pl.vx > 0 ? BRAKE : ACC; }
    else { if (pl.vx > 0) { pl.vx -= FRIC; if (pl.vx < 0) pl.vx = 0; } else if (pl.vx < 0) { pl.vx += FRIC; if (pl.vx > 0) pl.vx = 0; } }
    if (ctl && (keys_hit & KEY_A)) {
      pl.vy = JUMP_V - (pl.vx > 600 ? 80 : 0); pl.on_ground = 0; pl.slide = 0; pl.jump_held = 1;
      sfx(SFX_JUMP);
    } else if (ctl && (keys_hit & (KEY_B | KEY_DOWN)) && !pl.slide) {
      pl.slide = SLIDE_FRAMES;
      if (pl.vx < 520) pl.vx = 520;
      spawn_fx(0, pl.x >> 8, GROUND_Y);
    }
  } else {
    if (right) pl.vx += AIR_ACC; else if (left) pl.vx -= AIR_ACC;
    if (pl.jump_held && !(keys & KEY_A)) { pl.jump_held = 0; if (pl.vy < JUMP_CUT) pl.vy = JUMP_CUT; }
    pl.vy += GRAV; if (pl.vy > MAXFALL) pl.vy = MAXFALL;
  }
  if (pl.slide) { pl.slide--; pl.vx -= 6; }
  if (pl.vx > MAXV) pl.vx = MAXV;
  if (pl.vx < -MAXV / 2) pl.vx = -MAXV / 2;
  pl.x += pl.vx;
  if (pl.x < (cam_x + 8) << 8) { pl.x = (cam_x + 8) << 8; if (pl.vx < 0) pl.vx = 0; }
  pl.y += pl.vy;
  if (ko_timer) return;
  s32 feet = pl.y >> 8;
  if (!pl.on_ground && pl.vy >= 0) {
    int g = ground_at(pl.x >> 8, feet_prev, feet);
    if (feet >= g) { pl.y = g << 8; pl.vy = 0; pl.on_ground = 1; pl.land = 5; sfx(SFX_LAND); spawn_fx(0, pl.x >> 8, g); }
  } else if (pl.on_ground) {
    int f = pl.y >> 8;
    if (f < GROUND_Y && !on_pad(pl.x >> 8, f)) { pl.on_ground = 0; pl.vy = 0; }
  }
  /* animation */
  int sp = pl.vx < 0 ? -pl.vx : pl.vx;
  if (pl.land) pl.land--;
  if (pl.hurt) pl.frame = CF_HURT;
  else if (!pl.on_ground) pl.frame = pl.vy < -350 ? CF_JUMP : pl.vy < 350 ? CF_APEX : CF_FALL;
  else if (pl.slide) pl.frame = CF_SLIDE;
  else if (pl.land) { pl.frame = CF_LAND; pl.anim_t = 2 << 9; }   /* squash, then pick up the stride at push-off */
  else if (sp < 40) { pl.frame = (frame_count >> 5) & 1 ? CF_IDLE1 : CF_IDLE0; pl.anim_t = 0; }
  else {
    /* 8-frame cycle; cadence rises with speed to about 3.3 strides a second at top speed */
    pl.anim_t += 40 + sp / 5;
    pl.frame = CF_RUN0 + ((pl.anim_t >> 9) & 7);
  }
}

static void player_box(int* x0, int* y0, int* x1, int* y1) {
  int x = pl.x >> 8, y = pl.y >> 8;
  *x0 = x - 6; *x1 = x + 6; *y1 = y;
  *y0 = pl.slide ? y - 14 : pl.on_ground ? y - 36 : y - 30;
}

static void update_cars(void) {
  int px = pl.x >> 8;
  /* spawn: when the camera reaches a car's trigger, warn, then send it in */
  while (next_car < NUM_CARS && lvl_car[next_car * 4] <= cam_x + 120) {
    for (int i = 0; i < 6; i++) if (!cars[i].active) {
      Car* c = &cars[i];
      c->active = 1; c->type = lvl_car[next_car * 4 + 1]; c->vx = -lvl_car[next_car * 4 + 2];
      c->warn = 50; c->bonked = 0; c->phase = lvl_car[next_car * 4 + 3] * 64;
      c->y0 = c->type == CAR_ORCA ? GROUND_Y - 18 - 28 : GROUND_Y + 2 - 28;
      if (c->type == CAR_WASP) c->y0 = GROUND_Y - 6 - 28;
      c->y = c->y0 << 8;
      break;
    }
    next_car++;
  }
  int bx0, by0, bx1, by1;
  player_box(&bx0, &by0, &bx1, &by1);
  for (int i = 0; i < 6; i++) {
    Car* c = &cars[i];
    if (!c->active) continue;
    if (c->warn) { c->warn--; c->x = (cam_x + 248) << 8; if (!c->warn) sfx(SFX_WHOOSH); continue; }
    c->x += c->vx;
    if (c->bonked) {
      c->vy += 40; c->y += c->vy;
      if ((c->y >> 8) > 200) c->active = 0;
      continue;
    }
    if (c->type == CAR_WASP) { c->phase += 5; c->y = (c->y0 << 8) + lu_sin(c->phase) * 18 * 256 / 4096; }
    if ((c->x >> 8) < cam_x - 80) { c->active = 0; continue; }
    if (ko_timer) continue;
    const u8* h = &car_hit[c->type * 4];
    int cx0 = (c->x >> 8) + h[0] + 2, cx1 = (c->x >> 8) + h[2] - 2;
    int cy0 = (c->y >> 8) + h[1] + 1, cy1 = (c->y >> 8) + h[3] - 1;
    if (bx1 > cx0 && bx0 < cx1 && by1 > cy0 && by0 < cy1) {
      if (pl.vy > 0 && (pl.y >> 8) - (pl.vy >> 8) - 2 <= cy0 + 6 && !pl.on_ground) {
        /* roof bounce */
        pl.vy = -1150; pl.jump_held = (keys & KEY_A) ? 1 : 0;
        c->bonked = 1; c->vy = -300;
        gs.score += 100; gs.orbs += 1;
        sfx(SFX_BUMP);
        spawn_fx(1, (c->x >> 8) + 32, cy0);
      } else hit_player();
    }
    (void)px;
  }
}

static void update_orbs(void) {
  int bx0, by0, bx1, by1;
  player_box(&bx0, &by0, &bx1, &by1);
  int lo = cam_x - 16, hi = cam_x + 256;
  for (int i = 0; i < NUM_ORBS; i++) {
    int ox = lvl_orb[i * 2]; if (ox < lo) continue; if (ox > hi) break;
    if (orb_taken[i]) continue;
    int oy = lvl_orb[i * 2 + 1];
    if (bx1 > ox - 6 && bx0 < ox + 6 && by1 > oy - 6 && by0 < oy + 6 && !pl.hurt) {
      orb_taken[i] = 1; gs.orbs++; gs.score += 10;
      sfx(SFX_ORB);
      spawn_fx(1, ox, oy);
    }
  }
  for (int i = 0; i < 8; i++) {
    Loose* l = &loose[i];
    if (!l->active) continue;
    l->t++;
    l->vy += 40; l->x += l->vx; l->y += l->vy;
    if ((l->y >> 8) > GROUND_Y - 6) { l->y = (GROUND_Y - 6) << 8; l->vy = -l->vy * 3 / 4; }
    if (l->t > 200) { l->active = 0; continue; }
    int ox = l->x >> 8, oy = l->y >> 8;
    if (l->t > 40 && !pl.hurt && bx1 > ox - 6 && bx0 < ox + 6 && by1 > oy - 6 && by0 < oy + 6) {
      l->active = 0; gs.orbs++; sfx(SFX_ORB);
    }
  }
}

/* ---- drawing */
static void draw_char(int tile, int pal, int x, int y, int hflip, int blink) {
  if (blink) return;
  int sx = x - 16 - cam_x, sy = y - 47;
  if (sx < -32 || sx > 240) return;
  ObjAttr* o = oam_add(); if (!o) return;
  o->attr0 = ATTR0_Y(sy) | ATTR0_SQUARE;
  o->attr1 = ATTR1_X(sx) | ATTR1_SIZE_32 | (hflip ? ATTR1_HFLIP : 0);
  o->attr2 = ATTR2_TILE(tile) | ATTR2_PAL(pal) | ATTR2_PRIO(1);
  o = oam_add(); if (!o) return;
  o->attr0 = ATTR0_Y(sy + 32) | ATTR0_WIDE;
  o->attr1 = ATTR1_X(sx) | ATTR1_SIZE_32 | (hflip ? ATTR1_HFLIP : 0);
  o->attr2 = ATTR2_TILE(tile + 16) | ATTR2_PAL(pal) | ATTR2_PRIO(1);
}

static void draw_world(int partner_frame, s32 partner_x, s32 partner_y, int show_player, int hero_x, int hero_frame, int seat_mode) {
  oam_begin();
  /* hero car first so it covers the seated characters' legs */
  int hsx = hero_x - cam_x;
  if (hsx > -64 && hsx < 240) {
    ObjAttr* o = oam_add();
    o->attr0 = ATTR0_Y(GROUND_Y - 28) | ATTR0_WIDE;
    o->attr1 = ATTR1_X(hsx) | ATTR1_SIZE_64;
    o->attr2 = ATTR2_TILE(T_HERO + hero_frame * 32) | ATTR2_PAL(P_HERO) | ATTR2_PRIO(1);
    int bx = LEVEL_LEN - 220 - cam_x;
    if (!seat_mode && bx > -64 && bx < 240) {
      o = oam_add();
      o->attr0 = ATTR0_Y(64) | ATTR0_WIDE;
      o->attr1 = ATTR1_X(bx) | ATTR1_SIZE_64;
      o->attr2 = ATTR2_TILE(T_BANNER) | ATTR2_PAL(P_HERO) | ATTR2_PRIO(2);
    }
  }
  if (show_player) draw_char(T_PLAYER, gs.who, pl.x >> 8, pl.y >> 8, 0, pl.inv && (frame_count & 2));
  draw_char(T_PARTNER, partner_who, partner_x, partner_y, 0, 0);
  (void)partner_frame;
  /* effects */
  for (int i = 0; i < 4; i++) if (fx_t[i]) {
    int sx = fx_x[i] - cam_x - 4, sy = fx_y[i] - 8;
    int t = fx_t[i]++;
    int tile = fx_kind[i] == 0 ? T_FX + (t / 5 > 2 ? 2 : t / 5) : T_FX + 3 + ((t >> 2) & 1);
    if (fx_kind[i] == 0) sy = fx_y[i] - 6 - t / 6;
    if (t > 15) fx_t[i] = 0;
    ObjAttr* o = oam_add(); if (!o) break;
    o->attr0 = ATTR0_Y(sy); o->attr1 = ATTR1_X(sx); o->attr2 = ATTR2_TILE(tile) | ATTR2_PAL(P_ORB) | ATTR2_PRIO(1);
  }
  /* cars */
  for (int i = 0; i < 6; i++) {
    Car* c = &cars[i];
    if (!c->active) continue;
    ObjAttr* o = oam_add(); if (!o) break;
    if (c->warn) {
      if ((c->warn >> 2) & 1) { o->attr0 = ATTR0_HIDE; continue; }
      const u8* h = &car_hit[c->type * 4];
      o->attr0 = ATTR0_Y((c->y >> 8) + (h[1] + h[3]) / 2 - 8);
      o->attr1 = ATTR1_X(240 - 18) | ATTR1_SIZE_16;
      o->attr2 = ATTR2_TILE(T_WARN) | ATTR2_PAL(P_ORB) | ATTR2_PRIO(0);
      continue;
    }
    int sx = (c->x >> 8) - cam_x, sy = c->y >> 8;
    if (sx < -64 || sx >= 240 || sy > 160) { o->attr0 = ATTR0_HIDE; continue; }
    o->attr0 = ATTR0_Y(sy) | ATTR0_WIDE | (c->bonked ? ATTR0_AFF : 0);
    o->attr1 = ATTR1_X(sx) | ATTR1_SIZE_64 | (c->bonked ? ATTR1_AFFIDX(0) : 0);
    o->attr2 = ATTR2_TILE(T_CARS + (c->type * 2 + ((frame_count >> 2) & 1)) * 32) | ATTR2_PAL(P_CAR + c->type) | ATTR2_PRIO(1);
  }
  obj_aff(0, 256, 0, 0, -256);   /* bonked cars flip upside down */
  /* orbs */
  int f = (frame_count >> 3) & 3;
  int lo = cam_x - 16, hi = cam_x + 248;
  for (int i = 0; i < NUM_ORBS; i++) {
    int ox = lvl_orb[i * 2]; if (ox < lo) continue; if (ox > hi) break;
    if (orb_taken[i]) continue;
    ObjAttr* o = oam_add(); if (!o) break;
    o->attr0 = ATTR0_Y(lvl_orb[i * 2 + 1] - 8);
    o->attr1 = ATTR1_X(ox - cam_x - 8) | ATTR1_SIZE_16;
    o->attr2 = ATTR2_TILE(T_ORB + f * 4) | ATTR2_PAL(P_ORB) | ATTR2_PRIO(1);
  }
  for (int i = 0; i < 8; i++) if (loose[i].active && !(loose[i].t > 150 && (frame_count & 2))) {
    ObjAttr* o = oam_add(); if (!o) break;
    o->attr0 = ATTR0_Y((loose[i].y >> 8) - 8);
    o->attr1 = ATTR1_X((loose[i].x >> 8) - cam_x - 8) | ATTR1_SIZE_16;
    o->attr2 = ATTR2_TILE(T_ORB + f * 4) | ATTR2_PAL(P_ORB) | ATTR2_PRIO(1);
  }
  oam_hide_rest();
}

static void scroll(void) {
  REG_BGHOFS(1) = cam_x;
  REG_BGHOFS(2) = cam_x / 3;
  REG_BGHOFS(3) = cam_x / 10;
}

static void upload_chars(int pf, int partner_frame) {
  dma3_copy32(OBJ_TILES + T_PLAYER * 16, char_src(gs.who) + pf * 192, 192);
  dma3_copy32(OBJ_TILES + T_PARTNER * 16, char_src(partner_who) + partner_frame * 192, 192);
}

/* ---- the ending: both hop into the car and it takes off */
static void finale(void) {
  int car_x = LEVEL_LEN - 120;
  s32 hx = car_x;
  int pf = CF_IDLE0, qf = CF_IDLE0;
  s32 ax = pl.x, ay = pl.y, bx = hist[(hist_pos - 16) & 63].x << 8, by = hist[(hist_pos - 16) & 63].y << 8;
  s32 seat_a = (car_x + 34) << 8, seat_b = (car_x + 20) << 8, seat_y = (GROUND_Y - 13) << 8;
  audio_fade(90);
  for (int t = 0; t < 280; t++) {
    int seated = 0;
    if (t < 50) {
      /* hop in: two arcs, partner slightly later */
      int ta = t, tb = t - 12;
      if (ta >= 0 && ta <= 32) { s32 k = ta * 256 / 32; pl.x = ax + (seat_a - ax) * k / 256; pl.y = ay + (seat_y - ay) * k / 256 - (lu_sin(ta * 4) * 40 * 256 / 4096); pf = CF_JUMP; }
      if (ta > 32) { pl.x = seat_a; pl.y = seat_y; pf = CF_CHEER0; }
      if (tb < 0) { qf = CF_RUN0 + ((t * 3 >> 3) & 7); bx += 600; if (bx > seat_b - (40 << 8)) bx = seat_b - (40 << 8); }
      else if (tb <= 32) { s32 k = tb * 256 / 32; s32 sx0 = seat_b - (40 << 8); bx = sx0 + (seat_b - sx0) * k / 256; by = (GROUND_Y << 8) + (seat_y - (GROUND_Y << 8)) * k / 256 - (lu_sin(tb * 4) * 40 * 256 / 4096); qf = CF_JUMP; }
      else { bx = seat_b; by = seat_y; qf = CF_CHEER0; }
      if (t == 20 || t == 32) sfx(SFX_DOOR);
    } else {
      seated = 1;
      pf = qf = (t >> 3) & 1 ? CF_CHEER1 : CF_CHEER0;
      if (t < 50 + 3) { pf = qf = CF_CHEER0; }
      pf = qf = CF_IDLE0;
      if (t == 90) sfx(SFX_BOOST);
      s32 dx = 0;
      if (t > 100) { int k = t - 100; dx = k * k * 6; }
      hx = car_x + (dx >> 8);
      pl.x = seat_a + dx; pl.y = seat_y + ((t < 100 && (t & 2)) ? 256 : 0);
      bx = seat_b + dx; by = pl.y;
      if (t == 200) fade_to(16, 30);
    }
    upload_chars(pf, qf);
    (void)seated;
    draw_world(qf, bx >> 8, by >> 8, 1, hx, (t >> 2) & 1, t >= 50);
    scroll();
    video_wait();
    stream_bg();
  }
}

int run_stage(void) {
  video_off();
  setup_video();
  for (int i = 0; i < NUM_ORBS; i++) orb_taken[i] = 0;
  partner_who = gs.who ^ 1;
  gs.lives = 3; gs.orbs = 0; gs.run_time = 0;
  reset_at(0);
  hist_pos = 0;
  audio_play(&song_run);
  set_fade(-16);
  REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_BG2 | DCNT_BG3 | DCNT_OBJ | DCNT_OBJ_1D;
  /* READY */
  for (int t = 0; t < 70; t++) {
    upload_chars(CF_IDLE0, CF_IDLE0);
    draw_world(CF_IDLE0, (pl.x >> 8) - 18, GROUND_Y, 1, LEVEL_LEN - 120, 0, 0);
    if (t > 20) obj_text(100, 56, t < 55 ? "READY" : "GO!", T_FONT, P_HUD);
    oam_hide_rest();
    hud(); scroll();
    video_wait();
    if (t < 16) set_fade(-16 + t);
    else set_fade(0);
  }
  for (;;) {
    gs.run_time++;
    if (keys_hit & KEY_SELECT) { int t = gs.who; gs.who = partner_who; partner_who = t; sfx(SFX_SELECT); }
    if (keys_hit & KEY_START) {
      /* pause */
      sfx(SFX_SELECT);
      audio_set_master(6);
      for (;;) {
        obj_text(96, 64, "PAUSED", T_FONT, P_HUD);
        oam_hide_rest();
        video_wait();
        if (keys_hit & KEY_START) break;
      }
      audio_set_master(16);
      continue;
    }
    update_player();
    update_cars();
    update_orbs();
    /* partner replays your path 16 frames late */
    hist[hist_pos & 63].x = (pl.x >> 8) - 12; hist[hist_pos & 63].y = pl.y >> 8; hist[hist_pos & 63].frame = pl.frame;
    if (pl.hurt) { hist[hist_pos & 63].frame = CF_RUN0 + ((frame_count * 3 >> 3) & 7); hist[hist_pos & 63].y = GROUND_Y; }
    hist_pos++;
    Hist* h = &hist[(hist_pos - 10) & 63];
    /* camera */
    s32 target = (pl.x >> 8) - 72;
    if (target > cam_x) cam_x += (target - cam_x + 3) >> 2 > 8 ? 8 : (target - cam_x + 3) >> 2;
    if (cam_x > LEVEL_LEN - 240) cam_x = LEVEL_LEN - 240;
    if (cam_x < 0) cam_x = 0;
    stream_bg();
    upload_chars(pl.frame, h->frame);
    draw_world(h->frame, h->x, h->y, 1, LEVEL_LEN - 120, (frame_count >> 2) & 1, 0);
    hud();
    scroll();
    video_wait();
    if (ko_timer) {
      if (++ko_timer == 60) {
        fade_to(-16, 20);
        if (--gs.lives <= 0) { audio_stop(); return 1; }
        int cp = 0;
        for (int i = 0; i < NUM_CHECKPOINTS; i++) if (lvl_check[i] <= (pl.x >> 8)) cp = i;
        if ((pl.x >> 8) < 0) cp = 0;
        reset_at(cp);
        fade_to(0, 16);
      }
    }
    if ((pl.x >> 8) >= LEVEL_LEN - 170 && !ko_timer) break;
  }
  finale();
  return 0;
}
