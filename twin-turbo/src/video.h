#pragma once
#include "gba.h"

extern ObjAttr oam_shadow[128];
extern u16 txt_map[32 * 32];
extern int txt_dirty;
extern u32 frame_count;
extern u16 keys, keys_prev, keys_hit;

/* text BG: font at tile `font_base` in the char block of `txt_bg` */
void txt_setup(int bg, int cbb, int sbb, int font_base, int pal);
void txt_clear(void);
void txt_put(int x, int y, const char* s);
void txt_putc(int x, int y, char c, int pal);
void txt_center(int y, const char* s);
void txt_num(int x, int y, int v, int digits);   /* zero padded if digits > 0 */
int  fmt_int(char* buf, int v);

void oam_begin(void);
ObjAttr* oam_add(void);
void oam_hide_rest(void);
void obj_aff(int idx, s32 pa, s32 pb, s32 pc, s32 pd);

void video_wait(void);          /* VBlankIntrWait + OAM/text upload + input */
void video_off(void);
void video_hblank_dma(const void* src, volatile void* dst, int words);   /* DMA2 per-line table (161 entries) */
void video_hblank_stop(void);
void fade_to(int target, int frames);   /* blend brightness: 0 = normal, +16 white, -16 black */
void set_fade(int v);
extern int fade_level;

void obj_text(int x, int y, const char* s, int tile_base, int pal);   /* 8x8 sprite glyphs */

void vblank_isr(void);
