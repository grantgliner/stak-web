#include "video.h"
#include "audio.h"

ObjAttr oam_shadow[128] __attribute__((aligned(4)));
u16 txt_map[32 * 32] EWRAM_BSS __attribute__((aligned(4)));
int txt_dirty;
u32 frame_count;
u16 keys, keys_prev, keys_hit;
int fade_level;
static int fade_target, fade_step_frames, fade_ctr;
static int oam_next;
static int txt_sbb = -1, txt_font, txt_pal;

/* ---- HBlank DMA tables: slot 0 -> DMA0 (32-bit entries), slot 1 -> DMA2 (16-bit entries) */
typedef struct { const void* src; volatile void* dst; int units; int on; } Hdma;
static volatile Hdma hd[2];

void video_hblank_dma(const void* src, volatile void* dst, int words) {
  int slot = words > 0 ? 0 : 1;
  int u = words > 0 ? words : -words;
  hd[slot].on = 0;
  hd[slot].src = src; hd[slot].dst = dst; hd[slot].units = u;
  hd[slot].on = 1;
}
void video_hblank_stop(void) {
  hd[0].on = hd[1].on = 0;
  REG_DMA0CNT = 0; REG_DMA2CNT = 0;
}

void vblank_isr(void) {
  audio_vblank();
  if (hd[0].on) {
    REG_DMA0CNT = 0;
    const u32* s = (const u32*)hd[0].src; volatile u32* d = (volatile u32*)hd[0].dst;
    for (int i = 0; i < hd[0].units; i++) d[i] = s[i];
    REG_DMA0SAD = (u32)(s + hd[0].units);
    REG_DMA0DAD = (u32)d;
    REG_DMA0CNT = DMA_ENABLE | DMA_HBLANK | DMA_REPEAT | DMA_32 | DMA_DST_RELOAD | hd[0].units;
  }
  if (hd[1].on) {
    REG_DMA2CNT = 0;
    const u16* s = (const u16*)hd[1].src; volatile u16* d = (volatile u16*)hd[1].dst;
    for (int i = 0; i < hd[1].units; i++) d[i] = s[i];
    REG_DMA2SAD = (u32)(s + hd[1].units);
    REG_DMA2DAD = (u32)d;
    REG_DMA2CNT = DMA_ENABLE | DMA_HBLANK | DMA_REPEAT | DMA_DST_RELOAD | hd[1].units;
  }
}

/* ---- text */
void txt_setup(int bg, int cbb, int sbb, int font_base, int pal) {
  (void)bg; (void)cbb;
  txt_sbb = sbb; txt_font = font_base; txt_pal = pal;
  txt_clear();
}
void txt_clear(void) { for (int i = 0; i < 32 * 32; i++) txt_map[i] = 0; txt_dirty = 1; }
void txt_putc(int x, int y, char c, int pal) {
  if (x < 0 || x >= 32 || y < 0 || y >= 32) return;
  if (c >= 'a' && c <= 'z') c -= 32;
  if (c < 32 || c > 127) c = '?';
  txt_map[y * 32 + x] = c == ' ' ? 0 : ((txt_font + c - 32) | (pal << 12));
  txt_dirty = 1;
}
void txt_put(int x, int y, const char* s) { for (; *s; s++, x++) txt_putc(x, y, *s, txt_pal); }
void txt_center(int y, const char* s) { int n = 0; while (s[n]) n++; txt_put((30 - n) / 2, y, s); }
int fmt_int(char* buf, int v) {
  char t[12]; int n = 0, neg = v < 0; if (neg) v = -v;
  do { t[n++] = '0' + v % 10; v /= 10; } while (v);
  int k = 0; if (neg) buf[k++] = '-';
  while (n) buf[k++] = t[--n];
  buf[k] = 0; return k;
}
void txt_num(int x, int y, int v, int digits) {
  char b[12]; int n = fmt_int(b, v);
  for (int i = n; i < digits; i++) txt_putc(x++, y, '0', txt_pal);
  txt_put(x, y, b);
}

/* ---- OAM */
void oam_begin(void) { oam_next = 0; }
ObjAttr* oam_add(void) { return oam_next < 128 ? &oam_shadow[oam_next++] : 0; }
void oam_hide_rest(void) { for (int i = oam_next; i < 128; i++) oam_shadow[i].attr0 = ATTR0_HIDE; }
void obj_aff(int idx, s32 pa, s32 pb, s32 pc, s32 pd) {
  ObjAffine* m = (ObjAffine*)oam_shadow + idx;
  m->pa = pa; m->pb = pb; m->pc = pc; m->pd = pd;
}

void obj_text(int x, int y, const char* s, int tile_base, int pal) {
  for (; *s; s++, x += 8) {
    char c = *s;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c == ' ') continue;
    ObjAttr* o = oam_add(); if (!o) return;
    o->attr0 = ATTR0_Y(y);
    o->attr1 = ATTR1_X(x);
    o->attr2 = ATTR2_TILE(tile_base + c - 32) | ATTR2_PAL(pal) | ATTR2_PRIO(0);
  }
}

/* ---- fades */
void set_fade(int v) {
  fade_level = v;
  if (v == 0) { REG_BLDCNT = 0; return; }
  if (v > 0) { REG_BLDCNT = 0x00BF; REG_BLDY = v > 16 ? 16 : v; }
  else { REG_BLDCNT = 0x00FF; REG_BLDY = -v > 16 ? 16 : -v; }
}
void fade_to(int target, int frames) {
  for (int i = 1; i <= frames; i++) {
    int v = fade_level + (target - fade_level) * i / frames;
    video_wait();
    set_fade(v);
  }
  set_fade(target);
}

void video_wait(void) {
  audio_frame();
  VBlankIntrWait();
  dma3_copy32((void*)MEM_OAM, oam_shadow, sizeof(oam_shadow) / 4);
  if (txt_dirty && txt_sbb >= 0) { dma3_copy32(SCREENBLOCK(txt_sbb), txt_map, sizeof(txt_map) / 4); txt_dirty = 0; }
  keys_prev = keys;
  keys = ~REG_KEYINPUT & 0x3FF;
  keys_hit = keys & ~keys_prev;
  frame_count++;
  extern void sfx_frame(void);
  sfx_frame();
}

void video_off(void) {
  video_wait();
  REG_DISPCNT = 0x0080;
  video_hblank_stop();
  for (int i = 0; i < 128; i++) oam_shadow[i].attr0 = ATTR0_HIDE;
  dma3_copy32((void*)MEM_OAM, oam_shadow, sizeof(oam_shadow) / 4);
  for (int i = 0; i < 4; i++) { REG_BGHOFS(i) = 0; REG_BGVOFS(i) = 0; }
  REG_WININ = 0; REG_WINOUT = 0;
}
