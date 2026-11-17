/* STAK! GBA - title screen with the mode picker, relaxed / classic / expert play, game over. */
#include "gba.h"
#include "physics.h"
#include "video.h"
#include "sound.h"
#include "save.h"
#include "gen/pieces_gen.h"
#include "gen/music_gen.h"
#include "gen/gfx_gen.h"

/* ------------------------------------------------------------------ constants */
#define PX_PER_CELL 12
#define SCREEN_CX   148              /* screen x of the base centre */
#define BASE_SY     136              /* screen y of the base top when the camera is at 0 */
#define VIEW_CELLS  FX(136.0 / 12)   /* cells visible above the camera line */
#define SIDEBAR_W   56
#define BASE_HW     FX(0.205 / 0.025 / 2)   /* 4.1 cells */
#define BASE_HH     FX(1.0)
#define HOLD_GAP    FX(0.75)
#define FALL_Y      FX(-4.0)
#define FALL_X      FX(18.0)         /* 0.45 m, same as the side fail zones */
#define ROT_STEP    (FX_PI / 4)      /* 45 degrees */
#define ROT_FRAMES  6

enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_OVER };
enum { MTX_TRAY = 0, MTX_TRAY_HI = 1, MTX_HELD = 2, MTX_DYN = 3 };

/* piece pools, weighted like the web version's: piece indices are
   0 Plus, 1 Rectangle, 2 L, 3 L-Reverse, 4 Arrow, 5 PlusCut, 6 PeterPan, 7 S,
   8 Z, 9 T, 10 Zig, 11 Zag, 12 Circle, 13 Semicircle, 14 Lollipop, 15 Seesaw */
static const u8 pool_relaxed[] = { 0, 2, 3, 1, 0, 2, 3, 1, 7, 8, 7, 8, 9, 10, 11 };
static const u8 pool_classic[] = { 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const u8 pool_expert[] = { 0, 2, 3, 1, 6, 5, 4, 7, 8, 12, 13, 14, 15, 0, 2, 3, 1, 6, 5, 4, 7, 8, 12, 13, 14, 15, 9, 10, 11 };
enum { MODE_RELAXED, MODE_CLASSIC, MODE_EXPERT };
typedef struct { const char* name; const char* desc; const u8* pool; u8 npool; const Song* song; } ModeDef;
static const ModeDef modes[3] = {
  { "RELAXED", "STACK SIMPLE SHAPES!", pool_relaxed, sizeof pool_relaxed, &song_relaxed },
  { "CLASSIC", "STACK TRICKIER ANGLES!", pool_classic, sizeof pool_classic, &song_classic },
  { "EXPERT", "STACK CURVES AND ANGLES!", pool_expert, sizeof pool_expert, &song_expert },
};

static const char* const messages[50] = {
  "Seriously?", "That's it?", "I mean... it's something.", "It's a magic number.",
  "Unfortunately, this isn't golf.", "Failure is part of the learning process.",
  "Take your time. These blocks aren't going anywhere.", "It's a lucky number.",
  "Stuck between a block and a hard place.", "Almost at double digits!", "Take a moment to reflect.",
  "Imagine greater heights.", "Keep at it!", "Unlucky.", "You gonna pick those up?",
  "Best $3,500 you've ever spent.", "Sweet.", "It's not nothing.", "Not bad!", "Prime stacking.",
  "It's all about rotation, rotation, rotation.", "Blackjack.", "Tell your friends.", "Beautiful work.",
  "Sometimes it's you. Sometimes it's gravity. Sometimes it's the physics engine.", "Funnier than 24.",
  "We truly live in an age of wonders.", "You've stacked the secret number!", "It's not you, it's gravity.",
  "Don't cry because it's over. Smile because it happened.", "Thriving.", "Having fun? Tell a friend!",
  "Make sure to take a break once in a while.", "Be the block.", "Stacking ain't no walk in the park.",
  "You've attained stacking proficiency.", "Hey! Nice stack!", "Visionary stacking.",
  "Take a moment to admire your work.", "Quite the accomplishment.", "You have become one with the blocks.",
  "Sound stacking. Well done.", "Stacking gives life meaning.",
  "The real expert mode is stacking everything in your living room.", "Pretty, pretty, pretty good!",
  "Stakmaster. Your time starts now.", "Way to finish strong.", "You had no other choice.",
  "Thank you for playing.", "I'm proud of you.",
};

/* ------------------------------------------------------------------ state */
static int state, mode = MODE_CLASSIC;
static u32 frame;
static u16 keys, keys_prev, keys_down;
static u32 rng = 0x1234567;

typedef struct { u8 shape, color; } Slot;
static Slot tray[3];
static int cursor, holding;
static fx hx, hy, hangle, hangle_from, hangle_to;
static int hrot_t, hcolliding, move_frames, hold_spawn_t;
static int blocks, score, new_best;
static fx cam_y, cam_target;
static int over_t, over_shown, pause_sel;
static int pop_t, pop_val; static fx pop_x, pop_y;
static int last_placed = -1, last_color = -1;

/* title decoration */
typedef struct { fx x, y, a, vy, w; u8 seed, color; } Deco;   /* shape and colour follow the mode */
static Deco deco[8] EWRAM_BSS;
static int logo_t;

/* ------------------------------------------------------------------ helpers */
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int rnd_n(int n) { return (int)((rnd() >> 8) % (u32)n); }

static void read_keys(void) {
  keys_prev = keys;
  keys = ~REG_KEYINPUT & 0x3FF;
  keys_down = keys & ~keys_prev;
}

static inline int sx_of(fx x) { return SCREEN_CX + (int)(((s64)x * PX_PER_CELL) >> 16); }
static inline int sy_of(fx y) { return BASE_SY - (int)(((s64)(y - cam_y) * PX_PER_CELL) >> 16); }

/* a block sprite with its COM at (sx, sy), turned by (c, s): the sprite is
   centred on the COM, or on the piece's box with the offset turned too (the
   lollipop). mtx: an affine matrix, or PLAIN_UP / PLAIN_DOWN for a plain sprite
   (cheaper per scanline) upright or flipped to 180 degrees. c = s = 0 centres
   it on its box (the tray). */
#define PLAIN_UP   (-1)
#define PLAIN_DOWN (-2)
static void put_piece(ObjAttr* o, int shape, int sx, int sy, fx c, fx s, int pal, int mtx, int prio, int blend) {
  const PieceDef* d = &piece_defs[shape];
  if (d->sox | d->soy) {
    fx ox = fxmul(c, d->sox) - fxmul(s, d->soy), oy = fxmul(s, d->sox) + fxmul(c, d->soy);
    sx += (ox * PX_PER_CELL + 0x8000) >> 16; sy -= (oy * PX_PER_CELL + 0x8000) >> 16;
  }
  int half = d->small ? 16 : 32, x = sx - half, y = sy - half;
  if (x <= -2 * half || x >= 240 || y <= -2 * half || y >= 160) { o->attr0 = ATTR0_HIDE; return; }
  o->attr0 = ATTR0_Y(y) | (mtx >= 0 ? ATTR0_AFF : 0) | ATTR0_SQUARE | (blend ? ATTR0_BLEND : 0);
  o->attr1 = ATTR1_X(x) | (d->small ? ATTR1_SIZE_32 : ATTR1_SIZE_64) |
             (mtx >= 0 ? ATTR1_AFFIDX(mtx) : mtx == PLAIN_DOWN ? ATTR1_HFLIP | ATTR1_VFLIP : 0);
  o->attr2 = ATTR2_TILE(d->tile) | ATTR2_PRIO(prio) | ATTR2_PAL(pal);
}

/* affine matrix cache for physics bodies, keyed by quantized angle */
static s16 mtx_key[32]; static int mtx_used;
static int mtx_for_angle(fx c, fx s, fx angle) {
  int q = (int)(((s64)angle * 10430) >> 16) >> 7;   /* 512 steps per turn */
  for (int i = 0; i < mtx_used; i++) if (mtx_key[i] == q) return MTX_DYN + i;
  if (MTX_DYN + mtx_used < 32) {
    mtx_key[mtx_used] = q;
    obj_affine_set(MTX_DYN + mtx_used, c, s, 256);
    return MTX_DYN + mtx_used++;
  }
  /* out of matrices: nearest existing */
  int best = 0, bd = 1 << 30;
  for (int i = 0; i < mtx_used; i++) { int d = mtx_key[i] - q; if (d < 0) d = -d; if (d > 256) d = 512 - d; if (d < bd) { bd = d; best = i; } }
  return MTX_DYN + best;
}

static Slot new_slot(int idx) {
  const ModeDef* md = &modes[mode];
  Slot s;
  for (;;) {
    s.shape = md->pool[rnd_n(md->npool)];
    int clash = 0;
    for (int k = 0; k < 3; k++) if (k != idx && tray[k].shape == s.shape) clash = 1;
    if (!clash) break;
  }
  /* a new colour each time, so neighbours stand apart (expert has only three) */
  do s.color = rnd_n(themes[mode].ncolors); while (s.color == last_color);
  last_color = s.color;
  return s;
}

static fx tower_top(void) {
  fx top = 0;
  for (int i = 0; i < phys_count; i++) {
    Body* b = &phys_bodies[i];
    if ((b->flags & (BODY_USED | BODY_STATIC)) != BODY_USED) continue;
    if (b->bmax.y > top) top = b->bmax.y;
  }
  return top;
}

/* ------------------------------------------------------------------ HUD */
static void draw_tray_panels(void) {
  for (int k = 0; k < 3; k++)
    txt_panel(0, 1 + k * 6, 7, 6, (state == ST_PLAY && k == cursor) ? PAL_PANEL_HI : PAL_PANEL);
}
static void draw_hud(void) {
  txt_clear_rect(20, 0, 12, 5);
  txt_put(23, 0, "BLOCKS", PAL_TXT_YELLOW);
  txt_big_num(30, 1, blocks, PAL_TXT_WHITE);
  char b[16] = "BEST "; fmt_int(b + 5, save.best[mode] > blocks ? save.best[mode] : blocks);
  int n = 0; while (b[n]) n++;
  txt_put(30 - n, 3, b, PAL_TXT_GRAY);
}

/* ------------------------------------------------------------------ title */
/* the mode picker: name, what it is, best */
static void draw_mode_picker(void) {
  int on = TXT_ON_PANEL;
  txt_panel(1, 13, 28, 6, PAL_PANEL);
  txt_put(3, 14, "<", PAL_TXT_WHITE | on); txt_put(26, 14, ">", PAL_TXT_WHITE | on);
  char b[24]; int n = 0;
  for (const char* p = modes[mode].name; *p; p++) b[n++] = *p;
  for (const char* p = " MODE"; *p; p++) b[n++] = *p;
  b[n] = 0;
  txt_center(14, 1, 28, b, PAL_TXT_YELLOW | on);
  txt_center(15, 1, 28, modes[mode].desc, PAL_TXT_GRAY | on);
  char c[16] = "BEST "; fmt_int(c + 5, save.best[mode]);
  txt_center(17, 1, 28, c, PAL_TXT_WHITE | on);
  txt_put(22, 19, "HOMEBREW", PAL_TXT_GRAY);
}

static void title_enter(void) {
  state = ST_TITLE;
  phys_init();
  txt_clear();
  mode = save.mode;
  video_set_theme(mode);
  REG_BGHOFS(2) = 0;
  REG_DISPCNT = DCNT_MODE0 | DCNT_OBJ_1D | DCNT_BG0 | DCNT_BG1 | DCNT_BG2 | DCNT_OBJ;
  REG_BLDCNT = 0;
  logo_t = 0;
  for (int i = 0; i < 8; i++) {
    Deco* d = &deco[i];
    d->x = FX(-8) + rnd_n(16 * 256) * 256 + FX(-4) + FX(4);
    d->y = FX(2) + i * FX(2.2);
    d->vy = FX(0.012) + rnd_n(FX(0.02));
    d->a = rnd_n(FX(6.28)); d->w = rnd_n(FX(0.04)) - FX(0.02);
    d->seed = rnd_n(256); d->color = rnd_n(7);
  }
  draw_mode_picker();
  snd_play_song(&song_menu);
}

/* the background layer drifts: clouds blow sideways, champagne bubbles rise,
   stars sink slowly on the title; it scrolls at a quarter of the camera */
static void layer_scroll(fx cam) {
  int y = -(int)(((s64)cam * PX_PER_CELL) >> 18);
  if (mode == MODE_RELAXED) REG_BGHOFS(2) = (u16)(frame / 6);
  if (mode == MODE_EXPERT) y += frame / 3;
  else if (state == ST_TITLE && mode == MODE_CLASSIC) y -= frame / 4;
  REG_BGVOFS(2) = (u16)y;
}

static void title_update(void) {
  logo_t++;
  /* logo drops in with a bounce, then bobs gently */
  int off;
  if (logo_t < 40) { int t = 40 - logo_t; off = t * t * 110 / 1600; }
  else if (logo_t < 52) { int t = logo_t - 46; off = 6 - (t * t) / 6; if (off < 0) off = 0; off = -off; }
  else off = (fx_sin((logo_t - 52) * 2400) * 3) >> 16;
  REG_BGVOFS(1) = (u16)(off - 5);
  REG_BGHOFS(1) = 0;
  /* blinking prompt */
  if (logo_t > 50) {
    if ((logo_t / 32) & 1) txt_clear_rect(0, 12, 30, 1);
    else txt_center(12, 0, 30, "PRESS START", PAL_TXT_WHITE);
  }
  /* falling decorative blocks */
  oam_reset_alloc(0); mtx_used = 0;
  for (int i = 0; i < 8; i++) {
    Deco* d = &deco[i];
    d->y -= d->vy; d->a += d->w;
    if (d->a > FX_PI) d->a -= 2 * FX_PI; if (d->a < -FX_PI) d->a += 2 * FX_PI;
    if (d->y < FX(-3)) { d->y = FX(15); d->x = FX(-10) + rnd_n(FX(16)); d->seed = rnd_n(256); d->color = rnd_n(7); }
    ObjAttr* o = oam_alloc();
    fx c = fx_cos(d->a), s = fx_sin(d->a);
    int m = mtx_for_angle(c, s, d->a);
    int sx = 120 + (int)(((s64)d->x * PX_PER_CELL) >> 16) + (i & 1 ? 40 : -40);
    put_piece(o, modes[mode].pool[d->seed % modes[mode].npool], sx, BASE_SY - (int)(((s64)d->y * PX_PER_CELL) >> 16), c, s,
              d->color % themes[mode].ncolors, m, 2, 0);
  }
  layer_scroll(0);
  if (keys_down & (KEY_LEFT | KEY_RIGHT)) {
    mode = (mode + ((keys_down & KEY_RIGHT) ? 1 : 2)) % 3;
    video_set_theme(mode);
    REG_BGHOFS(2) = 0;
    draw_mode_picker();
    snd_sfx(SFX_SELECT);
  }
  if (keys_down & (KEY_START | KEY_A)) {
    rng ^= frame * 2654435761u; if (!rng) rng = 1;
    if (save.mode != mode) { save.mode = mode; save_write(); }
    snd_sfx(SFX_SELECT);
    extern void play_enter(void);
    play_enter();
  }
}

/* ------------------------------------------------------------------ play */
void play_enter(void) {
  state = ST_PLAY;
  phys_init();
  phys_add_static_box(0, -BASE_HH, BASE_HW, BASE_HH);
  txt_clear();
  REG_DISPCNT = DCNT_MODE0 | DCNT_OBJ_1D | DCNT_BG0 | DCNT_BG2 | DCNT_OBJ;
  REG_BG0VOFS = 0;
  /* held piece is drawn semi-transparent like the "inactive" material */
  REG_BLDCNT = (1 << 6) | (0x2F << 8);     /* alpha blend, 2nd target: BG0-3 + backdrop + OBJ */
  REG_BLDALPHA = 12 | (7 << 8);
  video_set_theme(mode);
  REG_BGHOFS(2) = 0;
  last_color = -1;
  tray[0].shape = tray[1].shape = tray[2].shape = 0xFF;
  for (int k = 0; k < 3; k++) tray[k] = new_slot(k);
  cursor = 0; holding = 0; blocks = 0; score = 0; new_best = 0;
  cam_y = cam_target = 0; pop_t = 0; last_placed = -1;
  draw_tray_panels(); draw_hud();
#ifdef BENCH
  /* a settled tower of BENCH rectangles (asleep, as it would be after play) */
  for (int k = 0; k < BENCH; k++) { int b = phys_add_piece(1, k % 7, (k & 1) ? FX(0.3) : FX(-0.3), FX(1.5) + k * FX(3.0), 0); phys_bodies[b].flags |= BODY_SLEEP; }
  blocks = BENCH;
#endif
  snd_play_song(modes[mode].song);
}

static void game_over(void) {
  dbg_printf("GAME OVER blocks=%d", blocks);
  state = ST_OVER; over_t = 0; over_shown = 0; holding = 0;
  if (blocks > save.best[mode]) { save.best[mode] = blocks; new_best = 1; }
  if (mode == MODE_CLASSIC && blocks > save.best_blocks) save.best_blocks = blocks;
  if ((u32)score > save.best_score) save.best_score = score;
  save_write();
  snd_stop_song();
  snd_sfx(SFX_GAMEOVER);
  draw_tray_panels();
}

static void held_update_collision(void) {
  Poly polys[PIECE_MAX_PARTS];
  int n = phys_piece_world(tray[cursor].shape, hx, hy, hangle, polys);
  hcolliding = phys_overlaps(polys, n, FX(0.02));
}

static void play_input(void) {
  if (keys_down & KEY_START) { state = ST_PAUSE; pause_sel = 0; snd_sfx(SFX_SELECT); return; }
  if (!holding) {
    if (keys_down & KEY_UP) { cursor = (cursor + 2) % 3; snd_sfx(SFX_SELECT); draw_tray_panels(); }
    if (keys_down & KEY_DOWN) { cursor = (cursor + 1) % 3; snd_sfx(SFX_SELECT); draw_tray_panels(); }
    if (keys_down & KEY_A) {
      const PieceDef* d = &piece_defs[tray[cursor].shape];
      holding = 1; hold_spawn_t = 0;
      hx = 0; hy = tower_top() + d->bottom + HOLD_GAP;
      hangle = hangle_from = hangle_to = 0; hrot_t = ROT_FRAMES;
      move_frames = 0;
      snd_sfx(SFX_PICKUP);
    }
    return;
  }
  hold_spawn_t++;
  /* move */
  int dx = (keys & KEY_RIGHT ? 1 : 0) - (keys & KEY_LEFT ? 1 : 0);
  int dy = (keys & KEY_UP ? 1 : 0) - (keys & KEY_DOWN ? 1 : 0);
  if (dx || dy) {
    move_frames++;
    fx sp = FX(0.05) + move_frames * FX(0.008); if (sp > FX(0.22)) sp = FX(0.22);
    hx += dx * sp; hy += dy * sp;
    if (hx < FX(-6.8)) hx = FX(-6.8); if (hx > FX(7.0)) hx = FX(7.0);
    if (hy < FX(-1.5)) hy = FX(-1.5);
    if (hy > FX(60)) hy = FX(60);
  } else move_frames = 0;
  /* rotate in 45 degree steps (L = counter-clockwise, R = clockwise) */
  if (keys_down & (KEY_L | KEY_R)) {
    hangle_from = hangle; hangle_to += (keys_down & KEY_L) ? ROT_STEP : -ROT_STEP;
    hrot_t = 0;
    snd_sfx(keys_down & KEY_L ? SFX_ROTATE_L : SFX_ROTATE_R);
  }
  if (hrot_t < ROT_FRAMES) {
    hrot_t++;
    hangle = hangle_from + (fx)(((s64)(hangle_to - hangle_from) * hrot_t) / ROT_FRAMES);
  } else {
    /* normalise once settled */
    while (hangle_to > FX_PI) hangle_to -= 2 * FX_PI;
    while (hangle_to < -FX_PI) hangle_to += 2 * FX_PI;
    hangle = hangle_from = hangle_to;
  }
  held_update_collision();
  if (keys_down & KEY_B) { holding = 0; snd_sfx(SFX_PUTBACK); return; }
  if ((keys_down & KEY_A) && hold_spawn_t > 1) {
    if (hcolliding || hrot_t < ROT_FRAMES) { snd_sfx(SFX_ERROR); return; }
    int b = phys_add_piece(tray[cursor].shape, tray[cursor].color, hx, hy, hangle_to);
    if (b < 0) { snd_sfx(SFX_ERROR); return; }      /* every body slot is in use */
    last_placed = b;
    blocks++;
    score += 250 * blocks;
    pop_t = 50; pop_val = 250 * blocks; pop_x = hx; pop_y = hy + piece_defs[tray[cursor].shape].top;
    tray[cursor] = new_slot(cursor);
    holding = 0; cursor = 0;
    snd_sfx(SFX_PLACE);
    draw_tray_panels(); draw_hud();
  }
}

static void check_fall(void) {
  for (int i = 0; i < phys_count; i++) {
    Body* b = &phys_bodies[i];
    if ((b->flags & (BODY_USED | BODY_STATIC)) != BODY_USED) continue;
    if (b->p.y < FALL_Y || b->p.x > FALL_X || b->p.x < -FALL_X) { game_over(); return; }
  }
}

/* after a game over, blocks that fell past the base keep falling: drop them
   once they are well out of sight so they stop costing physics time */
static void drop_fallen(void) {
  if (phys_busy()) return;
  for (int i = 0; i < phys_count; i++) {
    Body* b = &phys_bodies[i];
    if ((b->flags & (BODY_USED | BODY_STATIC)) == BODY_USED && b->p.y < FALL_Y - FX(40.0)) phys_remove(i);
  }
}

static void update_camera(void) {
  if (state == ST_OVER) {           /* pan down to the base to watch the tower land */
    cam_target = 0;
    cam_y += (cam_target - cam_y) / 12;
    if (cam_y < FX(0.05)) cam_y = 0;
    return;
  }
  fx top = tower_top();
  fx focus = top + FX(4.5);
  if (holding && hy + FX(3.0) > focus) focus = hy + FX(3.0);
  fx t = focus - VIEW_CELLS; if (t < 0) t = 0;
  if (holding && hy - FX(2.5) < t) { t = hy - FX(2.5); if (t < 0) t = 0; }
  cam_target = t;
  cam_y += (cam_target - cam_y) / 8;
}

static void draw_world(void) {
  oam_reset_alloc(0); mtx_used = 0;
  /* tray matrices: normal and highlighted (with a little bob) */
  obj_affine_set(MTX_TRAY, FX_ONE, 0, 192);
  obj_affine_set(MTX_TRAY_HI, FX_ONE, 0, 214 + ((fx_sin(frame * 3000) * 8) >> 16));
  /* score pop-up */
  if (pop_t > 0) {
    pop_t--;
    char b[12] = "+"; int n = fmt_int(b + 1, pop_val) + 1;
    int x = sx_of(pop_x) - n * 4, y = sy_of(pop_y) - 18 - (50 - pop_t) / 3;
    if (pop_t > 8 || (pop_t & 1))
      for (int i = 0; i < n; i++) {
        ObjAttr* o = oam_alloc(); if (!o) break;
        if (y < -8 || y > 160) { o->attr0 = ATTR0_HIDE; continue; }
        o->attr0 = ATTR0_Y(y); o->attr1 = ATTR1_X(x + i * 7) | ATTR1_SIZE_8;
        o->attr2 = ATTR2_TILE(OBJ_TILE_FONT + b[i] - OBJ_FONT_FIRST) | ATTR2_PRIO(0) | ATTR2_PAL(OBJPAL_UI);
      }
  }
  /* held piece */
  if (holding) {
    fx c = fx_cos(hangle), s = fx_sin(hangle);
    obj_affine_set(MTX_HELD, c, s, 256);
    ObjAttr* o = oam_alloc();
    int pal = hcolliding ? OBJPAL_RED : tray[cursor].color;
    put_piece(o, tray[cursor].shape, sx_of(hx), sy_of(hy), c, s, pal, hangle == 0 ? PLAIN_UP : MTX_HELD, 1, 1);
  }
  /* tray */
  for (int k = 0; k < 3; k++) {
    if (holding && k == cursor) continue;
    if (state == ST_OVER) continue;
    ObjAttr* o = oam_alloc();
    put_piece(o, tray[k].shape, 28, 8 + 24 + k * 48, 0, 0, tray[k].color,
              (k == cursor && state == ST_PLAY) ? MTX_TRAY_HI : MTX_TRAY, 0, 0);
  }
  /* bodies */
  for (int i = 0; i < phys_count; i++) {
    Body* b = &phys_bodies[i];
    if ((b->flags & (BODY_USED | BODY_STATIC)) != BODY_USED) continue;
    int sx = sx_of(b->p.x), sy = sy_of(b->p.y);
    if (sx < -40 || sx > 280 || sy < -40 || sy > 200) continue;
    ObjAttr* o = oam_alloc(); if (!o) break;
    /* The GBA draws only ~1210 cycles of sprites per scanline: a 64x64 affine
     * sprite costs 138, a plain one 64. Upright or upside-down blocks (most of a
     * settled tower) use plain sprites, with flips for 180 degrees, so a busy row
     * doesn't run out and drop the last sprites. */
    int q = ((int)(((s64)b->a * 10430) >> 16) >> 7) & 511;
    if (q == 0 || q == 256) put_piece(o, b->shape, sx, sy, q ? -FX_ONE : FX_ONE, 0, b->color, q ? PLAIN_DOWN : PLAIN_UP, 2, 0);
    else put_piece(o, b->shape, sx, sy, b->c, b->s, b->color, mtx_for_angle(b->c, b->s, b->a), 2, 0);
  }
  /* base slab */
  int by = sy_of(0);
  for (int h = 0; h < 2; h++) {
    ObjAttr* o = oam_alloc(); if (!o) break;
    if (by >= 160) { o->attr0 = ATTR0_HIDE; continue; }
    o->attr0 = ATTR0_Y(by) | ATTR0_WIDE; o->attr1 = ATTR1_X(SCREEN_CX - 64 + h * 64) | ATTR1_SIZE_64 | (h ? ATTR1_HFLIP : 0);
    o->attr2 = ATTR2_TILE(OBJ_TILE_BASE) | ATTR2_PRIO(2) | ATTR2_PAL(OBJPAL_BASE);
  }
  layer_scroll(cam_y);
}

static void over_update(void) {
  over_t++;
  /* show the results once the camera has reached the base and the pieces had a moment to land */
  if (!over_shown && ((over_t >= 60 && cam_y == 0) || over_t >= 240)) {
    over_shown = 1;
    txt_panel(8, 4, 21, 12, PAL_PANEL);
    txt_center(5, 8, 21, "GAME OVER", PAL_TXT_YELLOW | TXT_ON_PANEL);
    char b[24] = "BLOCKS  "; fmt_int(b + 8, blocks); txt_center(7, 8, 21, b, PAL_TXT_WHITE | TXT_ON_PANEL);
    const char* msg = blocks < 50 ? messages[blocks] : blocks >= 100 ? "Stacking among the stars." : "Masterful.";
    txt_wrap(9, 9, 19, msg, PAL_TXT_GRAY | TXT_ON_PANEL);
    if (new_best) txt_center(12, 8, 21, "NEW BEST!", PAL_TXT_YELLOW | TXT_ON_PANEL);
    txt_center(14, 8, 21, "A RETRY   B TITLE", PAL_TXT_WHITE | TXT_ON_PANEL);
  }
  if (over_shown && over_t > 70) {
    if (keys_down & KEY_A) { snd_sfx(SFX_SELECT); play_enter(); }
    else if (keys_down & (KEY_B | KEY_START)) { snd_sfx(SFX_PUTBACK); title_enter(); }
  }
}

static void pause_update(void) {
  if (pause_sel == 0) {
    txt_panel(11, 6, 15, 7, PAL_PANEL);
    txt_center(7, 11, 15, "PAUSED", PAL_TXT_YELLOW | TXT_ON_PANEL);
    txt_center(9, 11, 15, "START RESUME", PAL_TXT_WHITE | TXT_ON_PANEL);
    txt_center(11, 11, 15, "SELECT QUIT", PAL_TXT_GRAY | TXT_ON_PANEL);
    pause_sel = 1;
  }
  if (keys_down & KEY_START) { txt_clear_rect(11, 6, 15, 7); draw_hud(); state = ST_PLAY; snd_sfx(SFX_SELECT); }
  else if (keys_down & KEY_SELECT) { snd_sfx(SFX_PUTBACK); title_enter(); }
}

/* ------------------------------------------------------------------ main loop */
/* Physics gets whatever is left of the frame once the game has drawn it. A step
   that does not fit carries on in the next frame (bodies keep their last
   positions until it ends), so a big wobbly tower runs in slow motion instead of
   dropping frames, and the controls, camera and music stay at 60 fps. At most one
   step starts per frame, so the simulation never runs faster than real time. */
#define PHYS_MARGIN 160      /* ticks kept back for estimate error and the loop's own tail */
static void run_physics(void) {
  u16 deadline = (u16)(vblank_tick + FRAME_TICKS - PHYS_MARGIN);
  int r = phys_run(deadline, 1);
  /* finished the step carried over from the last frame: start this frame's if there is room */
  if ((r & (PHYS_STARTED | PHYS_DONE)) == PHYS_DONE && (s16)(deadline - REG_TM2CNT_L) > 600) phys_run(deadline, 1);
#ifdef PROFILE
  if (r & PHYS_DONE) {
    static u32 last_done; if (frame - last_done > 1) dbg_printf("step took %lu frames: bodies=%d awake=%d pts=%d", frame - last_done, phys_count, phys_awake, phys_points);
    last_done = frame;
  }
#endif
}

int main(void) {
  REG_WAITCNT = 0x4317;   /* faster ROM waitstates + prefetch */
  REG_TM2CNT_H = 0; REG_TM2CNT_L = 0; REG_TM2CNT_H = 0x80 | 1;  /* 64-cycle ticks for profiling */
  video_init();
  snd_init();
  save_load();
  title_enter();
  int sky = 1024 - 161;
  for (;;) {
    video_vsync(sky);
#ifdef PROFILE
    u16 f_start = REG_TM2CNT_L;
#endif
    read_keys();
    frame++;
    switch (state) {
      case ST_TITLE:
        title_update();
        sky = 1024 - 161 - 40;
        break;
      case ST_PLAY:
      case ST_OVER: {
        if (state == ST_PLAY) play_input();
        if (state == ST_PLAY) check_fall();
        else if (state == ST_OVER) { over_update(); drop_fallen(); }
        if (state == ST_TITLE) break;
        update_camera();
        draw_world();
        sky = 1024 - 161 - (int)(((s64)cam_y * PX_PER_CELL) >> 17);
#ifdef PROFILE   /* the game's own share of the frame, before physics gets the rest */
        { static u32 sum, n; static u16 mx; u16 gt = (u16)(REG_TM2CNT_L - vblank_tick); sum += gt; n++; if (gt > mx) mx = gt;
          if (n == 256) { dbg_printf("game work: avg %lu max %u ticks (from VBlank), bodies %d", sum / n, mx, phys_count); sum = n = 0; mx = 0; } }
#endif
        if (state != ST_PAUSE) run_physics();
        break;
      }
      case ST_PAUSE:
        pause_update();
        break;
    }
#ifdef PROFILE   /* frames that ran past the next VBlank (4389 ticks) */
    { u16 ft = (u16)(REG_TM2CNT_L - f_start);
      if (ft > FRAME_TICKS - 120) dbg_printf("late f%lu ticks=%u bodies=%d awake=%d pts=%d", frame, ft, phys_count, phys_awake, phys_points); }
#endif
  }
}
