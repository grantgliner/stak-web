/* STAK! GBA - title screen with the mode picker, relaxed / classic / expert /
   arcade play, game over. */
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
enum { MTX_TRAY = 0, MTX_HELD = 3, MTX_DYN = 4 };    /* one matrix per tray slot, then the held piece */

/* piece pools, weighted like the web version's: piece indices are
   0 Plus, 1 Rectangle, 2 L, 3 L-Reverse, 4 Arrow, 5 PlusCut, 6 PeterPan, 7 S,
   8 Z, 9 T, 10 Zig, 11 Zag, 12 Circle, 13 Semicircle, 14 Lollipop, 15 Seesaw */
static const u8 pool_relaxed[] = { 0, 2, 3, 1, 0, 2, 3, 1, 7, 8, 7, 8, 9, 10, 11 };
static const u8 pool_classic[] = { 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const u8 pool_expert[] = { 0, 2, 3, 1, 6, 5, 4, 7, 8, 12, 13, 14, 15, 0, 2, 3, 1, 6, 5, 4, 7, 8, 12, 13, 14, 15, 9, 10, 11 };
/* arcade opens with the five simplest pieces, then the classic pool from 10
   placements and the expert one from 30, as the web version does */
static const u8 pool_arcade[] = { 1, 2, 3, 9, 0 };
enum { MODE_RELAXED, MODE_CLASSIC, MODE_EXPERT, MODE_ARCADE, NMODES };
typedef struct { const char* name; const char* desc; const u8* pool; u8 npool; const Song* song; } ModeDef;
static const ModeDef modes[NMODES] = {
  { "RELAXED", "STACK SIMPLE SHAPES!", pool_relaxed, sizeof pool_relaxed, &song_relaxed },
  { "CLASSIC", "STACK TRICKIER ANGLES!", pool_classic, sizeof pool_classic, &song_classic },
  { "EXPERT", "STACK CURVES AND ANGLES!", pool_expert, sizeof pool_expert, &song_expert },
  { "ARCADE", "SPECIAL PIECES, NEW RULES!", pool_arcade, sizeof pool_arcade, &song_expert },
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

/* ------------------------------------------------------------------ arcade rules
 * Special pieces (the web version's odds): magnets 10% (half +, half -),
 * sticky 3%, explosive 3%, weird 2%. Three placements in a row without a new
 * special make the next one special, and one slot of the opening tray is.
 * Conditions come round in turn: 4 calm turns, then one is announced 3 turns
 * ahead, lasts 3 turns, and 2-4 calm turns follow before the next (never the
 * same one twice running). */
enum { SP_NONE, SP_MAG_POS, SP_MAG_NEG, SP_STICKY, SP_EXPLOSIVE, SP_WEIRD };
enum { CN_NONE, CN_GIANT, CN_TINY, CN_WEIRD, CN_MAGNETIC, CN_STICKY, CN_EXPLOSIVE };
enum { CP_CALM, CP_INCOMING, CP_ACTIVE };
static const char* const cond_title[7] = { "ALL CLEAR", "GIANT PIECES", "TINY PIECES", "WEIRD ROTATION", "MAGNET STORM", "STICKY SITUATION", "POWDER KEG" };
static const char* const cond_summary[7] = { "", "PIECES GROW TO 1.5X SIZE.", "PIECES SHRINK TO HALF SIZE.", "L AND R TIP PIECES TOWARD OR AWAY FROM YOU.",
                                             "EVERY PIECE IS MAGNETIC.", "EVERY PIECE IS STICKY.", "EVERY PIECE IS EXPLOSIVE." };
static const u8 cond_music[7] = { TR_PLAIN, TR_DOWN3, TR_UP3, TR_WEIRD, TR_METAL, TR_DAMP, TR_RUMBLE };
static const u8 sp_palette[6] = { 0, OBJPAL_MAG_POS, OBJPAL_MAG_NEG, OBJPAL_STICKY, OBJPAL_EXPLOSIVE, OBJPAL_WEIRD };
static const u16 size_scale[3] = { 128, 256, 384 };      /* tiny, normal, giant (Q8) */

/* ------------------------------------------------------------------ state */
static int state, mode = MODE_CLASSIC;
static u32 frame;
static u16 keys, keys_prev, keys_down;
static u32 rng = 0x1234567;

typedef struct { u8 shape, color, special, size, suppressed; } Slot;   /* suppressed: special hidden by weird rotation, or 0xFF */
static Slot tray[3];
static int cursor, holding;
static fx hx, hy, hangle, hangle_from, hangle_to;
static int hrot_t, hcolliding, move_frames, hold_spawn_t;
static int blocks, score, new_best;
static fx cam_y, cam_target;
static int over_t, over_shown, pause_sel;
static int pop_t, pop_val; static fx pop_x, pop_y;
static int last_placed = -1, last_color = -1;

/* arcade (EWRAM: IWRAM is kept for the physics) */
static u8 htip EWRAM_BSS, htip_from EWRAM_BSS, htip_to EWRAM_BSS;   /* held weird piece: quarter turns toward the viewer */
static PhysShape hps EWRAM_BSS;                      /* the held piece's shape when sized or tipped */
static u8 hvar EWRAM_BSS;                            /* hps is in use */
static u8 body_sp[MAX_BODIES] EWRAM_BSS;             /* each body's special */
static u8 sticky_flag[MAX_BODIES] EWRAM_BSS, explosive_flag[MAX_BODIES] EWRAM_BSS, det_mark[MAX_BODIES] EWRAM_BSS;
static u8 n_sticky EWRAM_BSS, n_explosive EWRAM_BSS, n_det EWRAM_BSS, weld_formed EWRAM_BSS;
static u8 cn_phase EWRAM_BSS, cn_cond EWRAM_BSS, cn_left EWRAM_BSS, cn_prev EWRAM_BSS;
static u8 dry_spell EWRAM_BSS;                       /* placements since the last new special */
static u8 banner_t EWRAM_BSS, shake_t EWRAM_BSS;
static int shake_x EWRAM_BSS, shake_y EWRAM_BSS;
typedef struct { fx x, y; u8 t; } Boom;
#define MAX_BOOMS 4
#define BOOM_FRAMES 26
static Boom booms[MAX_BOOMS] EWRAM_BSS;

/* title decoration */
typedef struct { fx x, y, a, vy, w; u8 seed, color; } Deco;   /* shape and colour follow the mode */
static Deco deco[8] EWRAM_BSS;
static int logo_t;

/* ------------------------------------------------------------------ helpers */
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int rnd_n(int n) { return (int)((rnd() >> 8) % (u32)n); }
static u16* best_of(int m) { return m == MODE_ARCADE ? &save.best_arcade : &save.best[m]; }
static int arcade(void) { return mode == MODE_ARCADE; }
static int active_cond(void) { return cn_phase == CP_ACTIVE ? cn_cond : CN_NONE; }

static void read_keys(void) {
  keys_prev = keys;
  keys = ~REG_KEYINPUT & 0x3FF;
  keys_down = keys & ~keys_prev;
}

static inline int sx_of(fx x) { return SCREEN_CX + shake_x + (int)(((s64)x * PX_PER_CELL) >> 16); }
static inline int sy_of(fx y) { return BASE_SY + shake_y - (int)(((s64)(y - cam_y) * PX_PER_CELL) >> 16); }

/* a block sprite with its COM at (sx, sy), turned by (c, s): the sprite is
   centred on the COM, or on the piece's box with the offset (sox, soy) turned
   too (the lollipop). mtx: an affine matrix, or PLAIN_UP / PLAIN_DOWN for a
   plain sprite (cheaper per scanline) upright or flipped to 180 degrees. c = s
   = 0 centres it on its box (the tray). dbl: double-size render area, for
   sprites drawn larger than they are. */
#define PLAIN_UP   (-1)
#define PLAIN_DOWN (-2)
static void put_sprite(ObjAttr* o, int shape, int sx, int sy, fx c, fx s, fx sox, fx soy, int pal, int mtx, int prio, int blend, int dbl) {
  const PieceDef* d = &piece_defs[shape];
  if (sox | soy) {
    fx ox = fxmul(c, sox) - fxmul(s, soy), oy = fxmul(s, sox) + fxmul(c, soy);
    sx += (ox * PX_PER_CELL + 0x8000) >> 16; sy -= (oy * PX_PER_CELL + 0x8000) >> 16;
  }
  int half = d->small ? 16 : 32, ext = dbl ? 2 * half : half, x = sx - ext, y = sy - ext;
  if (x <= -2 * ext || x >= 240 || y <= -2 * ext || y >= 160) { o->attr0 = ATTR0_HIDE; return; }
  o->attr0 = ATTR0_Y(y) | (mtx >= 0 ? (dbl ? ATTR0_AFF_DBL : ATTR0_AFF) : 0) | ATTR0_SQUARE | (blend ? ATTR0_BLEND : 0);
  o->attr1 = ATTR1_X(x) | (d->small ? ATTR1_SIZE_32 : ATTR1_SIZE_64) |
             (mtx >= 0 ? ATTR1_AFFIDX(mtx) : mtx == PLAIN_DOWN ? ATTR1_HFLIP | ATTR1_VFLIP : 0);
  o->attr2 = ATTR2_TILE(d->tile) | ATTR2_PRIO(prio) | ATTR2_PAL(pal);
}
static void put_piece(ObjAttr* o, int shape, int sx, int sy, fx c, fx s, int pal, int mtx, int prio, int blend) {
  put_sprite(o, shape, sx, sy, c, s, piece_defs[shape].sox, piece_defs[shape].soy, pal, mtx, prio, blend, 0);
}
/* an 8x8 glyph from the OBJ font ('+' to '9') centred at (sx, sy) */
static void put_glyph(char ch, int sx, int sy, int pal, int prio) {
  ObjAttr* o = oam_alloc(); if (!o) return;
  int x = sx - 4, y = sy - 4;
  if (x <= -8 || x >= 240 || y <= -8 || y >= 160) { o->attr0 = ATTR0_HIDE; return; }
  o->attr0 = ATTR0_Y(y); o->attr1 = ATTR1_X(x) | ATTR1_SIZE_8;
  o->attr2 = ATTR2_TILE(OBJ_TILE_FONT + ch - OBJ_FONT_FIRST) | ATTR2_PRIO(prio) | ATTR2_PAL(pal);
}

/* affine matrices for bodies and effects, shared by everything drawn at the
   same angle and scale this frame */
static u32 mtx_key[32] EWRAM_BSS; static int mtx_used;
static int mtx_get(fx c, fx s, fx angle, int sx8, int sy8) {
  int q = (int)(((s64)angle * 10430) >> 16) >> 7;   /* 512 steps per turn */
  u32 key = (u32)(q & 511) | (u32)(sx8 & 1023) << 9 | (u32)(sy8 & 2047) << 19;
  for (int i = 0; i < mtx_used; i++) if (mtx_key[i] == key) return MTX_DYN + i;
  if (MTX_DYN + mtx_used < 32) {
    mtx_key[mtx_used] = key;
    obj_affine_set2(MTX_DYN + mtx_used, c, s, sx8, sy8);
    return MTX_DYN + mtx_used++;
  }
  /* out of matrices: the nearest angle at the same scale (or any) */
  int best = 0, bd = 1 << 30;
  for (int i = 0; i < mtx_used; i++) {
    int d = (int)(mtx_key[i] & 511) - (q & 511); if (d < 0) d = -d; if (d > 256) d = 512 - d;
    if ((mtx_key[i] >> 9) != (key >> 9)) d += 1024;
    if (d < bd) { bd = d; best = i; }
  }
  return MTX_DYN + best;
}
static int mtx_for_angle(fx c, fx s, fx angle) { return mtx_get(c, s, angle, 256, 256); }

/* ------------------------------------------------------------------ slots and specials */
static const u8* pool_now(int* n) {
  const ModeDef* md = &modes[mode];
  if (arcade() && blocks >= 30) { *n = sizeof pool_expert; return pool_expert; }
  if (arcade() && blocks >= 10) { *n = sizeof pool_classic; return pool_classic; }
  *n = md->npool; return md->pool;
}
static int roll_special(int guaranteed) {
#if defined(ARCADE_TEST) && ARCADE_TEST > 6     /* testing: every piece has special ARCADE_TEST - 6 */
  return ARCADE_TEST - 6 == SP_MAG_POS ? (rnd_n(2) ? SP_MAG_POS : SP_MAG_NEG) : ARCADE_TEST - 6;
#endif
  int r = guaranteed ? rnd_n(18) : rnd_n(100);
  if (r < 10) return rnd_n(2) ? SP_MAG_POS : SP_MAG_NEG;
  if (r < 13) return SP_STICKY;
  if (r < 16) return SP_EXPLOSIVE;
  if (r < 18) return SP_WEIRD;
  return SP_NONE;
}
/* a magnet storm keeps the tray's charges even */
static int balanced_charge(int idx) {
  int p = 0, n = 0;
  for (int k = 0; k < 3; k++) if (k != idx) { if (tray[k].special == SP_MAG_POS) p++; else if (tray[k].special == SP_MAG_NEG) n++; }
  return p < n ? SP_MAG_POS : n < p ? SP_MAG_NEG : rnd_n(2) ? SP_MAG_POS : SP_MAG_NEG;
}
/* size and special from the condition; hint: a special to keep (or -1 to roll one) */
static void slot_rules(Slot* s, int idx, int hint) {
  int c = active_cond();
  s->size = c == CN_GIANT ? 2 : c == CN_TINY ? 0 : 1;
  s->special = SP_NONE; s->suppressed = 0xFF;
  if (!arcade()) return;
  int sp = c == CN_MAGNETIC ? balanced_charge(idx) : c == CN_STICKY ? SP_STICKY : c == CN_EXPLOSIVE ? SP_EXPLOSIVE :
           hint >= 0 ? hint : roll_special(0);
  if (c == CN_WEIRD) { s->suppressed = sp; sp = SP_WEIRD; }
  s->special = sp;
}
static Slot new_slot(int idx, int hint) {
  int npool; const u8* pool = pool_now(&npool);
  Slot s;
  for (;;) {
    s.shape = pool[rnd_n(npool)];
    int clash = 0;
    for (int k = 0; k < 3; k++) if (k != idx && tray[k].shape == s.shape) clash = 1;
    if (!clash) break;
  }
  /* a new colour each time, so neighbours stand apart (expert has only three) */
  do s.color = rnd_n(themes[mode].ncolors); while (s.color == last_color);
  last_color = s.color;
  tray[idx].special = SP_NONE;               /* not counted while it is being replaced */
  slot_rules(&s, idx, hint);
  return s;
}
static int slot_pal(const Slot* s) { return s->special ? sp_palette[s->special] : s->color; }

/* ------------------------------------------------------------------ arcade conditions */
static void hud_condition(void) {
  if (!arcade()) return;
  int c = cn_phase == CP_CALM ? CN_NONE : cn_cond;
  for (int i = 0; i < 16; i++) PAL_BG[PAL_TXT_ACCENT * 16 + i] = accent_pal[c * 16 + i];
  txt_clear_rect(7, 0, 16, 2);
  txt_put(7, 0, cond_title[c], PAL_TXT_ACCENT);
  char b[16]; int n = 0;
  const char* t = cn_phase == CP_CALM ? "NO CONDITION" : cn_phase == CP_INCOMING ? (cn_left == 1 ? "NEXT TURN" : "IN # TURNS") :
                  cn_left == 1 ? "FINAL TURN" : "# TURNS LEFT";
  for (; *t; t++) b[n++] = *t == '#' ? '0' + cn_left : *t;
  b[n] = 0;
  txt_put(7, 1, b, PAL_TXT_GRAY);
  /* dots for the turns of the phase, lit for the ones left */
  if (cn_phase != CP_CALM) for (int k = 0; k < 3; k++) txt_put(7 + n + 1 + k, 1, k < cn_left ? "*" : ".", k < cn_left ? PAL_TXT_ACCENT : PAL_TXT_GRAY);
}
static void banner_clear(void) { if (banner_t) { banner_t = 0; txt_clear_rect(8, 6, 21, 6); } }
static void banner_show(int c) {
  txt_panel(8, 6, 21, 6, PAL_PANEL);
  txt_center(7, 8, 21, cond_title[c], PAL_TXT_ACCENT | TXT_ON_PANEL);
  txt_wrap(9, 9, 19, cond_summary[c], PAL_TXT_WHITE | TXT_ON_PANEL);
  banner_t = 150;
}
/* one placement: returns 1 if the active condition changed */
static int cond_advance(void) {
#if defined(ARCADE_TEST) && ARCADE_TEST <= 6
  return 0;
#endif
  int was = active_cond();
  switch (cn_phase) {
    case CP_CALM:
      if (cn_left > 1) cn_left--;
      else {
        int c; do c = 1 + rnd_n(6); while (c == cn_prev);
        cn_phase = CP_INCOMING; cn_cond = c; cn_left = 3;
        snd_sfx(SFX_WARN);
      }
      break;
    case CP_INCOMING:
      if (cn_left > 1) cn_left--;
      else { cn_phase = CP_ACTIVE; cn_left = 3; }
      break;
    case CP_ACTIVE:
      if (cn_left > 1) cn_left--;
      else { cn_prev = cn_cond; cn_phase = CP_CALM; cn_left = 2 + rnd_n(3); }
      break;
  }
  int now = active_cond();
  if (now != was) {
    snd_set_treatment(cond_music[now]);
    if (now) { snd_sfx(SFX_CHIME); banner_show(now); } else banner_clear();
  }
  hud_condition();
  return now != was;
}
/* a condition began or ended: the tray's pieces take its size and specials
   (one that imposed a special leaves them to be rolled again) */
static void tray_refresh(int was) {
  int imposed = was == CN_MAGNETIC || was == CN_STICKY || was == CN_EXPLOSIVE;
  for (int k = 0; k < 3; k++) {
    int hint = imposed ? -1 : tray[k].suppressed != 0xFF ? tray[k].suppressed : tray[k].special;
    tray[k].special = SP_NONE;
    slot_rules(&tray[k], k, hint);
  }
}

/* ------------------------------------------------------------------ arcade physics hooks */
static void on_sticky(int a, int b) { if (phys_weld(a, b)) weld_formed = 1; }
static void on_explosive(int a, int b) {
  if ((phys_bodies[a].flags | phys_bodies[b].flags) & BODY_STATIC) return;    /* the base is safe */
  if (explosive_flag[a]) { if (det_mark[a] != 2) n_det++; det_mark[a] = 2; if (!det_mark[b]) det_mark[b] = 1; }
  if (explosive_flag[b]) { if (det_mark[b] != 2) n_det++; det_mark[b] = 2; if (!det_mark[a]) det_mark[a] = 1; }
}
static void boom_at(fx x, fx y) {
  int k = 0;
  for (int i = 1; i < MAX_BOOMS; i++) if (booms[i].t < booms[k].t) k = i;   /* a free slot, or the oldest */
  for (int i = 0; i < MAX_BOOMS; i++) if (!booms[i].t) { k = i; break; }
  booms[k].x = x; booms[k].y = y; booms[k].t = BOOM_FRAMES;
}
static void body_forget(int i) {
  if (sticky_flag[i]) { sticky_flag[i] = 0; n_sticky--; }
  if (explosive_flag[i]) { explosive_flag[i] = 0; n_explosive--; }
  body_sp[i] = SP_NONE;
}
/* between physics steps: sticky blocks hold on to whatever they touch, and
   explosive ones blow up with everything they touch (but not the base) */
static void arcade_after_step(void) {
  if (n_sticky) { weld_formed = 0; phys_touching(sticky_flag, on_sticky); if (weld_formed) snd_sfx(SFX_SQUISH); }
  if (!n_explosive) return;
  n_det = 0;
  phys_touching(explosive_flag, on_explosive);
  if (!n_det) return;
  for (int i = 0; i < phys_count; i++) if (det_mark[i] == 2) boom_at(phys_bodies[i].p.x, phys_bodies[i].p.y);
  for (int i = 0; i < phys_count; i++) if (det_mark[i]) {
    det_mark[i] = 0;
    if (!(phys_bodies[i].flags & BODY_USED)) continue;
    body_forget(i);
    phys_remove_wake(i);
    if (last_placed == i) last_placed = -1;
  }
  shake_t = 14;
  snd_sfx(SFX_EXPLODE);
}

/* ------------------------------------------------------------------ the held piece */
static void held_shape(void) {
  const Slot* s = &tray[cursor];
  int tip = htip_to & 3;
  hvar = s->size != 1 || tip;
  if (hvar) phys_shape(s->shape, s->size, tip, &hps);
}
static fx held_bottom(void) { return hvar ? hps.bottom : piece_defs[tray[cursor].shape].bottom; }
static fx held_top(void) { return hvar ? hps.top : piece_defs[tray[cursor].shape].top; }

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
  txt_clear_rect(24, 0, 8, 3); txt_clear_rect(18, 3, 14, 2);    /* arcade's condition line runs to column 22 */
  txt_put(24, 0, "BLOCKS", PAL_TXT_YELLOW);
  txt_big_num(30, 1, blocks, PAL_TXT_WHITE);
  int best = *best_of(mode);
  char b[16] = "BEST "; fmt_int(b + 5, best > blocks ? best : blocks);
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
  char c[16] = "BEST "; fmt_int(c + 5, *best_of(mode));
  txt_center(17, 1, 28, c, PAL_TXT_WHITE | on);
  txt_put(22, 19, "HOMEBREW", PAL_TXT_GRAY);
}

static void title_enter(void) {
  state = ST_TITLE;
  phys_init();
  txt_clear();
  mode = save.mode;
  video_set_theme(mode);
  for (int i = 0; i < 16; i++) PAL_BG[LOGO_COLOR_BASE + i] = logo_pal[i];   /* arcade's HUD borrows these */
  REG_BGHOFS(2) = 0;
  REG_DISPCNT = DCNT_MODE0 | DCNT_OBJ_1D | DCNT_BG0 | DCNT_BG1 | DCNT_BG2 | DCNT_OBJ;
  REG_BLDCNT = 0;
  logo_t = 0; shake_x = shake_y = 0;
  for (int i = 0; i < 8; i++) {
    Deco* d = &deco[i];
    d->x = FX(-8) + rnd_n(16 * 256) * 256 + FX(-4) + FX(4);
    d->y = FX(2) + i * FX(2.2);
    d->vy = FX(0.012) + rnd_n(FX(0.02));
    d->a = rnd_n(FX(6.28)); d->w = rnd_n(FX(0.04)) - FX(0.02);
    d->seed = rnd_n(256); d->color = rnd_n(7);
  }
  draw_mode_picker();
  snd_set_treatment(TR_PLAIN);
  snd_play_song(&song_menu);
}

/* the background layer drifts: clouds blow sideways, champagne bubbles rise,
   stars sink slowly on the title, arcade confetti floats up and across; it
   scrolls at a quarter of the camera */
static void layer_scroll(fx cam) {
  int y = -(int)(((s64)cam * PX_PER_CELL) >> 18);
  if (mode == MODE_RELAXED || mode == MODE_ARCADE) REG_BGHOFS(2) = (u16)(frame / (mode == MODE_ARCADE ? 9 : 6));
  if (mode == MODE_EXPERT) y += frame / 3;
  else if (mode == MODE_ARCADE) y += frame / 5;
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
  /* falling decorative blocks (arcade: some of them special) */
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
    int pal = d->color % themes[mode].ncolors;
    if (mode == MODE_ARCADE && (d->seed & 3) == 0) pal = sp_palette[1 + (d->seed >> 2) % 5];
    put_piece(o, modes[mode].pool[d->seed % modes[mode].npool], sx, BASE_SY - (int)(((s64)d->y * PX_PER_CELL) >> 16), c, s, pal, m, 2, 0);
  }
  layer_scroll(0);
  if (keys_down & (KEY_LEFT | KEY_RIGHT)) {
    mode = (mode + ((keys_down & KEY_RIGHT) ? 1 : NMODES - 1)) % NMODES;
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
  cursor = 0; holding = 0; blocks = 0; score = 0; new_best = 0;
  cam_y = cam_target = 0; pop_t = 0; last_placed = -1;
  /* arcade: lighter gravity, grippier and less bouncy blocks (the web version's
     arcade world), the condition cycle from the top, no specials yet */
  for (int i = 0; i < MAX_BODIES; i++) { body_sp[i] = sticky_flag[i] = explosive_flag[i] = det_mark[i] = 0; }
  n_sticky = n_explosive = 0;
  cn_phase = CP_CALM; cn_left = 4; cn_cond = cn_prev = CN_NONE; dry_spell = 0;
#if defined(ARCADE_TEST) && ARCADE_TEST <= 6     /* testing: condition ARCADE_TEST from the start, for good */
  cn_phase = CP_ACTIVE; cn_cond = ARCADE_TEST; cn_left = 3; snd_set_treatment(cond_music[cn_cond]);
#endif
  banner_t = shake_t = 0; shake_x = shake_y = 0;
  for (int k = 0; k < MAX_BOOMS; k++) booms[k].t = 0;
  htip = htip_from = htip_to = 0; hvar = 0;
  if (cn_phase != CP_ACTIVE) snd_set_treatment(TR_PLAIN);
  if (arcade()) { phys_set_params(FX(392.4 * 0.85), FX(0.9), FX(0.02)); phys_after_step = arcade_after_step; }
  tray[0].shape = tray[1].shape = tray[2].shape = 0xFF;
  tray[0].special = tray[1].special = tray[2].special = SP_NONE;
  int lucky = arcade() ? rnd_n(3) : -1;               /* one opening slot is special */
  for (int k = 0; k < 3; k++) tray[k] = new_slot(k, k == lucky ? roll_special(1) : -1);
  draw_tray_panels(); draw_hud(); hud_condition();
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
  banner_clear();
  u16* best = best_of(mode);
  if (blocks > *best) { *best = blocks; new_best = 1; }
  if (mode == MODE_CLASSIC && blocks > save.best_blocks) save.best_blocks = blocks;
  if ((u32)score > save.best_score) save.best_score = score;
  save_write();
  snd_stop_song();
  snd_set_treatment(TR_PLAIN);
  snd_sfx(SFX_GAMEOVER);
  draw_tray_panels();
}

static void held_update_collision(void) {
  Poly polys[PIECE_MAX_PARTS];
  int n = hvar ? phys_shape_world(&hps, hx, hy, hangle, polys) : phys_piece_world(tray[cursor].shape, hx, hy, hangle, polys);
  hcolliding = phys_overlaps(polys, n, FX(0.02));
}

static void place_held(void) {
  Slot* sl = &tray[cursor];
  int b = hvar ? phys_add_shape(&hps, sl->color, hx, hy, hangle_to) : phys_add_piece(sl->shape, sl->color, hx, hy, hangle_to);
  if (b < 0) { snd_sfx(SFX_ERROR); return; }      /* every body slot is in use */
  last_placed = b;
  blocks++;
  score += 250 * blocks;
  pop_t = 50; pop_val = 250 * blocks; pop_x = hx; pop_y = hy + held_top();
  int sp = sl->special;
  body_sp[b] = sp;
  phys_charge[b] = sp == SP_MAG_POS ? 1 : sp == SP_MAG_NEG ? -1 : 0;
  sticky_flag[b] = sp == SP_STICKY; explosive_flag[b] = sp == SP_EXPLOSIVE;
  n_sticky += sticky_flag[b]; n_explosive += explosive_flag[b];
  holding = 0; htip = htip_from = htip_to = 0;
  if (arcade()) {
    /* the condition moves on a turn (the tray follows if it changed), then the
       empty slot is dealt: special for sure after three plain ones in a row */
    int was = active_cond();
    sl->special = SP_NONE;
    if (cond_advance()) tray_refresh(was);
    tray[cursor] = new_slot(cursor, dry_spell >= 3 ? roll_special(1) : -1);
    dry_spell = tray[cursor].special != SP_NONE ? 0 : dry_spell + 1;
  } else tray[cursor] = new_slot(cursor, -1);
  cursor = 0;
  snd_sfx(SFX_PLACE);
  draw_tray_panels(); draw_hud();
}

static void play_input(void) {
  if (keys_down & KEY_START) { banner_clear(); state = ST_PAUSE; pause_sel = 0; snd_sfx(SFX_SELECT); return; }
  if (!holding) {
    if (keys_down & KEY_UP) { cursor = (cursor + 2) % 3; snd_sfx(SFX_SELECT); draw_tray_panels(); }
    if (keys_down & KEY_DOWN) { cursor = (cursor + 1) % 3; snd_sfx(SFX_SELECT); draw_tray_panels(); }
    if (keys_down & KEY_A) {
      holding = 1; hold_spawn_t = 0;
      htip = htip_from = htip_to = 0;
      held_shape();
      hx = 0; hy = tower_top() + held_bottom() + HOLD_GAP;
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
  /* rotate in 45 degree steps (L = counter-clockwise, R = clockwise); a weird
     piece tips toward (R) or away from (L) the viewer in quarter turns instead */
  if (keys_down & (KEY_L | KEY_R)) {
    if (tray[cursor].special == SP_WEIRD) {
      if (hrot_t >= ROT_FRAMES) {
        htip_from = htip_to & 3; htip_to = htip_from + ((keys_down & KEY_R) ? 1 : -1);
        hrot_t = 0;
        held_shape();
        snd_sfx(SFX_TIP);
      }
    } else {
      hangle_from = hangle; hangle_to += (keys_down & KEY_L) ? ROT_STEP : -ROT_STEP;
      hrot_t = 0;
      snd_sfx(keys_down & KEY_L ? SFX_ROTATE_L : SFX_ROTATE_R);
    }
  }
  if (hrot_t < ROT_FRAMES) {
    hrot_t++;
    hangle = hangle_from + (fx)(((s64)(hangle_to - hangle_from) * hrot_t) / ROT_FRAMES);
  } else {
    /* normalise once settled */
    while (hangle_to > FX_PI) hangle_to -= 2 * FX_PI;
    while (hangle_to < -FX_PI) hangle_to += 2 * FX_PI;
    hangle = hangle_from = hangle_to;
    htip_to &= 3; htip = htip_from = htip_to;
  }
  held_update_collision();
  if (keys_down & KEY_B) { holding = 0; htip = htip_from = htip_to = 0; snd_sfx(SFX_PUTBACK); return; }
  if ((keys_down & KEY_A) && hold_spawn_t > 1) {
    if (hcolliding || hrot_t < ROT_FRAMES) { snd_sfx(SFX_ERROR); return; }
    place_held();
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
    if ((b->flags & (BODY_USED | BODY_STATIC)) == BODY_USED && b->p.y < FALL_Y - FX(40.0)) { body_forget(i); phys_remove(i); }
  }
}

static void update_camera(void) {
  if (shake_t) { shake_t--; shake_x = shake_t ? ((shake_t & 2) ? 2 : -2) * (shake_t > 7 ? 2 : 1) : 0; shake_y = shake_t ? ((shake_t & 1) ? 1 : -1) * (shake_t > 7 ? 2 : 1) : 0; }
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

/* explosions: a fireball (the circle sprite, fire palette, growing and
   see-through) with white sparks flying out */
static void draw_booms(void) {
  for (int k = 0; k < MAX_BOOMS; k++) {
    Boom* bm = &booms[k];
    if (!bm->t) continue;
    int age = BOOM_FRAMES - bm->t--;
    int sx = sx_of(bm->x), sy = sy_of(bm->y);
    static const s8 dirs[8][2] = { { 16, 0 }, { 11, 11 }, { 0, 16 }, { -11, 11 }, { -16, 0 }, { -11, -11 }, { 0, -16 }, { 11, -11 } };
    if (age < 18) for (int i = 0; i < 8; i++) {
      int r = age * 2 + 4;
      put_glyph(i & 1 ? '.' : '+', sx + dirs[i][0] * r / 16, sy + dirs[i][1] * r / 16 - (age * age) / 40, OBJPAL_UI, 0);
    }
    int sc = 140 + age * 18; if (sc > 500) sc = 500;
    ObjAttr* o = oam_alloc(); if (!o) continue;
    put_sprite(o, 12, sx, sy, 0, 0, 0, 0, OBJPAL_FIRE, mtx_get(FX_ONE, 0, 0, sc, sc), 0, age > 6, 1);
  }
}

static void draw_world(void) {
  oam_reset_alloc(0); mtx_used = 0;
  draw_booms();
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
        o->attr2 = ATTR2_TILE(OBJ_TILE_FONT + b[i] - OBJ_FONT_FIRST) | ATTR2_PRIO(banner_t ? 2 : 0) | ATTR2_PAL(OBJPAL_UI);   /* behind the condition banner */
      }
  }
  /* held piece */
  if (holding) {
    const Slot* sl = &tray[cursor];
    fx c = fx_cos(hangle), s = fx_sin(hangle);
    int sc = size_scale[sl->size], sy8 = sc;
    fx sox = piece_defs[sl->shape].sox, soy = piece_defs[sl->shape].soy;
    if (hvar) { sox = hps.sox; soy = hps.soy; sy8 = (int)(((s64)hps.sy * 256) >> 16); }
    if (hrot_t < ROT_FRAMES && htip_from != htip_to) {
      /* tipping: squash through the turn (never thinner than the block is thick) */
      fx a = (FX_PI / 2) * htip_from + (fx)(((s64)(FX_PI / 2) * (htip_to - htip_from) * hrot_t) / ROT_FRAMES);
      fx cy = fx_cos(a), th = (fx)(((s64)FX_ONE << 16) / (piece_defs[sl->shape].top + piece_defs[sl->shape].bottom));
      if (cy < th && cy > -th) {            /* edge-on: the side it ends up on (tips 2 and 3 are upside down) */
        int t = hrot_t * 2 < ROT_FRAMES ? htip_from : htip_to;
        cy = (t & 3) >= 2 ? -th : th;
      }
      sy8 = (int)(((s64)cy * sc) >> 16);
      soy = fxmul(piece_defs[sl->shape].soy, fxmul(cy, size_scale[sl->size] << 8));
    }
    int pal = hcolliding ? OBJPAL_RED : slot_pal(sl);
    if (sl->special == SP_MAG_POS || sl->special == SP_MAG_NEG) put_glyph(sl->special == SP_MAG_POS ? '+' : '-', sx_of(hx), sy_of(hy), OBJPAL_UI, 1);
    ObjAttr* o = oam_alloc();
    if (o) {
      int plain = hangle == 0 && sc == 256 && sy8 == 256;
      if (!plain) { obj_affine_set2(MTX_HELD, c, s, sc, sy8); }
      put_sprite(o, sl->shape, sx_of(hx), sy_of(hy), c, s, sox, soy, pal, plain ? PLAIN_UP : MTX_HELD, 1, 1, sc > 256);
    }
  }
  /* tray: one matrix per slot (sizes differ in arcade), the cursor's bobbing */
  for (int k = 0; k < 3; k++) {
    if (holding && k == cursor) continue;
    if (state == ST_OVER) continue;
    const Slot* sl = &tray[k];
    int hi = k == cursor && state == ST_PLAY;
    int sc = (hi ? 214 + ((fx_sin(frame * 3000) * 8) >> 16) : 192) * size_scale[sl->size] >> 8;
    obj_affine_set(MTX_TRAY + k, FX_ONE, 0, sc);
    if (sl->special == SP_MAG_POS || sl->special == SP_MAG_NEG) put_glyph(sl->special == SP_MAG_POS ? '+' : '-', 28, 32 + k * 48, OBJPAL_UI, 0);
    ObjAttr* o = oam_alloc();
    put_sprite(o, sl->shape, 28, 8 + 24 + k * 48, 0, 0, 0, 0, slot_pal(sl), MTX_TRAY + k, 0, 0, sc > 256);
  }
  /* bodies */
  for (int i = 0; i < phys_count; i++) {
    Body* b = &phys_bodies[i];
    if ((b->flags & (BODY_USED | BODY_STATIC)) != BODY_USED) continue;
    int sx = sx_of(b->p.x), sy = sy_of(b->p.y);
    if (sx < -60 || sx > 300 || sy < -60 || sy > 220) continue;
    int sp = body_sp[i], pal = sp ? sp_palette[sp] : b->color;
    if (sp == SP_MAG_POS || sp == SP_MAG_NEG) put_glyph(sp == SP_MAG_POS ? '+' : '-', sx, sy, OBJPAL_UI, 2);
    ObjAttr* o = oam_alloc(); if (!o) break;
    const PhysShape* ps = phys_body_shape(i);
    if (ps) {
      int sx8 = (int)(((s64)ps->sx * 256) >> 16), sy8 = (int)(((s64)ps->sy * 256) >> 16);
      put_sprite(o, b->shape, sx, sy, b->c, b->s, ps->sox, ps->soy, pal, mtx_get(b->c, b->s, b->a, sx8, sy8), 2, 0, sx8 > 256);
      continue;
    }
    /* The GBA draws only ~1210 cycles of sprites per scanline: a 64x64 affine
     * sprite costs 138, a plain one 64. Upright or upside-down blocks (most of a
     * settled tower) use plain sprites, with flips for 180 degrees, so a busy row
     * doesn't run out and drop the last sprites. */
    int q = ((int)(((s64)b->a * 10430) >> 16) >> 7) & 511;
    if (q == 0 || q == 256) put_piece(o, b->shape, sx, sy, q ? -FX_ONE : FX_ONE, 0, pal, q ? PLAIN_DOWN : PLAIN_UP, 2, 0);
    else put_piece(o, b->shape, sx, sy, b->c, b->s, pal, mtx_for_angle(b->c, b->s, b->a), 2, 0);
  }
  /* base slab */
  int by = sy_of(0);
  for (int h = 0; h < 2; h++) {
    ObjAttr* o = oam_alloc(); if (!o) break;
    if (by >= 160) { o->attr0 = ATTR0_HIDE; continue; }
    o->attr0 = ATTR0_Y(by) | ATTR0_WIDE; o->attr1 = ATTR1_X(sx_of(0) - 64 + h * 64) | ATTR1_SIZE_64 | (h ? ATTR1_HFLIP : 0);
    o->attr2 = ATTR2_TILE(OBJ_TILE_BASE) | ATTR2_PRIO(2) | ATTR2_PAL(OBJPAL_BASE);
  }
  layer_scroll(cam_y);
  /* the condition banner fades out on its own */
  if (banner_t && !--banner_t) txt_clear_rect(8, 6, 21, 6);
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

/* arcade: explosive blocks glow and weird ones shimmer (palette animation, in VBlank) */
static void animate_palettes(void) {
  if (mode != MODE_ARCADE || state == ST_TITLE) return;
  const u16* e = &explosive_anim[((frame >> 2) & 15) * 16];
  const u16* w = &weird_anim[((frame >> 3) & 15) * 16];
  for (int i = 0; i < 16; i++) { PAL_OBJ[OBJPAL_EXPLOSIVE * 16 + i] = e[i]; PAL_OBJ[OBJPAL_WEIRD * 16 + i] = w[i]; }
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
    animate_palettes();
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
      if (ft > FRAME_TICKS - 120) dbg_printf("late f%lu ticks=%u bodies=%d awake=%d pts=%d", frame, ft, phys_count, phys_awake, phys_points);
    }
#endif
  }
}
