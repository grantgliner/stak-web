/* Drive stage: Mode 7 race, three laps against three rivals.
 *
 * Mode 1. BG2 is the 1024x1024 affine track; its PA/PB/PC/PD/X/Y are rewritten
 * every scanline by HBlank DMA0 from a table built once per frame (double
 * buffered). BG1 is the horizon panorama, scrolled with the heading. WIN0
 * shows BG2 only below the horizon. PAL_BG[0] follows a per-line sky gradient
 * on DMA2. BG0 is the text HUD. */
#include "game.h"
#include "video.h"
#include "audio.h"
#include "gen/drive_gen.h"
#include "gen/run_gen.h"
#include "gen/music_gen.h"

#define SBB_TRACK 8          /* CBB1, 16 KB of u8 map */
#define SBB_HZ 24
#define SBB_HUD 28
#define FONT_TILE 160        /* in CBB2, after the horizon tiles */
#define T_REAR 0
#define T_RIVAL 96
#define P_REAR 7

#define CAM_H 45             /* camera height, world px */
#define CAR_LINE 149         /* screen line where the player car touches the road */
#define LAPS 3
#define NRIV 3
#define FINISH_WP 4          /* waypoint of the start/finish checker */

#define MAXV 820             /* Q8 px/frame */
#define BOOSTV 1300
#define SEAV 380

extern const u16 recip_lut[256];

static u32 m7[2][161 * 4] EWRAM_BSS __attribute__((aligned(4)));
static int m7_back;

typedef struct { s32 x, y; int t, lap, spd, base, lane, pal; } Rival;
static Rival riv[NRIV];

static s32 px, py;           /* player car, Q8 world px */
static int pang;             /* heading, Q8 of a 256-step turn (0..65535) */
static int pspd, boost_t, boosts, bump_cd, pidx, plap, foc8;
static s32 camx, camy, fx12, fy12;

/* interpolated sin/cos for smooth rotation; a is Q8 of the 256-step LUT */
static s32 isin(int a) {
  int i = (a >> 8) & 255, f = a & 255;
  s32 s0 = sin_lut[i], s1 = sin_lut[(i + 1) & 255];
  return s0 + (((s1 - s0) * f) >> 8);
}
static s32 icos(int a) { return isin(a + (64 << 8)); }

static int wpx(int i) { return track_wp[(i % NUM_WP) * 2]; }
static int wpy(int i) { return track_wp[(i % NUM_WP) * 2 + 1]; }

static int col_at(s32 x8, s32 y8) {
  int x = x8 >> 11, y = y8 >> 11;            /* Q8 px -> 8 px cells */
  if (x < 0 || y < 0 || x >= 128 || y >= 128) return 0;
  return track_col[y * 128 + x];
}

static int iabs(int v) { return v < 0 ? -v : v; }
static int approx_len(int dx, int dy) {
  dx = iabs(dx); dy = iabs(dy);
  return dx > dy ? dx + (dy * 3 >> 3) : dy + (dx * 3 >> 3);
}

/* ---------------------------------------------------------------- per-line affine table */
static void build_m7(void) {
  u32* t = m7[m7_back];
  s32 sn = isin(pang), cs = icos(pang);
  fx12 = cs; fy12 = sn;                       /* forward */
  s32 rx = -sn, ry = cs;                      /* right */
  int car_z = CAM_H * foc8 / (CAR_LINE - HORIZON) >> 8;
  camx = px - fx12 * car_z / 16;              /* Q12 * px -> Q8: >> 4 */
  camy = py - fy12 * car_z / 16;
  u32 first[4];
  for (int y = HORIZON + 1; y < 161; y++) {
    int v = y - HORIZON;
    s32 s12 = (CAM_H * recip_lut[v]) >> 4;    /* world px per screen px, Q12 */
    s32 pa = (s12 * rx) >> 16, pc = (s12 * ry) >> 16;
    s32 z4 = (s12 * (foc8 >> 8)) >> 8;         /* forward distance, Q4 */
    s32 bx = camx + ((fx12 * z4) >> 8) - 120 * pa;
    s32 by = camy + ((fy12 * z4) >> 8) - 120 * pc;
    u32* e = t + y * 4;
    e[0] = (u16)pa; e[1] = (u16)pc; e[2] = bx; e[3] = by;
    if (y == HORIZON + 1) { first[0] = e[0]; first[1] = e[1]; first[2] = e[2]; first[3] = e[3]; }
  }
  for (int y = 0; y <= HORIZON; y++) { u32* e = t + y * 4; e[0] = first[0]; e[1] = first[1]; e[2] = first[2]; e[3] = first[3]; }
  video_hblank_dma(t, &REG_BG2PA, 4);
  m7_back ^= 1;
  REG_BGHOFS(1) = (pang >> 7) & 511;          /* full turn = 512 px of panorama */
}

/* ---------------------------------------------------------------- setup */
static void setup_video(void) {
  REG_DISPCNT = 0x0080;
  dma3_copy32(PAL_BG, track_pal, 32 / 2);
  dma3_copy32(PAL_BG + 14 * 16, hz_pal, 8);
  dma3_copy32(PAL_BG + 15 * 16, hud_pal, 8);
  dma3_copy32(PAL_OBJ, run_obj_pal, sizeof(run_obj_pal) / 4);
  dma3_copy32(PAL_OBJ + P_REAR * 16, rear_pal, 8);
  dma3_copy32(CHARBLOCK(0), track_tiles, sizeof(track_tiles) / 4);
  dma3_copy32(SCREENBLOCK(SBB_TRACK), track_map, sizeof(track_map) / 4);
  dma3_copy32(CHARBLOCK(2), hz_tiles, sizeof(hz_tiles) / 4);
  dma3_copy32(CHARBLOCK(2) + FONT_TILE * 16, font_tiles, sizeof(font_tiles) / 4);
  dma3_fill32(SCREENBLOCK(SBB_HZ), 0, 1024);
  for (int b = 0; b < 2; b++)
    for (int i = 0; i < HZ_ROWS * 32; i++)
      SCREENBLOCK(SBB_HZ + b)[i] = hz_map[b * HZ_ROWS * 32 + i] | (14 << 12);
  dma3_copy32(OBJ_TILES + T_REAR * 16, rear_tiles, sizeof(rear_tiles) / 4);
  dma3_copy32(OBJ_TILES + T_RIVAL * 16, rival_tiles, sizeof(rival_tiles) / 4);
  REG_BGCNT[0] = BG_PRIO(0) | BG_CBB(2) | BG_SBB(SBB_HUD);
  REG_BGCNT[1] = BG_PRIO(2) | BG_CBB(2) | BG_SBB(SBB_HZ) | BG_SIZE_64x32;
  REG_BGCNT[2] = BG_PRIO(1) | BG_CBB(0) | BG_SBB(SBB_TRACK) | BG_SIZE_64x64;  /* affine 1024x1024 */
  REG_BGVOFS(1) = 0;
  REG_WIN0H = 240;
  REG_WIN0V = (HORIZON << 8) | 160;
  REG_WININ = 0x0035;      /* inside: BG0 BG2 OBJ + effects */
  REG_WINOUT = 0x0033;     /* outside (sky): BG0 BG1 OBJ + effects */
  txt_setup(0, 2, SBB_HUD, FONT_TILE, 15);
  video_hblank_dma(drive_sky, &PAL_BG[0], -1);
}

static void reset_race(void) {
  /* grid behind the checker on the first (northbound) straight */
  int gx = wpx(0);
  px = (gx + 20) << 8; py = (wpy(0)) << 8;
  pang = 192 << 8; pspd = 0; boost_t = 0; bump_cd = 0; pidx = 0; plap = 0;
  foc8 = 128 << 8;
  static const int gwp[NRIV] = {2, 2, 0};
  static const int glane[NRIV] = {-20, 20, -20};
  static const int gbase[NRIV] = {700, 760, 730};
  for (int i = 0; i < NRIV; i++) {
    Rival* r = &riv[i];
    r->x = (gx + glane[i]) << 8; r->y = wpy(gwp[i]) << 8;
    r->t = gwp[i] + 1; r->lap = 0; r->spd = 0; r->base = gbase[i];
    r->lane = (i & 1) ? 14 : -14; r->pal = 2 + i;
  }
  boosts = gs.orbs / 8; if (boosts < 2) boosts = 2; if (boosts > 9) boosts = 9;
}

static int prog_player(void) { return plap * NUM_WP + pidx; }
static int prog_rival(const Rival* r) { return r->lap * NUM_WP + r->t - 1; }

static int place_now(void) {
  int p = 1, me = prog_player();
  for (int i = 0; i < NRIV; i++) if (prog_rival(&riv[i]) > me) p++;
  return p;
}

/* ---------------------------------------------------------------- simulation */
static void update_player(int steer_ok) {
  int k = keys;
#ifdef AUTOPILOT
  {
    int tx = wpx(pidx + 4) - (px >> 8), ty = wpy(pidx + 4) - (py >> 8);
    s32 cr = icos(pang) * ty - isin(pang) * tx;
    k = KEY_A | (cr > 4096 * 3 ? KEY_RIGHT : cr < -4096 * 3 ? KEY_LEFT : 0);
    if (boosts && pspd > 700) k |= KEY_R;
  }
#endif
  if (steer_ok) {
    int turn = pspd > 300 ? 230 : pspd * 230 / 300;
    if (k & KEY_LEFT) pang -= turn;
    if (k & KEY_RIGHT) pang += turn;
    pang &= 0xFFFF;
    if ((((keys_hit & (KEY_R | KEY_L))) || (k & ~keys & KEY_R)) && boosts > 0 && boost_t < 20) { boosts--; boost_t = 70; sfx(SFX_BOOST); }
  }
  int c = col_at(px, py);
  int cap = boost_t ? BOOSTV : c == 0 ? SEAV : MAXV;
  if (steer_ok && (k & KEY_A)) pspd += boost_t ? 40 : 9;
  else if (boost_t) pspd += 40;
  else pspd -= 4;
  if (steer_ok && (k & KEY_B)) pspd -= 18;
  if (pspd > cap) pspd -= pspd - cap > 24 ? 24 : pspd - cap;
  if (pspd < 0) pspd = 0;
  if (boost_t) boost_t--;
  if (c == 3 && boost_t < 30) { boost_t = 45; sfx(SFX_WHOOSH); }
  s32 nx = px + ((icos(pang) * pspd) >> 12);
  s32 ny = py + ((isin(pang) * pspd) >> 12);
  if (col_at(nx, ny) == 2) {
    /* barrier: stop, lose speed, nudge back toward the centreline */
    int tx = wpx(pidx + 1) << 8, ty = wpy(pidx + 1) << 8;
    px += (tx - px) >> 4; py += (ty - py) >> 4;
    pspd = pspd * 5 / 8;
    if (!bump_cd) { sfx(SFX_BUMP); bump_cd = 20; }
  } else { px = nx; py = ny; }
  if (bump_cd) bump_cd--;
  /* progress: nearest waypoint in a window ahead of / behind the last one */
  int best = pidx, bd = 0x7FFFFFFF;
  for (int d = -4; d <= 8; d++) {
    int i = (pidx + d + NUM_WP) % NUM_WP;
    int dx = (px >> 8) - track_wp[i * 2], dy = (py >> 8) - track_wp[i * 2 + 1];
    int dd = dx * dx + dy * dy;
    if (dd < bd) { bd = dd; best = i; }
  }
  if (best < 20 && pidx > NUM_WP - 20) plap++;
  else if (best > NUM_WP - 20 && pidx < 20) plap--;
  pidx = best;
  /* field of view widens while boosting */
  int ft = boost_t ? (104 << 8) : (128 << 8);
  foc8 += (ft - foc8) >> 3;
}

static void update_rivals(int go) {
  int me = prog_player();
  for (int i = 0; i < NRIV; i++) {
    Rival* r = &riv[i];
    int want = r->base;
    int gap = prog_rival(r) - me;
    if (gap > 30) want -= 90; else if (gap < -30) want += 110;
    if (!go) want = 0;
    r->spd += r->spd < want ? 10 : -10;
    if (r->spd < 0) r->spd = 0;
    int a = r->t % NUM_WP, b = (r->t + NUM_WP - 1) % NUM_WP;
    int sx = track_wp[a * 2] - track_wp[b * 2], sy = track_wp[a * 2 + 1] - track_wp[b * 2 + 1];
    int tx = (track_wp[a * 2] << 8) + (-sy * r->lane << 8) / 16;
    int ty = (track_wp[a * 2 + 1] << 8) + (sx * r->lane << 8) / 16;
    int dx = (tx - r->x) >> 8, dy = (ty - r->y) >> 8;
    int L = approx_len(dx, dy);
    if (L < 20) { r->t++; if (r->t >= NUM_WP) { r->t -= NUM_WP; r->lap++; } }
    if (L < 1) L = 1;
    r->x += dx * r->spd / L;
    r->y += dy * r->spd / L;
    /* contact with the player */
    int ex = (r->x - px) >> 8, ey = (r->y - py) >> 8;
    if (iabs(ex) < 9 && iabs(ey) < 9) {
      int ax = ex == 0 ? 1 : ex, ay = ey;
      r->x += ax << 7; r->y += ay << 7;
      px -= ax << 7; py -= ay << 7;
      pspd = pspd * 7 / 8;
      if (!bump_cd) { sfx(SFX_BUMP); bump_cd = 20; }
    }
  }
}

/* ---------------------------------------------------------------- drawing */
typedef struct { int z, kind, x, y, scale, pal; } Spr;

static void draw_sprites(int bank) {
  Spr s[NRIV + 1];
  int n = 0;
  int car_z = CAM_H * foc8 / (CAR_LINE - HORIZON) >> 8;
  s[n].z = car_z; s[n].kind = 0; n++;
  s32 rx = -fy12, ry = fx12;
  for (int i = 0; i < NRIV; i++) {
    int dx = (riv[i].x - camx) >> 4, dy = (riv[i].y - camy) >> 4;   /* Q4 px */
    int z = (dx * fx12 + dy * fy12) >> 16;                            /* px */
    int x = (dx * rx + dy * ry) >> 12;                                /* Q4 px */
    if (z < 12 || z > 900) continue;
    int foc = foc8 >> 8;
    s[n].z = z; s[n].kind = 1; s[n].pal = riv[i].pal;
    s[n].x = 120 + (x * foc / z >> 4);
    s[n].y = HORIZON + CAM_H * foc / z;
    s[n].scale = (car_z * 7 << 6) / z;                               /* Q8, 1.75x at the player's distance */
    if (s[n].scale > 480) s[n].scale = 480;
    n++;
  }
  /* nearest first so it lands on top */
  for (int i = 1; i < n; i++) for (int j = i; j > 0 && s[j].z < s[j - 1].z; j--) { Spr t = s[j]; s[j] = s[j - 1]; s[j - 1] = t; }
  int aff = 0;
  for (int i = 0; i < n; i++) {
    ObjAttr* o = oam_add(); if (!o) break;
    if (s[i].kind == 0) {
      int hover = (frame_count >> 3) & 1;
      o->attr0 = ATTR0_Y(CAR_LINE - 29 + hover) | ATTR0_WIDE;
      o->attr1 = ATTR1_X(88) | ATTR1_SIZE_64;
      o->attr2 = ATTR2_TILE(T_REAR + bank * 32) | ATTR2_PAL(P_REAR) | ATTR2_PRIO(0);
    } else {
      int sc = s[i].scale;
      if (s[i].x < -64 || s[i].x > 300 || s[i].y > 200) { o->attr0 = ATTR0_HIDE; continue; }
      int inv = (256 << 8) / (sc < 16 ? 16 : sc);
      obj_aff(aff, inv, 0, 0, inv);
      int oy = s[i].y - ((12 * sc) >> 8) - 32;
      o->attr0 = ATTR0_Y(oy) | ATTR0_AFF_DBL | ATTR0_SQUARE;
      o->attr1 = ATTR1_X(s[i].x - 32) | ATTR1_SIZE_32 | ATTR1_AFFIDX(aff);
      o->attr2 = ATTR2_TILE(T_RIVAL + ((frame_count >> 2) & 1) * 16) | ATTR2_PAL(s[i].pal) | ATTR2_PRIO(0);
      aff++;
    }
  }
}

static const char* ordinal(int p) { return p == 1 ? "1ST" : p == 2 ? "2ND" : p == 3 ? "3RD" : "4TH"; }

static int finished_place;
static void hud(int lap) {
  txt_put(1, 1, "LAP");
  txt_putc(5, 1, '0' + lap, 15); txt_putc(6, 1, '/', 15); txt_putc(7, 1, '0' + LAPS, 15);
  txt_put(12, 1, "POS");
  txt_put(16, 1, ordinal(finished_place ? finished_place : place_now()));
  txt_put(21, 1, "BOOST");
  txt_putc(27, 1, '0' + boosts, 15);
}

/* ---------------------------------------------------------------- stage */
int drive_stage(void) {
  video_off();
  setup_video();
  reset_race();
  set_fade(16);
  build_m7();
  REG_DISPCNT = DCNT_MODE1 | DCNT_BG0 | DCNT_BG1 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D | DCNT_WIN0;
  audio_play(&song_drive);
  int t = 0, finished = 0, fin_t = 0;
  finished_place = 0;
  const int GO_AT = 200;
  for (;;) {
    t++;
    if (t <= 24) set_fade(16 - t * 16 / 24);
    int go = t >= GO_AT;
    if (!finished) {
      update_player(go);
      update_rivals(go);
    } else {
      /* coast through the finish */
      pspd -= pspd > 300 ? 6 : 0;
      px += (icos(pang) * pspd) >> 12; py += (isin(pang) * pspd) >> 12;
      update_rivals(1);
    }
    /* countdown */
    if (t == GO_AT - 135 || t == GO_AT - 90 || t == GO_AT - 45) sfx(SFX_COUNT);
    if (t == GO_AT) sfx(SFX_GO);
    txt_put(13, 7, "    ");
    if (t > GO_AT - 135 && t < GO_AT) txt_putc(15, 7, '0' + 3 - (t - (GO_AT - 135)) / 45, 15);
    else if (t >= GO_AT && t < GO_AT + 40) txt_put(13, 7, "GO!!");
    int lap = (prog_player() - FINISH_WP) / NUM_WP + 1;
    if (lap < 1) lap = 1;
    if (!finished && prog_player() >= FINISH_WP + LAPS * NUM_WP) {
      finished = 1; fin_t = t;
      gs.place = place_now();
      finished_place = gs.place;
      gs.drive_time = t - GO_AT;
      gs.score += (5 - gs.place) * 1000 + boosts * 200;
      sfx(SFX_GO);
      audio_fade(150);
    }
    if (lap > LAPS) lap = LAPS;
    hud(lap);
    if (finished) {
      txt_center(7, "FINISH!");
      txt_put(11, 9, ordinal(gs.place));
      txt_put(15, 9, "PLACE");
    }
    int bank = (keys & KEY_LEFT) && go ? 0 : (keys & KEY_RIGHT) && go ? 2 : 1;
    build_m7();
    oam_begin();
    draw_sprites(bank);
    oam_hide_rest();
    video_wait();
    if (finished && t - fin_t > 150) break;
  }
  fade_to(16, 40);
  video_hblank_stop();
  txt_clear();
  return 0;
}
