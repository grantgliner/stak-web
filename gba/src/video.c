#include "video.h"
#include "sound.h"
#include "gen/gfx_gen.h"
#include "gen/pieces_gen.h"

ObjAttr oam_shadow[128] EWRAM_BSS __attribute__((aligned(4)));   /* EWRAM: IWRAM is kept for the physics */
u16 txt_map[32 * 32] EWRAM_BSS;
int txt_dirty;
static int oam_next;
static u16 sky_ram[1024] EWRAM_BSS __attribute__((aligned(4)));   /* DMA0 can't read ROM */

void video_init(void) {
  REG_DISPCNT = 0x0080;   /* forced blank while loading */
  /* palettes */
  for (int i = 0; i < 96; i++) PAL_BG[i] = bg_pal[i];
  for (int i = 0; i < LOGO_COLORS; i++) PAL_BG[LOGO_COLOR_BASE + i] = logo_pal[i];
  for (int i = 0; i < 256; i++) PAL_OBJ[i] = obj_pal[i];
  /* BG tiles */
  dma3_copy32(CHARBLOCK(CBB_TEXT) + TILE_FONT * 16, font_tiles, sizeof(font_tiles) / 4);
  dma3_copy32(CHARBLOCK(CBB_TEXT) + TILE_FONT_PANEL * 16, font_panel_tiles, sizeof(font_panel_tiles) / 4);
  dma3_copy32(CHARBLOCK(CBB_TEXT) + TILE_BIG * 16, bigdigit_tiles, sizeof(bigdigit_tiles) / 4);
  dma3_copy32(CHARBLOCK(CBB_TEXT) + TILE_PANEL * 16, panel_tiles, sizeof(panel_tiles) / 4);
  dma3_copy32(CHARBLOCK(CBB_STARS), star_tiles, sizeof(star_tiles) / 4);
  dma3_copy32(SCREENBLOCK(SBB_STARS), star_map, sizeof(star_map) / 4);
  dma3_copy32(CHARBLOCK(CBB_LOGO), logo_tiles, sizeof(logo_tiles) / 4);
  /* OBJ tiles */
  for (int i = 0; i < PIECE_COUNT; i++) dma3_copy32(OBJ_TILES + OBJ_TILE_PIECE(i) * 16, piece_tiles[i], 512);
  dma3_copy32(OBJ_TILES + OBJ_TILE_BASE * 16, base_tiles, sizeof(base_tiles) / 4);
  dma3_copy32(OBJ_TILES + OBJ_TILE_ARROW * 16, arrow_tiles, sizeof(arrow_tiles) / 4);
  dma3_copy32(OBJ_TILES + OBJ_TILE_FONT * 16, font_tiles, sizeof(font_tiles) / 4);
  /* logo map: LOGO_W/8 x LOGO_H/8 tiles at (2,1) */
  dma3_fill32(SCREENBLOCK(SBB_LOGO), 0, 512);
  {
    int tw = LOGO_W / 8, th = LOGO_H / 8, n = 0;
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) SCREENBLOCK(SBB_LOGO)[(y + 1) * 32 + x + 2] = n++;
  }
  REG_BGCNT[0] = BG_PRIO(1) | BG_CBB(CBB_TEXT) | BG_SBB(SBB_TEXT);
  REG_BGCNT[1] = BG_PRIO(1) | BG_CBB(CBB_LOGO) | BG_SBB(SBB_LOGO) | BG_8BPP;
  REG_BGCNT[2] = BG_PRIO(3) | BG_CBB(CBB_STARS) | BG_SBB(SBB_STARS);
  dma3_copy32(sky_ram, sky_grad, 512);
  txt_clear();
  oam_clear();
  for (int i = 0; i < 128; i++) ((volatile ObjAttr*)MEM_OAM)[i].attr0 = ATTR0_HIDE;
  /* VBlank interrupt for VBlankIntrWait */
  REG_DISPSTAT = 0x0008;
  REG_IE = 0x0001;
  REG_IME = 1;
}

/* The sky gradient's HBlank DMA is re-armed from the VBlank interrupt, not the
 * main loop: when a busy physics frame runs long (a tower falling), the main loop
 * misses a VBlank and a DMA left running would paint the next frame from the
 * wrong part of the gradient, which flashes. */
static volatile int sky_off = 1024 - 161;
volatile u16 vblank_tick;

/* Music and sound effects tick here too, so they keep time while the main loop
 * is busy. */
void vblank_isr(void) {
  vblank_tick = REG_TM2CNT_L;
  int o = sky_off;
  REG_DMA0CNT = 0;
  PAL_BG[0] = sky_ram[o];
  REG_DMA0SAD = (u32)&sky_ram[o + 1];
  REG_DMA0DAD = (u32)PAL_BG;
  REG_DMA0CNT = DMA_ENABLE | DMA_HBLANK | DMA_REPEAT | DMA_DST_FIXED | 1;
  snd_tick();
}

void video_vsync(int sky_offset) {
  if (sky_offset < 0) sky_offset = 0;
  if (sky_offset > 1024 - 161) sky_offset = 1024 - 161;
  sky_off = sky_offset;
  VBlankIntrWait();
  dma3_copy32((void*)MEM_OAM, oam_shadow, sizeof(oam_shadow) / 4);
  if (txt_dirty) { dma3_copy32(SCREENBLOCK(SBB_TEXT), txt_map, sizeof(txt_map) / 4); txt_dirty = 0; }
}

void oam_clear(void) {
  for (int i = 0; i < 128; i++) { oam_shadow[i].attr0 = ATTR0_HIDE; oam_shadow[i].attr1 = 0; oam_shadow[i].attr2 = 0; }
  oam_next = 0;
}
void oam_reset_alloc(int first) {
  for (int i = first; i < 128; i++) oam_shadow[i].attr0 = ATTR0_HIDE;
  oam_next = first;
}
ObjAttr* oam_alloc(void) { return oam_next < 128 ? &oam_shadow[oam_next++] : 0; }

void obj_affine_set(int idx, s32 c, s32 s, s32 scale8) {
  ObjAffine* m = (ObjAffine*)oam_shadow + idx;
  /* screen -> texture: R(theta) / scale, Q8 */
  s32 pa = (c >> 8) * 256 / scale8, ps = (s >> 8) * 256 / scale8;
  m->pa = pa; m->pb = -ps; m->pc = ps; m->pd = pa;
}

/* ---- text ---- */
void txt_clear(void) { for (int i = 0; i < 32 * 32; i++) txt_map[i] = 0; txt_dirty = 1; }
void txt_clear_rect(int x, int y, int w, int h) {
  for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) if (i >= 0 && i < 32 && j >= 0 && j < 32) txt_map[j * 32 + i] = 0;
  txt_dirty = 1;
}
static inline u16 glyph(char c, int pal) {
  if (c >= 'a' && c <= 'z') c -= 32;
  if (c < 32 || c > 127) c = '?';
  int base = (pal & TXT_ON_PANEL) ? TILE_FONT_PANEL : TILE_FONT;
  return (base + c - 32) | ((pal & 15) << 12);
}
void txt_put(int x, int y, const char* s, int pal) {
  for (; *s; s++, x++) if (x >= 0 && x < 32 && y >= 0 && y < 32) txt_map[y * 32 + x] = glyph(*s, pal);
  txt_dirty = 1;
}
int txt_wrap(int x, int y, int w, const char* s, int pal) {
  int lines = 0;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    int len = 0, last = -1;
    while (s[len] && len < w) { if (s[len] == ' ') last = len; len++; }
    if (s[len] && last > 0) len = last;
    int pad = (w - len) / 2;
    for (int i = 0; i < len; i++) if (y + lines < 32) txt_map[(y + lines) * 32 + x + pad + i] = glyph(s[i], pal);
    s += len; lines++;
  }
  txt_dirty = 1;
  return lines;
}
int fmt_int(char* buf, int v) {
  char t[12]; int n = 0, neg = v < 0; if (neg) v = -v;
  do { t[n++] = '0' + v % 10; v /= 10; } while (v);
  int k = 0; if (neg) buf[k++] = '-';
  while (n) buf[k++] = t[--n];
  buf[k] = 0; return k;
}
void txt_num(int x, int y, int value, int pal) { char b[12]; fmt_int(b, value); txt_put(x, y, b, pal); }
void txt_big_num(int xr, int y, int value, int pal) {
  char b[12]; int n = fmt_int(b, value);
  int x = xr - n * 2;
  for (int i = 0; i < n; i++, x += 2) {
    int d = b[i] - '0'; u16 t = TILE_BIG + d * 4;
    txt_map[y * 32 + x] = t | (pal << 12); txt_map[y * 32 + x + 1] = (t + 1) | (pal << 12);
    txt_map[(y + 1) * 32 + x] = (t + 2) | (pal << 12); txt_map[(y + 1) * 32 + x + 1] = (t + 3) | (pal << 12);
  }
  txt_dirty = 1;
}
void txt_center(int y, int x0, int w, const char* s, int pal) {
  int n = 0; while (s[n]) n++;
  txt_put(x0 + (w - n) / 2, y, s, pal);
}
void txt_panel(int x, int y, int w, int h, int pal) {
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
    int t = PANEL_FILL;
    if (j == 0) t = i == 0 ? PANEL_TL : i == w - 1 ? PANEL_TR : PANEL_TOP;
    else if (j == h - 1) t = i == 0 ? PANEL_BL : i == w - 1 ? PANEL_BR : PANEL_BOT;
    else if (i == 0) t = PANEL_LEFT; else if (i == w - 1) t = PANEL_RIGHT;
    txt_map[(y + j) * 32 + x + i] = (TILE_PANEL + t) | (pal << 12);
  }
  txt_dirty = 1;
}
